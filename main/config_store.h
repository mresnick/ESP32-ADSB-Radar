#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// Fallback credentials for main/live_config_http_server.c's HTTP Basic Auth
// when the provisioning form's own fields (which prefill with these same
// values) are missing - see config_store_ensure_live_cfg_auth.
#define LIVE_CFG_DEFAULT_USERNAME "adsbradar"
#define LIVE_CFG_DEFAULT_PASSWORD "adsbradar"

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char sky_host[65];
    uint16_t sky_port;
    double home_lat;
    double home_lon;
    double range_nm;
    // HTTP Basic Auth credentials gating main/live_config_http_server.c -
    // set via provisioning_http_server.c's form (defaulting to
    // LIVE_CFG_DEFAULT_USERNAME/PASSWORD there), editable only through
    // re-provisioning, same as wifi_ssid/wifi_pass. Unlike the fields above,
    // a missing value here doesn't fail config_store_load - see
    // config_store_ensure_live_cfg_auth.
    char live_cfg_username[33];
    char live_cfg_password[65];
} radar_config_t;

// Loads config from NVS namespace "radarcfg". Returns ESP_OK only if a
// complete, valid config was found; ESP_ERR_NVS_NOT_FOUND or
// ESP_ERR_INVALID_ARG (bad numeric field) otherwise - both mean "not
// configured yet", callers should fall back to provisioning mode.
esp_err_t config_store_load(radar_config_t *out);

esp_err_t config_store_save(const radar_config_t *cfg);

// Fills in LIVE_CFG_DEFAULT_USERNAME/PASSWORD for whichever of
// live_cfg_username/live_cfg_password is still empty - a config saved
// before these fields existed won't have them. No-op if both are already
// present. Call once at boot right after a successful config_store_load,
// so picking up live-config support (or upgrading from an older field set)
// never requires re-provisioning.
esp_err_t config_store_ensure_live_cfg_auth(radar_config_t *cfg);

// Shared field validation, used by config_store_load itself and by both
// provisioning_http_server.c and live_config_http_server.c so a submitted
// form and a value already sitting in NVS are held to exactly the same
// bounds. Each returns false (leaving *out unchanged) if `str` doesn't
// parse as a valid number in range.
bool config_store_parse_port(const char *str, uint16_t *out);
bool config_store_parse_lat(const char *str, double *out);
bool config_store_parse_lon(const char *str, double *out);
bool config_store_parse_range_nm(const char *str, double *out);

#endif
