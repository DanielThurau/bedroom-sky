#pragma once

struct LandmarkDef {
    const char* name;
    double lat, lon, alt_m;
};

static constexpr LandmarkDef LANDMARKS[] = {
    {"SFO", 37.6213, -122.3790,  4.0},
    {"OAK", 37.7213, -122.2208,  2.0},
    {"SJC", 37.3626, -121.9291, 18.0},
};

static constexpr int NUM_LANDMARKS = sizeof(LANDMARKS) / sizeof(LANDMARKS[0]);
