#include "skyaware_client.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "skyaware_client";

typedef struct {
    skyaware_chunk_cb_t on_chunk;
    void *user_ctx;
    size_t total_bytes;  // diagnostics only (logging)
} fetch_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }

    fetch_ctx_t *ctx = (fetch_ctx_t *)evt->user_data;
    ctx->total_bytes += evt->data_len;
    ctx->on_chunk(evt->data, evt->data_len, ctx->user_ctx);
    return ESP_OK;
}

esp_err_t skyaware_client_fetch(const char *host, uint16_t port,
                                 skyaware_chunk_cb_t on_chunk, void *user_ctx)
{
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%u/data/aircraft.json", host, port);

    fetch_ctx_t ctx = { .on_chunk = on_chunk, .user_ctx = user_ctx, .total_bytes = 0 };

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &ctx,
        .timeout_ms = 5000,
        // buffer_size intentionally left at the esp_http_client default
        // (DEFAULT_HTTP_BUF_SIZE, 512): aircraft_model.c's scanner
        // re-attempts each structural step from scratch on whatever's in
        // its own small carry buffer, so it doesn't care how
        // esp_http_client happens to chunk the stream - see
        // TROUBLESHOOTING.md for more.
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    // Force the server to close the connection after this response, ruling
    // out any keep-alive-related state confusion between requests (each
    // fetch creates a brand new client/connection anyway).
    esp_http_client_set_header(client, "Connection", "close");

    int64_t start_us = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(client);
    int64_t elapsed_ms = (esp_timer_get_time() - start_us) / 1000;
    int status = esp_http_client_get_status_code(client);
    int64_t content_length = esp_http_client_get_content_length(client);
    bool is_chunked = esp_http_client_is_chunked_response(client);
    ESP_LOGD(TAG, "perform=%s status=%d content_length=%lld received=%u chunked=%d elapsed_ms=%lld",
             esp_err_to_name(err), status, content_length, (unsigned)ctx.total_bytes, (int)is_chunked, elapsed_ms);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP GET %s failed: %s", url, esp_err_to_name(err));
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP GET %s returned status %d", url, status);
        return ESP_FAIL;
    }

    return ESP_OK;
}
