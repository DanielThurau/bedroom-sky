#pragma once
#include "projection.h"
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <utility>

class Terrain;

class Streets {
public:
    struct Street {
        std::vector<std::pair<double, double>> points; // (lat, lon) polyline
        std::string name;
        int rank; // 0=primary, 1=secondary, 2=tertiary

        // Computed ENU
        std::vector<glm::vec3> enu_points;
    };

    Streets() = default;
    ~Streets() { if (thread_.joinable()) thread_.join(); }

    void fetch_async(double center_lat, double center_lon, double radius_km);
    void refetch_if_needed(double lat, double lon, double radius_km);
    void update_enu(const ObserverConfig& observer, const Terrain* terrain = nullptr);

    bool is_ready() const { return ready_.load(); }
    bool is_loading() const { return loading_.load(); }

    const std::vector<Street>& streets() const { return streets_; }
    uint32_t generation() const { return generation_; }

private:
    void fetch_thread(double center_lat, double center_lon, double radius_km);
    void parse_overpass(const std::string& json);
    bool load_cache(double center_lat, double center_lon, double radius_km);
    void save_cache(double center_lat, double center_lon, double radius_km);

    std::vector<Street> streets_;
    uint32_t generation_ = 0;

    double last_obs_lat_ = 1e99, last_obs_lon_ = 1e99, last_obs_alt_ = 1e99;
    double fetch_center_lat_ = 1e99, fetch_center_lon_ = 1e99;
    double fetch_radius_km_ = 0;

    std::atomic<bool> ready_{false};
    std::atomic<bool> loading_{false};
    std::thread thread_;
    std::mutex mutex_;
};
