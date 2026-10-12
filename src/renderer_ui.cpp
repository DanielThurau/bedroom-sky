#include "renderer.h"
#include <cmath>
#include <cstring>

// Liang-Barsky line clipping to axis-aligned rectangle
static bool clip_line_to_rect(float& x0, float& y0, float& x1, float& y1,
                              float xmin, float ymin, float xmax, float ymax) {
    float dx = x1 - x0, dy = y1 - y0;
    float p[4] = {-dx, dx, -dy, dy};
    float q[4] = {x0 - xmin, xmax - x0, y0 - ymin, ymax - y0};
    float t0 = 0.0f, t1 = 1.0f;
    for (int i = 0; i < 4; i++) {
        if (std::abs(p[i]) < 1e-8f) {
            if (q[i] < 0.0f) return false;
        } else {
            float t = q[i] / p[i];
            if (p[i] < 0) { if (t > t0) t0 = t; }
            else           { if (t < t1) t1 = t; }
        }
    }
    if (t0 > t1) return false;
    float nx0 = x0 + t0 * dx, ny0 = y0 + t0 * dy;
    float nx1 = x0 + t1 * dx, ny1 = y0 + t1 * dy;
    x0 = nx0; y0 = ny0; x1 = nx1; y1 = ny1;
    return true;
}

void Renderer::draw_minimap(const Streets& streets, const Buildings& buildings,
                            const ObserverConfig& observer, const Config& config) {
    float s = dpi_scale_;
    float map_size = 200.0f * s;
    float pad = 10.0f * s;
    float range = minimap_range_;

    // Bottom-right corner
    float map_x = width_ - pad - map_size;
    float map_y = pad;

    // Background
    draw_filled_rect(map_x, map_y, map_size, map_size,
                     glm::vec4(0.02f, 0.02f, 0.06f, 0.85f));

    uint32_t st_gen = streets.generation();
    uint32_t bl_gen = buildings.generation();

    // Static geometry cache: only rebuild when data, window size, or zoom changes
    // (NOT when heading/fov changes -- the view cone is drawn separately)
    bool cache_hit =
        minimap_cached_count_ > 0 &&
        st_gen == minimap_cached_streets_gen_ &&
        bl_gen == minimap_cached_buildings_gen_ &&
        width_ == minimap_cached_w_ && height_ == minimap_cached_h_ &&
        range == minimap_cached_range_;

    if (cache_hit) {
        glUseProgram(terrain_shader_);
        glBindVertexArray(minimap_vao_);
        glDrawArrays(GL_LINES, 0, minimap_cached_count_);
    } else {
        minimap_cached_streets_gen_ = st_gen;
        minimap_cached_buildings_gen_ = bl_gen;
        minimap_cached_w_ = width_;
        minimap_cached_h_ = height_;
        minimap_cached_range_ = range;

        minimap_verts_.clear();

        float mx0 = map_x, my0 = map_y;
        float mx1 = map_x + map_size, my1 = map_y + map_size;

        auto enu_to_px = [&](float east, float north) -> std::pair<float, float> {
            return {map_x + (east / (2.0f * range) + 0.5f) * map_size,
                    map_y + (north / (2.0f * range) + 0.5f) * map_size};
        };

        auto push_line = [&](float px0, float py0, float px1, float py1,
                             float r, float g, float b, float a) {
            if (!clip_line_to_rect(px0, py0, px1, py1, mx0, my0, mx1, my1))
                return;
            float nx0 = px0 / width_ * 2.0f - 1.0f;
            float ny0 = py0 / height_ * 2.0f - 1.0f;
            float nx1 = px1 / width_ * 2.0f - 1.0f;
            float ny1 = py1 / height_ * 2.0f - 1.0f;
            minimap_verts_.push_back(nx0); minimap_verts_.push_back(ny0);
            minimap_verts_.push_back(r);   minimap_verts_.push_back(g);
            minimap_verts_.push_back(b);   minimap_verts_.push_back(a);
            minimap_verts_.push_back(nx1); minimap_verts_.push_back(ny1);
            minimap_verts_.push_back(r);   minimap_verts_.push_back(g);
            minimap_verts_.push_back(b);   minimap_verts_.push_back(a);
        };

        // Early-cull threshold: skip geometry entirely outside visible range
        float cull_range = range * 1.5f;

        // Streets
        if (streets.is_ready()) {
            for (const auto& st : streets.streets()) {
                if (st.enu_points.size() < 2) continue;
                for (size_t i = 0; i + 1 < st.enu_points.size(); i++) {
                    float ex = st.enu_points[i].x, ey = st.enu_points[i].y;
                    if (std::abs(ex) > cull_range || std::abs(ey) > cull_range) continue;
                    auto [px0, py0] = enu_to_px(ex, ey);
                    auto [px1, py1] = enu_to_px(st.enu_points[i+1].x, st.enu_points[i+1].y);
                    push_line(px0, py0, px1, py1, 0.15f, 0.2f, 0.35f, 0.5f);
                }
            }
        }

        // Building footprints (ground outline only)
        // LOD: when zoomed out, skip small buildings (they're subpixel)
        int min_verts = (range > 5000.0f) ? 6 : (range > 2000.0f) ? 4 : 3;
        if (buildings.is_ready()) {
            for (const auto& b : buildings.buildings()) {
                if ((int)b.enu_base.size() < min_verts) continue;
                // Cull entire building by centroid distance
                float cx = 0, cy = 0;
                for (const auto& v : b.enu_base) { cx += v.x; cy += v.y; }
                cx /= b.enu_base.size(); cy /= b.enu_base.size();
                if (std::abs(cx) > cull_range || std::abs(cy) > cull_range) continue;

                int n = (int)b.enu_base.size();
                for (int i = 0; i < n; i++) {
                    int j = (i + 1) % n;
                    auto [px0, py0] = enu_to_px(b.enu_base[i].x, b.enu_base[i].y);
                    auto [px1, py1] = enu_to_px(b.enu_base[j].x, b.enu_base[j].y);
                    push_line(px0, py0, px1, py1, 0.3f, 0.22f, 0.1f, 0.5f);
                }
            }
        }

        // Landmark crosshairs
        for (int li = 0; li < NUM_LANDMARKS; li++) {
            const auto& lm = LANDMARKS[li];
            glm::dvec3 enu = geodetic_to_enu(lm.lat, lm.lon, lm.alt_m,
                                              observer.lat_deg, observer.lon_deg, observer.alt_m);
            auto [px, py] = enu_to_px((float)enu.x, (float)enu.y);
            if (px < mx0 - 20*s || px > mx1 + 20*s ||
                py < my0 - 20*s || py > my1 + 20*s) continue;
            float csz = 4.0f * s;
            push_line(px - csz, py, px + csz, py, 1.0f, 0.6f, 0.2f, 0.7f);
            push_line(px, py - csz, px, py + csz, 1.0f, 0.6f, 0.2f, 0.7f);
        }

        // Border
        auto push_border = [&](float px0, float py0, float px1, float py1) {
            float nx0 = px0 / width_ * 2.0f - 1.0f;
            float ny0 = py0 / height_ * 2.0f - 1.0f;
            float nx1 = px1 / width_ * 2.0f - 1.0f;
            float ny1 = py1 / height_ * 2.0f - 1.0f;
            minimap_verts_.push_back(nx0); minimap_verts_.push_back(ny0);
            minimap_verts_.push_back(0.2f); minimap_verts_.push_back(0.3f);
            minimap_verts_.push_back(0.5f); minimap_verts_.push_back(0.6f);
            minimap_verts_.push_back(nx1); minimap_verts_.push_back(ny1);
            minimap_verts_.push_back(0.2f); minimap_verts_.push_back(0.3f);
            minimap_verts_.push_back(0.5f); minimap_verts_.push_back(0.6f);
        };
        push_border(mx0, my0, mx1, my0);
        push_border(mx1, my0, mx1, my1);
        push_border(mx1, my1, mx0, my1);
        push_border(mx0, my1, mx0, my0);

        // Upload static geometry
        minimap_cached_count_ = (GLsizei)(minimap_verts_.size() / 6);
        if (minimap_cached_count_ > 0) {
            glUseProgram(terrain_shader_);
            glBindVertexArray(minimap_vao_);
            glBindBuffer(GL_ARRAY_BUFFER, minimap_vbo_);
            glBufferData(GL_ARRAY_BUFFER, minimap_verts_.size() * sizeof(float),
                         minimap_verts_.data(), GL_STREAM_DRAW);
            glDrawArrays(GL_LINES, 0, minimap_cached_count_);
        }
    } // end cache miss rebuild

    // --- Dynamic overlay (drawn every frame, cheap) ---

    // View cone -- just 2 lines via marker_shader_, no batching needed
    {
        float heading_rad = glm::radians((float)config.heading_deg);
        float half_fov_rad = glm::radians((float)config.fov_deg * 0.5f);
        float cone_len = map_size * 0.4f;
        float cx = map_x + map_size * 0.5f;
        float cy = map_y + map_size * 0.5f;

        float mx0 = map_x, my0 = map_y;
        float mx1 = map_x + map_size, my1 = map_y + map_size;

        auto cone_line = [&](float angle) {
            float ex = cx + sinf(angle) * cone_len;
            float ey = cy + cosf(angle) * cone_len;
            float lx0 = cx, ly0 = cy, lx1 = ex, ly1 = ey;
            if (!clip_line_to_rect(lx0, ly0, lx1, ly1, mx0, my0, mx1, my1)) return;
            float verts[] = {
                lx0 / width_ * 2.0f - 1.0f, ly0 / height_ * 2.0f - 1.0f,
                lx1 / width_ * 2.0f - 1.0f, ly1 / height_ * 2.0f - 1.0f
            };
            glUseProgram(marker_shader_);
            glm::mat4 identity(1.0f);
            glUniformMatrix4fv(marker_uTransform_,
                               1, GL_FALSE, glm::value_ptr(identity));
            glUniform4f(marker_uColor_,
                        0.3f, 0.8f, 1.0f, 0.6f);
            glBindVertexArray(marker_vao_);
            glBindBuffer(GL_ARRAY_BUFFER, marker_vbo_);
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
            glDrawArrays(GL_LINES, 0, 2);
        };
        cone_line(heading_rad - half_fov_rad);
        cone_line(heading_rad + half_fov_rad);
    }

    // Observer dot at center
    {
        float cx = map_x + map_size * 0.5f;
        float cy = map_y + map_size * 0.5f;
        float dot = 3.0f * s;
        draw_filled_rect(cx - dot, cy - dot, dot * 2, dot * 2,
                         glm::vec4(1.0f, 1.0f, 1.0f, 0.9f));
    }

    // "N" compass label at top center
    draw_text("N", map_x + map_size * 0.5f - 4 * s, map_y + map_size - 14 * s,
              1.0f, glm::vec3(0.6f, 0.7f, 0.9f));

    // Landmark labels
    {
        auto enu_to_px_label = [&](float east, float north) -> std::pair<float, float> {
            return {map_x + (east / (2.0f * range) + 0.5f) * map_size,
                    map_y + (north / (2.0f * range) + 0.5f) * map_size};
        };
        for (int li = 0; li < NUM_LANDMARKS; li++) {
            const auto& lm = LANDMARKS[li];
            glm::dvec3 enu = geodetic_to_enu(lm.lat, lm.lon, lm.alt_m,
                                              observer.lat_deg, observer.lon_deg, observer.alt_m);
            auto [px, py] = enu_to_px_label((float)enu.x, (float)enu.y);
            if (px < map_x || px > map_x + map_size ||
                py < map_y || py > map_y + map_size) continue;
            draw_text(lm.name, px + 5 * s, py - 3 * s, 1.0f,
                      glm::vec3(0.8f, 0.5f, 0.2f));
        }
    }

    // Scale indicator at bottom-left of minimap
    {
        char scale_buf[32];
        if (range >= 1000.0f)
            snprintf(scale_buf, sizeof(scale_buf), "%.0fkm", range / 1000.0f);
        else
            snprintf(scale_buf, sizeof(scale_buf), "%.0fm", range);
        draw_text(scale_buf, map_x + 4 * s, map_y + 4 * s, 1.0f,
                  glm::vec3(0.4f, 0.45f, 0.55f));
    }

    // Zoom +/- buttons at bottom-right of minimap
    {
        float btn_sz = 20.0f * s;
        float btn_gap = 4.0f * s;
        float char_px = 8.0f * s;
        float bx = map_x + map_size - btn_sz - 4 * s;
        float by_plus = map_y + 4 * s + btn_sz + btn_gap;
        float by_minus = map_y + 4 * s;

        mm_zin_x0_ = bx; mm_zin_y0_ = by_plus;
        mm_zin_x1_ = bx + btn_sz; mm_zin_y1_ = by_plus + btn_sz;
        mm_zout_x0_ = bx; mm_zout_y0_ = by_minus;
        mm_zout_x1_ = bx + btn_sz; mm_zout_y1_ = by_minus + btn_sz;

        draw_filled_rect(bx, by_plus, btn_sz, btn_sz,
                         glm::vec4(0.1f, 0.15f, 0.25f, 0.8f));
        draw_text("+", bx + (btn_sz - char_px) * 0.5f,
                  by_plus + (btn_sz - char_px) * 0.5f, 1.0f,
                  glm::vec3(0.7f, 0.85f, 1.0f));

        draw_filled_rect(bx, by_minus, btn_sz, btn_sz,
                         glm::vec4(0.1f, 0.15f, 0.25f, 0.8f));
        draw_text("-", bx + (btn_sz - char_px) * 0.5f,
                  by_minus + (btn_sz - char_px) * 0.5f, 1.0f,
                  glm::vec3(0.7f, 0.85f, 1.0f));
    }
}

void Renderer::draw_hud(const ObserverConfig& observer, int aircraft_count, double fetch_time,
                        bool show_location, bool demo_mode) {
    float s = dpi_scale_;
    float line_h = 18.0f * s;
    float margin = 10.0f * s;

    char buf[256];

    // Top-left: status info
    snprintf(buf, sizeof(buf), "%s", demo_mode ? "Bedroom Sky - PUBLIC DEMO (SFO)" :
                                               "Bedroom Sky - Live Flight Viewer");
    draw_text(buf, margin, height_ - margin - line_h, 1.0f, glm::vec3(0.35f, 0.45f, 0.6f));

    if (show_location) {
        snprintf(buf, sizeof(buf), "Looking from (%.4f, %.4f) heading %.0f",
                 observer.lat_deg, observer.lon_deg, observer.heading_deg);
    } else {
        snprintf(buf, sizeof(buf), "Observer location hidden");
    }
    draw_text(buf, margin, height_ - margin - line_h * 2, 1.0f, glm::vec3(0.4f, 0.4f, 0.5f));

    snprintf(buf, sizeof(buf), "HDG %.0f  PITCH %.0f  FOV %.0f",
             observer.heading_deg, observer.pitch_deg, observer.fov_deg);
    draw_text(buf, margin, height_ - margin - line_h * 3, 1.0f, glm::vec3(0.5f, 0.5f, 0.6f));

    snprintf(buf, sizeof(buf), "Aircraft: %d  (%.1fs)", aircraft_count, fetch_time);
    draw_text(buf, margin, height_ - margin - line_h * 4, 1.0f, glm::vec3(0.5f, 0.5f, 0.6f));

    // Bottom-left: controls
    snprintf(buf, sizeof(buf), "[AD] look  [WS] pitch  [ZX] zoom  [scroll] pan");
    draw_text(buf, margin, margin + line_h * 2, 1.0f, glm::vec3(0.3f, 0.3f, 0.4f));

    snprintf(buf, sizeof(buf), "[R] reset view  [scroll] pan  [Ctrl+C] quit");
    draw_text(buf, margin, margin + line_h, 1.0f, glm::vec3(0.3f, 0.3f, 0.4f));

    snprintf(buf, sizeof(buf), "[TAB] settings  [S] settings  [?] help");
    draw_text(buf, margin, margin, 1.0f, glm::vec3(0.3f, 0.3f, 0.4f));

    // Top-right buttons: [Settings] [?]
    float btn_size = 24.0f * s;
    float btn_gap = 6.0f * s;
    float char_sz = 8.0f * s;

    // "?" button
    float help_x = width_ - margin - btn_size;
    float help_y = height_ - margin - btn_size;
    help_btn_x0_ = help_x; help_btn_y0_ = help_y;
    help_btn_x1_ = help_x + btn_size; help_btn_y1_ = help_y + btn_size;

    draw_filled_rect(help_x, help_y, btn_size, btn_size,
        show_help_ ? glm::vec4(0.2f,0.5f,0.7f,0.9f) : glm::vec4(0.15f,0.2f,0.3f,0.7f));
    draw_text("?", help_x + (btn_size - char_sz) * 0.5f,
              help_y + (btn_size - char_sz) * 0.5f, 1.0f, glm::vec3(0.8f,0.9f,1.0f));

    // Settings gear button (just shows "S" for now)
    float set_x = help_x - btn_gap - btn_size;
    float set_y = help_y;
    settings_btn_x0_ = set_x; settings_btn_y0_ = set_y;
    settings_btn_x1_ = set_x + btn_size; settings_btn_y1_ = set_y + btn_size;

    draw_filled_rect(set_x, set_y, btn_size, btn_size,
        show_settings_ ? glm::vec4(0.2f,0.5f,0.4f,0.9f) : glm::vec4(0.15f,0.2f,0.3f,0.7f));
    draw_text("S", set_x + (btn_size - char_sz) * 0.5f,
              set_y + (btn_size - char_sz) * 0.5f, 1.0f, glm::vec3(0.8f,0.9f,1.0f));

    // Theme cycle button -- shows current theme abbreviation
    static const char* theme_labels[] = {"A", "N", "B", "E", "G", "X"};
    static const char* theme_names[]  = {"Amber", "Neon", "Blueprint", "Ember", "Ghost", "Matrix"};

    float theme_x = set_x - btn_gap - btn_size;
    float theme_y = set_y;
    theme_btn_x0_ = theme_x; theme_btn_y0_ = theme_y;
    theme_btn_x1_ = theme_x + btn_size; theme_btn_y1_ = theme_y + btn_size;

    draw_filled_rect(theme_x, theme_y, btn_size, btn_size,
                     glm::vec4(0.15f, 0.2f, 0.3f, 0.7f));
    draw_text(theme_labels[building_theme_],
              theme_x + (btn_size - char_sz) * 0.5f,
              theme_y + (btn_size - char_sz) * 0.5f, 1.0f, glm::vec3(0.8f,0.9f,1.0f));

    // Theme name label below the button
    draw_text(theme_names[building_theme_],
              theme_x - (strlen(theme_names[building_theme_]) - 1) * 4 * s,
              theme_y - 14 * s, 1.0f, glm::vec3(0.4f, 0.45f, 0.55f));

    if (show_help_) draw_help_tooltip();
}

// ---------------------------------------------------------------------------
// draw_transits_panel — upcoming sun/moon transits, top-left below HUD.
// ---------------------------------------------------------------------------

void Renderer::draw_transits_panel(const TransitPredictor& predictor) {
    const auto& transits = predictor.transits();
    if (transits.empty()) return;

    float s = dpi_scale_;
    float line_h = 14.0f * s;
    float pad = 8.0f * s;
    float margin = 10.0f * s;

    int n_show = (int)std::min((size_t)4, transits.size());

    // Compute panel dims
    float panel_w = 260.0f * s;
    float header_h = 16.0f * s;
    float panel_h = header_h + n_show * line_h + pad * 2;

    // Position: below the top-left HUD block (4 lines + header)
    float top_hud_bottom = height_ - margin - 18.0f * s * 5;
    float px = margin;
    float py = top_hud_bottom - panel_h - 10.0f * s;
    if (py < margin) py = margin;

    draw_filled_rect(px, py, panel_w, panel_h,
                     glm::vec4(0.04f, 0.05f, 0.10f, 0.82f));

    // Header
    draw_text("UPCOMING TRANSITS", px + pad, py + panel_h - header_h,
              1.0f, glm::vec3(0.55f, 0.65f, 0.85f));

    char buf[128];
    for (int i = 0; i < n_show; i++) {
        const Transit& tr = transits[i];

        glm::vec3 color;
        const char* tag;
        if (tr.body == CelestialBody::Sun) {
            tag = "SUN";
            color = glm::vec3(1.0f, 0.85f, 0.35f);
        } else {
            tag = "MOO";
            color = glm::vec3(0.75f, 0.88f, 1.0f);
        }

        double t = std::max(0.0, tr.time_to_transit_s);
        int mins = (int)(t / 60.0);
        int secs = (int)(t - mins * 60.0);

        std::string cs = tr.callsign.empty() ? tr.plane_id : tr.callsign;
        while (!cs.empty() && cs.back() == ' ') cs.pop_back();
        if (cs.size() > 8) cs.resize(8);

        snprintf(buf, sizeof(buf), "%s %2dm%02ds %-8s %3.0f/%2.0f d%.2f",
                 tag, mins, secs, cs.c_str(),
                 tr.body_az_deg, tr.body_el_deg, tr.closest_sep_deg);

        float row_y = py + panel_h - header_h - (i + 1) * line_h - 2 * s;
        draw_text(buf, px + pad, row_y, 1.0f, color);
    }
}

void Renderer::draw_help_tooltip() {
    float s = dpi_scale_;
    float line_h = 14.0f * s;
    float pad = 12.0f * s;
    float tooltip_w = 340.0f * s;

    // Tooltip lines
    const char* lines[] = {
        "=== HUD Guide ===",
        "",
        "HDG 114 - Compass heading (degrees)",
        "  Direction you are looking.",
        "  0 = North, 90 = East, 180 = South.",
        "",
        "PITCH 5 - Angle above horizon",
        "  0 = level, 90 = straight up.",
        "",
        "FOV 60 - Field of view (degrees)",
        "  Zoom level. Lower = more zoom.",
        "",
        "Aircraft: 24 (0.8s)",
        "  Tracked planes (API fetch time).",
        "",
        "--- Plane Labels ---",
        "UAL123 35000ft 450kt",
        "  Callsign, altitude, ground speed.",
        "",
        "--- Marker Shape ---",
        "Diamond points in climb/descent dir:",
        "  ^ tall top = climbing",
        "  v tall bottom = descending",
        "  <> symmetric = level flight",
        "",
        "--- Marker Color ---",
        "Hue shifts with acceleration:",
        "  Green-white = accelerating",
        "  Cyan (default) = steady",
        "  Red = decelerating",
        "",
        "--- Horizon ---",
        "Bearing ticks every 10 degrees.",
        "N/E/S/W at cardinal directions.",
        "Terrain silhouette = visible ridges.",
        "",
        "Click ? again to close.",
    };
    int num_lines = sizeof(lines) / sizeof(lines[0]);

    float tooltip_h = num_lines * line_h + pad * 2;

    // Position: anchored to top-right, below the ? button
    float tx = width_ - pad - tooltip_w - 10.0f * s;
    float ty = height_ - (34.0f * s) - tooltip_h;

    // Background
    draw_filled_rect(tx, ty, tooltip_w, tooltip_h,
                     glm::vec4(0.05f, 0.07f, 0.12f, 0.92f));

    // Border
    float bx0 = (tx / width_) * 2.0f - 1.0f;
    float by0 = (ty / height_) * 2.0f - 1.0f;
    float bx1 = ((tx + tooltip_w) / width_) * 2.0f - 1.0f;
    float by1 = ((ty + tooltip_h) / height_) * 2.0f - 1.0f;
    float border[] = { bx0,by0, bx1,by0, bx1,by1, bx0,by1 };

    glUseProgram(marker_shader_);
    glm::mat4 identity(1.0f);
    glUniformMatrix4fv(marker_uTransform_,
                       1, GL_FALSE, glm::value_ptr(identity));
    glUniform4f(marker_uColor_,
                0.2f, 0.4f, 0.6f, 0.8f);
    glBindVertexArray(marker_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, marker_vbo_);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(border), border);
    glDrawArrays(GL_LINE_LOOP, 0, 4);

    // Text lines
    for (int i = 0; i < num_lines; i++) {
        float lx = tx + pad;
        float ly = ty + tooltip_h - pad - line_h * (i + 1);
        bool is_header = (lines[i][0] == '=' || lines[i][0] == '-');
        bool is_example = (i == 12 || i == 16); // "Aircraft: 24" and "UAL123" lines
        glm::vec3 color;
        if (is_header)
            color = glm::vec3(0.3f, 0.6f, 0.9f);
        else if (is_example)
            color = glm::vec3(0.1f, 0.9f, 0.7f);
        else
            color = glm::vec3(0.6f, 0.65f, 0.75f);

        draw_text(lines[i], lx, ly, 1.0f, color);
    }
}

bool Renderer::draw_settings_panel(Config& config) {
    if (!show_settings_) return false;

    // Init slider values from config on first open
    if (!slider_values_init_) {
        config_to_values(config, slider_values_);
        slider_values_init_ = true;
    }
    // Don't sync keyboard values here -- let sliders be authoritative
    // when panel is open. Keyboard input is disabled when panel is open anyway.

    float s = dpi_scale_;
    float pad = 10.0f * s;
    float row_h = 20.0f * s;
    float char_px = 8.0f * s;
    float panel_w = 420.0f * s;
    float btn_w = 20.0f * s;
    float bar_h = 10.0f * s;

    // Section headers
    struct Section { int start_idx; const char* title; };
    Section sections[] = {
        {0,  "-- Observer --"},
        {6,  "-- Filters --"},
        {10, "-- Style --"},
        {17, "-- API --"},
    };
    int num_sections = 4;

    // Visible rows limited to fit screen, scrollable
    int total_rows = NUM_SLIDERS + num_sections;
    int max_visible_rows = (int)((height_ * 0.75f - pad * 2) / row_h);
    if (max_visible_rows < 5) max_visible_rows = 5;
    int visible_rows = std::min(total_rows, max_visible_rows);
    float panel_h = visible_rows * row_h + pad * 2;

    // Clamp scroll
    int max_scroll = std::max(0, total_rows - max_visible_rows);
    if (settings_scroll_ < 0) settings_scroll_ = 0;
    if (settings_scroll_ > max_scroll) settings_scroll_ = max_scroll;

    // Add space for description bar at bottom
    float desc_h = (selected_slider_ >= 0) ? row_h * 2 + pad : 0;
    float total_panel_h = panel_h + desc_h;

    // Panel position: left side
    float px = pad;
    float py = height_ - 40.0f * s - total_panel_h;

    // Background (including description area)
    draw_filled_rect(px, py, panel_w, total_panel_h, glm::vec4(0.04f, 0.06f, 0.10f, 0.94f));

    // Description bar at bottom if a slider is selected
    if (selected_slider_ >= 0 && selected_slider_ < NUM_SLIDERS) {
        float desc_y = py + pad * 0.5f;
        // Highlight bar
        draw_filled_rect(px + pad * 0.5f, desc_y, panel_w - pad, desc_h - pad * 0.5f,
                         glm::vec4(0.08f, 0.10f, 0.16f, 0.9f));
        // Slider name
        draw_text(SLIDER_DEFS[selected_slider_].label,
                  px + pad, desc_y + row_h + 2*s, 1.0f,
                  glm::vec3(0.1f, 0.9f, 0.7f));
        // Description text
        draw_text(SLIDER_DEFS[selected_slider_].desc,
                  px + pad, desc_y + 2*s, 1.0f,
                  glm::vec3(0.5f, 0.55f, 0.65f));
    }

    // Sliders area starts above description
    float sliders_py = py + desc_h;

    // Border
    float bx0 = (px / width_) * 2.0f - 1.0f;
    float by0 = (py / height_) * 2.0f - 1.0f;
    float bx1 = ((px + panel_w) / width_) * 2.0f - 1.0f;
    float by1 = ((py + panel_h) / height_) * 2.0f - 1.0f;
    float border[] = {bx0,by0, bx1,by0, bx1,by1, bx0,by1};
    glUseProgram(marker_shader_);
    glm::mat4 identity(1.0f);
    glUniformMatrix4fv(marker_uTransform_,
                       1, GL_FALSE, glm::value_ptr(identity));
    glUniform4f(marker_uColor_, 0.15f, 0.3f, 0.5f, 0.8f);
    glBindVertexArray(marker_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, marker_vbo_);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(border), border);
    glDrawArrays(GL_LINE_LOOP, 0, 4);

    // Draw scroll indicator
    if (total_rows > max_visible_rows) {
        char scroll_buf[64];
        snprintf(scroll_buf, sizeof(scroll_buf), "[scroll] %d/%d  [up/dn]",
                 settings_scroll_ + 1, max_scroll + 1);
        draw_text(scroll_buf, px + pad, py - 14 * s, 1.0f, glm::vec3(0.3f, 0.5f, 0.7f));
    }

    // Draw rows
    int visual_row = 0;  // absolute row index (including headers)
    int section_idx = 0;
    bool changed = false;

    float label_w = 120.0f * s;
    float value_w = 80.0f * s;

    for (int i = 0; i < NUM_SLIDERS; i++) {
        // Insert section header if needed
        if (section_idx < num_sections && sections[section_idx].start_idx == i) {
            int display_row = visual_row - settings_scroll_;
            if (display_row >= 0 && display_row < visible_rows) {
                float ry = sliders_py + panel_h - pad - row_h * (display_row + 1);
                draw_text(sections[section_idx].title,
                          px + pad, ry + 2 * s, 1.0f,
                          glm::vec3(0.3f, 0.6f, 0.9f));
            }
            visual_row++;
            section_idx++;
        }

        int display_row = visual_row - settings_scroll_;
        visual_row++;

        // Skip if outside visible scroll window
        if (display_row < 0 || display_row >= visible_rows) {
            slider_minus_[i] = {0,0,0,0};
            slider_plus_[i] = {0,0,0,0};
            slider_bar_[i] = {0,0,0,0};
            continue;
        }

        float ry = sliders_py + panel_h - pad - row_h * (display_row + 1);

        const auto& def = SLIDER_DEFS[i];

        // Label
        draw_text(def.label, px + pad, ry + 2 * s, 1.0f,
                  glm::vec3(0.55f, 0.60f, 0.70f));

        // Value text
        char val_buf[32];
        snprintf(val_buf, sizeof(val_buf), def.fmt, slider_values_[i]);
        char full_buf[48];
        snprintf(full_buf, sizeof(full_buf), "%s%s", val_buf, def.unit);
        draw_text(full_buf, px + pad + label_w, ry + 2 * s, 1.0f,
                  glm::vec3(0.1f, 0.9f, 0.7f));

        // [-] button
        float minus_x = px + pad + label_w + value_w;
        slider_minus_[i] = {minus_x, ry, minus_x + btn_w, ry + row_h};
        draw_filled_rect(minus_x, ry + 2*s, btn_w, bar_h + 4*s,
                         glm::vec4(0.2f, 0.15f, 0.15f, 0.8f));
        draw_text("-", minus_x + (btn_w - char_px) * 0.5f, ry + 2*s, 1.0f,
                  glm::vec3(1.0f, 0.5f, 0.5f));

        // Slider bar
        float bar_x = minus_x + btn_w + 4 * s;
        float bar_w = panel_w - pad * 2 - label_w - value_w - btn_w * 2 - 12 * s;
        slider_bar_[i] = {bar_x, ry, bar_x + bar_w, ry + row_h};

        // Bar background
        draw_filled_rect(bar_x, ry + (row_h - bar_h) * 0.5f, bar_w, bar_h,
                         glm::vec4(0.1f, 0.12f, 0.18f, 0.9f));

        // Bar fill
        double range = def.max_val - def.min_val;
        float t = (range > 0) ? (float)((slider_values_[i] - def.min_val) / range) : 0.0f;
        t = std::max(0.0f, std::min(1.0f, t));
        if (bar_w * t > 1.0f) {
            draw_filled_rect(bar_x, ry + (row_h - bar_h) * 0.5f,
                             bar_w * t, bar_h,
                             glm::vec4(0.1f, 0.4f, 0.5f, 0.8f));
        }

        // Bar thumb
        float thumb_x = bar_x + bar_w * t - 2 * s;
        draw_filled_rect(thumb_x, ry + (row_h - bar_h) * 0.5f - 2*s,
                         4 * s, bar_h + 4*s,
                         glm::vec4(0.2f, 0.8f, 0.7f, 0.9f));

        // [+] button
        float plus_x = bar_x + bar_w + 4 * s;
        slider_plus_[i] = {plus_x, ry, plus_x + btn_w, ry + row_h};
        draw_filled_rect(plus_x, ry + 2*s, btn_w, bar_h + 4*s,
                         glm::vec4(0.15f, 0.2f, 0.15f, 0.8f));
        draw_text("+", plus_x + (btn_w - char_px) * 0.5f, ry + 2*s, 1.0f,
                  glm::vec3(0.5f, 1.0f, 0.5f));
    }

    // Write slider values back to config
    Config prev = config;
    values_to_config(slider_values_, config);

    // Detect changes
    changed = (config.lat_deg != prev.lat_deg || config.lon_deg != prev.lon_deg ||
               config.alt_m != prev.alt_m || config.heading_deg != prev.heading_deg ||
               config.pitch_deg != prev.pitch_deg || config.fov_deg != prev.fov_deg ||
               config.min_alt_ft != prev.min_alt_ft || config.max_alt_ft != prev.max_alt_ft ||
               config.min_dist_km != prev.min_dist_km || config.max_dist_km != prev.max_dist_km ||
               config.poll_interval_s != prev.poll_interval_s ||
               config.flight_radius_nm != prev.flight_radius_nm);

    return changed;
}

void Renderer::draw_text(const std::string& text, float x, float y, float scale, glm::vec3 color) {
    if (text.empty()) return;

    glUseProgram(text_shader_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, font_texture_);
    glUniform1i(text_uFont_, 0);
    glUniform3fv(text_uColor_, 1, glm::value_ptr(color));

    glm::mat4 ortho = glm::ortho(0.0f, (float)width_, 0.0f, (float)height_);
    glUniformMatrix4fv(text_uProj_, 1, GL_FALSE, glm::value_ptr(ortho));

    const int char_w = 8, char_h = 8;
    const int num_chars = 96;
    const float tex_w = (float)(num_chars * char_w); // 768
    const float tex_h = (float)char_h;               // 8
    // Apply DPI scale so text is legible on Retina displays
    float actual_scale = scale * dpi_scale_;
    float cw = char_w * actual_scale;
    float ch = char_h * actual_scale;

    std::vector<float> vertices;
    vertices.reserve(text.size() * 24);
    float cx = x;
    for (char c : text) {
        int idx = c - 32;
        if (idx < 0 || idx >= 96) { cx += cw; continue; }

        // Single-row texture: each char is at x=[idx*8, (idx+1)*8], y=[0,8]
        float u0 = (idx * char_w) / tex_w;
        float u1 = ((idx + 1) * char_w) / tex_w;
        // v0=0 (bottom), v1=1 (top). Screen y goes up, texture v goes up. Match.
        float v0 = 0.0f;
        float v1 = 1.0f;

        // Two triangles: bottom-left, bottom-right, top-right, top-left
        float verts[] = {
            cx,      y,      u0, v0,
            cx + cw, y,      u1, v0,
            cx + cw, y + ch, u1, v1,
            cx,      y,      u0, v0,
            cx + cw, y + ch, u1, v1,
            cx,      y + ch, u0, v1,
        };
        vertices.insert(vertices.end(), verts, verts + 24);
        cx += cw;
    }

    glBindVertexArray(font_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, font_vbo_);
    glBufferSubData(GL_ARRAY_BUFFER, 0, vertices.size() * sizeof(float), vertices.data());
    glDrawArrays(GL_TRIANGLES, 0, (int)(vertices.size() / 4));
}

void Renderer::draw_filled_rect(float x, float y, float w, float h, glm::vec4 color) {
    // Convert pixel coords to NDC
    float x0 = (x / width_) * 2.0f - 1.0f;
    float y0 = (y / height_) * 2.0f - 1.0f;
    float x1 = ((x + w) / width_) * 2.0f - 1.0f;
    float y1 = ((y + h) / height_) * 2.0f - 1.0f;

    float verts[] = { x0,y0, x1,y0, x1,y1, x0,y0, x1,y1, x0,y1 };

    glUseProgram(marker_shader_);
    glm::mat4 identity(1.0f);
    glUniformMatrix4fv(marker_uTransform_,
                       1, GL_FALSE, glm::value_ptr(identity));
    glUniform4fv(marker_uColor_, 1, &color[0]);

    glBindVertexArray(marker_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, marker_vbo_);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

void Renderer::on_click(float win_x, float win_y) {
    float fb_x = win_x * dpi_scale_;
    float fb_y = height_ - (win_y * dpi_scale_);

    auto hit = [&](float x0, float y0, float x1, float y1) {
        return fb_x >= x0 && fb_x <= x1 && fb_y >= y0 && fb_y <= y1;
    };

    // Help button
    if (hit(help_btn_x0_, help_btn_y0_, help_btn_x1_, help_btn_y1_)) {
        show_help_ = !show_help_;
        if (show_help_) show_settings_ = false;
        return;
    }

    // Settings button
    if (hit(settings_btn_x0_, settings_btn_y0_, settings_btn_x1_, settings_btn_y1_)) {
        show_settings_ = !show_settings_;
        if (show_settings_) show_help_ = false;
        return;
    }

    // Theme cycle button
    if (hit(theme_btn_x0_, theme_btn_y0_, theme_btn_x1_, theme_btn_y1_)) {
        building_theme_ = (building_theme_ + 1) % NUM_BUILDING_THEMES;
        return;
    }

    // Minimap zoom buttons
    if (hit(mm_zin_x0_, mm_zin_y0_, mm_zin_x1_, mm_zin_y1_)) {
        zoom_minimap(1.0);
        return;
    }
    if (hit(mm_zout_x0_, mm_zout_y0_, mm_zout_x1_, mm_zout_y1_)) {
        zoom_minimap(-1.0);
        return;
    }

    // If settings panel is open, check slider buttons
    if (show_settings_) {
        for (int i = 0; i < NUM_SLIDERS; i++) {
            if (hit(slider_minus_[i].x0, slider_minus_[i].y0,
                    slider_minus_[i].x1, slider_minus_[i].y1)) {
                slider_values_[i] = std::max(SLIDER_DEFS[i].min_val,
                    slider_values_[i] - SLIDER_DEFS[i].step);
                selected_slider_ = i;
                return;
            }
            if (hit(slider_plus_[i].x0, slider_plus_[i].y0,
                    slider_plus_[i].x1, slider_plus_[i].y1)) {
                slider_values_[i] = std::min(SLIDER_DEFS[i].max_val,
                    slider_values_[i] + SLIDER_DEFS[i].step);
                selected_slider_ = i;
                return;
            }
            // Click on slider bar: set value proportionally
            if (hit(slider_bar_[i].x0, slider_bar_[i].y0,
                    slider_bar_[i].x1, slider_bar_[i].y1)) {
                float t = (fb_x - slider_bar_[i].x0) /
                          (slider_bar_[i].x1 - slider_bar_[i].x0);
                t = std::max(0.0f, std::min(1.0f, t));
                slider_values_[i] = SLIDER_DEFS[i].min_val +
                    t * (SLIDER_DEFS[i].max_val - SLIDER_DEFS[i].min_val);
                selected_slider_ = i;
                return;
            }
        }
        // Click outside all controls: don't close, let them keep tweaking
        return;
    }

    if (show_help_) {
        show_help_ = false;
    }
}

bool Renderer::is_over_minimap(float win_x, float win_y) const {
    float fb_x = win_x * dpi_scale_;
    float fb_y = height_ - (win_y * dpi_scale_);
    float s = dpi_scale_;
    float map_size = 200.0f * s;
    float pad = 10.0f * s;
    float map_x = width_ - pad - map_size;
    float map_y = pad;
    return fb_x >= map_x && fb_x <= map_x + map_size &&
           fb_y >= map_y && fb_y <= map_y + map_size;
}

void Renderer::zoom_minimap(double delta) {
    // Zoom in = smaller range, zoom out = larger range
    // Multiply by a factor per scroll step for smooth feel
    float factor = (delta > 0) ? 0.8f : 1.25f;
    minimap_range_ *= factor;
    // Clamp: 200m (street-level) to 20km (regional)
    minimap_range_ = std::max(200.0f, std::min(20000.0f, minimap_range_));
}
