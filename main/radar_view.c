#include "radar_view.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "GUI_Paint.h"
#include "LCD_1in28.h"
#include "fonts.h"
#include "geo_math.h"

#define CENTER_X 120
#define CENTER_Y 120
// Shrunk slightly from 105 to leave enough margin for the scaled N/E/S/W
// labels (14x14px at CARDINAL_LABEL_SCALE, below) to fully fit outside the
// outer ring - the tightest case is the S label, whose bottom edge lands at
// CENTER_Y + RADAR_RADIUS_PX + 2 (gap) + 14 = 234, inside the 240px canvas.
#define RADAR_RADIUS_PX 98
// A real sans-serif font needs less blow-up than a serif one to stay
// legible at this display's small label sizes.
#define CARDINAL_LABEL_SCALE 1.75
#define TARGET_LABEL_SCALE 1.4
#define TARGET_LABEL_SPACING 1  // extra px between characters, for readability

// Local (unrotated, "pointing north") simplified top-down airplane
// silhouette - nose/tail/main-wings/tail-wings - in pixels relative to the
// target's screen position, rotated by heading_deg before drawing.
// AIRCRAFT_SHAPE_LARGE/LIGHT draw this same silhouette scaled up/down by
// PLANE_LARGE_SCALE/PLANE_LIGHT_SCALE rather than separate shapes - a
// resized version of the same three-stroke icon reads clearly as
// "bigger"/"smaller aircraft" without needing more geometries to maintain.
// AIRCRAFT_SHAPE_UNKNOWN stays at the neutral 1.0 - no size claim either way
// for an aircraft that hasn't told us its category at all.
#define PLANE_NOSE_LEN 10
#define PLANE_TAIL_LEN 6
#define PLANE_WING_HALF_SPAN 7
#define PLANE_WING_Y (-1)
#define PLANE_TAIL_WING_HALF_SPAN 4
#define PLANE_TAIL_WING_Y 5
#define PLANE_LARGE_SCALE 1.4
#define PLANE_LIGHT_SCALE 0.75
// Largest distance any plane-silhouette vertex sits from the target's own
// position - PLANE_NOSE_LEN is the biggest of the five (nose/tail/two wing
// tips/two tail-wing tips) even after scaling, since scaling is uniform
// across all five. PLANE_LARGE_SCALE (not PLANE_LIGHT_SCALE, which shrinks
// it) is the one that grows this.
#define PLANE_MAX_REACH_PX ((int)(PLANE_NOSE_LEN * PLANE_LARGE_SCALE + 0.5))

// Rotorcraft icon: a rotor-disc circle (immediately distinct from any
// fixed-wing silhouette, unlike just resizing the plane icon) plus a short
// tail boom ending in a perpendicular tail-rotor tick, so heading is still
// legible even though dump1090 reports no rotor phase to actually sweep.
// Deliberately not derived from the PLANE_* constants above - a
// helicopter's silhouette has nothing in common with a fixed-wing one, so
// scaling one from the other would be coincidental, not meaningful.
#define HELI_BODY_RADIUS_PX 3
#define HELI_ROTOR_RADIUS_PX 8
#define HELI_TAIL_LEN 9
#define HELI_TAIL_ROTOR_HALF_SPAN 3
#define HELI_MAX_REACH_PX (HELI_TAIL_LEN + HELI_TAIL_ROTOR_HALF_SPAN)

#define AIRCRAFT_ICON_MAX_REACH_PX (PLANE_MAX_REACH_PX > HELI_MAX_REACH_PX ? PLANE_MAX_REACH_PX : HELI_MAX_REACH_PX)

// draw_target_label's opaque backing rect is drawn after (on top of) the
// aircraft icon, so if the label starts too close to the target's own
// position, a heading pointed roughly toward the label (e.g. heading_deg
// near 90) gets its nose painted over - the icon's vertices can land up to
// AIRCRAFT_ICON_MAX_REACH_PX px from center in any direction (whichever
// shape is drawn), so the gap needs to clear that plus a visible margin,
// not just a few px.
#define TARGET_LABEL_GAP_PX (AIRCRAFT_ICON_MAX_REACH_PX + 5)

#define RADAR_GREEN 0x97CF  // RGB565 conversion of #96F97B

static const char *TAG = "radar_view";

static UWORD *fb;

void radar_view_init(void)
{
    size_t image_size = LCD_1IN28_WIDTH * LCD_1IN28_HEIGHT * 2;
    fb = (UWORD *)heap_caps_malloc(image_size, MALLOC_CAP_DMA);
    if (fb == NULL) {
        ESP_LOGE(TAG, "framebuffer allocation failed");
        return;
    }
    Paint_NewImage(fb, LCD_1IN28_WIDTH, LCD_1IN28_HEIGHT, ROTATE_0, BLACK, 16);
    Paint_SelectImage(fb);
    Paint_SetRotate(ROTATE_0);
}

// Uses FontSans (main/Fonts/font_sans.c) - a sans-serif 8x8 bitmap font,
// glyph data copied verbatim from the Public Domain font8x8 project
// (https://github.com/dhepper/font8x8, itself rooted in the classic IBM
// VGA ROM font) - rather than Font12/Font8/Font16/Font20/Font24
// (main/Fonts/font12.c etc.), which all share the same "Courier New" design
// and therefore the same visible serifs (foot-flares, e.g. on the base of
// 'A') that make small on-screen text harder to read. FontSans's bytes are
// LSB-first per row (bit 0 is the leftmost pixel) - font8x8's native
// convention, kept as-is rather than transformed - which is why
// glyph_pixel_on tests `1 << col` here instead of the `0x80 >> col`
// convention the Waveshare-vendored fonts use. GLYPH_SUPERSAMPLE is how
// many sub-samples per axis draw_char_scaled tests per *output* pixel
// against the source bitmap for whatever scaling is applied; blending
// fg/bg by the resulting coverage fraction (rather than an all-or-nothing
// nearest-neighbor lookup) smooths diagonal/curved edges.
#define GLYPH_SUPERSAMPLE 3

static bool glyph_pixel_on(const uint8_t *ptr, int bytes_per_row, int row, int col)
{
    if (row < 0 || row >= FontSans.Height || col < 0 || col >= FontSans.Width) {
        return false;
    }
    return (ptr[row * bytes_per_row + col / 8] & (1 << (col % 8))) != 0;
}

// Linearly blends two RGB565 colors by coverage/max_coverage (0..max_coverage).
static UWORD blend565(UWORD fg, UWORD bg, int coverage, int max_coverage)
{
    if (coverage <= 0) {
        return bg;
    }
    if (coverage >= max_coverage) {
        return fg;
    }
    int fr = (fg >> 11) & 0x1F, fg_g = (fg >> 5) & 0x3F, fb = fg & 0x1F;
    int br = (bg >> 11) & 0x1F, bg_g = (bg >> 5) & 0x3F, bb = bg & 0x1F;
    int r = (fr * coverage + br * (max_coverage - coverage)) / max_coverage;
    int g = (fg_g * coverage + bg_g * (max_coverage - coverage)) / max_coverage;
    int b = (fb * coverage + bb * (max_coverage - coverage)) / max_coverage;
    return (UWORD)(((r & 0x1F) << 11) | ((g & 0x3F) << 5) | (b & 0x1F));
}

// Plots each glyph pixel by supersampling FontSans's raw bitmap data
// directly (GUI_Paint's Paint_DrawString_EN has no scale parameter, let
// alone anti-aliasing). Supports fractional scale factors, not just
// whole-pixel multiples. Returns the glyph's rendered width/height so
// draw_string_scaled can space characters correctly.
static void draw_char_scaled(int x, int y, char c, UWORD fg, UWORD bg, double scale, int *out_w, int *out_h)
{
    // FontSans defines glyphs for the full printable ASCII range, ' '
    // (0x20) through '~' (0x7E) - anything outside that would index past
    // the end of FontSans_Table and render whatever garbage bytes follow
    // it in flash. Clamp to a blank space instead.
    if (c < ' ' || c > '~') {
        c = ' ';
    }

    int bytes_per_row = FontSans.Width / 8 + (FontSans.Width % 8 ? 1 : 0);
    uint32_t offset = (uint32_t)(c - ' ') * FontSans.Height * bytes_per_row;
    const uint8_t *ptr = &FontSans.table[offset];

    int ow = (int)lround(FontSans.Width * scale);
    int oh = (int)lround(FontSans.Height * scale);
    const int max_coverage = GLYPH_SUPERSAMPLE * GLYPH_SUPERSAMPLE;

    for (int oy = 0; oy < oh; oy++) {
        for (int ox = 0; ox < ow; ox++) {
            int coverage = 0;
            for (int sy = 0; sy < GLYPH_SUPERSAMPLE; sy++) {
                double src_y = (oy + (sy + 0.5) / GLYPH_SUPERSAMPLE) * FontSans.Height / oh;
                int row = (int)src_y;
                for (int sx = 0; sx < GLYPH_SUPERSAMPLE; sx++) {
                    double src_x = (ox + (sx + 0.5) / GLYPH_SUPERSAMPLE) * FontSans.Width / ow;
                    if (glyph_pixel_on(ptr, bytes_per_row, row, (int)src_x)) {
                        coverage++;
                    }
                }
            }
            UWORD color = blend565(fg, bg, coverage, max_coverage);
            Paint_SetPixel((UWORD)(x + ox), (UWORD)(y + oy), color);
        }
    }

    *out_w = ow;
    *out_h = oh;
}

static void draw_string_scaled(int x, int y, const char *s, UWORD fg, UWORD bg, double scale, int spacing)
{
    int cx = x;
    while (*s != '\0') {
        int w, h;
        draw_char_scaled(cx, y, *s, fg, bg, scale, &w, &h);
        cx += w + spacing;
        s++;
    }
}

// FontSans is a fixed-width bitmap font (draw_char_scaled always scales the
// same Width/Height regardless of glyph), so a string's rendered width is
// exact and cheap to compute without drawing it.
static int measure_string_width(const char *s, double scale, int spacing)
{
    int n = (int)strlen(s);
    if (n == 0) {
        return 0;
    }
    int glyph_w = (int)lround(FontSans.Width * scale);
    return n * glyph_w + (n - 1) * spacing;
}

void radar_view_draw_status(const char *line1, const char *line2)
{
    Paint_Clear(BLACK);
    int line_h = (int)lround(FontSans.Height * TARGET_LABEL_SCALE) + 4;
    if (line1 != NULL) {
        draw_string_scaled(10, 100, line1, WHITE, BLACK, TARGET_LABEL_SCALE, TARGET_LABEL_SPACING);
    }
    if (line2 != NULL) {
        draw_string_scaled(10, 100 + line_h, line2, WHITE, BLACK, TARGET_LABEL_SCALE, TARGET_LABEL_SPACING);
    }
    LCD_1IN28_Display(fb);
}

static void draw_crosshairs(void)
{
    Paint_DrawLine(CENTER_X, CENTER_Y - RADAR_RADIUS_PX, CENTER_X, CENTER_Y + RADAR_RADIUS_PX,
                    RADAR_GREEN, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
    Paint_DrawLine(CENTER_X - RADAR_RADIUS_PX, CENTER_Y, CENTER_X + RADAR_RADIUS_PX, CENTER_Y,
                    RADAR_GREEN, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
}

// Labels sit immediately outside the outer ring, offset by the font's own
// (scaled) dimensions plus a small gap rather than hardcoded magic numbers,
// so they stay correctly placed if RADAR_RADIUS_PX or the font ever changes.
static void draw_cardinal_labels(void)
{
    int w = (int)lround(FontSans.Width * CARDINAL_LABEL_SCALE);
    int h = (int)lround(FontSans.Height * CARDINAL_LABEL_SCALE);

    draw_string_scaled(CENTER_X - w / 2, CENTER_Y - RADAR_RADIUS_PX - h - 2,
                        "N", WHITE, BLACK, CARDINAL_LABEL_SCALE, 0);
    draw_string_scaled(CENTER_X - w / 2, CENTER_Y + RADAR_RADIUS_PX + 2,
                        "S", WHITE, BLACK, CARDINAL_LABEL_SCALE, 0);
    draw_string_scaled(CENTER_X + RADAR_RADIUS_PX + 2, CENTER_Y - h / 2,
                        "E", WHITE, BLACK, CARDINAL_LABEL_SCALE, 0);
    draw_string_scaled(CENTER_X - RADAR_RADIUS_PX - w - 2, CENTER_Y - h / 2,
                        "W", WHITE, BLACK, CARDINAL_LABEL_SCALE, 0);
}

// Converts an HSL color (h in degrees [0,360), s/l in [0,1]) to RGB565,
// matching the standard HSL->RGB formula. Used only by altitude_color()
// below - a hue sweep is a much closer match to SkyAware/tar1090's
// altitude gradient than a handful of flat named colors would be.
static UWORD hsl_to_rgb565(double h, double s, double l)
{
    double c = (1.0 - fabs(2.0 * l - 1.0)) * s;
    double hp = h / 60.0;
    double x = c * (1.0 - fabs(fmod(hp, 2.0) - 1.0));
    double r1, g1, b1;
    if (hp < 1) { r1 = c; g1 = x; b1 = 0; }
    else if (hp < 2) { r1 = x; g1 = c; b1 = 0; }
    else if (hp < 3) { r1 = 0; g1 = c; b1 = x; }
    else if (hp < 4) { r1 = 0; g1 = x; b1 = c; }
    else if (hp < 5) { r1 = x; g1 = 0; b1 = c; }
    else { r1 = c; g1 = 0; b1 = x; }
    double m = l - c / 2.0;
    int r = (int)lround((r1 + m) * 31.0);   // 5 bits
    int g = (int)lround((g1 + m) * 63.0);   // 6 bits
    int b = (int)lround((b1 + m) * 31.0);   // 5 bits
    return (UWORD)(((r & 0x1F) << 11) | ((g & 0x3F) << 5) | (b & 0x1F));
}

// Altitude-to-hue control points, taken directly from dump1090-fa/
// dump1090-mutability's default ColorByAlt config (config.js.example) -
// the actual scheme SkyAware's own web map ships with, not an approximation
// of it: { h: [ {alt:2000,val:20}, {alt:10000,val:140}, {alt:40000,val:300} ],
// s: 85, l: 50 }. Only 3 points are needed because linearly interpolating
// hue between them already sweeps through yellow/green/cyan/blue/indigo on
// its own, without an explicit stop for each. Altitudes at or below the
// first point use its hue; at or above the last, its hue - same as the
// reference config's documented behavior.
typedef struct {
    double alt_ft;
    double hue_deg;
} altitude_hue_stop_t;

static const altitude_hue_stop_t ALTITUDE_HUE_STOPS[] = {
    {2000, 20},    // orange
    {10000, 140},  // light green
    {40000, 300},  // magenta
};
#define ALTITUDE_HUE_STOP_COUNT (sizeof(ALTITUDE_HUE_STOPS) / sizeof(ALTITUDE_HUE_STOPS[0]))
#define ALTITUDE_COLOR_SATURATION 0.85
#define ALTITUDE_COLOR_LIGHTNESS 0.50

// Unknown altitude (alt_baro missing) uses the reference config's own
// "unknown" entry (h:0 s:0 l:40 - a flat mid-gray, since s=0 drops hue
// entirely) rather than a locally-picked color.
static UWORD altitude_color(bool has_altitude, int altitude_ft)
{
    if (!has_altitude) {
        return hsl_to_rgb565(0, 0.0, 0.40);
    }
    double alt = altitude_ft;
    if (alt <= ALTITUDE_HUE_STOPS[0].alt_ft) {
        return hsl_to_rgb565(ALTITUDE_HUE_STOPS[0].hue_deg, ALTITUDE_COLOR_SATURATION, ALTITUDE_COLOR_LIGHTNESS);
    }
    for (size_t i = 0; i + 1 < ALTITUDE_HUE_STOP_COUNT; i++) {
        const altitude_hue_stop_t *lo = &ALTITUDE_HUE_STOPS[i];
        const altitude_hue_stop_t *hi = &ALTITUDE_HUE_STOPS[i + 1];
        if (alt <= hi->alt_ft) {
            double t = (alt - lo->alt_ft) / (hi->alt_ft - lo->alt_ft);
            double hue = lo->hue_deg + t * (hi->hue_deg - lo->hue_deg);
            return hsl_to_rgb565(hue, ALTITUDE_COLOR_SATURATION, ALTITUDE_COLOR_LIGHTNESS);
        }
    }
    const altitude_hue_stop_t *last = &ALTITUDE_HUE_STOPS[ALTITUDE_HUE_STOP_COUNT - 1];
    return hsl_to_rgb565(last->hue_deg, ALTITUDE_COLOR_SATURATION, ALTITUDE_COLOR_LIGHTNESS);
}

// Rotates a local (x,y) offset by heading_deg and adds it to the target's
// screen position, in one step - draw_aircraft_icon's vertices are all
// computed this way.
static void plane_point(int cx, int cy, double heading_deg, double lx, double ly, int *out_x, int *out_y)
{
    double rx, ry;
    geo_math_rotate_offset(heading_deg, lx, ly, &rx, &ry);
    *out_x = cx + (int)lround(rx);
    *out_y = cy + (int)lround(ry);
}

// Draws a small top-down airplane silhouette at (x, y) pointing toward
// heading_deg, colored by altitude_color(): a fuselage stroke from nose to
// tail, crossed by a wider main-wing stroke and a narrower tail-wing
// stroke, the same three-stroke shape most small ADS-B plane icons use.
// `scale` multiplies every local offset uniformly (1.0 for the normal size,
// PLANE_LARGE_SCALE for AIRCRAFT_SHAPE_LARGE - see draw_aircraft_icon).
static void draw_plane_icon(int x, int y, double heading_deg, double scale, UWORD color)
{
    int nose_x, nose_y, tail_x, tail_y;
    int wing_l_x, wing_l_y, wing_r_x, wing_r_y;
    int tw_l_x, tw_l_y, tw_r_x, tw_r_y;

    plane_point(x, y, heading_deg, 0, -PLANE_NOSE_LEN * scale, &nose_x, &nose_y);
    plane_point(x, y, heading_deg, 0, PLANE_TAIL_LEN * scale, &tail_x, &tail_y);
    plane_point(x, y, heading_deg, -PLANE_WING_HALF_SPAN * scale, PLANE_WING_Y * scale, &wing_l_x, &wing_l_y);
    plane_point(x, y, heading_deg, PLANE_WING_HALF_SPAN * scale, PLANE_WING_Y * scale, &wing_r_x, &wing_r_y);
    plane_point(x, y, heading_deg, -PLANE_TAIL_WING_HALF_SPAN * scale, PLANE_TAIL_WING_Y * scale, &tw_l_x, &tw_l_y);
    plane_point(x, y, heading_deg, PLANE_TAIL_WING_HALF_SPAN * scale, PLANE_TAIL_WING_Y * scale, &tw_r_x, &tw_r_y);

    Paint_DrawLine((UWORD)nose_x, (UWORD)nose_y, (UWORD)tail_x, (UWORD)tail_y,
                    color, DOT_PIXEL_2X2, LINE_STYLE_SOLID);
    Paint_DrawLine((UWORD)wing_l_x, (UWORD)wing_l_y, (UWORD)wing_r_x, (UWORD)wing_r_y,
                    color, DOT_PIXEL_2X2, LINE_STYLE_SOLID);
    Paint_DrawLine((UWORD)tw_l_x, (UWORD)tw_l_y, (UWORD)tw_r_x, (UWORD)tw_r_y,
                    color, DOT_PIXEL_2X2, LINE_STYLE_SOLID);
}

// Rotor disc + tail boom + tail-rotor tick - see the HELI_* constants above
// for why this doesn't share geometry with draw_plane_icon. Drawn
// widest-first (rotor disc, then tail boom/tick, then the small filled body
// on top) so the body reads as sitting inside the disc rather than being
// bisected by the tail boom line passing through the center.
static void draw_helicopter_icon(int x, int y, double heading_deg, UWORD color)
{
    int tail_x, tail_y, tr_l_x, tr_l_y, tr_r_x, tr_r_y;
    plane_point(x, y, heading_deg, 0, HELI_TAIL_LEN, &tail_x, &tail_y);
    plane_point(x, y, heading_deg, -HELI_TAIL_ROTOR_HALF_SPAN, HELI_TAIL_LEN, &tr_l_x, &tr_l_y);
    plane_point(x, y, heading_deg, HELI_TAIL_ROTOR_HALF_SPAN, HELI_TAIL_LEN, &tr_r_x, &tr_r_y);

    Paint_DrawCircle((UWORD)x, (UWORD)y, HELI_ROTOR_RADIUS_PX, color, DOT_PIXEL_2X2, DRAW_FILL_EMPTY);
    Paint_DrawLine((UWORD)x, (UWORD)y, (UWORD)tail_x, (UWORD)tail_y, color, DOT_PIXEL_2X2, LINE_STYLE_SOLID);
    Paint_DrawLine((UWORD)tr_l_x, (UWORD)tr_l_y, (UWORD)tr_r_x, (UWORD)tr_r_y, color, DOT_PIXEL_2X2, LINE_STYLE_SOLID);
    Paint_DrawCircle((UWORD)x, (UWORD)y, HELI_BODY_RADIUS_PX, color, DOT_PIXEL_1X1, DRAW_FILL_FULL);
}

// Dispatches to a genuinely different silhouette for rotorcraft, or the
// same fixed-wing silhouette at a shape-dependent scale otherwise -
// AIRCRAFT_SHAPE_UNKNOWN (no category broadcast, or a code radar_view.c
// doesn't distinguish) draws at the neutral 1.0 scale this project has
// always used.
static void draw_aircraft_icon(int x, int y, double heading_deg, aircraft_shape_t shape, UWORD color)
{
    if (shape == AIRCRAFT_SHAPE_ROTORCRAFT) {
        draw_helicopter_icon(x, y, heading_deg, color);
        return;
    }
    double scale = 1.0;
    if (shape == AIRCRAFT_SHAPE_LARGE) {
        scale = PLANE_LARGE_SCALE;
    } else if (shape == AIRCRAFT_SHAPE_LIGHT) {
        scale = PLANE_LIGHT_SCALE;
    }
    draw_plane_icon(x, y, heading_deg, scale, color);
}

// Just the flight number (broadcast callsign, or ICAO hex as a fallback -
// see aircraft_model.c) - no altitude text and no separate ICAO-hex line.
// Altitude is conveyed by the aircraft icon's color (altitude_color())
// instead of a numeric line, and the hex address was redundant clutter
// (two identifiers for the same aircraft) whenever a real callsign was
// already available.
static void draw_target_label(int x, int y, const radar_target_t *t)
{
    char callsign_upper[sizeof(t->callsign)];
    size_t i = 0;
    for (; i < sizeof(callsign_upper) - 1 && t->callsign[i] != '\0'; i++) {
        callsign_upper[i] = (char)toupper((unsigned char)t->callsign[i]);
    }
    callsign_upper[i] = '\0';

    int glyph_h = (int)lround(FontSans.Height * TARGET_LABEL_SCALE);
    int block_w = measure_string_width(callsign_upper, TARGET_LABEL_SCALE, TARGET_LABEL_SPACING);
    int pad = 1;

    // Paint_DrawRectangle (unlike Paint_SetPixel) rejects the whole
    // primitive if ANY one coordinate falls outside the canvas rather than
    // clipping - so a label anchored purely off targets[i]'s screen
    // position can silently fail to draw at all for targets near the edge
    // of the round 240x240 display (common at max range, i.e. exactly when
    // the display is busiest). Clamp the label's own origin so its full
    // backing rect - and the text drawn at the same origin - always stays
    // on-canvas instead of just trusting the target's raw position.
    int lx = x + TARGET_LABEL_GAP_PX;
    if (lx - pad < 0) {
        lx = pad;
    } else if (lx + block_w + pad > LCD_1IN28_WIDTH) {
        lx = LCD_1IN28_WIDTH - pad - block_w;
    }
    // ly anchors the line a half-glyph above the target's own y, so the
    // label reads as roughly centered on the dot rather than hanging
    // entirely below it.
    int ly = y - glyph_h / 2;
    if (ly - pad < 0) {
        ly = pad;
    } else if (ly + glyph_h + pad > LCD_1IN28_HEIGHT) {
        ly = LCD_1IN28_HEIGHT - pad - glyph_h;
    }

    // Opaque backing rect behind the text, drawn first, so this label -
    // which callers draw in back-to-front z-order - fully blanks out
    // whatever overlapping content (another label, an arrow) was drawn
    // underneath it rather than leaving a mixed-pixel jumble.
    Paint_DrawRectangle((UWORD)(lx - pad), (UWORD)(ly - pad),
                         (UWORD)(lx + block_w + pad), (UWORD)(ly + glyph_h + pad),
                         BLACK, DOT_PIXEL_1X1, DRAW_FILL_FULL);

    draw_string_scaled(lx, ly, callsign_upper, WHITE, BLACK, TARGET_LABEL_SCALE, TARGET_LABEL_SPACING);
}

// Corners of the round 240x240 canvas fall outside the inscribed range-ring
// circle (radius RADAR_RADIUS_PX=98 centered at 120,120), so this never
// overlaps a target label or the cardinal labels next to it.
#define OVERFLOW_LABEL_SCALE 1.2

static void draw_overflow_indicator(int hidden_count)
{
    char text[8];
    snprintf(text, sizeof(text), "+%d", hidden_count);
    draw_string_scaled(6, 6, text, WHITE, BLACK, OVERFLOW_LABEL_SCALE, TARGET_LABEL_SPACING);
}

#define MARKER_DOT_RADIUS_PX 3  // small filled dot marking a static point of interest

static void draw_marker_dot(int x, int y)
{
    // A static point of interest (e.g. an airport) has no heading and,
    // unlike a target's label, sits right on the aircraft's own flight
    // path - a text label here kept getting covered up by aircraft labels
    // passing over it, for no real benefit (the marker's position already
    // conveys "airport here"). A small solid dot instead: RADAR_GREEN
    // rather than WHITE (what a target's label uses) so it still reads as
    // "fixed reference point, not traffic" at a glance, and a filled
    // circle rather than draw_aircraft_icon's plane silhouette so the two
    // are never ambiguous. Always safely on-canvas: x,y come from the same
    // range-clamped projection aircraft targets use (RADAR_RADIUS_PX from
    // center), and +-MARKER_DOT_RADIUS_PX is tiny next to that margin.
    Paint_DrawCircle((UWORD)x, (UWORD)y, MARKER_DOT_RADIUS_PX, RADAR_GREEN, DOT_PIXEL_1X1, DRAW_FILL_FULL);
}

#define RING_LABEL_SCALE 0.9
#define RING_LABEL_MIN_RADIUS_PX 20  // skip labeling a ring too small to comfortably fit text

// Draws a small "<N>NM" label along the positive-x (east) axis just
// *outside* the given ring radius (only the 50% ring is ever passed in
// practice - see radar_view_draw_radar - so there's no risk of colliding
// with the "E" cardinal label out at the 100%/outer ring).
static void draw_ring_label(int radius_px, double ring_nm)
{
    if (radius_px < RING_LABEL_MIN_RADIUS_PX) {
        return;
    }
    char text[8];
    long rounded = lround(ring_nm);
    if (fabs(ring_nm - (double)rounded) < 0.05) {
        snprintf(text, sizeof(text), "%ldNM", rounded);
    } else {
        snprintf(text, sizeof(text), "%.1fNM", ring_nm);
    }
    int lx = CENTER_X + radius_px + 2;
    int ly = CENTER_Y + 3;  // just below the horizontal crosshair line
    draw_string_scaled(lx, ly, text, RADAR_GREEN, BLACK, RING_LABEL_SCALE, 0);
}

void radar_view_draw_radar(double range_nm, const radar_marker_t *markers, int marker_count,
                            const radar_target_t *targets, int count, int hidden_count)
{
    Paint_Clear(BLACK);

    // Rings sit at 25/50/75/100% of range_nm so they stay evenly spaced
    // regardless of whatever range_nm is configured to.
    static const double RING_FRACTIONS[] = {0.25, 0.5, 0.75, 1.0};
    const size_t RING_COUNT = sizeof(RING_FRACTIONS) / sizeof(RING_FRACTIONS[0]);
    // Rings are evenly spaced by construction, so labeling all of them is
    // redundant - one distance readout is enough to infer the rest. The
    // middle ring rather than the innermost/outermost: it's clear of both
    // the crowded center (targets/markers/home dot) and the cardinal
    // labels out at the edge.
    const size_t LABELED_RING_INDEX = 1;  // 50%
    int labeled_ring_radius_px = 0;
    for (size_t ring = 0; ring < RING_COUNT; ring++) {
        int r = (int)lround(RING_FRACTIONS[ring] * RADAR_RADIUS_PX);
        if (ring == LABELED_RING_INDEX) {
            labeled_ring_radius_px = r;
        }
        Paint_DrawCircle(CENTER_X, CENTER_Y, r, RADAR_GREEN, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
    }

    draw_crosshairs();
    draw_cardinal_labels();

    // Distance label along the positive-x (east) axis, drawn after the
    // crosshair so its opaque background cleanly covers the line underneath
    // rather than the line cutting through the text. Lets range be read off
    // directly instead of having to remember what range_nm dynamic zoom
    // last settled on.
    draw_ring_label(labeled_ring_radius_px, RING_FRACTIONS[LABELED_RING_INDEX] * range_nm);

    Paint_DrawCircle(CENTER_X, CENTER_Y, 3, WHITE, DOT_PIXEL_1X1, DRAW_FILL_FULL);

    // Markers before targets: aircraft come and go every refresh and are
    // the reason someone's looking at this display, so their labels should
    // win any overlap with a marker dot, not the other way around.
    for (int i = 0; i < marker_count; i++) {
        int x, y;
        geo_math_polar_to_screen(markers[i].bearing_deg, markers[i].dist_nm, range_nm,
                                  CENTER_X, CENTER_Y, RADAR_RADIUS_PX, &x, &y);
        draw_marker_dot(x, y);
    }

    for (int i = 0; i < count; i++) {
        int x, y;
        geo_math_polar_to_screen(targets[i].bearing_deg, targets[i].dist_nm, range_nm,
                                  CENTER_X, CENTER_Y, RADAR_RADIUS_PX, &x, &y);

        draw_aircraft_icon(x, y, targets[i].heading_deg, targets[i].shape,
                            altitude_color(targets[i].has_altitude, targets[i].altitude_ft));
        draw_target_label(x, y, &targets[i]);
    }

    if (hidden_count > 0) {
        draw_overflow_indicator(hidden_count);
    }

    LCD_1IN28_Display(fb);
}
