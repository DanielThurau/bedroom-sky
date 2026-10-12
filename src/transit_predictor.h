#pragma once
#include "tracked_plane.h"
#include "projection.h"
#include <string>
#include <vector>
#include <unordered_map>

// Predicts upcoming sun/moon transits — when a plane is about to pass
// angularly in front of the sun or moon as seen from the observer.
//
// update() samples each plane's dead-reckoned future over the next
// `lookahead_s` seconds, computes the angular separation between the plane
// and each celestial body at that future moment, and records a transit if
// the minimum separation falls within `threshold_deg`.
//
// Results are cached between updates; call update() at whatever cadence
// the caller prefers (every 2s is plenty).

enum class CelestialBody {
    Sun,
    Moon,
};

struct Transit {
    std::string plane_id;       // icao24
    std::string callsign;
    CelestialBody body;
    double time_to_transit_s;   // seconds from update_utc_s
    double closest_sep_deg;     // minimum angular separation (deg)
    double body_az_deg;         // body az at transit moment
    double body_el_deg;         // body el at transit moment
};

class TransitPredictor {
public:
    // Compute upcoming transits for every plane.
    //   planes:        current tracked planes
    //   observer:      current observer pose
    //   utc_unix_s:    UTC time "now" in Unix seconds
    //   lookahead_s:   how far into the future to look
    //   threshold_deg: angular separation below this counts as a transit
    //   step_s:        temporal resolution of the search (smaller = sharper)
    void update(const std::unordered_map<std::string, TrackedPlane>& planes,
                const ObserverConfig& observer,
                double utc_unix_s,
                double lookahead_s,
                double threshold_deg,
                double step_s = 2.0);

    const std::vector<Transit>& transits() const { return transits_; }

    // Look up the soonest transit for a specific plane, if any.
    // Returns nullptr if the plane has no pending transit.
    const Transit* transit_for(const std::string& icao24) const;

private:
    std::vector<Transit> transits_;
};
