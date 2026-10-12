#pragma once
#include "projection.h"
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>

// Fetches live ADS-B data from airplanes.live API
// Public API; requires no authentication.
class FlightTracker {
public:
    FlightTracker();
    ~FlightTracker();

    // Set query center and radius before starting the polling thread.
    void set_area(double latitude, double longitude, double radius_nm);

    // Start background polling thread
    void start(double poll_interval_sec = 5.0);
    void stop();

    // Get current snapshot (thread-safe copy)
    std::vector<AircraftPos> get_aircraft() const;

    // Stats
    int last_aircraft_count() const { return aircraft_count_.load(); }
    double last_fetch_time() const { return last_fetch_time_.load(); }

private:
    void poll_loop(double interval);
    std::vector<AircraftPos> fetch_opensky();

    double center_lat_ = 0.0, center_lon_ = 0.0;
    double radius_nm_ = 50.0;

    mutable std::mutex mutex_;
    std::vector<AircraftPos> aircraft_;
    std::atomic<int> aircraft_count_{0};
    std::atomic<double> last_fetch_time_{0.0};

    std::thread thread_;
    std::atomic<bool> running_{false};
};
