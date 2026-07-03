#include "wifi_manager.h"

#include <stdbool.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *TAG = "wifi_manager";

#define STA_MAX_RETRY 5
#define CONNECTED_BIT BIT0
#define FAIL_BIT BIT1

static EventGroupHandle_t s_event_group;
static int s_retry_count;
// Once true, disconnects always retry unconditionally rather than giving up
// after STA_MAX_RETRY: the credentials are already known-good at that point
// (we've associated successfully at least once), so a later disconnect is a
// transient link hiccup (e.g. a mesh AP handoff, or a brief drop while WiFi
// power-save is active), not a bad-password situation that should fall back
// to provisioning mode.
static bool s_ever_connected;

static void sta_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        if (s_ever_connected) {
            ESP_LOGW(TAG, "STA disconnected (reason=%d), reconnecting...", disc->reason);
            esp_wifi_connect();
        } else if (s_retry_count < STA_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGW(TAG, "STA disconnected (reason=%d), retry %d/%d", disc->reason, s_retry_count, STA_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_event_group, FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_retry_count = 0;
        s_ever_connected = true;
        xEventGroupSetBits(s_event_group, CONNECTED_BIT);
    }
}

esp_err_t wifi_manager_start_ap(const char *ssid, const char *password)
{
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.ap.ssid, ssid, sizeof(wifi_cfg.ap.ssid) - 1);
    wifi_cfg.ap.ssid_len = strlen(ssid);
    wifi_cfg.ap.channel = 1;
    wifi_cfg.ap.max_connection = 4;

    if (password != NULL && strlen(password) > 0) {
        strncpy((char *)wifi_cfg.ap.password, password, sizeof(wifi_cfg.ap.password) - 1);
        wifi_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wifi_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP '%s' started, connect and browse to 192.168.4.1", ssid);
    return ESP_OK;
}

esp_err_t wifi_manager_start_sta_and_wait(const char *ssid, const char *password, uint32_t timeout_ms)
{
    s_event_group = xEventGroupCreate();
    s_retry_count = 0;

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    esp_event_handler_instance_t wifi_handler, ip_handler;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                          &sta_event_handler, NULL, &wifi_handler));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                          &sta_event_handler, NULL, &ip_handler));

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);
    // Many modern APs (mesh systems especially) run WPA2/WPA3-transition
    // mode with Protected Management Frames preferred or required; without
    // advertising PMF capability here, the 4-way handshake can fail right
    // after a successful association even with a correct password.
    wifi_cfg.sta.pmf_cfg.capable = true;
    wifi_cfg.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Modem power-save causes periodic brief disconnects/reassociations on
    // some APs (especially mesh systems) - not needed on a mains-powered
    // device, and reconnect handling above is a safety net, not a
    // substitute for just avoiding the churn.
    esp_wifi_set_ps(WIFI_PS_NONE);

    EventBits_t bits = xEventGroupWaitBits(s_event_group, CONNECTED_BIT | FAIL_BIT,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));

    if (bits & CONNECTED_BIT) {
        ESP_LOGI(TAG, "STA connected to '%s'", ssid);
        // Keep wifi_handler/ip_handler registered for the rest of the
        // process lifetime so later disconnects (see s_ever_connected above)
        // keep getting reconnected - this event group is no longer needed
        // once connected, but the handlers must stay live.
        vEventGroupDelete(s_event_group);
        s_event_group = NULL;
        return ESP_OK;
    }

    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_handler);
    vEventGroupDelete(s_event_group);
    s_event_group = NULL;

    ESP_LOGW(TAG, "STA connect to '%s' failed or timed out", ssid);
    return ESP_FAIL;
}
