#ifndef GEO_MATH_H
#define GEO_MATH_H

// Great-circle distance between two lat/lon points, in nautical miles.
double geo_math_distance_nm(double lat1, double lon1, double lat2, double lon2);

// Initial compass bearing from point 1 to point 2, in degrees [0, 360).
double geo_math_bearing_deg(double lat1, double lon1, double lat2, double lon2);

// Projects a bearing/distance pair onto a north-up screen, centered at (cx, cy)
// with the given pixel radius representing range_nm. Output coordinates may
// fall outside the screen bounds (or be negative) for targets near the edge
// of the configured range - callers pass them through to GUI_Paint as-is,
// whose own bounds checks silently drop off-screen points.
void geo_math_polar_to_screen(double bearing_deg, double dist_nm, double range_nm,
                               int cx, int cy, int radius_px,
                               int *out_x, int *out_y);

// Rotates a local offset (lx, ly), where (0,-1) means "pointing north/up",
// clockwise by heading_deg - used to orient a small screen-space shape (e.g.
// a heading arrow) drawn around a point. Matches the same north-up, clockwise
// convention as geo_math_polar_to_screen.
void geo_math_rotate_offset(double heading_deg, double lx, double ly,
                             double *out_x, double *out_y);

#endif
