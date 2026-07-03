// Host-side ("native") tests for main/geo_math.c (haversine distance/
// bearing and the polar-to-screen/rotation helpers radar_view.c uses for
// layout). Plain C, no ESP-IDF dependency at all - geo_math.c only pulls
// in <math.h> - so no fakes/ stub is needed here, unlike
// test_aircraft_model.c:
//
//   gcc -std=c11 -I ../../main test_geo_math.c ../../main/geo_math.c -o test_geo_math
//   ./test_geo_math
//
// Same small self-contained runner style as test_aircraft_model.c (no test
// framework dependency for two files).

#include <math.h>
#include <stdio.h>

#include "geo_math.h"

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

#define CHECK_NEAR(a, b, eps)                                                                          \
    do {                                                                                                \
        double _a = (a), _b = (b), _eps = (eps);                                                        \
        if (fabs(_a - _b) > _eps) {                                                                      \
            g_fail++;                                                                                   \
            fprintf(stderr, "FAIL %s:%d: %s (%.6f) not within %.6f of %s (%.6f)\n",                      \
                    __FILE__, __LINE__, #a, _a, _eps, #b, _b);                                           \
        } else {                                                                                         \
            g_pass++;                                                                                   \
        }                                                                                                \
    } while (0)

#define EARTH_RADIUS_NM 3440.065

// --- geo_math_distance_nm ---------------------------------------------------

static void test_distance_zero_for_same_point(void)
{
    CHECK_NEAR(geo_math_distance_nm(42.3656, -71.0096, 42.3656, -71.0096), 0.0, 1e-9);
}

// A quarter of the way around the equator has an exact closed form
// (R * pi/2), independent of the haversine implementation details -
// a cross-check rather than re-deriving the same formula under test.
static void test_distance_quarter_equator(void)
{
    // acos(-1.0) for pi rather than M_PI - like geo_math.c itself, this
    // avoids depending on math.h's GNU/POSIX extension macros being visible
    // under a strict -std=c11 host build (see geo_math.c's own comment).
    double dist = geo_math_distance_nm(0.0, 0.0, 0.0, 90.0);
    CHECK_NEAR(dist, EARTH_RADIUS_NM * (acos(-1.0) / 2.0), 0.5);
}

// Pole to pole is exactly half the circumference (R * pi) - also exercises
// the dlambda=0 / antipodal-latitude edge of the haversine formula.
static void test_distance_pole_to_pole(void)
{
    double dist = geo_math_distance_nm(90.0, 0.0, -90.0, 0.0);
    CHECK_NEAR(dist, EARTH_RADIUS_NM * acos(-1.0), 0.5);
}

static void test_distance_symmetric(void)
{
    double a = geo_math_distance_nm(40.7128, -74.0060, 51.4700, -0.4543);
    double b = geo_math_distance_nm(51.4700, -0.4543, 40.7128, -74.0060);
    CHECK_NEAR(a, b, 1e-9);
}

static void test_distance_never_negative(void)
{
    CHECK(geo_math_distance_nm(10.0, 10.0, -10.0, -10.0) >= 0.0);
}

// --- geo_math_bearing_deg ---------------------------------------------------

static void test_bearing_cardinal_directions(void)
{
    CHECK_NEAR(geo_math_bearing_deg(0.0, 0.0, 1.0, 0.0), 0.0, 1e-6);    // due north
    CHECK_NEAR(geo_math_bearing_deg(0.0, 0.0, 0.0, 1.0), 90.0, 1e-6);   // due east
    CHECK_NEAR(geo_math_bearing_deg(0.0, 0.0, -1.0, 0.0), 180.0, 1e-6); // due south
    CHECK_NEAR(geo_math_bearing_deg(0.0, 0.0, 0.0, -1.0), 270.0, 1e-6); // due west
}

static void test_bearing_always_in_range(void)
{
    double b = geo_math_bearing_deg(51.4700, -0.4543, 40.7128, -74.0060);
    CHECK(b >= 0.0 && b < 360.0);
}

// --- geo_math_polar_to_screen -----------------------------------------------

static void test_polar_to_screen_zero_distance_is_center(void)
{
    int x, y;
    geo_math_polar_to_screen(123.0, 0.0, 20.0, 120, 120, 98, &x, &y);
    CHECK(x == 120);
    CHECK(y == 120);
}

static void test_polar_to_screen_cardinal_bearings(void)
{
    int x, y;
    // North: straight up from center (screen Y decreases upward).
    geo_math_polar_to_screen(0.0, 20.0, 20.0, 120, 120, 98, &x, &y);
    CHECK(x == 120);
    CHECK(y == 120 - 98);
    // East: straight right from center.
    geo_math_polar_to_screen(90.0, 20.0, 20.0, 120, 120, 98, &x, &y);
    CHECK(x == 120 + 98);
    CHECK(y == 120);
}

// Anything beyond range_nm must clamp to the outer ring, not project past it
// - callers rely on this to keep off-range aircraft from landing off-canvas.
static void test_polar_to_screen_clamps_beyond_range(void)
{
    int x_at_range, y_at_range, x_beyond, y_beyond;
    geo_math_polar_to_screen(180.0, 20.0, 20.0, 120, 120, 98, &x_at_range, &y_at_range);
    geo_math_polar_to_screen(180.0, 500.0, 20.0, 120, 120, 98, &x_beyond, &y_beyond);
    CHECK(x_at_range == x_beyond);
    CHECK(y_at_range == y_beyond);
}

// range_nm == 0 must not divide by zero - degenerate config, but the
// function has an explicit branch for it that's worth pinning down.
static void test_polar_to_screen_zero_range_no_crash(void)
{
    int x, y;
    geo_math_polar_to_screen(45.0, 5.0, 0.0, 120, 120, 98, &x, &y);
    CHECK(x == 120);
    CHECK(y == 120);
}

// --- geo_math_rotate_offset --------------------------------------------------

static void test_rotate_offset_zero_heading_is_identity(void)
{
    double x, y;
    geo_math_rotate_offset(0.0, 3.0, -7.0, &x, &y);
    CHECK_NEAR(x, 3.0, 1e-9);
    CHECK_NEAR(y, -7.0, 1e-9);
}

// "North" (0,-1) rotated 90 degrees clockwise must point "east" (1,0) on
// screen - the same clockwise, north-up convention geo_math_polar_to_screen
// uses, which is what lets draw_heading_arrow orient correctly.
static void test_rotate_offset_90_matches_polar_convention(void)
{
    double x, y;
    geo_math_rotate_offset(90.0, 0.0, -1.0, &x, &y);
    CHECK_NEAR(x, 1.0, 1e-9);
    CHECK_NEAR(y, 0.0, 1e-9);
}

static void test_rotate_offset_180_reverses(void)
{
    double x, y;
    geo_math_rotate_offset(180.0, 0.0, -1.0, &x, &y);
    CHECK_NEAR(x, 0.0, 1e-9);
    CHECK_NEAR(y, 1.0, 1e-9);
}

static void test_rotate_offset_preserves_magnitude(void)
{
    double x, y;
    geo_math_rotate_offset(37.0, 5.0, -12.0, &x, &y);
    double mag_in = sqrt(5.0 * 5.0 + 12.0 * 12.0);
    double mag_out = sqrt(x * x + y * y);
    CHECK_NEAR(mag_in, mag_out, 1e-9);
}

int main(void)
{
    test_distance_zero_for_same_point();
    test_distance_quarter_equator();
    test_distance_pole_to_pole();
    test_distance_symmetric();
    test_distance_never_negative();

    test_bearing_cardinal_directions();
    test_bearing_always_in_range();

    test_polar_to_screen_zero_distance_is_center();
    test_polar_to_screen_cardinal_bearings();
    test_polar_to_screen_clamps_beyond_range();
    test_polar_to_screen_zero_range_no_crash();

    test_rotate_offset_zero_heading_is_identity();
    test_rotate_offset_90_matches_polar_convention();
    test_rotate_offset_180_reverses();
    test_rotate_offset_preserves_magnitude();

    fprintf(stderr, "\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
