#include "cache_paths.h"
#include "terrain.h"
#include "json_helpers.h"
#include <curl/curl.h>
#include <sstream>
#include <iostream>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <fstream>

static double parse_elevation(const std::string& json, size_t& pos) {
    // Find next "elevation": value
    size_t ep = json.find("\"elevation\":", pos);
    if (ep == std::string::npos) return 0;
    ep += 12;
    while (ep < json.size() && (json[ep] == ' ' || json[ep] == ':')) ep++;
    size_t end = ep;
    while (end < json.size() && json[end] != ',' && json[end] != '}') end++;
    pos = end;
    try { return std::stod(json.substr(ep, end - ep)); } catch (...) { return 0; }
}

void Terrain::fetch_async(double center_lat, double center_lon, double radius_km, int grid_size) {
    if (loading_.load()) return;

    fetch_center_lat_ = center_lat;
    fetch_center_lon_ = center_lon;
    last_obs_lat_ = 1e99; // force ENU recompute for new data

    // Try disk cache first — instant load if center is within 1km
    if (load_cache(center_lat, center_lon, radius_km, grid_size)) {
        return;
    }

    // Cache miss — fetch from API in background
    loading_ = true;
    if (thread_.joinable()) thread_.join();
    thread_ = std::thread(&Terrain::fetch_thread, this, center_lat, center_lon, radius_km, grid_size);
}

void Terrain::refetch_if_needed(double lat, double lon, double radius_km, int grid_size) {
    if (loading_.load()) return;
    if (fetch_center_lat_ > 1e98) return; // haven't fetched yet

    double dlat_km = (lat - fetch_center_lat_) * 111.0;
    double dlon_km = (lon - fetch_center_lon_) * 111.0 * cos(glm::radians(lat));
    if (dlat_km * dlat_km + dlon_km * dlon_km < 1.0) return; // within 1km

    std::cout << "Observer moved >1km from terrain center, re-fetching..." << std::endl;
    ready_ = false;
    fetch_async(lat, lon, radius_km, grid_size);
}

bool Terrain::load_cache(double center_lat, double center_lon, double radius_km, int grid_size) {
    std::ifstream f(cache_path("terrain_cache.bin"), std::ios::binary);
    if (!f) return false;

    double cached_lat, cached_lon, cached_radius;
    int32_t cached_size;
    f.read(reinterpret_cast<char*>(&cached_lat), sizeof(double));
    f.read(reinterpret_cast<char*>(&cached_lon), sizeof(double));
    f.read(reinterpret_cast<char*>(&cached_radius), sizeof(double));
    f.read(reinterpret_cast<char*>(&cached_size), sizeof(int32_t));
    if (!f) return false;

    // Must match grid size and radius
    if (cached_size != grid_size || cached_radius != radius_km) return false;

    // Center must be within 1km
    double dlat_km = (center_lat - cached_lat) * 111.0;
    double dlon_km = (center_lon - cached_lon) * 111.0 * cos(glm::radians(center_lat));
    if (dlat_km * dlat_km + dlon_km * dlon_km > 1.0) return false;

    int total = cached_size * cached_size;
    std::vector<GridPoint> grid(total);
    for (int i = 0; i < total; i++) {
        f.read(reinterpret_cast<char*>(&grid[i].lat), sizeof(double));
        f.read(reinterpret_cast<char*>(&grid[i].lon), sizeof(double));
        f.read(reinterpret_cast<char*>(&grid[i].elevation), sizeof(double));
        grid[i].enu = glm::vec3(0);
    }
    if (!f) return false;

    points_ = std::move(grid);
    grid_w_ = cached_size;
    grid_h_ = cached_size;
    build_mesh();
    ready_ = true;

    std::cout << "Terrain loaded from cache (" << grid_w_ << "x" << grid_h_
              << ", " << triangles_.size() / 3 << " triangles)" << std::endl;
    return true;
}

void Terrain::save_cache(double center_lat, double center_lon, double radius_km, int grid_size) {
    auto f = open_private_cache("terrain_cache.bin");
    if (!f) return;

    int32_t size = grid_size;
    f.write(reinterpret_cast<const char*>(&center_lat), sizeof(double));
    f.write(reinterpret_cast<const char*>(&center_lon), sizeof(double));
    f.write(reinterpret_cast<const char*>(&radius_km), sizeof(double));
    f.write(reinterpret_cast<const char*>(&size), sizeof(int32_t));

    for (const auto& pt : points_) {
        f.write(reinterpret_cast<const char*>(&pt.lat), sizeof(double));
        f.write(reinterpret_cast<const char*>(&pt.lon), sizeof(double));
        f.write(reinterpret_cast<const char*>(&pt.elevation), sizeof(double));
    }
    std::cout << "Terrain saved to cache" << std::endl;
}

void Terrain::fetch_thread(double center_lat, double center_lon, double radius_km, int grid_size) {
    std::cout << "Fetching terrain data (" << grid_size << "x" << grid_size
              << " grid, " << radius_km << "km radius)..." << std::endl;

    // Build grid of lat/lon points
    double deg_per_km_lat = 1.0 / 111.0;
    double deg_per_km_lon = 1.0 / (111.0 * cos(glm::radians(center_lat)));

    double lat_min = center_lat - radius_km * deg_per_km_lat;
    double lat_max = center_lat + radius_km * deg_per_km_lat;
    double lon_min = center_lon - radius_km * deg_per_km_lon;
    double lon_max = center_lon + radius_km * deg_per_km_lon;

    std::vector<GridPoint> grid;
    grid.resize(grid_size * grid_size);

    for (int y = 0; y < grid_size; y++) {
        for (int x = 0; x < grid_size; x++) {
            double lat = lat_min + (lat_max - lat_min) * y / (grid_size - 1);
            double lon = lon_min + (lon_max - lon_min) * x / (grid_size - 1);
            grid[y * grid_size + x] = {lat, lon, 0.0, glm::vec3(0)};
        }
    }

    // Fetch elevations in batches (API limit ~200 points per request)
    int batch_size = 100;
    int total = grid_size * grid_size;

    for (int start = 0; start < total; start += batch_size) {
        int end = std::min(start + batch_size, total);

        // Build JSON body
        std::ostringstream body;
        body << "{\"locations\":[";
        for (int i = start; i < end; i++) {
            if (i > start) body << ",";
            body << "{\"latitude\":" << grid[i].lat
                 << ",\"longitude\":" << grid[i].lon << "}";
        }
        body << "]}";

        CURL* curl = curl_easy_init();
        if (!curl) continue;

        std::string response;
        std::string body_str = body.str();
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, "https://api.open-elevation.com/api/v1/lookup");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            std::cerr << "Terrain fetch failed for batch " << start << ": "
                      << curl_easy_strerror(res) << std::endl;
            continue;
        }

        // Parse elevations from response
        size_t parse_pos = 0;
        for (int i = start; i < end; i++) {
            grid[i].elevation = parse_elevation(response, parse_pos);
        }

        std::cout << "  Terrain batch " << end << "/" << total << std::endl;
    }

    // Store results
    {
        std::lock_guard<std::mutex> lock(mutex_);
        points_ = std::move(grid);
        grid_w_ = grid_size;
        grid_h_ = grid_size;
        build_mesh();
    }

    save_cache(center_lat, center_lon, radius_km, grid_size);
    ready_ = true;
    loading_ = false;
    std::cout << "Terrain loaded: " << grid_w_ << "x" << grid_h_
              << " (" << triangles_.size() / 3 << " triangles)" << std::endl;
}

void Terrain::build_mesh() {
    triangles_.clear();

    int w = grid_w_, h = grid_h_;
    if (w < 2 || h < 2) return;

    // Build triangle strip grid
    for (int y = 0; y < h - 1; y++) {
        for (int x = 0; x < w - 1; x++) {
            int i00 = y * w + x;
            int i10 = y * w + x + 1;
            int i01 = (y + 1) * w + x;
            int i11 = (y + 1) * w + x + 1;

            triangles_.push_back(i00);
            triangles_.push_back(i10);
            triangles_.push_back(i01);

            triangles_.push_back(i10);
            triangles_.push_back(i11);
            triangles_.push_back(i01);
        }
    }
}

void Terrain::update_enu(const ObserverConfig& observer) {
    // Only recompute when observer geodetic position actually changes
    if (std::abs(observer.lat_deg - last_obs_lat_) < 1e-9 &&
        std::abs(observer.lon_deg - last_obs_lon_) < 1e-9 &&
        std::abs(observer.alt_m - last_obs_alt_) < 0.01) {
        return;
    }
    last_obs_lat_ = observer.lat_deg;
    last_obs_lon_ = observer.lon_deg;
    last_obs_alt_ = observer.alt_m;

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& pt : points_) {
        pt.enu = glm::vec3(geodetic_to_enu(pt.lat, pt.lon, pt.elevation,
                                            observer.lat_deg, observer.lon_deg, observer.alt_m));
    }
    compute_visibility();
}

void Terrain::compute_visibility() {
    int n = (int)points_.size();
    visible_.assign(n, false);
    if (n == 0) return;

    // For each point, compute azimuth, elevation angle, and distance from observer
    struct RayInfo {
        int index;
        float elev_angle;
        float dist;
    };

    // Bin points by azimuth direction (1-degree bins)
    const int NUM_BINS = 360;
    std::vector<std::vector<RayInfo>> bins(NUM_BINS);

    for (int i = 0; i < n; i++) {
        float e = points_[i].enu.x;
        float nr = points_[i].enu.y;
        float u = points_[i].enu.z;
        float hdist = std::sqrt(e * e + nr * nr);
        if (hdist < 1.0f) continue; // skip point at observer

        float azimuth_deg = std::atan2(e, nr) * (180.0f / (float)M_PI);
        if (azimuth_deg < 0) azimuth_deg += 360.0f;
        int bin = std::min((int)azimuth_deg, NUM_BINS - 1);

        bins[bin].push_back({i, std::atan2(u, hdist), hdist});
    }

    // Sweep each azimuth bin from near to far.
    // A point is visible if its elevation angle exceeds the max of all closer points.
    // Starting max at 0 means only terrain above the geometric horizon is shown.
    for (auto& bin : bins) {
        std::sort(bin.begin(), bin.end(), [](const RayInfo& a, const RayInfo& b) {
            return a.dist < b.dist;
        });
        float max_elev = 0.0f;
        for (auto& r : bin) {
            if (r.elev_angle >= max_elev) {
                visible_[r.index] = true;
                max_elev = r.elev_angle;
            }
        }
    }

    // Build compact list of only visible triangles
    visible_tris_.clear();
    for (size_t i = 0; i + 2 < triangles_.size(); i += 3) {
        if (visible_[triangles_[i]] || visible_[triangles_[i+1]] || visible_[triangles_[i+2]]) {
            visible_tris_.push_back(triangles_[i]);
            visible_tris_.push_back(triangles_[i+1]);
            visible_tris_.push_back(triangles_[i+2]);
        }
    }
    generation_++;
}

double Terrain::sample_elevation(double lat, double lon) const {
    if (!ready_.load() || grid_w_ < 2 || grid_h_ < 2 || points_.empty()) return 0.0;

    // Find grid bounds from corner points
    double lat_min = points_[0].lat;
    double lat_max = points_[(grid_h_ - 1) * grid_w_].lat;
    double lon_min = points_[0].lon;
    double lon_max = points_[grid_w_ - 1].lon;

    // Normalize to grid coordinates [0, grid_size-1]
    double fy = (lat - lat_min) / (lat_max - lat_min) * (grid_h_ - 1);
    double fx = (lon - lon_min) / (lon_max - lon_min) * (grid_w_ - 1);

    // Clamp to grid
    if (fx < 0) fx = 0; if (fx > grid_w_ - 1.001) fx = grid_w_ - 1.001;
    if (fy < 0) fy = 0; if (fy > grid_h_ - 1.001) fy = grid_h_ - 1.001;

    int ix = (int)fx, iy = (int)fy;
    double tx = fx - ix, ty = fy - iy;

    // Bilinear interpolation
    double e00 = points_[iy * grid_w_ + ix].elevation;
    double e10 = points_[iy * grid_w_ + ix + 1].elevation;
    double e01 = points_[(iy + 1) * grid_w_ + ix].elevation;
    double e11 = points_[(iy + 1) * grid_w_ + ix + 1].elevation;

    return e00 * (1 - tx) * (1 - ty) + e10 * tx * (1 - ty) +
           e01 * (1 - tx) * ty + e11 * tx * ty;
}
