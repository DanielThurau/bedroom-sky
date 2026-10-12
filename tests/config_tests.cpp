#include "config_local.h"
#include "cache_paths.h"
#include <iostream>
#include <chrono>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("bedroom-sky-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directories(directory);
        const auto env = directory / ".env";
        Config c;
        require(c.lat_deg == 37.6213 && !c.show_location, "Public defaults must not reveal a home");
        { std::ofstream f(env); f << "# comment\nexport BEDROOM_SKY_LATITUDE=\"51.5\" # comment\n"
                                   "BEDROOM_SKY_LONGITUDE='-0.1'\nBEDROOM_SKY_SHOW_LOCATION=true\nOTHER_KEY=ignored\n"; }
        load_local_config(c, env);
        require(c.lat_deg == 51.5 && c.lon_deg == -0.1 && c.show_location, "Global location parsing failed");
#ifndef _WIN32
        setenv("BEDROOM_SKY_LATITUDE", "-33.8", 1);
        apply_config_environment(c);
        require(c.lat_deg == -33.8, "Environment must override file");
        unsetenv("BEDROOM_SKY_LATITUDE");
#endif
        for (const char* value : {"nan", "inf", "91", "51.5garbage", "$(echo 51)", ""}) {
            bool rejected = false;
            try { set_local_config(c, "BEDROOM_SKY_LATITUDE", value); }
            catch (const std::exception& error) {
                rejected = true;
                require(!*value || std::string(error.what()).find(value) == std::string::npos,
                        "Errors must not print configuration values");
            }
            require(rejected, "Malformed or out-of-range input accepted");
        }
        bool rejected = false;
        try { set_local_config(c, "BEDROOM_SKY_TYPO", "123"); }
        catch (const std::exception&) { rejected = true; }
        require(rejected, "Unknown app keys must fail");
        validate_local_config(c);
        require(SLIDER_DEFS[0].min_val <= -33.8 && SLIDER_DEFS[0].max_val >= 51.5,
                "Latitude UI must support locations outside the Bay Area");
        initialize_cache_directory(directory / "cache", false);
        const auto local = cache_path("terrain_cache.bin");
        { auto output = open_private_cache("terrain_cache.bin"); output << "private fixture"; }
#ifndef _WIN32
        require((std::filesystem::status(local).permissions() & std::filesystem::perms::group_all) ==
                std::filesystem::perms::none, "Cache readable by group");
        require((std::filesystem::status(local).permissions() & std::filesystem::perms::others_all) ==
                std::filesystem::perms::none, "Cache readable by others");
#endif
        initialize_cache_directory(directory / "cache", true);
        require(cache_path("terrain_cache.bin") != local, "Demo must use a separate cache");
        require(!std::filesystem::exists(cache_path("terrain_cache.bin")), "Demo loaded local cache");
        std::filesystem::remove_all(directory);
        std::cout << "Configuration and private-cache tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(directory);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
