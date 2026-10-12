#pragma once
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>

// All live-tuneable parameters in one place.
// The settings panel reads/writes these directly.
struct Config {
    // Observer
    // Public demo location: San Francisco International Airport.
    // Personal settings belong in the ignored .env file.
    double lat_deg    = 37.6213;
    double lon_deg    = -122.3790;
    double alt_m      = 4.0;
    double heading_deg = 0.0;
    double pitch_deg   = 5.0;
    double fov_deg     = 70.0;

    // Fixed viewing direction; override locally for your installation.
    double room_heading_deg = 0.0;
    bool show_location = false;  // Hide exact coordinates in the normal HUD.

    // Rendering filters
    double min_alt_ft  = 0.0;
    double max_alt_ft  = 59500.0;
    double max_dist_km = 60.0;      // max horizontal render distance
    double min_dist_km = 5.0;       // min horizontal render distance

    // Rendering style
    float marker_size   = 0.025f;   // NDC diamond size
    float trail_opacity = 1.0f;     // trail line alpha multiplier
    float label_scale   = 1.0f;     // text scale multiplier
    float fade_start_s  = 15.0f;    // seconds before stale fade begins
    float fade_end_s    = 45.0f;    // seconds at full fade
    double trail_length_s = 190.0;  // history trail duration
    double predict_length_s = 50.0;  // predicted path duration

    // API
    double poll_interval_s = 10.0;  // seconds between API polls
    double flight_radius_nm = 50.0; // Query centered on the observer, wherever they are.

    // Projection
    float near_clip = 1.0f;
    float far_clip  = 500000.0f;

    // Sun/moon transit prediction
    double transit_lookahead_s = 600.0;   // look this far ahead for transits
    double transit_threshold_deg = 1.0;   // angular window that counts as a transit
};

struct SliderDef {
    const char* label;
    const char* unit;
    double min_val, max_val, step;
    const char* fmt;
    const char* desc;
};

static const SliderDef SLIDER_DEFS[] = {
    // Observer
    {"Latitude",      "deg", -90.0,    90.0,    0.0001, "%.5f",
     "Your GPS latitude. North/south position on Earth. 0.0001 = ~11m."},
    {"Longitude",     "deg", -180.0,   180.0,   0.0001, "%.5f",
     "Your GPS longitude. East/west position on Earth. 0.0001 = ~8m."},
    {"Elevation",     "m",  -500.0, 10000.0,    1.0,   "%.0f",
     "Eye height above sea level in meters. Ground + floor + eye height."},
    {"Heading",       "deg",   0.0,   360.0,    1.0,   "%.0f",
     "Compass direction you are looking. 0=N, 90=E, 180=S, 270=W."},
    {"Pitch",         "deg", -10.0,    89.0,    0.5,   "%.1f",
     "Vertical look angle. 0=horizon, positive=up, negative=below horizon."},
    {"FOV",           "deg",  10.0,   120.0,    1.0,   "%.0f",
     "Field of view. Human eye ~45. Lower=zoomed in, higher=wide angle."},
    // Filters
    {"Min Alt",       "ft",    0.0, 60000.0,  500.0,   "%.0f",
     "Hide planes below this altitude. Raise to filter ground clutter."},
    {"Max Alt",       "ft",    0.0, 60000.0,  500.0,   "%.0f",
     "Hide planes above this altitude. Lower to see only approach traffic."},
    {"Min Dist",      "km",    0.0,   500.0,    5.0,   "%.0f",
     "Hide planes closer than this. Useful to ignore very close helicopters."},
    {"Max Dist",      "km",    0.0,   500.0,    5.0,   "%.0f",
     "Hide planes farther than this. Lower to reduce clutter at the horizon."},
    // Style
    {"Marker Size",   "ndc",   0.005,   0.05,  0.001, "%.3f",
     "Base size of plane diamond markers. Scales with distance automatically."},
    {"Trail Alpha",   "",      0.0,     1.0,   0.05,  "%.2f",
     "Opacity of history trails behind planes. 0=off, 1=fully visible."},
    {"Label Scale",   "x",     0.5,     3.0,   0.1,   "%.1f",
     "Text size multiplier for plane callsign/altitude/speed labels."},
    {"Fade Start",    "s",     5.0,    60.0,   1.0,   "%.0f",
     "Seconds after last data before a plane starts fading out."},
    {"Fade End",      "s",    10.0,   120.0,   1.0,   "%.0f",
     "Seconds after last data when a plane fully disappears."},
    {"Trail Length",  "s",    10.0,   300.0,   10.0,  "%.0f",
     "How many seconds of history trail to show behind each plane."},
    {"Predict Length","s",    10.0,   300.0,   10.0,  "%.0f",
     "How many seconds of predicted path to show ahead of each plane."},
    // API
    {"Poll Interval", "s",     5.0,    60.0,   1.0,   "%.0f",
     "Seconds between API requests. Lower=fresher data, higher=less bandwidth."},
    {"Room Heading",  "deg",   0.0,   360.0,   1.0,   "%.0f",
     "Fixed compass bearing for the orange viewing-direction line."},
    // Transit prediction
    {"Transit Look",  "s",    60.0,  1800.0,  30.0,   "%.0f",
     "How far ahead to scan for sun/moon transits. Longer = more matches but more stale."},
    {"Transit Window","deg",   0.25,    5.0,   0.25,  "%.2f",
     "Angular separation that counts as a transit. 0.5 deg is sun/moon diameter."},
};

static const int NUM_SLIDERS = sizeof(SLIDER_DEFS) / sizeof(SLIDER_DEFS[0]);

// Pack all slider values into a flat double array for easy manipulation
inline void config_to_values(const Config& c, double* v) {
    v[0]  = c.lat_deg;
    v[1]  = c.lon_deg;
    v[2]  = c.alt_m;
    v[3]  = c.heading_deg;
    v[4]  = c.pitch_deg;
    v[5]  = c.fov_deg;
    v[6]  = c.min_alt_ft;
    v[7]  = c.max_alt_ft;
    v[8]  = c.min_dist_km;
    v[9]  = c.max_dist_km;
    v[10] = c.marker_size;
    v[11] = c.trail_opacity;
    v[12] = c.label_scale;
    v[13] = c.fade_start_s;
    v[14] = c.fade_end_s;
    v[15] = c.trail_length_s;
    v[16] = c.predict_length_s;
    v[17] = c.poll_interval_s;
    v[18] = c.room_heading_deg;
    v[19] = c.transit_lookahead_s;
    v[20] = c.transit_threshold_deg;
}

inline void values_to_config(const double* v, Config& c) {
    c.lat_deg        = v[0];
    c.lon_deg        = v[1];
    c.alt_m          = v[2];
    c.heading_deg    = v[3];
    c.pitch_deg      = v[4];
    c.fov_deg        = v[5];
    c.min_alt_ft     = v[6];
    c.max_alt_ft     = v[7];
    c.min_dist_km    = v[8];
    c.max_dist_km    = v[9];
    c.marker_size    = (float)v[10];
    c.trail_opacity  = (float)v[11];
    c.label_scale    = (float)v[12];
    c.fade_start_s   = (float)v[13];
    c.fade_end_s     = (float)v[14];
    c.trail_length_s  = v[15];
    c.predict_length_s = v[16];
    c.poll_interval_s = v[17];
    c.room_heading_deg = v[18];
    c.transit_lookahead_s = v[19];
    c.transit_threshold_deg = v[20];
}
