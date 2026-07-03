#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdint.h>

#include "esp_err.h"

// Starts an open (password == NULL) or WPA2 SoftAP with the given SSID.
// The AP netif is always 192.168.4.1/24 (ESP-IDF's default AP config).
esp_err_t wifi_manager_start_ap(const char *ssid, const char *password);

// Connects as a station and blocks until either an IP is obtained or
// timeout_ms elapses. Returns ESP_OK on success, ESP_FAIL on timeout/failure.
esp_err_t wifi_manager_start_sta_and_wait(const char *ssid, const char *password, uint32_t timeout_ms);

#endif
