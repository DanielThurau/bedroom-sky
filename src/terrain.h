#pragma once
#include "projection.h"
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>

// Fetches terrain elevation data and builds a renderable mesh.
// Uses Open-Elevation API for a grid of points around the observer.
class Terrain {
public:
    struct GridPoint {
        double lat, lon, elevation;
        glm::vec3 enu; // computed relative to observer
    };

    Terrain() = default;
    ~Terrain() { if (thread_.joinable()) thread_.join(); }

    // Fetch elevation grid in background thread
    void fetch_async(double center_lat, double center_lon, double radius_km, int grid_size);

    // Re-fetch terrain if observer moved > 1km from fetch center (call from main loop)
    void refetch_if_needed(double lat, double lon, double radius_km, int grid_size);

    // Recompute ENU positions relative to observer (call each frame or when observer moves)
    void update_enu(const ObserverConfig& observer);

    // Get renderable data (thread-safe)
    bool is_ready() const { return ready_.load(); }
    bool is_loading() const { return loading_.load(); }

    int grid_w() const { return grid_w_; }
    int grid_h() const { return grid_h_; }
    const std::vector<GridPoint>& points() const { return points_; }

    // Sample ground elevation at a lat/lon by bilinear interpolation of the grid.
    // Returns 0 if terrain not ready or point is outside grid.
    double sample_elevation(double lat, double lon) const;

    // Get triangle mesh for rendering (indices into points array)
    // Each triangle = 3 indices
    const std::vector<int>& triangles() const { return triangles_; }
    const std::vector<bool>& visible() const { return visible_; }
    // Pre-filtered triangle indices (only visible skyline triangles)
    const std::vector<int>& visible_triangles() const { return visible_tris_; }
    uint32_t generation() const { return generation_; }


private:
    void fetch_thread(double center_lat, double center_lon, double radius_km, int grid_size);
    void build_mesh();
    void compute_visibility();
    bool load_cache(double center_lat, double center_lon, double radius_km, int grid_size);
    void save_cache(double center_lat, double center_lon, double radius_km, int grid_size);

    std::vector<GridPoint> points_;
    std::vector<int> triangles_;
    std::vector<bool> visible_;
    std::vector<int> visible_tris_;
    uint32_t generation_ = 0;
    int grid_w_ = 0, grid_h_ = 0;
    double last_obs_lat_ = 1e99, last_obs_lon_ = 1e99, last_obs_alt_ = 1e99;
    double fetch_center_lat_ = 1e99, fetch_center_lon_ = 1e99;

    std::atomic<bool> ready_{false};
    std::atomic<bool> loading_{false};
    std::thread thread_;
    std::mutex mutex_;
};
