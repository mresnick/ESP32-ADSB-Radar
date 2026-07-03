#include "http_post_utils.h"

#include <stdlib.h>
#include <string.h>

esp_err_t http_read_post_body(httpd_req_t *req, char *out, size_t out_cap, size_t *out_len)
{
    if (req->content_len == 0 || req->content_len >= out_cap) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t total = 0;
    while (total < req->content_len) {
        int received = httpd_req_recv(req, out + total, req->content_len - total);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return ESP_FAIL;
        }
        total += (size_t)received;
    }
    out[total] = '\0';
    *out_len = total;
    return ESP_OK;
}

static void url_decode(char *dst, size_t dst_size, const char *src, size_t src_len)
{
    size_t di = 0;
    for (size_t si = 0; si < src_len && di < dst_size - 1; si++) {
        char c = src[si];
        if (c == '+') {
            dst[di++] = ' ';
        } else if (c == '%' && si + 2 < src_len) {
            char hex[3] = {src[si + 1], src[si + 2], '\0'};
            dst[di++] = (char)strtol(hex, NULL, 16);
            si += 2;
        } else {
            dst[di++] = c;
        }
    }
    dst[di] = '\0';
}

bool form_urlencoded_get(const char *body, size_t body_len, const char *key, char *out, size_t out_size)
{
    size_t key_len = strlen(key);
    const char *p = body;
    const char *end = body + body_len;

    while (p < end) {
        const char *amp = memchr(p, '&', end - p);
        const char *pair_end = amp ? amp : end;
        const char *eq = memchr(p, '=', pair_end - p);

        if (eq && (size_t)(eq - p) == key_len && strncmp(p, key, key_len) == 0) {
            url_decode(out, out_size, eq + 1, pair_end - (eq + 1));
            return true;
        }

        p = amp ? amp + 1 : end;
    }
    return false;
}

esp_err_t http_send_error(httpd_req_t *req, const char *message)
{
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, message, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
