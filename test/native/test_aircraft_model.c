// Host-side ("native") tests for the incremental aircraft.json scanner in
// main/aircraft_model.c. Plain C, no ESP-IDF dependency beyond the fake
// esp_log.h stub in test/native/fakes/ - build with the host's own
// compiler, no hardware needed:
//
//   gcc -std=c11 -I ../../main -I fakes test_aircraft_model.c ../../main/aircraft_model.c -o test_aircraft_model
//   ./test_aircraft_model
//
// There is no existing test framework anywhere in this repo, so this is a
// small self-contained runner (plain asserts + a pass/fail tally) rather
// than pulling in a dependency for one test file.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aircraft_model.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) {                                                              \
            g_pass++;                                                            \
        } else {                                                                 \
            g_fail++;                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                        \
    } while (0)

#define MAX_CAPTURED 16

typedef struct {
    aircraft_t items[MAX_CAPTURED];
    int count;
} capture_t;

static void capture_cb(const aircraft_t *ac, void *user_ctx)
{
    capture_t *c = (capture_t *)user_ctx;
    if (c->count < MAX_CAPTURED) {
        c->items[c->count] = *ac;
    }
    c->count++;
}

static bool aircraft_eq(const aircraft_t *a, const aircraft_t *b)
{
    return strcmp(a->hex, b->hex) == 0 &&
           strcmp(a->flight, b->flight) == 0 &&
           a->lat == b->lat &&
           a->lon == b->lon &&
           a->track == b->track &&
           a->altitude_ft == b->altitude_ft &&
           a->has_position == b->has_position &&
           a->has_altitude == b->has_altitude &&
           a->on_ground == b->on_ground &&
           strcmp(a->category, b->category) == 0 &&
           strcmp(a->aircraft_type, b->aircraft_type) == 0 &&
           strcmp(a->tail_number, b->tail_number) == 0;
}

// Feeds `data[0..len)` to `sc` in chunks of `chunk_size` bytes (last chunk
// may be smaller). Stops early once the scanner reaches a terminal outcome.
static void feed_in_chunks(aircraft_model_scanner_t *sc, const char *data, size_t len, size_t chunk_size)
{
    size_t off = 0;
    while (off < len && aircraft_model_scanner_outcome(sc) == AIRCRAFT_SCAN_INCOMPLETE) {
        size_t n = len - off < chunk_size ? len - off : chunk_size;
        aircraft_model_scanner_feed(sc, data + off, n);
        off += n;
    }
}

// --- Synthetic document -----------------------------------------------------
//
// Exercises: escaped characters, a \uXXXX escape (in "flight"), negative
// numbers, decimals, exponents (both positive-exponent-with-explicit-'+'-
// omitted and negative-exponent forms), the alt_baro:"ground" string case,
// a missing-position aircraft (flight falls back to hex), an unknown nested
// object/array (skip_container recursion) and true/false/null literals
// inside a nested array (skip_value), a numeric top-level header key before
// the array, a trailing top-level key *after* the array closes (tests that
// SCAN_STATE_KEY_OR_END correctly continues past the array to the true
// top-level '}'), and an ADS-B "category" field present on one aircraft but
// absent on the other two (tests that a missing category is left blank
// rather than carrying over stale data from a previous object), and a "t"
// (ICAO aircraft type) plus "r" (registration/tail number) field on the
// same aircraft as "category", absent on the other two (tests both real
// parsing and the "not every aircraft has this" blank-fallback case).
static const char *kDoc =
    "{\"now\":1234567890.5, \"messages\":99999, \"aircraft\": [\n"
    "  {\"hex\":\"a4bcba\",\"flight\":\"UA\\u0041B  \",\"lat\":40.712,\"lon\":-74.006,"
    "\"track\":2.705e2,\"alt_baro\":35000,\"squawk\":\"1200\",\"extra_num\":-1.5e-3,"
    "\"category\":\"A7\",\"t\":\"B738\",\"r\":\"N12345\","
    "\"nested\":{\"a\":1,\"b\":[1,2,3]},\"arr\":[{\"x\":true},{\"y\":false},null]},\n"
    "  {\"hex\":\"b0a1c2\",\"alt_baro\":\"ground\",\"messages\":42,\"seen\":0.5},\n"
    "  {\"hex\":\"c3d4e5\",\"flight\":\"SWA202  \",\"lat\":40.70,\"lon\":-74.01,\"track\":0}\n"
    "] , \"extra_tail_key\":{\"nested\":true} }";

static void expected_aircraft(aircraft_t *out, int n)
{
    memset(out, 0, n * sizeof(*out));

    strcpy(out[0].hex, "a4bcba");
    strcpy(out[0].flight, "UA?B"); // A -> '?' placeholder, trailing spaces trimmed
    out[0].lat = 40.712;
    out[0].lon = -74.006;
    out[0].track = 270.5;
    out[0].altitude_ft = 35000;
    out[0].has_position = true;
    out[0].has_altitude = true;
    out[0].on_ground = false;
    strcpy(out[0].category, "A7");
    strcpy(out[0].aircraft_type, "B738");
    strcpy(out[0].tail_number, "N12345");

    strcpy(out[1].hex, "b0a1c2");
    strcpy(out[1].flight, "b0a1c2"); // no "flight" key - falls back to hex
    out[1].lat = 0;
    out[1].lon = 0;
    out[1].track = 0;
    out[1].altitude_ft = 0;
    out[1].has_position = false;
    out[1].has_altitude = false;
    out[1].on_ground = true;

    strcpy(out[2].hex, "c3d4e5");
    strcpy(out[2].flight, "SWA202");
    out[2].lat = 40.70;
    out[2].lon = -74.01;
    out[2].track = 0;
    out[2].altitude_ft = 0;
    out[2].has_position = true;
    out[2].has_altitude = false;
    out[2].on_ground = false;
}

static void check_capture_matches_expected(const capture_t *cap, int expected_count)
{
    aircraft_t expected[3];
    expected_aircraft(expected, 3);

    CHECK(cap->count == expected_count);
    for (int i = 0; i < expected_count && i < cap->count; i++) {
        CHECK(aircraft_eq(&cap->items[i], &expected[i]));
    }
}

// 1. Baseline smoke test: whole document in a single feed() call.
static void test_baseline(void)
{
    aircraft_model_scanner_t sc;
    capture_t cap = {0};
    aircraft_model_scanner_init(&sc, capture_cb, &cap);

    size_t len = strlen(kDoc);
    aircraft_model_scanner_feed(&sc, kDoc, len);

    CHECK(aircraft_model_scanner_outcome(&sc) == AIRCRAFT_SCAN_COMPLETE);
    CHECK(aircraft_model_scanner_total_emitted(&sc) == 3);
    check_capture_matches_expected(&cap, 3);
}

// 2. Every possible two-way chunk-boundary split - the highest-value test:
// a chunk boundary can legally fall anywhere (mid-escape, mid-number,
// mid-whitespace, exactly on a delimiter), and the result must be identical
// regardless of where it falls.
static void test_every_split_point(void)
{
    size_t len = strlen(kDoc);
    for (size_t split = 0; split <= len; split++) {
        aircraft_model_scanner_t sc;
        capture_t cap = {0};
        aircraft_model_scanner_init(&sc, capture_cb, &cap);

        aircraft_model_scanner_feed(&sc, kDoc, split);
        aircraft_model_scanner_feed(&sc, kDoc + split, len - split);

        if (aircraft_model_scanner_outcome(&sc) != AIRCRAFT_SCAN_COMPLETE || cap.count != 3) {
            fprintf(stderr, "split-point failure at offset %zu/%zu\n", split, len);
        }
        CHECK(aircraft_model_scanner_outcome(&sc) == AIRCRAFT_SCAN_COMPLETE);
        check_capture_matches_expected(&cap, 3);
    }
}

// 3. Byte-at-a-time feed - forces resumption at literally every position in
// the document, a stricter superset of the two-way split test above.
static void test_byte_at_a_time(void)
{
    aircraft_model_scanner_t sc;
    capture_t cap = {0};
    aircraft_model_scanner_init(&sc, capture_cb, &cap);

    size_t len = strlen(kDoc);
    feed_in_chunks(&sc, kDoc, len, 1);

    CHECK(aircraft_model_scanner_outcome(&sc) == AIRCRAFT_SCAN_COMPLETE);
    check_capture_matches_expected(&cap, 3);
}

// 4. Multiple complete aircraft objects delivered within one chunk: feeding
// the whole array section (well past all three objects) as one chunk must
// drain all three in a single feed() call, not just the first.
static void test_multiple_objects_one_chunk(void)
{
    aircraft_model_scanner_t sc;
    capture_t cap = {0};
    aircraft_model_scanner_init(&sc, capture_cb, &cap);

    const char *array_start = strstr(kDoc, "[");
    size_t prefix_len = (size_t)(array_start - kDoc) + 1;
    aircraft_model_scanner_feed(&sc, kDoc, prefix_len);
    CHECK(cap.count == 0); // nothing parsed yet - only the header + '[' fed so far

    aircraft_model_scanner_feed(&sc, kDoc + prefix_len, strlen(kDoc) - prefix_len);
    CHECK(aircraft_model_scanner_outcome(&sc) == AIRCRAFT_SCAN_COMPLETE);
    check_capture_matches_expected(&cap, 3);
}

// 5. Genuinely truncated feed: stop partway through the second aircraft
// object and never supply another byte. Must report INCOMPLETE (not
// COMPLETE, not stuck retrying forever), while still having emitted the one
// aircraft object that DID complete before the cutoff - the concrete proof
// that a truncated transfer no longer discards everything.
static void test_truncated(void)
{
    aircraft_model_scanner_t sc;
    capture_t cap = {0};
    aircraft_model_scanner_init(&sc, capture_cb, &cap);

    const char *marker = strstr(kDoc, "\"b0a1c2\"");
    CHECK(marker != NULL);
    size_t cut = (size_t)(marker - kDoc) + strlen("\"b0a1c2\",");

    aircraft_model_scanner_feed(&sc, kDoc, cut);

    CHECK(aircraft_model_scanner_outcome(&sc) == AIRCRAFT_SCAN_INCOMPLETE);
    CHECK(cap.count == 1); // only the first aircraft completed before the cut
    aircraft_t expected[3];
    expected_aircraft(expected, 3);
    CHECK(cap.count >= 1 && aircraft_eq(&cap.items[0], &expected[0]));
}

// 6. Genuinely malformed feed (missing ':' after a key) - must reach
// MALFORMED, not sit waiting for more data forever.
static void test_malformed(void)
{
    static const char *kBadDoc = "{\"aircraft\":[{\"hex\"\"a4bcba\"}]}";

    aircraft_model_scanner_t sc;
    capture_t cap = {0};
    aircraft_model_scanner_init(&sc, capture_cb, &cap);

    aircraft_model_scanner_feed(&sc, kBadDoc, strlen(kBadDoc));

    CHECK(aircraft_model_scanner_outcome(&sc) == AIRCRAFT_SCAN_MALFORMED);
}

// 7. Empty aircraft array - must be COMPLETE with zero callback invocations
// (distinct from "malformed"/"incomplete" - this is a legitimately empty,
// well-formed feed).
static void test_empty_array(void)
{
    static const char *kEmptyDoc = "{\"now\":1,\"aircraft\":[]}";

    aircraft_model_scanner_t sc;
    capture_t cap = {0};
    aircraft_model_scanner_init(&sc, capture_cb, &cap);

    aircraft_model_scanner_feed(&sc, kEmptyDoc, strlen(kEmptyDoc));

    CHECK(aircraft_model_scanner_outcome(&sc) == AIRCRAFT_SCAN_COMPLETE);
    CHECK(cap.count == 0);
    CHECK(aircraft_model_scanner_total_emitted(&sc) == 0);
}

// 8. Buffer-overflow edge case: a single string value that never terminates
// within AIRCRAFT_SCAN_BUF_SIZE bytes. Must fail cleanly (MALFORMED) rather
// than overflow or hang - fed in realistic small chunks (not one giant
// chunk), so this exercises the "value keeps growing across many feed()
// calls without ever completing" path, not just an oversized single call.
static void test_buffer_overflow(void)
{
    size_t huge_len = AIRCRAFT_SCAN_BUF_SIZE + 1000;
    char *huge = malloc(huge_len + 64);
    size_t pos = 0;
    memcpy(huge + pos, "{\"aircraft\":[{\"hex\":\"aa\",\"flight\":\"", 36);
    pos += 36;
    memset(huge + pos, 'A', huge_len);
    pos += huge_len;
    memcpy(huge + pos, "\"}]}", 4);
    pos += 4;

    aircraft_model_scanner_t sc;
    capture_t cap = {0};
    aircraft_model_scanner_init(&sc, capture_cb, &cap);

    feed_in_chunks(&sc, huge, pos, 64);

    CHECK(aircraft_model_scanner_outcome(&sc) == AIRCRAFT_SCAN_MALFORMED);
    free(huge);
}

// 9. A number immediately followed by a bare 'e'/'E' with no exponent
// digits (e.g. "5e}") is not a valid JSON number - extract_number must
// rewind to just before the 'e' rather than consuming it, leaving the
// stray 'e' to be rejected as an unexpected byte where a delimiter was
// expected. Must reach MALFORMED, not silently swallow the 'e' or hang
// waiting for more input.
static void test_bare_exponent_rewind(void)
{
    static const char *kBadExpDoc = "{\"aircraft\":[{\"hex\":\"aaaaaa\",\"track\":5e}]}";

    aircraft_model_scanner_t sc;
    capture_t cap = {0};
    aircraft_model_scanner_init(&sc, capture_cb, &cap);

    aircraft_model_scanner_feed(&sc, kBadExpDoc, strlen(kBadExpDoc));

    CHECK(aircraft_model_scanner_outcome(&sc) == AIRCRAFT_SCAN_MALFORMED);
}

int main(void)
{
    test_baseline();
    test_every_split_point();
    test_byte_at_a_time();
    test_multiple_objects_one_chunk();
    test_truncated();
    test_malformed();
    test_empty_array();
    test_buffer_overflow();
    test_bare_exponent_rewind();

    fprintf(stderr, "\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
