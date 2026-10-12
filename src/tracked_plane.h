#pragma once
#include "projection.h"
#include "landmarks.h"
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>

// A tracked plane with interpolation state.
// Each plane lives as long as the API reports it, plus a grace period.
// Between 10s API updates, we interpolate position using velocity + heading.
struct TrackedPlane {
    std::string icao24;
    std::string callsign;
    std::string ac_type;       // aircraft type (e.g., "B738")
    std::string registration;  // tail number (e.g., "N12345")
    std::string ac_operator;   // airline/operator name

    // Previous sample (for interpolation baseline)
    double prev_lat = 0, prev_lon = 0, prev_alt = 0;
    float prev_heading = 0;
    float prev_velocity = 0;
    double prev_time = 0; // monotonic seconds when prev sample arrived

    // Current target sample (latest from API)
    double target_lat = 0, target_lon = 0, target_alt = 0;
    float target_heading = 0;
    float target_velocity = 0;
    double target_time = 0; // monotonic seconds when this sample arrived

    // Interpolated state (what we render)
    double render_lat = 0, render_lon = 0, render_alt = 0;
    float render_heading = 0;
    float render_velocity = 0;
    float render_vert_rate = 0;  // ft/min (positive = ascending)
    float render_accel = 0;      // m/s² (positive = accelerating)

    // ENU position relative to observer (recomputed each frame)
    glm::dvec3 enu{0.0};

    bool on_ground = false;
    double last_seen = 0;     // monotonic time of last API report
    double first_seen = 0;    // when we first started tracking
    bool has_prev = false;    // do we have a previous sample for lerp?

    // Position history for trail rendering
    static constexpr int MAX_HISTORY = 60;
    struct HistoryPoint { double lat, lon, alt, time; };
    HistoryPoint history[MAX_HISTORY];
    int history_count = 0;
    int history_head = 0; // ring buffer write index
    double last_history_time = 0;

    void record_history(double now) {
        // Record a position every ~2 seconds
        if (now - last_history_time < 2.0) return;
        last_history_time = now;
        history[history_head] = {render_lat, render_lon, render_alt, now};
        history_head = (history_head + 1) % MAX_HISTORY;
        if (history_count < MAX_HISTORY) history_count++;
    }

    // Get history point by age index (0 = most recent, count-1 = oldest)
    HistoryPoint get_history(int age) const {
        int idx = (history_head - 1 - age + MAX_HISTORY * 2) % MAX_HISTORY;
        return history[idx];
    }

    // Update with a new API sample
    void push_sample(const AircraftPos& pos, double now) {
        // Shift current target into prev
        if (target_time > 0) {
            prev_lat = target_lat;
            prev_lon = target_lon;
            prev_alt = target_alt;
            prev_heading = target_heading;
            prev_velocity = target_velocity;
            prev_time = target_time;
            has_prev = true;
        }

        target_lat = pos.lat_deg;
        target_lon = pos.lon_deg;
        target_alt = pos.alt_m;
        target_heading = pos.heading_deg;
        target_velocity = pos.velocity_ms;
        target_time = now;
        on_ground = pos.on_ground;
        last_seen = now;

        if (!pos.callsign.empty())
            callsign = pos.callsign;
        if (!pos.ac_type.empty()) ac_type = pos.ac_type;
        if (!pos.registration.empty()) registration = pos.registration;
        if (!pos.ac_operator.empty()) ac_operator = pos.ac_operator;

        if (first_seen == 0)
            first_seen = now;
    }

    // Interpolate to produce smooth render state.
    // Between samples: lerp from prev→target over the expected interval.
    // After target (extrapolation): dead-reckon using velocity + heading.
    void interpolate(double now) {
        if (!has_prev || prev_time >= target_time) {
            // No interpolation data yet - use target directly,
            // but dead-reckon with velocity if we have it
            double dt = now - target_time;
            if (dt > 0 && target_velocity > 0) {
                dead_reckon(target_lat, target_lon, target_alt,
                            target_heading, target_velocity, dt);
            } else {
                render_lat = target_lat;
                render_lon = target_lon;
                render_alt = target_alt;
            }
            render_heading = target_heading;
            render_velocity = target_velocity;
            render_vert_rate = 0;
            render_accel = 0;
            return;
        }

        double interval = target_time - prev_time;
        double elapsed = now - prev_time;
        double t = elapsed / interval;

        if (t <= 1.0) {
            // Between samples: smooth hermite interpolation
            double s = smoothstep(t);
            render_lat = prev_lat + (target_lat - prev_lat) * s;
            render_lon = prev_lon + (target_lon - prev_lon) * s;
            render_alt = prev_alt + (target_alt - prev_alt) * s;
            render_heading = lerp_angle(prev_heading, target_heading, (float)s);
            render_velocity = prev_velocity + (target_velocity - prev_velocity) * (float)s;
        } else {
            // Past the target sample: dead-reckon from target
            double dt = now - target_time;
            dead_reckon(target_lat, target_lon, target_alt,
                        target_heading, target_velocity, dt);
            render_heading = target_heading;
            render_velocity = target_velocity;
        }

        // Vertical rate: altitude delta between samples, converted to ft/min
        double alt_delta_m = target_alt - prev_alt;
        render_vert_rate = (float)(alt_delta_m * 3.28084 * 60.0 / interval);

        // Acceleration: velocity delta between samples
        render_accel = (float)((target_velocity - prev_velocity) / interval);
    }

    // Call after interpolate() to log history
    void update_history(double now) {
        record_history(now);
    }

    // Landing detection
    bool was_airborne = false;   // was this plane in the air before?
    bool landed = false;         // did it just land?
    double landed_time = 0;      // when it landed
    std::string landed_airport;  // which airport

    void check_landing(double now) {
        if (on_ground && was_airborne && !landed) {
            landed = true;
            landed_time = now;
            double best_dist = 1e9;
            landed_airport = "???";
            for (int i = 0; i < NUM_LANDMARKS; i++) {
                double dlat = (render_lat - LANDMARKS[i].lat) * 111000.0;
                double dlon = (render_lon - LANDMARKS[i].lon) * 111000.0 * cos(glm::radians(render_lat));
                double d = sqrt(dlat*dlat + dlon*dlon);
                if (d < best_dist) {
                    best_dist = d;
                    landed_airport = LANDMARKS[i].name;
                }
            }
            if (best_dist > 5000.0) {
                landed = false;
                landed_airport.clear();
            }
        }
        if (!on_ground && render_alt > 100) {
            was_airborne = true;
            landed = false;
        }
    }

    // How long since we last got data for this plane
    double staleness(double now) const {
        return now - last_seen;
    }

private:
    static double smoothstep(double t) {
        t = std::max(0.0, std::min(1.0, t));
        return t * t * (3.0 - 2.0 * t);
    }

    static float lerp_angle(float a, float b, float t) {
        // Shortest-path angular interpolation
        float diff = b - a;
        while (diff > 180.0f) diff -= 360.0f;
        while (diff < -180.0f) diff += 360.0f;
        float result = a + diff * t;
        while (result < 0) result += 360.0f;
        while (result >= 360.0f) result -= 360.0f;
        return result;
    }

    void dead_reckon(double base_lat, double base_lon, double base_alt,
                     float heading, float velocity, double dt) {
        // Project forward using ground speed + heading
        // velocity is m/s, heading is compass degrees
        double heading_rad = glm::radians((double)heading);
        double dist = velocity * dt; // meters

        // Convert to lat/lon displacement
        double dlat_m = dist * cos(heading_rad);
        double dlon_m = dist * sin(heading_rad);

        render_lat = base_lat + (dlat_m / EARTH_RADIUS_M) * (180.0 / M_PI);
        render_lon = base_lon + (dlon_m / (EARTH_RADIUS_M * cos(glm::radians(base_lat)))) * (180.0 / M_PI);
        render_alt = base_alt; // altitude stays constant for dead reckoning
    }
};

// Manages the set of all tracked planes. Handles matching incoming API data
// to existing planes by ICAO24, creating new trackers, and garbage-collecting
// stale ones.
class PlaneTracker {
public:
    // Ingest a batch of raw API positions. Matches by icao24.
    void update(const std::vector<AircraftPos>& raw_positions, double now) {
        // Update existing or create new
        for (const auto& pos : raw_positions) {
            auto it = planes_.find(pos.icao24);
            if (it != planes_.end()) {
                it->second.push_sample(pos, now);
            } else {
                TrackedPlane plane;
                plane.icao24 = pos.icao24;
                plane.push_sample(pos, now);
                planes_[pos.icao24] = std::move(plane);
            }
        }

        // Garbage collect planes not seen for >60s
        for (auto it = planes_.begin(); it != planes_.end(); ) {
            if (it->second.staleness(now) > 60.0) {
                it = planes_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Interpolate all planes and compute ENU positions for this frame
    void tick(double now, const ObserverConfig& observer) {
        for (auto& [id, plane] : planes_) {
            plane.interpolate(now);
            plane.update_history(now);
            plane.check_landing(now);
            plane.enu = geodetic_to_enu(
                plane.render_lat, plane.render_lon, plane.render_alt,
                observer.lat_deg, observer.lon_deg, observer.alt_m);
        }
    }

    // Get all currently tracked planes (including fading ones)
    const std::unordered_map<std::string, TrackedPlane>& planes() const {
        return planes_;
    }

    int active_count() const { return (int)planes_.size(); }

private:
    std::unordered_map<std::string, TrackedPlane> planes_;
};
