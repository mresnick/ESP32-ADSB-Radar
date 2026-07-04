#ifndef RADAR_VIEW_H
#define RADAR_VIEW_H

#include <stdbool.h>

// Coarse icon-shape bucket derived from an aircraft's raw ADS-B emitter
// category (see classify_shape() in app_main.c) - draw_aircraft_icon
// (radar_view.c) draws a genuinely different silhouette for
// AIRCRAFT_SHAPE_ROTORCRAFT, and the same fixed-wing silhouette scaled up
// for AIRCRAFT_SHAPE_LARGE / down for AIRCRAFT_SHAPE_LIGHT.
// AIRCRAFT_SHAPE_UNKNOWN covers both "category not broadcast" and any
// category code not mapped to LIGHT/LARGE/ROTORCRAFT, and draws at the
// neutral (unscaled) size this project has always used.
typedef enum {
    AIRCRAFT_SHAPE_UNKNOWN = 0,
    AIRCRAFT_SHAPE_LIGHT,
    AIRCRAFT_SHAPE_LARGE,
    AIRCRAFT_SHAPE_ROTORCRAFT,
} aircraft_shape_t;

typedef struct {
    double dist_nm;
    double bearing_deg;
    double heading_deg;
    // On-screen label text - callsign, ICAO aircraft type, registration/
    // tail number, or empty (no label at all), depending on
    // radar_config_t.label_mode (see config_store.h and app_main.c, which
    // resolves the mode into this plain string so this module stays
    // unaware of config entirely). Empty only means the user chose
    // RADAR_LABEL_NONE - every other mode falls back to callsign (which
    // itself falls back to ICAO hex - see aircraft_model.c) if the chosen
    // field isn't broadcast/available for a given aircraft, so drawing code
    // can treat "empty" as "draw nothing" unambiguously.
    char label[10];
    int altitude_ft;
    bool has_altitude;
    aircraft_shape_t shape;
} radar_target_t;

// A static (non-moving) point of interest - currently just nearby major
// airports (see main/airports.h) - drawn distinctly from aircraft targets:
// a small fixed dot, no label/heading/altitude icon, since it doesn't have
// any of those and a label just ended up permanently covered by aircraft
// labels passing over it.
typedef struct {
    double dist_nm;
    double bearing_deg;
} radar_marker_t;

// Allocates the DMA-capable framebuffer. Call once, after LCD_1IN28_Init.
void radar_view_init(void);

// Full-screen status text for boot/provisioning/error states.
void radar_view_draw_status(const char *line1, const char *line2);

// Draws range rings, N/E/S/W labels, crosshairs, a home marker, static
// markers (e.g. airports), and an altitude-colored aircraft icon + label
// per aircraft target. Markers are drawn before targets, so
// a target's opaque label can legibly overlap a marker but not vice versa.
// `hidden_count` is how many additional in-range airborne aircraft exist
// beyond the ones passed in `targets` (i.e. didn't make the top-`count`
// cut) - pass 0 if none. When positive, a small "+N" indicator is drawn so
// a busy feed reads as "N more are being hidden" rather than looking like a
// complete picture that just happens to stop at `count`.
void radar_view_draw_radar(double range_nm, const radar_marker_t *markers, int marker_count,
                            const radar_target_t *targets, int count, int hidden_count);

#endif
