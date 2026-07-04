#include "config_store.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define NAMESPACE "radarcfg"

static const char *TAG = "config_store";

static esp_err_t get_str(nvs_handle_t h, const char *key, char *out, size_t out_size)
{
    size_t len = out_size;
    return nvs_get_str(h, key, out, &len);
}

esp_err_t config_store_load(radar_config_t *out)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    char configured[4] = {0};
    err = get_str(h, "configured", configured, sizeof(configured));
    if (err != ESP_OK || strcmp(configured, "1") != 0) {
        ESP_LOGW(TAG, "'configured' check failed: err=%s value='%s'", esp_err_to_name(err), configured);
        nvs_close(h);
        return ESP_ERR_NVS_NOT_FOUND;
    }

    memset(out, 0, sizeof(*out));

    char port_str[6] = {0};
    char lat_str[32] = {0};
    char lon_str[32] = {0};
    char range_str[32] = {0};
    char max_aircraft_str[8] = {0};
    char refresh_str[16] = {0};
    char label_mode_str[4] = {0};

    esp_err_t e_ssid = get_str(h, "wifi_ssid", out->wifi_ssid, sizeof(out->wifi_ssid));
    esp_err_t e_pass = get_str(h, "wifi_pass", out->wifi_pass, sizeof(out->wifi_pass));
    esp_err_t e_host = get_str(h, "sky_host", out->sky_host, sizeof(out->sky_host));
    esp_err_t e_port = get_str(h, "sky_port", port_str, sizeof(port_str));
    esp_err_t e_lat = get_str(h, "home_lat", lat_str, sizeof(lat_str));
    esp_err_t e_lon = get_str(h, "home_lon", lon_str, sizeof(lon_str));
    esp_err_t e_range = get_str(h, "range_nm", range_str, sizeof(range_str));
    // Optional: a config saved before these fields existed simply won't
    // have these keys. Left blank/zero (out was memset above) rather than
    // failing the whole load - see config_store_ensure_live_cfg_auth,
    // config_store_ensure_max_aircraft, and config_store_ensure_refresh_interval.
    get_str(h, "cfg_username", out->live_cfg_username, sizeof(out->live_cfg_username));
    get_str(h, "cfg_password", out->live_cfg_password, sizeof(out->live_cfg_password));
    esp_err_t e_max_aircraft = get_str(h, "max_aircraft", max_aircraft_str, sizeof(max_aircraft_str));
    esp_err_t e_refresh = get_str(h, "refresh_sec", refresh_str, sizeof(refresh_str));
    esp_err_t e_label_mode = get_str(h, "label_mode", label_mode_str, sizeof(label_mode_str));
    nvs_close(h);

    ESP_LOGI(TAG, "load: ssid=%s(%s) pass=%s host=%s(%s) port=%s(%s) lat=%s(%s) lon=%s(%s) range=%s(%s) "
                  "max_aircraft=%s(%s) refresh=%s(%s) label_mode=%s(%s) live_cfg_user=%s live_cfg_password_present=%d",
             out->wifi_ssid, esp_err_to_name(e_ssid), esp_err_to_name(e_pass),
             out->sky_host, esp_err_to_name(e_host), port_str, esp_err_to_name(e_port),
             lat_str, esp_err_to_name(e_lat), lon_str, esp_err_to_name(e_lon),
             range_str, esp_err_to_name(e_range), max_aircraft_str, esp_err_to_name(e_max_aircraft),
             refresh_str, esp_err_to_name(e_refresh), label_mode_str, esp_err_to_name(e_label_mode),
             out->live_cfg_username, out->live_cfg_password[0] != '\0');

    if (e_ssid || e_pass || e_host || e_port || e_lat || e_lon || e_range) {
        return ESP_ERR_NVS_NOT_FOUND;
    }

    if (!config_store_parse_port(port_str, &out->sky_port)) {
        ESP_LOGW(TAG, "invalid sky_port '%s'", port_str);
        return ESP_ERR_INVALID_ARG;
    }
    if (!config_store_parse_lat(lat_str, &out->home_lat)) {
        ESP_LOGW(TAG, "invalid home_lat '%s'", lat_str);
        return ESP_ERR_INVALID_ARG;
    }
    if (!config_store_parse_lon(lon_str, &out->home_lon)) {
        ESP_LOGW(TAG, "invalid home_lon '%s'", lon_str);
        return ESP_ERR_INVALID_ARG;
    }
    if (!config_store_parse_range_nm(range_str, &out->range_nm)) {
        ESP_LOGW(TAG, "invalid range_nm '%s'", range_str);
        return ESP_ERR_INVALID_ARG;
    }
    if (e_max_aircraft == ESP_OK && !config_store_parse_max_aircraft(max_aircraft_str, &out->max_aircraft)) {
        ESP_LOGW(TAG, "invalid max_aircraft '%s' - defaulting", max_aircraft_str);
        out->max_aircraft = 0;
    }
    if (e_refresh == ESP_OK && !config_store_parse_refresh_interval_sec(refresh_str, &out->refresh_interval_sec)) {
        ESP_LOGW(TAG, "invalid refresh_sec '%s' - defaulting", refresh_str);
        out->refresh_interval_sec = 0;
    }
    if (e_label_mode == ESP_OK && !config_store_parse_label_mode(label_mode_str, &out->label_mode)) {
        ESP_LOGW(TAG, "invalid label_mode '%s' - defaulting", label_mode_str);
        out->label_mode = RADAR_LABEL_CALLSIGN;
    }

    return ESP_OK;
}

esp_err_t config_store_save(const radar_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    char port_str[6], lat_str[32], lon_str[32], range_str[16], max_aircraft_str[8], refresh_str[16];
    char label_mode_str[4];
    snprintf(port_str, sizeof(port_str), "%u", cfg->sky_port);
    snprintf(lat_str, sizeof(lat_str), "%f", cfg->home_lat);
    snprintf(lon_str, sizeof(lon_str), "%f", cfg->home_lon);
    snprintf(range_str, sizeof(range_str), "%.1f", cfg->range_nm);
    snprintf(max_aircraft_str, sizeof(max_aircraft_str), "%d", cfg->max_aircraft);
    snprintf(refresh_str, sizeof(refresh_str), "%.1f", cfg->refresh_interval_sec);
    snprintf(label_mode_str, sizeof(label_mode_str), "%d", (int)cfg->label_mode);

    ESP_LOGI(TAG, "save: ssid='%s' host='%s' port='%s' lat='%s' lon='%s' range='%s' max_aircraft='%s' "
                  "refresh='%s' label_mode='%s' live_cfg_user='%s' live_cfg_password_present=%d",
             cfg->wifi_ssid, cfg->sky_host, port_str, lat_str, lon_str, range_str, max_aircraft_str,
             refresh_str, label_mode_str, cfg->live_cfg_username, cfg->live_cfg_password[0] != '\0');

    esp_err_t e_ssid = nvs_set_str(h, "wifi_ssid", cfg->wifi_ssid);
    esp_err_t e_pass = nvs_set_str(h, "wifi_pass", cfg->wifi_pass);
    esp_err_t e_host = nvs_set_str(h, "sky_host", cfg->sky_host);
    esp_err_t e_port = nvs_set_str(h, "sky_port", port_str);
    esp_err_t e_lat = nvs_set_str(h, "home_lat", lat_str);
    esp_err_t e_lon = nvs_set_str(h, "home_lon", lon_str);
    esp_err_t e_range = nvs_set_str(h, "range_nm", range_str);
    esp_err_t e_max_aircraft = nvs_set_str(h, "max_aircraft", max_aircraft_str);
    esp_err_t e_refresh = nvs_set_str(h, "refresh_sec", refresh_str);
    esp_err_t e_label_mode = nvs_set_str(h, "label_mode", label_mode_str);
    esp_err_t e_username = nvs_set_str(h, "cfg_username", cfg->live_cfg_username);
    esp_err_t e_password = nvs_set_str(h, "cfg_password", cfg->live_cfg_password);
    esp_err_t e_flag = nvs_set_str(h, "configured", "1");

    ESP_LOGI(TAG, "save results: ssid=%s pass=%s host=%s port=%s lat=%s lon=%s range=%s max_aircraft=%s "
                  "refresh=%s label_mode=%s user=%s password=%s flag=%s",
             esp_err_to_name(e_ssid), esp_err_to_name(e_pass), esp_err_to_name(e_host), esp_err_to_name(e_port),
             esp_err_to_name(e_lat), esp_err_to_name(e_lon), esp_err_to_name(e_range), esp_err_to_name(e_max_aircraft),
             esp_err_to_name(e_refresh), esp_err_to_name(e_label_mode), esp_err_to_name(e_username),
             esp_err_to_name(e_password), esp_err_to_name(e_flag));

    esp_err_t any_err = e_ssid || e_pass || e_host || e_port || e_lat || e_lon || e_range || e_max_aircraft
                                || e_refresh || e_label_mode || e_username || e_password || e_flag
                             ? ESP_FAIL : ESP_OK;

    if (any_err == ESP_OK) {
        any_err = nvs_commit(h);
        ESP_LOGI(TAG, "nvs_commit: %s", esp_err_to_name(any_err));
    }

    nvs_close(h);
    return any_err;
}

esp_err_t config_store_ensure_live_cfg_auth(radar_config_t *cfg)
{
    bool changed = false;
    if (cfg->live_cfg_username[0] == '\0') {
        strncpy(cfg->live_cfg_username, LIVE_CFG_DEFAULT_USERNAME, sizeof(cfg->live_cfg_username) - 1);
        changed = true;
    }
    if (cfg->live_cfg_password[0] == '\0') {
        strncpy(cfg->live_cfg_password, LIVE_CFG_DEFAULT_PASSWORD, sizeof(cfg->live_cfg_password) - 1);
        changed = true;
    }
    if (!changed) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "defaulted missing live-config credentials");
    return config_store_save(cfg);
}

esp_err_t config_store_ensure_max_aircraft(radar_config_t *cfg)
{
    if (cfg->max_aircraft > 0) {
        return ESP_OK;
    }
    cfg->max_aircraft = MAX_AIRCRAFT_DEFAULT;
    ESP_LOGI(TAG, "defaulted missing max_aircraft to %d", MAX_AIRCRAFT_DEFAULT);
    return config_store_save(cfg);
}

esp_err_t config_store_ensure_refresh_interval(radar_config_t *cfg)
{
    if (cfg->refresh_interval_sec > 0) {
        return ESP_OK;
    }
    cfg->refresh_interval_sec = REFRESH_INTERVAL_DEFAULT_SEC;
    ESP_LOGI(TAG, "defaulted missing refresh_interval_sec to %.1f", REFRESH_INTERVAL_DEFAULT_SEC);
    return config_store_save(cfg);
}

bool config_store_parse_port(const char *str, uint16_t *out)
{
    char *endptr;
    // Checked against UINT16_MAX before the cast rather than after - casting
    // an out-of-range value (e.g. "65537") to uint16_t first would silently
    // wrap it into something that looks like a plausible port instead of
    // getting caught by the `== 0` check below.
    unsigned long v = strtoul(str, &endptr, 10);
    if (*endptr != '\0' || v == 0 || v > UINT16_MAX) {
        return false;
    }
    *out = (uint16_t)v;
    return true;
}

bool config_store_parse_lat(const char *str, double *out)
{
    char *endptr;
    double v = strtod(str, &endptr);
    if (*endptr != '\0' || v < -90 || v > 90) {
        return false;
    }
    *out = v;
    return true;
}

bool config_store_parse_lon(const char *str, double *out)
{
    char *endptr;
    double v = strtod(str, &endptr);
    if (*endptr != '\0' || v < -180 || v > 180) {
        return false;
    }
    *out = v;
    return true;
}

bool config_store_parse_range_nm(const char *str, double *out)
{
    char *endptr;
    double v = strtod(str, &endptr);
    if (*endptr != '\0' || v <= 0 || v > 500) {
        return false;
    }
    *out = v;
    return true;
}

bool config_store_parse_max_aircraft(const char *str, int *out)
{
    char *endptr;
    long v = strtol(str, &endptr, 10);
    if (*endptr != '\0' || v < 1 || v > MAX_AIRCRAFT_CAP) {
        return false;
    }
    *out = (int)v;
    return true;
}

bool config_store_parse_refresh_interval_sec(const char *str, double *out)
{
    char *endptr;
    double v = strtod(str, &endptr);
    if (*endptr != '\0' || v < REFRESH_INTERVAL_MIN_SEC || v > REFRESH_INTERVAL_MAX_SEC) {
        return false;
    }
    *out = v;
    return true;
}

bool config_store_parse_label_mode(const char *str, radar_label_mode_t *out)
{
    char *endptr;
    long v = strtol(str, &endptr, 10);
    if (*endptr != '\0' || v < RADAR_LABEL_CALLSIGN || v > RADAR_LABEL_NONE) {
        return false;
    }
    *out = (radar_label_mode_t)v;
    return true;
}
