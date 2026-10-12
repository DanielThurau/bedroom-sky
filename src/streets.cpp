#include "cache_paths.h"
#include "streets.h"
#include "terrain.h"
#include "json_helpers.h"
#include <sstream>
#include <cmath>
#include <fstream>

void Streets::parse_overpass(const std::string& json) {
    auto elems = json_find_elements(json);
    if (elems.empty()) {
        std::cerr << "Streets: no elements in response ("
                  << json.size() << " bytes)" << std::endl;
        return;
    }

    std::cout << "  Parsing " << elems.size() << " street elements..." << std::endl;

    std::vector<Street> streets;
    for (auto& [s, e] : elems) {
        std::string type = json_value(json, s, e, "type");
        if (type != "way") continue;

        std::string highway = json_value(json, s, e, "highway");
        int rank = -1;
        if (highway == "primary" || highway == "trunk") rank = 0;
        else if (highway == "secondary") rank = 1;
        else if (highway == "tertiary") rank = 2;
        else continue;

        auto points = json_parse_geometry(json, s, e);
        if (points.size() < 2) continue;

        std::string name = json_value(json, s, e, "name");

        streets.push_back({std::move(points), std::move(name), rank, {}});
    }

    streets_ = std::move(streets);
    std::cout << "Streets parsed: " << streets_.size() << " roads" << std::endl;
}

void Streets::fetch_thread(double center_lat, double center_lon, double radius_km) {
    double deg_per_km_lat = 1.0 / 111.0;
    double deg_per_km_lon = 1.0 / (111.0 * cos(glm::radians(center_lat)));

    double lat_min = center_lat - radius_km * deg_per_km_lat;
    double lat_max = center_lat + radius_km * deg_per_km_lat;
    double lon_min = center_lon - radius_km * deg_per_km_lon;
    double lon_max = center_lon + radius_km * deg_per_km_lon;

    std::ostringstream query;
    query << "[out:json][timeout:120];"
          << "way[\"highway\"~\"^(primary|secondary|tertiary|trunk)$\"]("
          << lat_min << "," << lon_min << "," << lat_max << "," << lon_max
          << ");out body geom qt;";

    std::cout << "Fetching streets (" << radius_km << "km radius)..." << std::endl;

    std::string response = overpass_fetch(query.str(), "Streets");
    if (response.empty()) {
        loading_ = false;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        parse_overpass(response);
    }

    if (streets_.empty()) {
        std::cerr << "Streets: parsed 0 streets, not caching" << std::endl;
        loading_ = false;
        return;
    }

    save_cache(center_lat, center_lon, radius_km);
    ready_ = true;
    loading_ = false;
}

void Streets::fetch_async(double center_lat, double center_lon, double radius_km) {
    if (loading_.load()) return;

    fetch_center_lat_ = center_lat;
    fetch_center_lon_ = center_lon;
    fetch_radius_km_ = radius_km;
    last_obs_lat_ = 1e99;

    if (load_cache(center_lat, center_lon, radius_km)) return;

    loading_ = true;
    if (thread_.joinable()) thread_.join();
    thread_ = std::thread(&Streets::fetch_thread, this, center_lat, center_lon, radius_km);
}

void Streets::refetch_if_needed(double lat, double lon, double radius_km) {
    if (loading_.load()) return;
    if (fetch_center_lat_ > 1e98) return;

    double dlat_km = (lat - fetch_center_lat_) * 111.0;
    double dlon_km = (lon - fetch_center_lon_) * 111.0 * cos(glm::radians(lat));
    if (dlat_km * dlat_km + dlon_km * dlon_km < 1.0) return;

    ready_ = false;
    fetch_async(lat, lon, radius_km);
}

void Streets::update_enu(const ObserverConfig& observer, const Terrain* terrain) {
    if (std::abs(observer.lat_deg - last_obs_lat_) < 1e-9 &&
        std::abs(observer.lon_deg - last_obs_lon_) < 1e-9 &&
        std::abs(observer.alt_m - last_obs_alt_) < 0.01) {
        return;
    }
    last_obs_lat_ = observer.lat_deg;
    last_obs_lon_ = observer.lon_deg;
    last_obs_alt_ = observer.alt_m;

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& st : streets_) {
        st.enu_points.resize(st.points.size());
        for (size_t i = 0; i < st.points.size(); i++) {
            auto [lat, lon] = st.points[i];
            double elev = terrain ? terrain->sample_elevation(lat, lon) : 0.0;
            st.enu_points[i] = glm::vec3(geodetic_to_enu(lat, lon, elev,
                observer.lat_deg, observer.lon_deg, observer.alt_m));
        }
    }
    generation_++;
}

bool Streets::load_cache(double center_lat, double center_lon, double radius_km) {
    std::ifstream f(cache_path("streets_cache.bin"), std::ios::binary);
    if (!f) return false;

    double cached_lat, cached_lon, cached_radius;
    int32_t num_streets;
    f.read(reinterpret_cast<char*>(&cached_lat), sizeof(double));
    f.read(reinterpret_cast<char*>(&cached_lon), sizeof(double));
    f.read(reinterpret_cast<char*>(&cached_radius), sizeof(double));
    f.read(reinterpret_cast<char*>(&num_streets), sizeof(int32_t));
    if (!f) return false;

    if (cached_radius != radius_km) return false;

    double dlat_km = (center_lat - cached_lat) * 111.0;
    double dlon_km = (center_lon - cached_lon) * 111.0 * cos(glm::radians(center_lat));
    if (dlat_km * dlat_km + dlon_km * dlon_km > 1.0) return false;

    std::vector<Street> streets(num_streets);
    for (int i = 0; i < num_streets; i++) {
        int32_t rank, num_pts, name_len;
        f.read(reinterpret_cast<char*>(&rank), sizeof(int32_t));
        f.read(reinterpret_cast<char*>(&name_len), sizeof(int32_t));
        if (!f || name_len < 0 || name_len > 1000) return false;
        streets[i].rank = rank;
        streets[i].name.resize(name_len);
        if (name_len > 0) f.read(&streets[i].name[0], name_len);

        f.read(reinterpret_cast<char*>(&num_pts), sizeof(int32_t));
        if (!f || num_pts < 0 || num_pts > 100000) return false;
        streets[i].points.resize(num_pts);
        for (int v = 0; v < num_pts; v++) {
            f.read(reinterpret_cast<char*>(&streets[i].points[v].first), sizeof(double));
            f.read(reinterpret_cast<char*>(&streets[i].points[v].second), sizeof(double));
        }
    }
    if (!f) return false;

    streets_ = std::move(streets);
    ready_ = true;
    std::cout << "Streets loaded from cache (" << streets_.size() << " roads)" << std::endl;
    return true;
}

void Streets::save_cache(double center_lat, double center_lon, double radius_km) {
    auto f = open_private_cache("streets_cache.bin");
    if (!f) return;

    int32_t num = (int32_t)streets_.size();
    f.write(reinterpret_cast<const char*>(&center_lat), sizeof(double));
    f.write(reinterpret_cast<const char*>(&center_lon), sizeof(double));
    f.write(reinterpret_cast<const char*>(&radius_km), sizeof(double));
    f.write(reinterpret_cast<const char*>(&num), sizeof(int32_t));

    for (const auto& st : streets_) {
        int32_t rank = st.rank;
        int32_t name_len = (int32_t)st.name.size();
        int32_t num_pts = (int32_t)st.points.size();
        f.write(reinterpret_cast<const char*>(&rank), sizeof(int32_t));
        f.write(reinterpret_cast<const char*>(&name_len), sizeof(int32_t));
        if (name_len > 0) f.write(st.name.data(), name_len);
        f.write(reinterpret_cast<const char*>(&num_pts), sizeof(int32_t));
        for (const auto& [lat, lon] : st.points) {
            f.write(reinterpret_cast<const char*>(&lat), sizeof(double));
            f.write(reinterpret_cast<const char*>(&lon), sizeof(double));
        }
    }
    std::cout << "Streets saved to cache" << std::endl;
}
