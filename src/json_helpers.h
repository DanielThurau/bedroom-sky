#pragma once
#include <string>
#include <vector>
#include <utility>
#include <cstddef>
#include <iostream>
#include <thread>
#include <chrono>
#include <curl/curl.h>

// Shared CURL write callback
inline size_t curl_write_cb(void* contents, size_t size, size_t nmemb, std::string* out) {
    out->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// Find "key": value within json[start..end), return the value as a trimmed string.
// Handles both string values ("foo") and bare values (123, true).
inline std::string json_value(const std::string& j, size_t start, size_t end, const char* key) {
    std::string needle = std::string("\"") + key + "\":";
    size_t p = j.find(needle, start);
    if (p == std::string::npos || p >= end) return "";
    p += needle.size();
    while (p < end && j[p] == ' ') p++;
    if (p >= end) return "";
    if (j[p] == '"') {
        p++;
        size_t q = j.find('"', p);
        if (q == std::string::npos || q > end) return "";
        return j.substr(p, q - p);
    }
    size_t q = p;
    while (q < end && j[q] != ',' && j[q] != '}' && j[q] != ']' && j[q] != ' ') q++;
    return j.substr(p, q - p);
}

// Convenience wrapper: search entire string
inline std::string json_value(const std::string& j, const char* key) {
    return json_value(j, 0, j.size(), key);
}

// Parse a JSON value as double (returns 0.0 on failure)
inline double json_double(const std::string& j, size_t start, size_t end, const char* key) {
    std::string v = json_value(j, start, end, key);
    if (v.empty() || v == "null") return 0.0;
    try { return std::stod(v); } catch (...) { return 0.0; }
}

inline double json_double(const std::string& j, const char* key) {
    return json_double(j, 0, j.size(), key);
}

// Find top-level element boundaries in the "elements":[ ... ] array (Overpass response)
inline std::vector<std::pair<size_t, size_t>> json_find_elements(const std::string& j) {
    std::vector<std::pair<size_t, size_t>> elems;
    size_t p = j.find("\"elements\"");
    if (p == std::string::npos) return elems;
    p = j.find('[', p);
    if (p == std::string::npos) return elems;
    p++;

    int depth = 0;
    size_t elem_start = 0;
    for (size_t i = p; i < j.size(); i++) {
        if (j[i] == '{') {
            if (depth == 0) elem_start = i;
            depth++;
        } else if (j[i] == '}') {
            depth--;
            if (depth == 0) elems.push_back({elem_start, i + 1});
        } else if (j[i] == ']' && depth == 0) {
            break;
        }
    }
    return elems;
}

// Overpass API endpoints — tries primary, falls back to mirror
static const char* OVERPASS_ENDPOINTS[] = {
    "https://overpass-api.de/api/interpreter",
    "https://overpass.kumi.systems/api/interpreter",
};
static const int NUM_OVERPASS_ENDPOINTS = 2;

// Fetch from Overpass API with automatic fallback to mirror.
// Returns the JSON response body, or empty string on total failure.
inline std::string overpass_fetch(const std::string& query, const char* label) {
    for (int ep = 0; ep < NUM_OVERPASS_ENDPOINTS; ep++) {
        CURL* curl = curl_easy_init();
        if (!curl) continue;

        char* encoded = curl_easy_escape(curl, query.c_str(), 0);
        std::string post_data = std::string("data=") + encoded;
        curl_free(encoded);

        std::string response;
        curl_easy_setopt(curl, CURLOPT_URL, OVERPASS_ENDPOINTS[ep]);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 180L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "bedroom-sky/1.0");

        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            std::cerr << label << ": fetch from " << OVERPASS_ENDPOINTS[ep]
                      << " failed: " << curl_easy_strerror(res) << std::endl;
            continue;
        }

        std::cout << label << " response from " << OVERPASS_ENDPOINTS[ep]
                  << ": " << (response.size() / 1024) << " KB" << std::endl;

        if (!response.empty() && response[0] == '{') {
            return response;
        }

        std::cerr << label << ": got non-JSON from " << OVERPASS_ENDPOINTS[ep]
                  << ", trying next..." << std::endl;

        // Brief pause before trying next endpoint
        if (ep + 1 < NUM_OVERPASS_ENDPOINTS) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }

    std::cerr << label << ": all endpoints failed" << std::endl;
    return "";
}

// Extract lat/lon pairs from inline "geometry":[{"lat":...,"lon":...},...] array
inline std::vector<std::pair<double, double>> json_parse_geometry(const std::string& j, size_t start, size_t end) {
    std::vector<std::pair<double, double>> pts;
    size_t p = j.find("\"geometry\"", start);
    if (p == std::string::npos || p >= end) return pts;
    p = j.find('[', p);
    if (p == std::string::npos || p >= end) return pts;
    size_t arr_end = p;
    int depth = 0;
    for (size_t i = p; i < end; i++) {
        if (j[i] == '[') depth++;
        else if (j[i] == ']') { depth--; if (depth == 0) { arr_end = i; break; } }
    }

    size_t pos = p;
    while (pos < arr_end) {
        size_t obj = j.find('{', pos);
        if (obj == std::string::npos || obj >= arr_end) break;
        size_t obj_end = j.find('}', obj);
        if (obj_end == std::string::npos || obj_end > arr_end) break;

        std::string lat_s = json_value(j, obj, obj_end + 1, "lat");
        std::string lon_s = json_value(j, obj, obj_end + 1, "lon");
        if (!lat_s.empty() && !lon_s.empty()) {
            try { pts.push_back({std::stod(lat_s), std::stod(lon_s)}); } catch (...) {}
        }
        pos = obj_end + 1;
    }
    return pts;
}
