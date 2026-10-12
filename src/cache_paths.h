#pragma once
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>

// Initialized once, before fetch threads start. Cache headers contain coordinates.
inline std::filesystem::path bedroom_sky_cache_directory;

inline std::filesystem::path default_cache_base() {
#ifdef _WIN32
    if (const char* value = std::getenv("LOCALAPPDATA"))
        return std::filesystem::path(value) / "BedroomSky";
#else
    if (const char* home = std::getenv("HOME")) {
#ifdef __APPLE__
        return std::filesystem::path(home) / "Library" / "Caches" / "bedroom-sky";
#else
        if (const char* cache = std::getenv("XDG_CACHE_HOME"))
            return std::filesystem::path(cache) / "bedroom-sky";
        return std::filesystem::path(home) / ".cache" / "bedroom-sky";
#endif
    }
#endif
    throw std::runtime_error("No user cache directory available");
}

inline void initialize_cache_directory(const std::filesystem::path& base, bool demo_mode) {
    // Always isolate demo data, even when a custom cache base is requested.
    bedroom_sky_cache_directory = (base.empty() ? default_cache_base() : base) /
                                 (demo_mode ? "demo" : "local");
    std::filesystem::create_directories(bedroom_sky_cache_directory);
#ifndef _WIN32
    std::filesystem::permissions(bedroom_sky_cache_directory,
        std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
#endif
}

inline std::filesystem::path cache_path(const char* filename) {
    if (bedroom_sky_cache_directory.empty())
        throw std::runtime_error("Cache directory is not initialized");
    return bedroom_sky_cache_directory / filename;
}

inline std::ofstream open_private_cache(const char* filename) {
    const auto path = cache_path(filename);
    std::ofstream output(path, std::ios::binary);
#ifndef _WIN32
    if (output) {
        std::error_code error;
        std::filesystem::permissions(path, std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write, std::filesystem::perm_options::replace, error);
        if (error) { output.close(); return {}; }
    }
#endif
    return output;
}
