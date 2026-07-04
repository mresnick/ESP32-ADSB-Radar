#ifndef AIRCRAFT_MODEL_H
#define AIRCRAFT_MODEL_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char hex[8];      // Mode S hex address, e.g. "a4bcba" - used as the "flight number" label
    char flight[10];  // callsign, trimmed of dump1090's trailing space-padding
    double lat;
    double lon;
    double track;       // heading in degrees, 0 if unknown
    int altitude_ft;
    bool has_position;  // true only if both lat/lon were present as numbers
    bool has_altitude;  // false if alt_baro was missing or "ground"
    bool on_ground;      // true if alt_baro was the string "ground"
    // Raw ADS-B emitter category (e.g. "A7" = rotorcraft, "A3"-"A5" =
    // large/heavy) - empty if not broadcast (older/lower ADS-B versions may
    // omit it). This module stays ignorant of what the codes mean; see
    // classify_shape() in app_main.c for the icon-shape mapping.
    char category[3];
} aircraft_t;

// A fully-populated dump1090/readsb aircraft object runs roughly 400-700 bytes
// compact-encoded (hex, flight, every nav_*/nic/nac_*/sil* field, mlat/tisb
// arrays, etc.). 2048 gives ~3x headroom over that for one in-progress
// object, while staying tiny next to the 64KB whole-response buffer this
// design replaces - the buffer is static (BSS), never malloc'd.
#define AIRCRAFT_SCAN_BUF_SIZE 2048

typedef enum {
    AIRCRAFT_SCAN_INCOMPLETE = 0,  // still receiving; no verdict yet
    AIRCRAFT_SCAN_COMPLETE,        // reached a clean top-level document end
    AIRCRAFT_SCAN_MALFORMED,       // genuine JSON syntax error, or scan buffer overflow
} aircraft_scan_outcome_t;

// Invoked once per fully-parsed aircraft object as the document streams in.
// `ac` is only valid for the duration of the call (backed by a stack
// temporary) - copy out anything you want to keep.
typedef void (*aircraft_model_cb_t)(const aircraft_t *ac, void *user_ctx);

// Incremental aircraft.json scanner. Feed it HTTP body chunks as they arrive
// (skyaware_client_fetch does this directly from its receive callback) - it
// never buffers more than one JSON record's worth of bytes, so there is no
// whole-response size cap. Zero heap allocation: keep this ~2KB+ struct
// `static`, not on the stack.
typedef struct {
    char buf[AIRCRAFT_SCAN_BUF_SIZE];
    size_t buf_len;
    int state;
    bool seen_aircraft;
    aircraft_scan_outcome_t outcome;
    aircraft_model_cb_t cb;
    void *user_ctx;
    unsigned long total_emitted;    // uncapped count of aircraft objects emitted so far
    unsigned long total_bytes_fed;  // diagnostics only, for malformed/incomplete logging
} aircraft_model_scanner_t;

void aircraft_model_scanner_init(aircraft_model_scanner_t *sc, aircraft_model_cb_t cb, void *user_ctx);

// Call once per HTTP receive chunk, in order. No-ops once the scanner has
// reached a terminal outcome (COMPLETE or MALFORMED).
void aircraft_model_scanner_feed(aircraft_model_scanner_t *sc, const char *data, size_t len);

aircraft_scan_outcome_t aircraft_model_scanner_outcome(const aircraft_model_scanner_t *sc);
unsigned long aircraft_model_scanner_total_emitted(const aircraft_model_scanner_t *sc);

#endif
