#include "flight_tracker.h"
#include "json_helpers.h"
#include <curl/curl.h>
#include <sstream>
#include <iostream>
#include <chrono>
#include <cstring>

FlightTracker::FlightTracker() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

FlightTracker::~FlightTracker() {
    stop();
    curl_global_cleanup();
}

void FlightTracker::set_area(double latitude, double longitude, double radius_nm) {
    center_lat_ = latitude; center_lon_ = longitude; radius_nm_ = radius_nm;
}

void FlightTracker::start(double poll_interval_sec) {
    if (running_) return;
    running_ = true;
    thread_ = std::thread(&FlightTracker::poll_loop, this, poll_interval_sec);
}

void FlightTracker::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

std::vector<AircraftPos> FlightTracker::get_aircraft() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return aircraft_;
}

void FlightTracker::poll_loop(double interval) {
    while (running_) {
        auto start = std::chrono::steady_clock::now();
        auto result = fetch_opensky();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            aircraft_ = std::move(result);
        }
        aircraft_count_ = (int)aircraft_.size();
        auto elapsed = std::chrono::steady_clock::now() - start;
        double elapsed_sec = std::chrono::duration<double>(elapsed).count();
        last_fetch_time_ = elapsed_sec;

        double sleep_sec = interval - elapsed_sec;
        if (sleep_sec > 0) {
            auto sleep_ms = (int)(sleep_sec * 1000);
            for (int i = 0; i < sleep_ms && running_; i += 100)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

std::vector<AircraftPos> FlightTracker::fetch_opensky() {
    std::vector<AircraftPos> result;

    CURL* curl = curl_easy_init();
    if (!curl) return result;

    std::ostringstream url;
    url << "https://api.airplanes.live/v2/point/"
        << center_lat_ << "/" << center_lon_ << "/" << (int)radius_nm_;

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.str().c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "bedroom-sky/1.0");

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        std::cerr << "API fetch failed: " << curl_easy_strerror(res) << std::endl;
        return result;
    }

    // Parse the "ac":[ ... ] array of aircraft objects
    size_t ac_pos = response.find("\"ac\":");
    if (ac_pos == std::string::npos) return result;
    size_t arr_start = response.find('[', ac_pos);
    if (arr_start == std::string::npos) return result;

    // Iterate through aircraft objects { ... }
    size_t pos = arr_start + 1;
    while (pos < response.size()) {
        size_t obj_start = response.find('{', pos);
        if (obj_start == std::string::npos) break;

        int depth = 1;
        size_t obj_end = obj_start + 1;
        while (obj_end < response.size() && depth > 0) {
            if (response[obj_end] == '{') depth++;
            else if (response[obj_end] == '}') depth--;
            obj_end++;
        }

        AircraftPos ac;
        ac.icao24 = json_value(response, obj_start, obj_end, "hex");
        ac.callsign = json_value(response, obj_start, obj_end, "flight");
        while (!ac.callsign.empty() && ac.callsign.back() == ' ')
            ac.callsign.pop_back();

        ac.lat_deg = json_double(response, obj_start, obj_end, "lat");
        ac.lon_deg = json_double(response, obj_start, obj_end, "lon");

        double alt_geom = json_double(response, obj_start, obj_end, "alt_geom");
        double alt_baro = json_double(response, obj_start, obj_end, "alt_baro");
        double alt_ft = (alt_geom > 0) ? alt_geom : alt_baro;
        ac.alt_m = alt_ft * 0.3048;

        // airplanes.live sets alt_baro to the string "ground" when on ground
        std::string alt_baro_str = json_value(response, obj_start, obj_end, "alt_baro");
        ac.on_ground = (alt_baro_str == "ground");

        ac.velocity_ms = (float)(json_double(response, obj_start, obj_end, "gs") * 0.514444);
        ac.heading_deg = (float)json_double(response, obj_start, obj_end, "track");

        // Database-enriched fields
        ac.ac_type = json_value(response, obj_start, obj_end, "t");
        ac.registration = json_value(response, obj_start, obj_end, "r");
        ac.ac_operator = json_value(response, obj_start, obj_end, "ownOp");

        if (ac.lat_deg != 0.0 || ac.lon_deg != 0.0) {
            result.push_back(ac);
        }

        pos = obj_end;
    }

    return result;
}
