#include "live_config_http_server.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"

#include "http_post_utils.h"

static const char *TAG = "live_config_http";

// Owns the live-editable config: run_radar_loop reads a fresh copy every
// refresh cycle (live_config_get_current) and save_post_handler writes to
// it below, so all access goes through g_lock. wifi_ssid/wifi_pass/
// live_cfg_username/live_cfg_password are seeded once at start and never
// modified here.
static radar_config_t g_cfg;
static SemaphoreHandle_t g_lock;

// Sized for the worst case: a full 32-char live_cfg_username + ':' + a full
// 64-char live_cfg_password is 97 raw bytes, base64-encoded to 132 chars,
// plus the 6-char "Basic " prefix - see HEADER_BUF_LEN/DECODED_BUF_LEN below.
#define HEADER_BUF_LEN 160
#define DECODED_BUF_LEN 112

// Checks the "Authorization: Basic <base64(user:pass)>" header against
// g_cfg.live_cfg_username/live_cfg_password. Using mbedtls's base64 decoder
// (already in the dependency tree via esp_http_client/esp-tls) rather than
// hand-rolling one - this is exactly the kind of small-but-easy-to-get-wrong
// parsing logic worth reusing a vetted implementation for.
static bool check_auth(httpd_req_t *req)
{
    char header[HEADER_BUF_LEN];
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) != ESP_OK) {
        return false;
    }
    static const char prefix[] = "Basic ";
    size_t prefix_len = sizeof(prefix) - 1;
    if (strncmp(header, prefix, prefix_len) != 0) {
        return false;
    }

    unsigned char decoded[DECODED_BUF_LEN];
    size_t decoded_len = 0;
    int rc = mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                                    (const unsigned char *)header + prefix_len, strlen(header) - prefix_len);
    if (rc != 0) {
        return false;
    }
    decoded[decoded_len] = '\0';

    char *colon = memchr(decoded, ':', decoded_len);
    if (colon == NULL) {
        return false;
    }
    *colon = '\0';
    const char *username = (const char *)decoded;
    const char *password = colon + 1;

    // live_cfg_username/live_cfg_password are fixed for this server's
    // lifetime (never edited via this endpoint), so reading g_cfg here
    // without g_lock is safe.
    return strcmp(username, g_cfg.live_cfg_username) == 0 && strcmp(password, g_cfg.live_cfg_password) == 0;
}

static esp_err_t send_auth_required(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32-ADSB-Radar\"");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Authentication required", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Renders the config form itself, optionally preceded by a status line
// (e.g. "Saved." after a POST) - used for both the plain GET and the
// POST response, so saving re-shows the same form with the just-saved
// values instead of navigating to an unrelated blank confirmation page.
static void render_form(char *html, size_t html_cap, const radar_config_t *cfg, const char *status)
{
    char status_html[64] = "";
    if (status != NULL) {
        snprintf(status_html, sizeof(status_html), "<p><b>%s</b></p>", status);
    }
    // Sized with margin over the worst case: ~1KB of literal markup
    // (including the viewport meta/style block below) plus a full 64-char
    // sky_host, status_html, and the other substituted fields - see
    // html_cap at both call sites. Get this wrong quietly and snprintf just
    // truncates the page; get the *caller's* stack budget wrong for a
    // buffer this size and it's a LoadProhibited panic instead (see
    // live_config_http_server_start's stack_size comment) - the two
    // failure modes look nothing alike, so when growing this template,
    // re-check both, not just this one.
    snprintf(html, html_cap,
        "<!DOCTYPE html><html><head><title>ESP32-ADSB-Radar Live Config</title>"
        // Without this, mobile browsers render at a desktop-width virtual
        // viewport (~980px) and shrink the whole page to fit - this pins
        // the viewport to the actual device width instead.
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<style>"
        "body{font-family:sans-serif;font-size:1.2em;margin:1em;max-width:480px}"
        // %% here is a literal '%' for CSS, escaped for snprintf's benefit -
        // a bare '%' would be parsed as (the start of) a conversion
        // specifier, which is undefined behavior for an unrecognized one.
        "input{width:100%%;box-sizing:border-box;font-size:1em;padding:0.5em;margin:0.3em 0}"
        "input[type=submit]{width:auto;padding:0.6em 1.5em}"
        "</style>"
        "</head><body>"
        "<h2>ESP32-ADSB-Radar Live Config</h2>%s"
        "<form method=\"POST\" action=\"/save\">"
        "SkyAware Host:<br><input name=\"host\" maxlength=\"64\" value=\"%s\" required><br>"
        "SkyAware Port:<br><input name=\"port\" type=\"number\" value=\"%u\" required><br>"
        "Home Latitude:<br><input name=\"lat\" type=\"number\" step=\"any\" value=\"%f\" required><br>"
        "Home Longitude:<br><input name=\"lon\" type=\"number\" step=\"any\" value=\"%f\" required><br>"
        "Radar Range (nm):<br><input name=\"range\" type=\"number\" step=\"any\" value=\"%.1f\" required><br><br>"
        "<input type=\"submit\" value=\"Save\">"
        "</form><p>Applies on the next radar refresh - no reboot.</p></body></html>",
        status_html, cfg->sky_host, cfg->sky_port,
        cfg->home_lat, cfg->home_lon, cfg->range_nm);
}

static esp_err_t form_get_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return send_auth_required(req);
    }

    radar_config_t snapshot;
    live_config_get_current(&snapshot);

    char html[1536];
    render_form(html, sizeof(html), &snapshot, NULL);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return send_auth_required(req);
    }

    char body[512];
    size_t total;
    esp_err_t read_err = http_read_post_body(req, body, sizeof(body), &total);
    if (read_err == ESP_ERR_INVALID_SIZE) {
        return http_send_error(req, "Request body missing or too large");
    } else if (read_err != ESP_OK) {
        return http_send_error(req, "Failed to read request body");
    }

    char host[65] = {0}, port_str[8] = {0}, lat_str[32] = {0}, lon_str[32] = {0}, range_str[32] = {0};
    bool ok = form_urlencoded_get(body, total, "host", host, sizeof(host));
    ok = ok && form_urlencoded_get(body, total, "port", port_str, sizeof(port_str));
    ok = ok && form_urlencoded_get(body, total, "lat", lat_str, sizeof(lat_str));
    ok = ok && form_urlencoded_get(body, total, "lon", lon_str, sizeof(lon_str));
    ok = ok && form_urlencoded_get(body, total, "range", range_str, sizeof(range_str));
    if (!ok || strlen(host) == 0) {
        return http_send_error(req, "Missing required field");
    }

    uint16_t port;
    double lat, lon, range;

    if (!config_store_parse_port(port_str, &port)) {
        return http_send_error(req, "Invalid SkyAware port");
    }
    if (!config_store_parse_lat(lat_str, &lat)) {
        return http_send_error(req, "Invalid latitude");
    }
    if (!config_store_parse_lon(lon_str, &lon)) {
        return http_send_error(req, "Invalid longitude");
    }
    if (!config_store_parse_range_nm(range_str, &range)) {
        return http_send_error(req, "Invalid radar range");
    }

    xSemaphoreTake(g_lock, portMAX_DELAY);
    strncpy(g_cfg.sky_host, host, sizeof(g_cfg.sky_host) - 1);
    g_cfg.sky_host[sizeof(g_cfg.sky_host) - 1] = '\0';
    g_cfg.sky_port = port;
    g_cfg.home_lat = lat;
    g_cfg.home_lon = lon;
    g_cfg.range_nm = range;
    esp_err_t save_err = config_store_save(&g_cfg);
    xSemaphoreGive(g_lock);

    if (save_err != ESP_OK) {
        ESP_LOGE(TAG, "config_store_save failed: %s", esp_err_to_name(save_err));
        return http_send_error(req, "Failed to save config");
    }

    radar_config_t snapshot;
    live_config_get_current(&snapshot);

    char html[1536];
    render_form(html, sizeof(html), &snapshot, "Saved.");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

void live_config_http_server_start(const radar_config_t *initial_cfg)
{
    g_cfg = *initial_cfg;
    g_lock = xSemaphoreCreateMutex();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 4;
    // HTTPD_DEFAULT_CONFIG's 4096-byte worker stack is too small once
    // save_post_handler's frame holds both its POST-body/field buffers and
    // render_form's ~1KB HTML output at the same time (all local variables
    // reserve stack space for the whole function call, not just from the
    // line they're declared on) - this genuinely overflowed under real
    // load-testing (a LoadProhibited panic, not merely a close call). If
    // render_form's template grows again, re-check this margin on real
    // hardware rather than assuming a clean build proves it's still enough -
    // this class of bug is invisible at compile time.
    config.stack_size = 8192;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }

    httpd_uri_t form_uri = {.uri = "/", .method = HTTP_GET, .handler = form_get_handler};
    httpd_register_uri_handler(server, &form_uri);

    httpd_uri_t save_uri = {.uri = "/save", .method = HTTP_POST, .handler = save_post_handler};
    httpd_register_uri_handler(server, &save_uri);

    ESP_LOGI(TAG, "live-config HTTP server started");
}

void live_config_get_current(radar_config_t *out)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    *out = g_cfg;
    xSemaphoreGive(g_lock);
}
