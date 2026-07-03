#ifndef AIRPORTS_H
#define AIRPORTS_H

#include <stddef.h>

typedef struct {
    const char *icao;  // 4-letter ICAO code, e.g. "KBOS"
    const char *iata;  // 3-letter IATA code, e.g. "BOS"
    double lat;
    double lon;
} airport_t;

// A small, hand-curated starter table of major-hub airport reference points
// (main/airports.c), NOT a comprehensive database - just enough to mark a
// nearby major airport on the radar as a small dot, the way flight-tracking
// sites conventionally do (see radar_view.c's draw_marker_dot - airports
// aren't currently labeled by name/code on screen, a label there just kept
// getting covered up by aircraft labels passing over it). Coordinates are
// each airport's commonly-published reference point, accurate to roughly 4
// decimal degrees (tens of meters) - plenty for a single-pixel-scale dot on
// a 20nm-range display, but not survey-grade. Extend this table directly
// (same {icao, iata, lat, lon} shape) with whatever regional airports
// matter for a given install - it's a flat `const` array kept in flash, not
// something that needs a database or a fetch at runtime.
extern const airport_t g_major_airports[];
extern const size_t g_major_airports_count;

#endif
