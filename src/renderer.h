#pragma once
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <vector>
#include <string>
#include "projection.h"
#include "tracked_plane.h"
#include "config.h"
#include "terrain.h"
#include "buildings.h"
#include "streets.h"
#include "landmarks.h"
#include "transit_predictor.h"

class Renderer {
public:
    bool init(int width, int height, const char* title);
    void shutdown();

    bool should_close() const;
    void begin_frame();
    void end_frame();

    void draw_sky(const ObserverConfig& observer,
                  const glm::mat4& view,
                  const glm::mat4& proj,
                  double utc_unix_s);
    void draw_horizon(const ObserverConfig& observer, const glm::mat4& view, const glm::mat4& proj, float room_heading = 0.0f);
    void draw_aircraft(const std::unordered_map<std::string, TrackedPlane>& planes,
                       const ObserverConfig& observer,
                       const glm::mat4& view,
                       const glm::mat4& proj,
                       double now,
                       const Config& config,
                       const TransitPredictor* transits = nullptr);
    void draw_celestial(const ObserverConfig& observer,
                        const glm::mat4& view,
                        const glm::mat4& proj,
                        double utc_unix_s);
    void draw_transits_panel(const TransitPredictor& transits);
    void draw_altitude_scale(const ObserverConfig& observer, const glm::mat4& view, const glm::mat4& proj);
    void draw_landmarks(const ObserverConfig& observer, const glm::mat4& view, const glm::mat4& proj);
    void draw_terrain(const Terrain& terrain, const glm::mat4& view, const glm::mat4& proj);
    void draw_buildings(const Buildings& buildings, const glm::mat4& view, const glm::mat4& proj);
    void draw_streets(const Streets& streets, const glm::mat4& view, const glm::mat4& proj);
    void draw_minimap(const Streets& streets, const Buildings& buildings,
                      const ObserverConfig& observer, const Config& config);
    void draw_hud(const ObserverConfig& observer, int aircraft_count, double fetch_time,
                  bool show_location = false, bool demo_mode = false);
    void draw_text(const std::string& text, float x, float y, float scale, glm::vec3 color);

    void on_click(float win_x, float win_y);

    bool show_help() const { return show_help_; }
    bool show_settings() const { return show_settings_; }
    void toggle_settings() { show_settings_ = !show_settings_; show_help_ = false; }
    void scroll_settings(int delta) { settings_scroll_ += delta; }

    bool is_over_minimap(float win_x, float win_y) const;
    void zoom_minimap(double delta);

    bool draw_settings_panel(Config& config);
    void draw_hover_tooltip();

    GLFWwindow* window() { return window_; }
    int width() const { return width_; }
    int height() const { return height_; }
    float dpi_scale() const { return dpi_scale_; }

    // Utility: draw a filled rectangle in pixel coordinates
    void draw_filled_rect(float x, float y, float w, float h, glm::vec4 color);

private:
    GLFWwindow* window_ = nullptr;
    int width_, height_;
    float dpi_scale_ = 1.0f;

    // Shaders
    GLuint sky_shader_ = 0;
    GLuint marker_shader_ = 0;
    GLuint text_shader_ = 0;
    GLuint terrain_shader_ = 0;  // 2D pass-through with per-vertex color (terrain, minimap)
    GLuint enu3d_shader_ = 0;    // 3D ENU with VP uniform (buildings, streets)
    GLuint celestial_shader_ = 0; // 3D ENU quad with soft disc fragment shader (sun, moon)

    // Cached uniform locations
    GLint marker_uTransform_ = -1;
    GLint marker_uColor_ = -1;
    GLint enu3d_uVP_ = -1;
    GLint text_uFont_ = -1;
    GLint text_uColor_ = -1;
    GLint text_uProj_ = -1;
    GLint celestial_uVP_ = -1;
    GLint celestial_uCore_ = -1;
    GLint celestial_uHalo_ = -1;
    GLint celestial_uIntensity_ = -1;
    GLint sky_uInvVP_ = -1;
    GLint sky_uCamPos_ = -1;
    GLint sky_uSunDir_ = -1;
    GLint sky_uSunEl_ = -1;
    GLint sky_uMoonDir_ = -1;
    GLint sky_uMoonEl_ = -1;

    // Geometry
    GLuint quad_vao_ = 0, quad_vbo_ = 0;
    GLuint marker_vao_ = 0, marker_vbo_ = 0;
    GLuint terrain_vao_ = 0, terrain_vbo_ = 0;

    // Terrain rendering cache (GPU-projected via enu3d_shader_, cached by generation only)
    std::vector<float> terrain_verts_;
    GLsizei terrain_cached_count_ = 0;
    uint32_t terrain_cached_gen_ = 0;

    // Buildings rendering (3D ENU) — filled rooftops + edge outlines
    GLuint buildings_vao_ = 0, buildings_vbo_ = 0;
    GLuint buildings_edge_vao_ = 0, buildings_edge_vbo_ = 0;
    std::vector<float> buildings_verts_;      // filled triangles
    std::vector<float> buildings_edge_verts_; // roof edge outlines
    GLsizei buildings_cached_count_ = 0;
    GLsizei buildings_edge_count_ = 0;
    uint32_t buildings_cached_gen_ = 0;
    uint32_t buildings_cached_theme_ = 999;

    // Streets rendering (3D ENU)
    GLuint streets_vao_ = 0, streets_vbo_ = 0;
    std::vector<float> streets_verts_;
    GLsizei streets_cached_count_ = 0;
    uint32_t streets_cached_gen_ = 0;

    // Minimap rendering
    GLuint minimap_vao_ = 0, minimap_vbo_ = 0;
    std::vector<float> minimap_verts_;
    GLsizei minimap_cached_count_ = 0;
    uint32_t minimap_cached_streets_gen_ = 0;
    uint32_t minimap_cached_buildings_gen_ = 0;
    int minimap_cached_w_ = 0, minimap_cached_h_ = 0;
    float minimap_cached_range_ = 0;
    float minimap_range_ = 8000.0f;

    // Generic 2D colored batch buffer (pos2 + color4 = 6 floats per vert)
    GLuint batch2d_vao_ = 0, batch2d_vbo_ = 0;

    // Celestial billboard buffer (pos3 + uv2 = 5 floats per vert, 6 verts per body)
    GLuint celestial_vao_ = 0, celestial_vbo_ = 0;

    // Aircraft batch buffers (persistent across frames to avoid heap churn)
    std::vector<float> ac_fill_verts_;
    std::vector<float> ac_outline_verts_;
    std::vector<float> ac_trail_verts_;
    std::vector<float> ac_pred_verts_;
    std::vector<float> ac_transit_verts_;

    // Minimap zoom button rects (framebuffer coords)
    float mm_zin_x0_ = 0, mm_zin_y0_ = 0, mm_zin_x1_ = 0, mm_zin_y1_ = 0;
    float mm_zout_x0_ = 0, mm_zout_y0_ = 0, mm_zout_x1_ = 0, mm_zout_y1_ = 0;

    // Bitmap font
    GLuint font_texture_ = 0;
    GLuint font_vao_ = 0, font_vbo_ = 0;

    // UI state
    bool show_help_ = false;
    bool show_settings_ = false;
    float help_btn_x0_ = 0, help_btn_y0_ = 0, help_btn_x1_ = 0, help_btn_y1_ = 0;
    float settings_btn_x0_ = 0, settings_btn_y0_ = 0, settings_btn_x1_ = 0, settings_btn_y1_ = 0;
    float theme_btn_x0_ = 0, theme_btn_y0_ = 0, theme_btn_x1_ = 0, theme_btn_y1_ = 0;

    int building_theme_ = 0;
    static constexpr int NUM_BUILDING_THEMES = 6;

    // Settings panel state
    double slider_values_[NUM_SLIDERS] = {};
    bool slider_values_init_ = false;
    int settings_scroll_ = 0;
    int selected_slider_ = -1;
    struct SliderBtnRect { float x0, y0, x1, y1; };
    SliderBtnRect slider_minus_[NUM_SLIDERS] = {};
    SliderBtnRect slider_plus_[NUM_SLIDERS] = {};
    SliderBtnRect slider_bar_[NUM_SLIDERS] = {};

    void init_shaders();
    void init_geometry();
    void init_font();
    void draw_help_tooltip();

    // Hover tooltip state (set by draw_aircraft, drawn by draw_hover_tooltip)
    bool hover_active_ = false;
    float hover_screen_x_ = 0, hover_screen_y_ = 0;
    std::string hover_callsign_;
    std::string hover_operator_;
    std::string hover_type_;
    std::string hover_reg_;
    std::string hover_icao24_;
    double hover_alt_ft_ = 0;
    float hover_velocity_ = 0;
    float hover_heading_ = 0;
    double hover_dist_km_ = 0;
    float hover_vert_rate_ = 0;
    std::string hover_origin_;
    std::string hover_destination_;
    bool hover_detail_loading_ = false;
};
