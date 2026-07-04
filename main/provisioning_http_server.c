#include "provisioning_http_server.h"

#include <stdbool.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config_store.h"
#include "http_post_utils.h"

static const char *TAG = "provisioning_http";

static const char FORM_HTML[] =
    "<!DOCTYPE html><html><head><title>ESP32-ADSB-Radar Setup</title>"
    // Without this, mobile browsers render at a desktop-width virtual
    // viewport (~980px) and shrink the whole page to fit, which is exactly
    // the "have to zoom in" symptom - this pins the viewport to the actual
    // device width so text/inputs render at their real, readable size.
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<style>"
    "body{font-family:sans-serif;font-size:1.2em;margin:1em;max-width:480px}"
    // select is listed alongside input so the label dropdown gets the exact
    // same width/box-sizing/font-size/padding/margin as every text/number
    // input, instead of rendering at the browser's own default size.
    "input,select{width:100%;box-sizing:border-box;font-size:1em;padding:0.5em;margin:0.3em 0}"
    "input[type=submit]{width:auto;padding:0.6em 1.5em}"
    "</style>"
    "</head><body>"
    "<h2>ESP32-ADSB-Radar Setup</h2>"
    "<form method=\"POST\" action=\"/save\">"
    "WiFi SSID:<br><input name=\"ssid\" maxlength=\"32\" required><br>"
    "WiFi Password:<br><input name=\"pass\" type=\"password\" maxlength=\"64\"><br>"
    "SkyAware Host:<br><input name=\"host\" maxlength=\"64\" required><br>"
    "SkyAware Port:<br><input name=\"port\" type=\"number\" value=\"8080\" required><br>"
    "Home Latitude:<br><input name=\"lat\" type=\"number\" step=\"any\" required><br>"
    "Home Longitude:<br><input name=\"lon\" type=\"number\" step=\"any\" required><br>"
    "Radar Range (nm):<br><input name=\"range\" type=\"number\" step=\"any\" value=\"20\" required><br>"
    "Max Aircraft Displayed:<br><input name=\"max_aircraft\" type=\"number\" min=\"1\" max=\"8\" value=\"8\" required><br>"
    "Refresh Interval (seconds):<br><input name=\"refresh\" type=\"number\" step=\"any\" min=\"1\" value=\"3\" required><br>"
    "Aircraft Label:<br><select name=\"label_mode\">"
    "<option value=\"0\" selected>Callsign</option>"
    "<option value=\"1\">None</option>"
    "</select><br>"
    "Live Config Username:<br><input name=\"live_user\" maxlength=\"32\" value=\"adsbradar\" required><br>"
    "Live Config Password:<br><input name=\"live_pass\" type=\"password\" maxlength=\"64\" value=\"adsbradar\" required><br><br>"
    "<input type=\"submit\" value=\"Save and Reboot\">"
    "</form></body></html>";

static esp_err_t form_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, FORM_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    char body[1025];
    size_t total;
    esp_err_t read_err = http_read_post_body(req, body, sizeof(body), &total);
    if (read_err == ESP_ERR_INVALID_SIZE) {
        return http_send_error(req, "Request body missing or too large");
    } else if (read_err != ESP_OK) {
        return http_send_error(req, "Failed to read request body");
    }
    ESP_LOGI(TAG, "POST /save body (%d bytes)", (int)total);

    radar_config_t cfg = {0};
    char port_str[8] = {0}, lat_str[32] = {0}, lon_str[32] = {0}, range_str[32] = {0};
    char max_aircraft_str[8] = {0}, refresh_str[16] = {0}, label_mode_str[4] = {0};

    bool ok = form_urlencoded_get(body, total, "ssid", cfg.wifi_ssid, sizeof(cfg.wifi_ssid));
    form_urlencoded_get(body, total, "pass", cfg.wifi_pass, sizeof(cfg.wifi_pass));
    ok = ok && form_urlencoded_get(body, total, "host", cfg.sky_host, sizeof(cfg.sky_host));
    ok = ok && form_urlencoded_get(body, total, "port", port_str, sizeof(port_str));
    ok = ok && form_urlencoded_get(body, total, "lat", lat_str, sizeof(lat_str));
    ok = ok && form_urlencoded_get(body, total, "lon", lon_str, sizeof(lon_str));
    ok = ok && form_urlencoded_get(body, total, "range", range_str, sizeof(range_str));
    ok = ok && form_urlencoded_get(body, total, "max_aircraft", max_aircraft_str, sizeof(max_aircraft_str));
    ok = ok && form_urlencoded_get(body, total, "refresh", refresh_str, sizeof(refresh_str));
    ok = ok && form_urlencoded_get(body, total, "label_mode", label_mode_str, sizeof(label_mode_str));
    ok = ok && form_urlencoded_get(body, total, "live_user", cfg.live_cfg_username, sizeof(cfg.live_cfg_username));
    ok = ok && form_urlencoded_get(body, total, "live_pass", cfg.live_cfg_password, sizeof(cfg.live_cfg_password));

    if (!ok || strlen(cfg.wifi_ssid) == 0) {
        return http_send_error(req, "Missing required field");
    }

    if (!config_store_parse_port(port_str, &cfg.sky_port)) {
        return http_send_error(req, "Invalid SkyAware port");
    }
    if (!config_store_parse_lat(lat_str, &cfg.home_lat)) {
        return http_send_error(req, "Invalid latitude");
    }
    if (!config_store_parse_lon(lon_str, &cfg.home_lon)) {
        return http_send_error(req, "Invalid longitude");
    }
    if (!config_store_parse_range_nm(range_str, &cfg.range_nm)) {
        return http_send_error(req, "Invalid radar range");
    }
    if (!config_store_parse_max_aircraft(max_aircraft_str, &cfg.max_aircraft)) {
        return http_send_error(req, "Invalid max aircraft displayed");
    }
    if (!config_store_parse_refresh_interval_sec(refresh_str, &cfg.refresh_interval_sec)) {
        return http_send_error(req, "Invalid refresh interval");
    }
    if (!config_store_parse_label_mode(label_mode_str, &cfg.label_mode)) {
        return http_send_error(req, "Invalid aircraft label");
    }

    esp_err_t err = config_store_save(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config_store_save failed: %s", esp_err_to_name(err));
        return http_send_error(req, "Failed to save config");
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, "<html><body>Saved. Rebooting...</body></html>", HTTPD_RESP_USE_STRLEN);

    // Reboot from a separate task so this handler's response has time to
    // flush over the socket before the device restarts.
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

void provisioning_http_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    // Default 1024 is too small for modern browsers' client-hint headers
    // (sec-ch-ua, sec-fetch-*, a long User-Agent, etc.) - undersizing this
    // gets every request rejected with "431 Request Header Fields Too Large"
    // before it ever reaches a handler.
    config.max_req_hdr_len = 4096;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }

    httpd_uri_t form_uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = form_get_handler},
        {.uri = "/generate_204", .method = HTTP_GET, .handler = form_get_handler},
        {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = form_get_handler},
        {.uri = "/ncsi.txt", .method = HTTP_GET, .handler = form_get_handler},
        {.uri = "/connecttest.txt", .method = HTTP_GET, .handler = form_get_handler},
    };
    for (size_t i = 0; i < sizeof(form_uris) / sizeof(form_uris[0]); i++) {
        httpd_register_uri_handler(server, &form_uris[i]);
    }

    httpd_uri_t save_uri = {.uri = "/save", .method = HTTP_POST, .handler = save_post_handler};
    httpd_register_uri_handler(server, &save_uri);

    ESP_LOGI(TAG, "provisioning HTTP server started");
}
