#pragma once
#include "config.h"
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>

// Numeric configuration only: no shell execution or variable expansion.
inline std::string config_trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

struct LocalConfigField {
    const char* name;
    double Config::*member;
    double minimum, maximum;
};

inline const LocalConfigField LOCAL_CONFIG_FIELDS[] = {
    {"BEDROOM_SKY_LATITUDE", &Config::lat_deg, -90, 90},
    {"BEDROOM_SKY_LONGITUDE", &Config::lon_deg, -180, 180},
    {"BEDROOM_SKY_ELEVATION_M", &Config::alt_m, -500, 10000},
    {"BEDROOM_SKY_HEADING_DEG", &Config::heading_deg, 0, 360},
    {"BEDROOM_SKY_PITCH_DEG", &Config::pitch_deg, -10, 89},
    {"BEDROOM_SKY_FOV_DEG", &Config::fov_deg, 10, 120},
    {"BEDROOM_SKY_ROOM_HEADING_DEG", &Config::room_heading_deg, 0, 360},
    {"BEDROOM_SKY_FLIGHT_RADIUS_NM", &Config::flight_radius_nm, 1, 250},
};

inline void set_local_config(Config& config, const std::string& key, std::string value) {
    value = config_trim(value);
    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                             (value.front() == '\'' && value.back() == '\''))) {
        value = value.substr(1, value.size() - 2);
    }
    if (key == "BEDROOM_SKY_SHOW_LOCATION") {
        if (value == "true" || value == "1") config.show_location = true;
        else if (value == "false" || value == "0") config.show_location = false;
        else throw std::runtime_error("Invalid boolean for " + key);
        return;
    }
    for (const auto& field : LOCAL_CONFIG_FIELDS) {
        if (key != field.name) continue;
        double number;
        try {
            size_t consumed = 0;
            number = std::stod(value, &consumed);
            if (consumed != value.size() || !std::isfinite(number))
                throw std::invalid_argument("not a finite number");
        } catch (const std::exception&) {
            throw std::runtime_error("Invalid number for " + key);
        }
        if (number < field.minimum || number > field.maximum)
            throw std::runtime_error("Out-of-range value for " + key);
        config.*(field.member) = number;
        return;
    }
    if (key.rfind("BEDROOM_SKY_", 0) == 0)
        throw std::runtime_error("Unknown BEDROOM_SKY configuration key (name withheld)");
}

inline void validate_local_config(const Config& config) {
    for (const auto& field : LOCAL_CONFIG_FIELDS) {
        const double value = config.*(field.member);
        if (!std::isfinite(value) || value < field.minimum || value > field.maximum)
            throw std::runtime_error(std::string("Out-of-range value for ") + field.name);
    }
}

inline void load_local_config(Config& config, const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot read local configuration file");
    std::string line;
    size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        line = config_trim(line.substr(0, line.find('#')));
        if (line.empty()) continue;
        if (line.rfind("export ", 0) == 0) line = config_trim(line.substr(7));
        const auto equal = line.find('=');
        if (equal == std::string::npos)
            throw std::runtime_error("Expected KEY=VALUE on line " + std::to_string(line_number));
        set_local_config(config, config_trim(line.substr(0, equal)), line.substr(equal + 1));
    }
    if (input.bad()) throw std::runtime_error("Cannot read local configuration file");
}

inline void apply_config_environment(Config& config) {
    for (const auto& field : LOCAL_CONFIG_FIELDS) {
        if (const char* value = std::getenv(field.name)) set_local_config(config, field.name, value);
    }
    if (const char* value = std::getenv("BEDROOM_SKY_SHOW_LOCATION"))
        set_local_config(config, "BEDROOM_SKY_SHOW_LOCATION", value);
}

inline std::filesystem::path find_local_config(const char* executable) {
    if (std::filesystem::exists(".env")) return ".env";
    const auto directory = std::filesystem::absolute(executable).parent_path();
    if (std::filesystem::exists(directory / ".env")) return directory / ".env";
    // Support running from the standard in-repository build directory.
    if (directory.filename() == "build" && std::filesystem::exists(directory.parent_path() / ".env"))
        return directory.parent_path() / ".env";
    return {};
}
