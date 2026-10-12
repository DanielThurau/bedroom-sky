#include "cache_paths.h"
#include "buildings.h"
#include "terrain.h"
#include "json_helpers.h"
#include <sstream>
#include <cmath>
#include <fstream>

void Buildings::parse_overpass(const std::string& json) {
    auto elems = json_find_elements(json);
    if (elems.empty()) {
        std::cerr << "Buildings: no elements in response ("
                  << json.size() << " bytes; response omitted for privacy)" << std::endl;
        return;
    }

    std::cout << "  Parsing " << elems.size() << " elements..." << std::endl;

    std::vector<Building> buildings;
    for (auto& [s, e] : elems) {
        std::string type = json_value(json, s, e, "type");
        if (type != "way") continue;

        if (json.find("\"building\"", s) >= e) continue;

        auto footprint = json_parse_geometry(json, s, e);
        if (footprint.size() < 3) continue;

        if (footprint.front() == footprint.back()) footprint.pop_back();
        if (footprint.size() < 3) continue;

        double height = 10.0;
        std::string h_str = json_value(json, s, e, "height");
        if (!h_str.empty()) {
            try { height = std::stod(h_str); } catch (...) {}
        } else {
            std::string levels_str = json_value(json, s, e, "building:levels");
            if (!levels_str.empty()) {
                try { height = std::stod(levels_str) * 3.0; } catch (...) {}
            }
        }

        buildings.push_back({std::move(footprint), height, {}, 0.0f});
    }

    buildings_ = std::move(buildings);
    std::cout << "Buildings parsed: " << buildings_.size() << " buildings" << std::endl;
}

void Buildings::fetch_thread(double center_lat, double center_lon, double radius_km) {
    double deg_per_km_lat = 1.0 / 111.0;
    double deg_per_km_lon = 1.0 / (111.0 * cos(glm::radians(center_lat)));

    double lat_min = center_lat - radius_km * deg_per_km_lat;
    double lat_max = center_lat + radius_km * deg_per_km_lat;
    double lon_min = center_lon - radius_km * deg_per_km_lon;
    double lon_max = center_lon + radius_km * deg_per_km_lon;

    std::ostringstream query;
    query << "[out:json][timeout:120];"
          << "way[\"building\"]("
          << lat_min << "," << lon_min << "," << lat_max << "," << lon_max
          << ");out body geom qt;";

    std::cout << "Fetching buildings (" << radius_km << "km radius)..." << std::endl;

    std::string response = overpass_fetch(query.str(), "Buildings");
    if (response.empty()) {
        loading_ = false;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        parse_overpass(response);
    }

    if (buildings_.empty()) {
        std::cerr << "Buildings: parsed 0 buildings, not caching" << std::endl;
        loading_ = false;
        return;
    }

    save_cache(center_lat, center_lon, radius_km);
    ready_ = true;
    loading_ = false;

    std::cout << "Buildings ready: " << buildings_.size() << " buildings" << std::endl;
}

void Buildings::fetch_async(double center_lat, double center_lon, double radius_km) {
    if (loading_.load()) return;

    fetch_center_lat_ = center_lat;
    fetch_center_lon_ = center_lon;
    fetch_radius_km_ = radius_km;
    last_obs_lat_ = 1e99;

    if (load_cache(center_lat, center_lon, radius_km)) {
        return;
    }

    loading_ = true;
    if (thread_.joinable()) thread_.join();
    thread_ = std::thread(&Buildings::fetch_thread, this, center_lat, center_lon, radius_km);
}

void Buildings::refetch_if_needed(double lat, double lon, double radius_km) {
    if (loading_.load()) return;
    if (fetch_center_lat_ > 1e98) return;

    double dlat_km = (lat - fetch_center_lat_) * 111.0;
    double dlon_km = (lon - fetch_center_lon_) * 111.0 * cos(glm::radians(lat));
    if (dlat_km * dlat_km + dlon_km * dlon_km < 1.0) return;

    std::cout << "Observer moved >1km, re-fetching buildings..." << std::endl;
    ready_ = false;
    fetch_async(lat, lon, radius_km);
}

void Buildings::update_enu(const ObserverConfig& observer, const Terrain* terrain) {
    if (std::abs(observer.lat_deg - last_obs_lat_) < 1e-9 &&
        std::abs(observer.lon_deg - last_obs_lon_) < 1e-9 &&
        std::abs(observer.alt_m - last_obs_alt_) < 0.01) {
        return;
    }
    last_obs_lat_ = observer.lat_deg;
    last_obs_lon_ = observer.lon_deg;
    last_obs_alt_ = observer.alt_m;

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& b : buildings_) {
        b.enu_base.resize(b.footprint.size());
        for (size_t i = 0; i < b.footprint.size(); i++) {
            auto [lat, lon] = b.footprint[i];
            double elev = terrain ? terrain->sample_elevation(lat, lon) : 0.0;
            b.enu_base[i] = glm::vec3(geodetic_to_enu(lat, lon, elev,
                observer.lat_deg, observer.lon_deg, observer.alt_m));
        }
        b.enu_height = (float)b.height_m;
    }
    generation_++;
}

bool Buildings::load_cache(double center_lat, double center_lon, double radius_km) {
    std::ifstream f(cache_path("buildings_cache.bin"), std::ios::binary);
    if (!f) return false;

    double cached_lat, cached_lon, cached_radius;
    int32_t num_buildings;
    f.read(reinterpret_cast<char*>(&cached_lat), sizeof(double));
    f.read(reinterpret_cast<char*>(&cached_lon), sizeof(double));
    f.read(reinterpret_cast<char*>(&cached_radius), sizeof(double));
    f.read(reinterpret_cast<char*>(&num_buildings), sizeof(int32_t));
    if (!f) return false;

    if (cached_radius != radius_km) return false;

    double dlat_km = (center_lat - cached_lat) * 111.0;
    double dlon_km = (center_lon - cached_lon) * 111.0 * cos(glm::radians(center_lat));
    if (dlat_km * dlat_km + dlon_km * dlon_km > 1.0) return false;

    std::vector<Building> buildings(num_buildings);
    for (int i = 0; i < num_buildings; i++) {
        double height;
        int32_t num_verts;
        f.read(reinterpret_cast<char*>(&height), sizeof(double));
        f.read(reinterpret_cast<char*>(&num_verts), sizeof(int32_t));
        if (!f || num_verts < 0 || num_verts > 10000) return false;

        buildings[i].height_m = height;
        buildings[i].footprint.resize(num_verts);
        for (int v = 0; v < num_verts; v++) {
            f.read(reinterpret_cast<char*>(&buildings[i].footprint[v].first), sizeof(double));
            f.read(reinterpret_cast<char*>(&buildings[i].footprint[v].second), sizeof(double));
        }
    }
    if (!f) return false;

    buildings_ = std::move(buildings);
    ready_ = true;

    std::cout << "Buildings loaded from cache (" << buildings_.size() << " buildings)" << std::endl;
    return true;
}

void Buildings::save_cache(double center_lat, double center_lon, double radius_km) {
    auto f = open_private_cache("buildings_cache.bin");
    if (!f) return;

    int32_t num = (int32_t)buildings_.size();
    f.write(reinterpret_cast<const char*>(&center_lat), sizeof(double));
    f.write(reinterpret_cast<const char*>(&center_lon), sizeof(double));
    f.write(reinterpret_cast<const char*>(&radius_km), sizeof(double));
    f.write(reinterpret_cast<const char*>(&num), sizeof(int32_t));

    for (const auto& b : buildings_) {
        double h = b.height_m;
        int32_t nv = (int32_t)b.footprint.size();
        f.write(reinterpret_cast<const char*>(&h), sizeof(double));
        f.write(reinterpret_cast<const char*>(&nv), sizeof(int32_t));
        for (const auto& [lat, lon] : b.footprint) {
            f.write(reinterpret_cast<const char*>(&lat), sizeof(double));
            f.write(reinterpret_cast<const char*>(&lon), sizeof(double));
        }
    }
    std::cout << "Buildings saved to cache" << std::endl;
}
