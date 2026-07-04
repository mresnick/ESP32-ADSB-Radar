#include "aircraft_model.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "aircraft_model";

// Hand-rolled incremental streaming scanner instead of a DOM parser: a DOM
// parser (e.g. cJSON) allocates a tree node per key/value plus a duplicated
// heap copy of every string, which for a ~150-aircraft/~50KB SkyAware
// response is 300KB+ of allocations - well past the ~30-40KB free heap left
// on a PSRAM-less ESP32. See TROUBLESHOOTING.md for the fuller history of
// designs this replaced.
//
// This scanner parses directly out of HTTP chunks as they arrive
// (aircraft_model_scanner_feed is called once per chunk) and emits one
// aircraft_t at a time via callback, so callers can rank by relevance
// (distance) themselves instead of relying on the server's JSON ordering,
// and there is no whole-document size limit - only a small fixed carry
// buffer (AIRCRAFT_SCAN_BUF_SIZE) sized to comfortably hold one in-progress
// JSON record.

typedef struct {
    const char *p;
    const char *end;
} scanner_t;

// A scanner primitive can be cut off in one of three ways: it completed
// (STEP_OK), it ran off the end of the currently-buffered bytes before it
// could tell whether the value was complete (STEP_NEED_MORE - ambiguous,
// more bytes may still be coming), or it saw a definitely-wrong byte with
// bytes still available (STEP_ERROR - a real syntax error, not just
// truncation). Distinguishing NEED_MORE from ERROR is what makes streaming
// possible: NEED_MORE means "try this same step again once more data
// arrives," ERROR means "this document is genuinely malformed."
typedef enum {
    STEP_OK = 0,
    STEP_NEED_MORE,
    STEP_ERROR,
} step_result_t;

static void skip_ws(scanner_t *s)
{
    while (s->p < s->end && (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r')) {
        s->p++;
    }
}

static step_result_t skip_value(scanner_t *s);

// Parses a JSON string starting at the opening quote. If dst is non-NULL,
// writes the (minimally unescaped) contents there, truncated to dst_size.
// Pass dst=NULL to just skip over a string.
static step_result_t extract_string(scanner_t *s, char *dst, size_t dst_size)
{
    if (s->p >= s->end) {
        return STEP_NEED_MORE;
    }
    if (*s->p != '"') {
        return STEP_ERROR;
    }
    s->p++;
    size_t n = 0;
    while (s->p < s->end && *s->p != '"') {
        char c = *s->p;
        if (c == '\\') {
            s->p++;
            if (s->p >= s->end) {
                return STEP_NEED_MORE;
            }
            switch (*s->p) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u':
                    // \uXXXX - the fields this app reads (hex, flight) are
                    // always plain ASCII in dump1090's output, so a
                    // placeholder is fine rather than decoding UTF-16. Must
                    // wait for all 4 hex digits to be buffered before
                    // committing to that, though - a chunk boundary landing
                    // inside the escape is a realistic event against a 2KB
                    // carry buffer (unlike the old 64KB whole-response one).
                    if (s->end - s->p < 5) {
                        return STEP_NEED_MORE;
                    }
                    s->p += 4;
                    c = '?';
                    break;
                default: c = *s->p; break;
            }
        }
        if (dst != NULL && n + 1 < dst_size) {
            dst[n++] = c;
        }
        s->p++;
    }
    if (s->p >= s->end) {
        return STEP_NEED_MORE; // never saw the closing quote in buffered bytes
    }
    s->p++; // closing quote
    if (dst != NULL) {
        dst[n] = '\0';
    }
    return STEP_OK;
}

// Parses a JSON number into *out. A scan stage that stops merely because it
// ran off the end of the buffer is NOT the same as the number being
// complete - more digits/exponent could still be in the next chunk - so
// each stage only confirms completion when it stops on a definite
// non-continuation byte with bytes still available.
static step_result_t extract_number(scanner_t *s, double *out)
{
    skip_ws(s);
    if (s->p >= s->end) {
        return STEP_NEED_MORE;
    }
    const char *start = s->p;
    const char *p = s->p;

    if (*p == '-') {
        p++;
        if (p >= s->end) {
            return STEP_NEED_MORE;
        }
    }
    bool any_digit = false;
    while (p < s->end && isdigit((unsigned char)*p)) {
        p++;
        any_digit = true;
    }
    if (p >= s->end) {
        return STEP_NEED_MORE;
    }
    if (!any_digit) {
        return STEP_ERROR; // definite non-digit right where a number was expected
    }
    if (*p == '.') {
        p++;
        if (p >= s->end) {
            return STEP_NEED_MORE;
        }
        while (p < s->end && isdigit((unsigned char)*p)) {
            p++;
        }
        if (p >= s->end) {
            return STEP_NEED_MORE;
        }
    }
    if (*p == 'e' || *p == 'E') {
        const char *exp_start = p;
        p++;
        if (p >= s->end) {
            return STEP_NEED_MORE;
        }
        if (*p == '+' || *p == '-') {
            p++;
            if (p >= s->end) {
                return STEP_NEED_MORE;
            }
        }
        const char *digits_start = p;
        while (p < s->end && isdigit((unsigned char)*p)) {
            p++;
        }
        if (p == digits_start) {
            p = exp_start; // no exponent digits after e/E - not part of the number
        } else if (p >= s->end) {
            return STEP_NEED_MORE;
        }
    }

    if (out != NULL) {
        *out = strtod(start, NULL);
    }
    s->p = p;
    return STEP_OK;
}

static step_result_t skip_container(scanner_t *s, char open, char close)
{
    if (s->p >= s->end) {
        return STEP_NEED_MORE;
    }
    if (*s->p != open) {
        return STEP_ERROR;
    }
    s->p++;
    skip_ws(s);
    if (s->p >= s->end) {
        return STEP_NEED_MORE;
    }
    if (*s->p == close) {
        s->p++;
        return STEP_OK;
    }
    while (1) {
        skip_ws(s);
        if (open == '{') {
            step_result_t r = extract_string(s, NULL, 0); // key
            if (r != STEP_OK) {
                return r;
            }
            skip_ws(s);
            if (s->p >= s->end) {
                return STEP_NEED_MORE;
            }
            if (*s->p != ':') {
                return STEP_ERROR;
            }
            s->p++;
        }
        step_result_t r = skip_value(s);
        if (r != STEP_OK) {
            return r;
        }
        skip_ws(s);
        if (s->p >= s->end) {
            return STEP_NEED_MORE;
        }
        if (*s->p == ',') {
            s->p++;
            continue;
        }
        if (*s->p == close) {
            s->p++;
            return STEP_OK;
        }
        return STEP_ERROR;
    }
}

static step_result_t skip_value(scanner_t *s)
{
    skip_ws(s);
    if (s->p >= s->end) {
        return STEP_NEED_MORE;
    }
    char c = *s->p;
    if (c == '"') {
        return extract_string(s, NULL, 0);
    }
    if (c == '{') {
        return skip_container(s, '{', '}');
    }
    if (c == '[') {
        return skip_container(s, '[', ']');
    }
    if (c == 't') {
        if (s->end - s->p < 4) {
            return STEP_NEED_MORE;
        }
        if (memcmp(s->p, "true", 4) == 0) {
            s->p += 4;
            return STEP_OK;
        }
        return STEP_ERROR;
    }
    if (c == 'f') {
        if (s->end - s->p < 5) {
            return STEP_NEED_MORE;
        }
        if (memcmp(s->p, "false", 5) == 0) {
            s->p += 5;
            return STEP_OK;
        }
        return STEP_ERROR;
    }
    if (c == 'n') {
        if (s->end - s->p < 4) {
            return STEP_NEED_MORE;
        }
        if (memcmp(s->p, "null", 4) == 0) {
            s->p += 4;
            return STEP_OK;
        }
        return STEP_ERROR;
    }
    return extract_number(s, NULL);
}

static void copy_trimmed(char *dst, size_t dst_size, const char *src)
{
    size_t n = strlen(src);
    while (n > 0 && src[n - 1] == ' ') {
        n--;
    }
    if (n > dst_size - 1) {
        n = dst_size - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static step_result_t parse_aircraft_object(scanner_t *s, aircraft_t *a)
{
    memset(a, 0, sizeof(*a));
    bool have_lat = false, have_lon = false, have_flight = false;

    if (s->p >= s->end) {
        return STEP_NEED_MORE;
    }
    s->p++; // consume '{'
    skip_ws(s);
    if (s->p >= s->end) {
        return STEP_NEED_MORE;
    }
    if (*s->p == '}') {
        s->p++;
    } else {
        while (1) {
            skip_ws(s);
            char key[24];
            step_result_t r = extract_string(s, key, sizeof(key));
            if (r != STEP_OK) {
                return r;
            }
            skip_ws(s);
            if (s->p >= s->end) {
                return STEP_NEED_MORE;
            }
            if (*s->p != ':') {
                return STEP_ERROR;
            }
            s->p++;

            if (strcmp(key, "hex") == 0) {
                r = extract_string(s, a->hex, sizeof(a->hex));
                if (r != STEP_OK) {
                    return r;
                }
            } else if (strcmp(key, "category") == 0) {
                r = extract_string(s, a->category, sizeof(a->category));
                if (r != STEP_OK) {
                    return r;
                }
            } else if (strcmp(key, "t") == 0) {
                r = extract_string(s, a->aircraft_type, sizeof(a->aircraft_type));
                if (r != STEP_OK) {
                    return r;
                }
            } else if (strcmp(key, "r") == 0) {
                r = extract_string(s, a->tail_number, sizeof(a->tail_number));
                if (r != STEP_OK) {
                    return r;
                }
            } else if (strcmp(key, "flight") == 0) {
                char tmp[16];
                r = extract_string(s, tmp, sizeof(tmp));
                if (r != STEP_OK) {
                    return r;
                }
                copy_trimmed(a->flight, sizeof(a->flight), tmp);
                have_flight = true;
            } else if (strcmp(key, "lat") == 0) {
                double v;
                r = extract_number(s, &v);
                if (r == STEP_OK) {
                    a->lat = v;
                    have_lat = true;
                } else if (r == STEP_ERROR) {
                    r = skip_value(s);
                    if (r != STEP_OK) {
                        return r;
                    }
                } else {
                    return r; // STEP_NEED_MORE
                }
            } else if (strcmp(key, "lon") == 0) {
                double v;
                r = extract_number(s, &v);
                if (r == STEP_OK) {
                    a->lon = v;
                    have_lon = true;
                } else if (r == STEP_ERROR) {
                    r = skip_value(s);
                    if (r != STEP_OK) {
                        return r;
                    }
                } else {
                    return r;
                }
            } else if (strcmp(key, "track") == 0) {
                double v;
                r = extract_number(s, &v);
                if (r == STEP_OK) {
                    a->track = v;
                } else if (r == STEP_ERROR) {
                    r = skip_value(s);
                    if (r != STEP_OK) {
                        return r;
                    }
                } else {
                    return r;
                }
            } else if (strcmp(key, "alt_baro") == 0) {
                // Usually a number (feet), but can be the string "ground".
                double v;
                r = extract_number(s, &v);
                if (r == STEP_OK) {
                    a->altitude_ft = (int)v;
                    a->has_altitude = true;
                } else if (r == STEP_NEED_MORE) {
                    return r;
                } else if (s->p < s->end && *s->p == '"') {
                    char tmp[8];
                    r = extract_string(s, tmp, sizeof(tmp));
                    if (r != STEP_OK) {
                        return r;
                    }
                    a->on_ground = (strcmp(tmp, "ground") == 0);
                } else if (s->p >= s->end) {
                    return STEP_NEED_MORE;
                } else {
                    r = skip_value(s);
                    if (r != STEP_OK) {
                        return r;
                    }
                }
            } else {
                r = skip_value(s);
                if (r != STEP_OK) {
                    return r;
                }
            }

            skip_ws(s);
            if (s->p >= s->end) {
                return STEP_NEED_MORE;
            }
            if (*s->p == ',') {
                s->p++;
                continue;
            }
            if (*s->p == '}') {
                s->p++;
                break;
            }
            return STEP_ERROR;
        }
    }

    if (have_lat && have_lon) {
        a->has_position = true;
    }
    if (!have_flight) {
        // No callsign broadcast yet - fall back to the Mode S hex so the
        // dot on screen still has a label.
        strncpy(a->flight, a->hex, sizeof(a->flight) - 1);
    }
    return STEP_OK;
}

// --- Incremental top-level driver -----------------------------------------
//
// The document is walked as a small state machine, one structural "step" at
// a time (consume the top-level '{', consume one key/value pair, parse one
// array element, consume the ',' or ']' after it). Every step is attempted
// against a scanner_t starting at buffer offset 0; on success the consumed
// prefix is compacted out of the buffer and the state advances. On
// STEP_NEED_MORE the buffer is left untouched (it's already at offset 0)
// and aircraft_model_scanner_feed simply returns, waiting for more bytes -
// the *same* step gets retried from scratch next time.
//
// This deliberately avoids threading incremental/checkpointed state through
// the primitives above: parse_aircraft_object already memsets its output on
// entry, so restarting it from the beginning on every retry is inherently
// safe. An object split across three chunks gets rescanned from its own
// start up to twice more, each restart bounded by AIRCRAFT_SCAN_BUF_SIZE -
// negligible CPU cost at this data scale, in exchange for a much simpler
// and harder-to-get-wrong resume model.

typedef enum {
    SCAN_STATE_TOP_OPEN = 0,  // expecting the top-level '{'
    SCAN_STATE_KEY_OR_END,    // expecting a key string, or '}' to end the top-level object
    SCAN_STATE_ARRAY_ELEMENT, // expecting '{' (one aircraft object) or ']' to end the array
    SCAN_STATE_ARRAY_SEP,     // expecting ',' or ']' after one array element
    SCAN_STATE_AFTER_ARRAY,   // expecting ',' (more top-level keys follow) or '}' after the array closes
} scan_state_t;

static step_result_t step_top_open(scanner_t *s)
{
    skip_ws(s);
    if (s->p >= s->end) {
        return STEP_NEED_MORE;
    }
    if (*s->p != '{') {
        return STEP_ERROR;
    }
    s->p++;
    return STEP_OK;
}

// Consumes one top-level key. If it's "aircraft" (and we haven't already
// seen it), consumes through the array's opening '[' and reports
// *out_entering_array. Any other key has its value fully skipped and
// consumed as an ordinary header/tail field. Reports *out_hit_end if the
// top-level object's closing '}' is reached instead of a key.
static step_result_t step_key_or_end(scanner_t *s, bool seen_aircraft, bool *out_hit_end, bool *out_entering_array)
{
    *out_hit_end = false;
    *out_entering_array = false;

    skip_ws(s);
    if (s->p >= s->end) {
        return STEP_NEED_MORE;
    }
    if (*s->p == '}') {
        s->p++;
        *out_hit_end = true;
        return STEP_OK;
    }

    char key[24];
    step_result_t r = extract_string(s, key, sizeof(key));
    if (r != STEP_OK) {
        return r;
    }
    skip_ws(s);
    if (s->p >= s->end) {
        return STEP_NEED_MORE;
    }
    if (*s->p != ':') {
        return STEP_ERROR;
    }
    s->p++;
    skip_ws(s);

    if (!seen_aircraft && strcmp(key, "aircraft") == 0) {
        if (s->p >= s->end) {
            return STEP_NEED_MORE;
        }
        if (*s->p != '[') {
            return STEP_ERROR;
        }
        s->p++;
        *out_entering_array = true;
        return STEP_OK;
    }

    r = skip_value(s);
    if (r != STEP_OK) {
        return r;
    }

    skip_ws(s);
    if (s->p >= s->end) {
        return STEP_NEED_MORE;
    }
    if (*s->p == ',') {
        s->p++;
        return STEP_OK;
    }
    if (*s->p == '}') {
        s->p++;
        *out_hit_end = true;
        return STEP_OK;
    }
    return STEP_ERROR;
}

static step_result_t step_array_element(scanner_t *s, aircraft_t *out_ac, bool *out_hit_array_end)
{
    *out_hit_array_end = false;
    skip_ws(s);
    if (s->p >= s->end) {
        return STEP_NEED_MORE;
    }
    if (*s->p == ']') {
        s->p++;
        *out_hit_array_end = true;
        return STEP_OK;
    }
    if (*s->p != '{') {
        return STEP_ERROR;
    }
    return parse_aircraft_object(s, out_ac);
}

static step_result_t step_array_sep(scanner_t *s, bool *out_hit_array_end)
{
    *out_hit_array_end = false;
    skip_ws(s);
    if (s->p >= s->end) {
        return STEP_NEED_MORE;
    }
    if (*s->p == ',') {
        s->p++;
        return STEP_OK;
    }
    if (*s->p == ']') {
        s->p++;
        *out_hit_array_end = true;
        return STEP_OK;
    }
    return STEP_ERROR;
}

// After the "aircraft" array's closing ']', the position is exactly like
// the position after any other top-level value: the next byte must be ','
// (more top-level keys follow) or '}' (this was the last key). This step
// exists as its own state because entering the array (via step_key_or_end)
// consumed only up through '[' - unlike an ordinary key/value pair, it
// never got to consume its own trailing ','/'}' as part of that same step.
static step_result_t step_after_array(scanner_t *s, bool *out_hit_end)
{
    *out_hit_end = false;
    skip_ws(s);
    if (s->p >= s->end) {
        return STEP_NEED_MORE;
    }
    if (*s->p == ',') {
        s->p++;
        return STEP_OK;
    }
    if (*s->p == '}') {
        s->p++;
        *out_hit_end = true;
        return STEP_OK;
    }
    return STEP_ERROR;
}

void aircraft_model_scanner_init(aircraft_model_scanner_t *sc, aircraft_model_cb_t cb, void *user_ctx)
{
    memset(sc, 0, sizeof(*sc));
    sc->state = SCAN_STATE_TOP_OPEN;
    sc->outcome = AIRCRAFT_SCAN_INCOMPLETE;
    sc->cb = cb;
    sc->user_ctx = user_ctx;
}

void aircraft_model_scanner_feed(aircraft_model_scanner_t *sc, const char *data, size_t len)
{
    if (sc->outcome != AIRCRAFT_SCAN_INCOMPLETE) {
        return; // already terminal (COMPLETE or MALFORMED) - ignore further data
    }
    if (len > sizeof(sc->buf) - sc->buf_len) {
        // A single JSON record has exceeded AIRCRAFT_SCAN_BUF_SIZE - either
        // a pathological/corrupt feed, or dump1090's per-aircraft schema has
        // grown enough to need a bigger buffer. Fail cleanly rather than
        // overflow.
        ESP_LOGE(TAG, "aircraft scan buffer overflow: state=%d buf_len=%u incoming=%u cap=%u",
                 sc->state, (unsigned)sc->buf_len, (unsigned)len, (unsigned)sizeof(sc->buf));
        sc->outcome = AIRCRAFT_SCAN_MALFORMED;
        return;
    }
    memcpy(sc->buf + sc->buf_len, data, len);
    sc->buf_len += len;
    sc->total_bytes_fed += len;

    while (sc->outcome == AIRCRAFT_SCAN_INCOMPLETE) {
        scanner_t s = { .p = sc->buf, .end = sc->buf + sc->buf_len };
        bool hit_end = false;
        bool entering_array = false;
        aircraft_t tmp;
        step_result_t r;

        switch ((scan_state_t)sc->state) {
            case SCAN_STATE_TOP_OPEN:
                r = step_top_open(&s);
                break;
            case SCAN_STATE_KEY_OR_END:
                r = step_key_or_end(&s, sc->seen_aircraft, &hit_end, &entering_array);
                break;
            case SCAN_STATE_ARRAY_ELEMENT:
                r = step_array_element(&s, &tmp, &hit_end);
                break;
            case SCAN_STATE_ARRAY_SEP:
                r = step_array_sep(&s, &hit_end);
                break;
            case SCAN_STATE_AFTER_ARRAY:
                r = step_after_array(&s, &hit_end);
                break;
            default:
                r = STEP_ERROR;
                break;
        }

        if (r == STEP_NEED_MORE) {
            return; // wait for the next feed() call; buffer already at offset 0
        }
        if (r == STEP_ERROR) {
            ESP_LOGW(TAG, "aircraft scan malformed: state=%d total_bytes_fed=%lu context=[%.*s]",
                     sc->state, sc->total_bytes_fed, (int)sc->buf_len, sc->buf);
            sc->outcome = AIRCRAFT_SCAN_MALFORMED;
            return;
        }

        // STEP_OK - compact out the consumed prefix.
        size_t consumed = (size_t)(s.p - sc->buf);
        memmove(sc->buf, sc->buf + consumed, sc->buf_len - consumed);
        sc->buf_len -= consumed;

        switch ((scan_state_t)sc->state) {
            case SCAN_STATE_TOP_OPEN:
                sc->state = SCAN_STATE_KEY_OR_END;
                break;
            case SCAN_STATE_KEY_OR_END:
                if (hit_end) {
                    if (sc->seen_aircraft) {
                        sc->outcome = AIRCRAFT_SCAN_COMPLETE;
                    } else {
                        // dump1090 always includes an "aircraft" key, even
                        // when the array is empty - reaching the top-level
                        // end without ever seeing it means something's
                        // wrong with this response.
                        ESP_LOGW(TAG, "aircraft scan malformed: top-level object closed without an \"aircraft\" key");
                        sc->outcome = AIRCRAFT_SCAN_MALFORMED;
                    }
                } else if (entering_array) {
                    sc->seen_aircraft = true;
                    sc->state = SCAN_STATE_ARRAY_ELEMENT;
                }
                // else: consumed one ordinary key/value pair, stay put
                break;
            case SCAN_STATE_ARRAY_ELEMENT:
                if (hit_end) {
                    sc->state = SCAN_STATE_AFTER_ARRAY; // array was empty - still need ','/'}' after ']'
                } else {
                    sc->total_emitted++;
                    if (sc->cb != NULL) {
                        sc->cb(&tmp, sc->user_ctx);
                    }
                    sc->state = SCAN_STATE_ARRAY_SEP;
                }
                break;
            case SCAN_STATE_ARRAY_SEP:
                sc->state = hit_end ? SCAN_STATE_AFTER_ARRAY : SCAN_STATE_ARRAY_ELEMENT;
                break;
            case SCAN_STATE_AFTER_ARRAY:
                if (hit_end) {
                    sc->outcome = AIRCRAFT_SCAN_COMPLETE;
                } else {
                    sc->state = SCAN_STATE_KEY_OR_END; // ',' consumed - more top-level keys follow
                }
                break;
            default:
                break;
        }
    }
}

aircraft_scan_outcome_t aircraft_model_scanner_outcome(const aircraft_model_scanner_t *sc)
{
    return sc->outcome;
}

unsigned long aircraft_model_scanner_total_emitted(const aircraft_model_scanner_t *sc)
{
    return sc->total_emitted;
}
