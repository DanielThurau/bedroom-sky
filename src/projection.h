#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

// WGS84 constants
constexpr double EARTH_RADIUS_M = 6371000.0;

struct ObserverConfig {
    double lat_deg;       // observer latitude
    double lon_deg;       // observer longitude
    double alt_m;         // observer altitude above sea level (meters)
    double heading_deg;   // compass heading we're looking toward (e.g. ~150 for SFO from SF)
    double pitch_deg;     // angle above horizon (0 = horizon, 90 = zenith)
    double fov_deg;       // field of view (your window's apparent FOV)
    // Local eye offset from the GPS coordinate origin, in ENU meters
    // Used to move POV within the room without changing GPS coords
    double eye_offset_east = 0;
    double eye_offset_north = 0;
    double eye_offset_up = 0;
};

struct AircraftPos {
    double lat_deg;
    double lon_deg;
    double alt_m;       // geometric altitude in meters
    float heading_deg;
    std::string callsign;
    std::string icao24;
    float velocity_ms;  // ground speed m/s
    bool on_ground;

    // Database-enriched fields (may be empty if API doesn't provide them)
    std::string ac_type;       // ICAO type code (e.g., "B738")
    std::string registration;  // tail number (e.g., "N12345")
    std::string ac_operator;   // airline/operator name
};

// Convert geodetic (lat/lon/alt) to ENU (East-North-Up) relative to observer
inline glm::dvec3 geodetic_to_enu(double lat, double lon, double alt,
                                   double ref_lat, double ref_lon, double ref_alt) {
    double lat_r = glm::radians(lat);
    double lon_r = glm::radians(lon);
    double ref_lat_r = glm::radians(ref_lat);
    double ref_lon_r = glm::radians(ref_lon);

    double dlat = lat_r - ref_lat_r;
    double dlon = lon_r - ref_lon_r;

    double east  = (EARTH_RADIUS_M + alt) * dlon * cos(ref_lat_r);
    double north = (EARTH_RADIUS_M + alt) * dlat;
    double up    = alt - ref_alt;

    return glm::dvec3(east, north, up);
}

// Build view matrix from observer config
// We look along the heading direction at the given pitch
inline glm::mat4 build_view_matrix(const ObserverConfig& obs) {
    // In ENU, heading 0=North(+Y), 90=East(+X)
    double heading_rad = glm::radians(obs.heading_deg);
    double pitch_rad = glm::radians(obs.pitch_deg);

    // Direction vector in ENU
    glm::vec3 dir(
        sin(heading_rad) * cos(pitch_rad),  // east component
        cos(heading_rad) * cos(pitch_rad),  // north component
        sin(pitch_rad)                       // up component
    );

    glm::vec3 eye((float)obs.eye_offset_east, (float)obs.eye_offset_north, (float)obs.eye_offset_up);
    glm::vec3 up(0.0f, 0.0f, 1.0f);

    // Handle edge case where dir is parallel to up
    if (std::abs(obs.pitch_deg - 90.0) < 0.01) {
        up = glm::vec3(-sin(heading_rad), -cos(heading_rad), 0.0f);
    }

    return glm::lookAt(eye, eye + dir, up);
}
