#ifndef HTTP_POST_UTILS_H
#define HTTP_POST_UTILS_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"

// Reads the full POST body into `out`, always NUL-terminating it and
// setting *out_len to the byte count (excluding the NUL). httpd_req_recv
// can return fewer bytes than content_len when the body spans multiple TCP
// segments, so this loops until it's all in (or a real error/EOF happens).
// Returns ESP_ERR_INVALID_SIZE if the request has no body or one that
// wouldn't fit in out_cap (including the trailing NUL), or ESP_FAIL if a
// socket read failed.
esp_err_t http_read_post_body(httpd_req_t *req, char *out, size_t out_cap, size_t *out_len);

// Finds `key=value` in an application/x-www-form-urlencoded body and
// URL-decodes the value into `out` (percent-escapes and '+' as space).
// Returns false if the key isn't present in `body`.
bool form_urlencoded_get(const char *body, size_t body_len, const char *key, char *out, size_t out_size);

// Sends a 400 Bad Request with `message` as a plain-text body. Always
// returns ESP_OK - matches esp_http_server's convention that a handler's
// return value reports whether the *handler* failed, not whether the
// request itself was valid.
esp_err_t http_send_error(httpd_req_t *req, const char *message);

#endif
