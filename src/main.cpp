#include "renderer.h"
#include "flight_tracker.h"
#include "tracked_plane.h"
#include "projection.h"
#include "config.h"
#include "config_local.h"
#include "cache_paths.h"
#include "terrain.h"
#include "buildings.h"
#include "streets.h"
#include "transit_predictor.h"
#include <iostream>
#include <cmath>
#include <chrono>
#include <thread>

static Config config;
static Renderer* g_renderer = nullptr;

// Left-click drag state
static bool dragging = false;
static double drag_last_x = 0, drag_last_y = 0;
static double drag_start_x = 0, drag_start_y = 0;
static bool drag_moved = false;

static void key_callback(GLFWwindow* /*window*/, int key, int /*scancode*/, int action, int /*mods*/) {
    if (key == GLFW_KEY_TAB && action == GLFW_PRESS && g_renderer) {
        g_renderer->toggle_settings();
    }
}

static void mouse_button_callback(GLFWwindow* window, int button, int action, int /*mods*/) {
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        if (action == GLFW_PRESS) {
            dragging = true;
            drag_moved = false;
            glfwGetCursorPos(window, &drag_last_x, &drag_last_y);
            drag_start_x = drag_last_x;
            drag_start_y = drag_last_y;
        } else if (action == GLFW_RELEASE) {
            // Only fire click if the mouse didn't drag
            if (!drag_moved && g_renderer) {
                g_renderer->on_click((float)drag_start_x, (float)drag_start_y);
            }
            dragging = false;
        }
    }
}

static void cursor_pos_callback(GLFWwindow* /*window*/, double xpos, double ypos) {
    if (!dragging || (g_renderer && g_renderer->show_settings())) return;

    // Dead zone: don't start panning until mouse moves >3px from click point
    if (!drag_moved) {
        double dist = std::abs(xpos - drag_start_x) + std::abs(ypos - drag_start_y);
        if (dist < 3.0) return;
        drag_moved = true;
        drag_last_x = xpos;
        drag_last_y = ypos;
        return;
    }

    double dx = xpos - drag_last_x;
    double dy = ypos - drag_last_y;
    drag_last_x = xpos;
    drag_last_y = ypos;

    // Horizontal drag = heading, vertical drag = pitch
    float sensitivity = 0.3f;
    config.heading_deg += dx * sensitivity;
    config.pitch_deg -= dy * sensitivity;

    if (config.heading_deg < 0) config.heading_deg += 360;
    if (config.heading_deg >= 360) config.heading_deg -= 360;
    config.pitch_deg = std::max(0.0, std::min(45.0, config.pitch_deg));
}

static void scroll_callback(GLFWwindow* window, double /*xoff*/, double yoff) {
    if (g_renderer && g_renderer->show_settings()) {
        g_renderer->scroll_settings((int)(-yoff));
    } else if (g_renderer) {
        double mx, my;
        glfwGetCursorPos(window, &mx, &my);
        if (g_renderer->is_over_minimap((float)mx, (float)my)) {
            g_renderer->zoom_minimap(yoff);
        } else {
            config.heading_deg += yoff * 2.0;
            if (config.heading_deg < 0) config.heading_deg += 360;
            if (config.heading_deg >= 360) config.heading_deg -= 360;
        }
    }
}

static ObserverConfig observer_from_config(const Config& c) {
    return {c.lat_deg, c.lon_deg, c.alt_m, c.heading_deg, c.pitch_deg, c.fov_deg};
}

int main(int argc, char** argv) {
    bool check_config = false;
    bool demo_mode = false;
    std::filesystem::path cache_directory;
    std::filesystem::path config_path;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--check-config") check_config = true;
        else if (arg == "--demo") demo_mode = true;
        else if (arg == "--cache-dir" && i + 1 < argc) cache_directory = argv[++i];
        else if (arg == "--config" && i + 1 < argc) config_path = argv[++i];
        else {
            std::cerr << "Usage: bedroom-sky [--demo | --config FILE] [--cache-dir DIR] [--check-config]\n";
            return 1;
        }
    }
    try {
        if (demo_mode && !config_path.empty())
            throw std::runtime_error("--demo cannot be combined with --config");
        if (!demo_mode) {
            if (config_path.empty()) config_path = find_local_config(argv[0]);
            if (!config_path.empty()) load_local_config(config, config_path);
            apply_config_environment(config);
        }
        validate_local_config(config);
    } catch (const std::exception& error) {
        std::cerr << "Configuration error: " << error.what() << '\n';
        return 1;
    }
    if (check_config) {
        std::cout << "Configuration valid (location values withheld).\n";
        return 0;
    }
    try {
        initialize_cache_directory(cache_directory, demo_mode);
    } catch (const std::exception&) {
        std::cerr << "Cannot initialize private cache directory; use --cache-dir DIR.\n";
        return 1;
    }
    std::cout << (demo_mode ? "=== Bedroom Sky - PUBLIC DEMO (SFO) ===" :
                              "=== Bedroom Sky - Local observer ===") << std::endl;

    Renderer renderer;
    if (!renderer.init(1280, 800, "Bedroom Sky")) {
        std::cerr << "Failed to initialize renderer" << std::endl;
        return 1;
    }

    g_renderer = &renderer;
    glfwSetKeyCallback(renderer.window(), key_callback);
    glfwSetMouseButtonCallback(renderer.window(), mouse_button_callback);
    glfwSetScrollCallback(renderer.window(), scroll_callback);
    glfwSetCursorPosCallback(renderer.window(), cursor_pos_callback);

    FlightTracker tracker;
    tracker.set_area(config.lat_deg, config.lon_deg, config.flight_radius_nm);
    tracker.start(config.poll_interval_s);

    PlaneTracker plane_tracker;

    Terrain terrain;
    terrain.fetch_async(config.lat_deg, config.lon_deg, 20.0, 40);

    Buildings buildings;
    buildings.fetch_async(config.lat_deg, config.lon_deg, 5.0);

    // Stagger Overpass API requests to avoid rate-limiting
    Streets streets;
    double st_lat = config.lat_deg, st_lon = config.lon_deg;
    if (!buildings.is_ready()) {
        // Buildings are fetching from API — wait until done before starting streets
        std::thread([&streets, &buildings, st_lat, st_lon]() {
            while (buildings.is_loading()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            std::this_thread::sleep_for(std::chrono::seconds(2));
            streets.fetch_async(st_lat, st_lon, 10.0);
        }).detach();
    } else {
        // Buildings loaded from cache — safe to fetch streets immediately
        streets.fetch_async(config.lat_deg, config.lon_deg, 10.0);
    }

    auto start_time = std::chrono::steady_clock::now();
    auto monotonic_now = [&]() -> double {
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        return std::chrono::duration<double>(elapsed).count();
    };
    auto utc_now = []() -> double {
        auto tp = std::chrono::system_clock::now().time_since_epoch();
        return std::chrono::duration<double>(tp).count();
    };

    TransitPredictor transit_predictor;
    double last_transit_update = -1e9;

    double last_ingest_time = 0;
    bool all_loaded = false;
    double last_poll_interval = config.poll_interval_s;
    double last_query_lat = config.lat_deg, last_query_lon = config.lon_deg;
    double last_query_radius = config.flight_radius_nm;

    std::cout << "Controls: AD=pan, WS=pitch, ZX=zoom, scroll=pan, Ctrl+C=quit" << std::endl;
    std::cout << "Click [S] for live settings panel" << std::endl;

    while (!renderer.should_close()) {
        double now = monotonic_now();
        renderer.begin_frame();

        // Keyboard input (only when settings panel is closed)
        if (!renderer.show_settings()) {
            GLFWwindow* win = renderer.window();
            float pan_speed = 2.0f;
            float pitch_speed = 1.0f;
            float zoom_speed = 2.0f;

            if (glfwGetKey(win, GLFW_KEY_A) == GLFW_PRESS) config.heading_deg -= pan_speed;
            if (glfwGetKey(win, GLFW_KEY_D) == GLFW_PRESS) config.heading_deg += pan_speed;
            if (glfwGetKey(win, GLFW_KEY_W) == GLFW_PRESS) config.pitch_deg = std::min(45.0, config.pitch_deg + pitch_speed);
            if (glfwGetKey(win, GLFW_KEY_S) == GLFW_PRESS) config.pitch_deg = std::max(0.0, config.pitch_deg - pitch_speed);
            if (glfwGetKey(win, GLFW_KEY_Z) == GLFW_PRESS) config.fov_deg = std::max(10.0, config.fov_deg - zoom_speed);
            if (glfwGetKey(win, GLFW_KEY_X) == GLFW_PRESS) config.fov_deg = std::min(120.0, config.fov_deg + zoom_speed);

            if (glfwGetKey(win, GLFW_KEY_R) == GLFW_PRESS) {
                config.heading_deg = config.room_heading_deg;
                config.pitch_deg = 5.0;
                config.fov_deg = 60.0;
            }

            if (config.heading_deg < 0) config.heading_deg += 360;
            if (config.heading_deg >= 360) config.heading_deg -= 360;
        }

        // Follow the configured observer when setting up at a new location.
        if (config.lat_deg != last_query_lat || config.lon_deg != last_query_lon ||
            config.flight_radius_nm != last_query_radius || config.poll_interval_s != last_poll_interval) {
            tracker.stop();
            tracker.set_area(config.lat_deg, config.lon_deg, config.flight_radius_nm);
            tracker.start(config.poll_interval_s);
            last_poll_interval = config.poll_interval_s;
            last_query_lat = config.lat_deg; last_query_lon = config.lon_deg;
            last_query_radius = config.flight_radius_nm;
        }

        // Ingest new API data
        auto raw_aircraft = tracker.get_aircraft();
        double fetch_time = tracker.last_fetch_time();
        if (!raw_aircraft.empty() && fetch_time != last_ingest_time) {
            plane_tracker.update(raw_aircraft, now);
            last_ingest_time = fetch_time;
        }

        ObserverConfig observer = observer_from_config(config);
        plane_tracker.tick(now, observer);

        // Recompute upcoming transits every 2s
        if (now - last_transit_update > 2.0) {
            transit_predictor.update(plane_tracker.planes(), observer, utc_now(),
                                     config.transit_lookahead_s,
                                     config.transit_threshold_deg);
            last_transit_update = now;
        }

        glm::mat4 view = build_view_matrix(observer);
        float aspect = (float)renderer.width() / (float)renderer.height();
        glm::mat4 proj = glm::perspective(
            glm::radians((float)config.fov_deg), aspect, 0.1f, config.far_clip);

        if (terrain.is_ready()) terrain.update_enu(observer);
        terrain.refetch_if_needed(config.lat_deg, config.lon_deg, 20.0, 40);

        const Terrain* terr_ptr = terrain.is_ready() ? &terrain : nullptr;
        if (buildings.is_ready()) buildings.update_enu(observer, terr_ptr);
        buildings.refetch_if_needed(config.lat_deg, config.lon_deg, 5.0);

        if (streets.is_ready()) streets.update_enu(observer, terr_ptr);
        streets.refetch_if_needed(config.lat_deg, config.lon_deg, 10.0);

        if (!all_loaded && terrain.is_ready() && buildings.is_ready() && streets.is_ready() &&
            !terrain.is_loading() && !buildings.is_loading() && !streets.is_loading()) {
            all_loaded = true;
            std::cout << "\n========================================" << std::endl;
            std::cout << "  ALL DATA LOADED - READY" << std::endl;
            std::cout << "========================================\n" << std::endl;
        }

        // Draw scene
        double utc_s = utc_now();
        renderer.draw_sky(observer, view, proj, utc_s);
        renderer.draw_celestial(observer, view, proj, utc_s);
        renderer.draw_terrain(terrain, view, proj);
        renderer.draw_buildings(buildings, view, proj);
        renderer.draw_streets(streets, view, proj);
        renderer.draw_horizon(observer, view, proj, (float)config.room_heading_deg);
        renderer.draw_aircraft(plane_tracker.planes(), observer, view, proj, now, config,
                               &transit_predictor);
        renderer.draw_landmarks(observer, view, proj);
        renderer.draw_altitude_scale(observer, view, proj);
        renderer.draw_hud(observer, plane_tracker.active_count(), tracker.last_fetch_time(),
                          config.show_location, demo_mode);
        renderer.draw_transits_panel(transit_predictor);
        renderer.draw_minimap(streets, buildings, observer, config);

        renderer.draw_settings_panel(config);
        renderer.draw_hover_tooltip();

        renderer.end_frame();
    }

    tracker.stop();
    renderer.shutdown();

    return 0;
}
