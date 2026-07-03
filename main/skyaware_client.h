#ifndef SKYAWARE_CLIENT_H
#define SKYAWARE_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Invoked once per HTTP receive event with a raw (not necessarily
// JSON-aligned) slice of the response body. `data` is only valid for the
// duration of the call - copy/consume it before returning.
typedef void (*skyaware_chunk_cb_t)(const char *data, size_t len, void *user_ctx);

// Fetches http://<host>:<port>/data/aircraft.json, forwarding each received
// chunk to `on_chunk` as it arrives - the body is never buffered in full, so
// there is no response-size cap. The returned esp_err_t reflects HTTP
// transport/status only (ESP_OK + status 200 means the transfer completed
// normally) - it says nothing about whether the body chunks handed to
// `on_chunk` formed complete, well-formed JSON; that's for the
// aircraft_model scanner (fed via on_chunk) to determine.
esp_err_t skyaware_client_fetch(const char *host, uint16_t port,
                                 skyaware_chunk_cb_t on_chunk, void *user_ctx);

#endif
