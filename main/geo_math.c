#include "geo_math.h"

#include <math.h>

// Not M_PI: that's a GNU/POSIX math.h extension gated behind feature-test
// macros (_DEFAULT_SOURCE, _USE_MATH_DEFINES, etc.) that aren't reliably
// defined across every toolchain this file gets compiled with (ESP-IDF's
// newlib vs. a host gcc/clang under -std=c11 for test/native/) - defining it
// ourselves avoids depending on which of those happens to be set.
#define GEO_MATH_PI 3.14159265358979323846
#define EARTH_RADIUS_NM 3440.065
#define DEG_TO_RAD(d) ((d) * GEO_MATH_PI / 180.0)
#define RAD_TO_DEG(r) ((r) * 180.0 / GEO_MATH_PI)

double geo_math_distance_nm(double lat1, double lon1, double lat2, double lon2)
{
    double phi1 = DEG_TO_RAD(lat1);
    double phi2 = DEG_TO_RAD(lat2);
    double dphi = DEG_TO_RAD(lat2 - lat1);
    double dlambda = DEG_TO_RAD(lon2 - lon1);

    double a = sin(dphi / 2) * sin(dphi / 2) +
               cos(phi1) * cos(phi2) * sin(dlambda / 2) * sin(dlambda / 2);
    double c = 2 * atan2(sqrt(a), sqrt(1 - a));

    return EARTH_RADIUS_NM * c;
}

double geo_math_bearing_deg(double lat1, double lon1, double lat2, double lon2)
{
    double phi1 = DEG_TO_RAD(lat1);
    double phi2 = DEG_TO_RAD(lat2);
    double dlambda = DEG_TO_RAD(lon2 - lon1);

    double y = sin(dlambda) * cos(phi2);
    double x = cos(phi1) * sin(phi2) - sin(phi1) * cos(phi2) * cos(dlambda);
    double theta = atan2(y, x);

    double bearing = fmod(RAD_TO_DEG(theta) + 360.0, 360.0);
    return bearing;
}

void geo_math_polar_to_screen(double bearing_deg, double dist_nm, double range_nm,
                               int cx, int cy, int radius_px,
                               int *out_x, int *out_y)
{
    if (dist_nm > range_nm) {
        dist_nm = range_nm;
    }

    double r_px = (range_nm > 0.0) ? (dist_nm / range_nm) * radius_px : 0.0;
    double angle_rad = DEG_TO_RAD(bearing_deg);

    *out_x = cx + (int)lround(r_px * sin(angle_rad));
    *out_y = cy - (int)lround(r_px * cos(angle_rad));
}

void geo_math_rotate_offset(double heading_deg, double lx, double ly,
                             double *out_x, double *out_y)
{
    double theta = DEG_TO_RAD(heading_deg);
    *out_x = lx * cos(theta) - ly * sin(theta);
    *out_y = lx * sin(theta) + ly * cos(theta);
}
