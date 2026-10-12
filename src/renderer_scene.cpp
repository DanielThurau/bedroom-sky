#include "renderer.h"
#include "celestial.h"
#include <chrono>
#include <cmath>
#include <cstring>

// ---------------------------------------------------------------------------
// draw_sky — fullscreen quad with gradient
// ---------------------------------------------------------------------------

void Renderer::draw_sky(const ObserverConfig& observer,
                         const glm::mat4& view,
                         const glm::mat4& proj,
                         double utc_unix_s) {
    // Sun/moon positions for the current wall-clock moment
    celestial::AzEl sun = celestial::sun_azel(
        observer.lat_deg, observer.lon_deg, utc_unix_s);
    celestial::AzEl moon = celestial::moon_azel(
        observer.lat_deg, observer.lon_deg, utc_unix_s);
    glm::dvec3 sun_d = celestial::azel_to_enu(sun.az_deg, sun.el_deg);
    glm::dvec3 moon_d = celestial::azel_to_enu(moon.az_deg, moon.el_deg);

    glm::mat4 vp = proj * view;
    glm::mat4 inv_vp = glm::inverse(vp);
    // Camera position: inverse of view applied to origin gives the eye in world coords
    glm::mat4 inv_view = glm::inverse(view);
    glm::vec3 cam_pos = glm::vec3(inv_view[3]);

    glUseProgram(sky_shader_);
    glUniformMatrix4fv(sky_uInvVP_, 1, GL_FALSE, glm::value_ptr(inv_vp));
    glUniform3f(sky_uCamPos_, cam_pos.x, cam_pos.y, cam_pos.z);
    glUniform3f(sky_uSunDir_, (float)sun_d.x, (float)sun_d.y, (float)sun_d.z);
    glUniform1f(sky_uSunEl_, (float)sun.el_deg);
    glUniform3f(sky_uMoonDir_, (float)moon_d.x, (float)moon_d.y, (float)moon_d.z);
    glUniform1f(sky_uMoonEl_, (float)moon.el_deg);

    glBindVertexArray(quad_vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// ---------------------------------------------------------------------------
// draw_celestial — sun and moon as soft-edged billboards in the sky.
// Drawn right after sky so terrain/buildings/planes overlay them naturally.
// ---------------------------------------------------------------------------

void Renderer::draw_celestial(const ObserverConfig& observer,
                               const glm::mat4& view,
                               const glm::mat4& proj,
                               double utc_unix_s) {
    glm::mat4 vp = proj * view;

    auto draw_body = [&](double az_deg, double el_deg,
                         double angular_diameter_deg,
                         glm::vec3 core, glm::vec3 halo,
                         float intensity) {
        if (el_deg < -1.0) return;  // below horizon
        if (intensity <= 0.0f) return;

        const double dist = 200000.0;  // 200 km, well within far_clip of 500km
        glm::dvec3 dir = celestial::azel_to_enu(az_deg, el_deg);
        glm::dvec3 center = dir * dist;

        // Billboard basis: up tangent and east tangent on the celestial sphere
        glm::dvec3 world_up(0.0, 0.0, 1.0);
        glm::dvec3 right = glm::normalize(glm::cross(world_up, dir));
        if (glm::length(right) < 1e-6) right = glm::dvec3(1, 0, 0);
        glm::dvec3 up = glm::normalize(glm::cross(dir, right));

        double half = dist * std::tan(0.5 * angular_diameter_deg * celestial::DEG2RAD);
        // Expand quad slightly so soft halo has room to fade
        double qr = half * 2.2;
        glm::dvec3 r_axis = right * qr;
        glm::dvec3 u_axis = up * qr;

        // 6 verts, pos3 + uv2 (clockwise from bottom-left as two triangles)
        glm::dvec3 c = center;
        glm::dvec3 bl = c - r_axis - u_axis;
        glm::dvec3 br = c + r_axis - u_axis;
        glm::dvec3 tr = c + r_axis + u_axis;
        glm::dvec3 tl = c - r_axis + u_axis;

        float verts[6 * 5] = {
            (float)bl.x, (float)bl.y, (float)bl.z, -1.0f, -1.0f,
            (float)br.x, (float)br.y, (float)br.z,  1.0f, -1.0f,
            (float)tr.x, (float)tr.y, (float)tr.z,  1.0f,  1.0f,
            (float)bl.x, (float)bl.y, (float)bl.z, -1.0f, -1.0f,
            (float)tr.x, (float)tr.y, (float)tr.z,  1.0f,  1.0f,
            (float)tl.x, (float)tl.y, (float)tl.z, -1.0f,  1.0f,
        };

        glUseProgram(celestial_shader_);
        glUniformMatrix4fv(celestial_uVP_, 1, GL_FALSE, glm::value_ptr(vp));
        glUniform3f(celestial_uCore_, core.r, core.g, core.b);
        glUniform3f(celestial_uHalo_, halo.r, halo.g, halo.b);
        glUniform1f(celestial_uIntensity_, intensity);

        glBindVertexArray(celestial_vao_);
        glBindBuffer(GL_ARRAY_BUFFER, celestial_vbo_);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    };

    // Sun — slightly exaggerated at 0.8° for visibility
    celestial::AzEl sun = celestial::sun_azel(
        observer.lat_deg, observer.lon_deg, utc_unix_s);
    draw_body(sun.az_deg, sun.el_deg, 0.8,
              glm::vec3(1.0f, 0.95f, 0.80f),
              glm::vec3(1.0f, 0.75f, 0.35f),
              1.0f);

    // Moon — 0.7° apparent, brightness scaled by illuminated fraction,
    // washed out during day when the sky is bright blue.
    double phase = 1.0;
    celestial::AzEl moon = celestial::moon_azel(
        observer.lat_deg, observer.lon_deg, utc_unix_s, &phase);
    float nightness = std::max(0.0f, std::min(1.0f,
        1.0f - ((float)sun.el_deg + 6.0f) / 12.0f));
    float moon_phase_brightness = 0.3f + 0.7f * (float)phase;
    float moon_intensity = moon_phase_brightness * (0.25f + 0.75f * nightness);
    draw_body(moon.az_deg, moon.el_deg, 0.7,
              glm::vec3(0.95f, 0.96f, 1.0f),
              glm::vec3(0.55f, 0.60f, 0.85f),
              moon_intensity);
}

// ---------------------------------------------------------------------------
// draw_horizon — horizon arc, batched bearing ticks, room heading line
// ---------------------------------------------------------------------------

void Renderer::draw_horizon(const ObserverConfig& observer,
                             const glm::mat4& view, const glm::mat4& proj,
                             float room_heading) {
    glm::mat4 vp = proj * view;

    const float horizon_dist = 200000.0f;
    const float ground_z = -(float)observer.alt_m;
    const int num_segments = 90;
    const float arc_half = 90.0f;
    float center_bearing = (float)observer.heading_deg;

    // --- Horizon line strip (1 draw call) ---
    std::vector<float> horizon_verts;
    horizon_verts.reserve((num_segments + 1) * 2);
    float ndc_y_at_center = 0.0f;
    bool got_center = false;

    for (int i = 0; i <= num_segments; i++) {
        float bearing = center_bearing - arc_half + (2.0f * arc_half * i / num_segments);
        float rad = glm::radians(bearing);
        glm::vec4 clip = vp * glm::vec4(sin(rad) * horizon_dist, cos(rad) * horizon_dist, ground_z, 1.0f);
        if (clip.w <= 0.0f) continue;
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        horizon_verts.push_back(ndc.x);
        horizon_verts.push_back(ndc.y);
        if (!got_center && i >= num_segments / 2) { ndc_y_at_center = ndc.y; got_center = true; }
    }

    glm::mat4 identity(1.0f);
    if (horizon_verts.size() >= 4) {
        glUseProgram(marker_shader_);
        glUniformMatrix4fv(marker_uTransform_, 1, GL_FALSE, glm::value_ptr(identity));
        glUniform4f(marker_uColor_, 0.25f, 0.30f, 0.40f, 0.9f);
        glBindVertexArray(marker_vao_);
        glBindBuffer(GL_ARRAY_BUFFER, marker_vbo_);
        glBufferSubData(GL_ARRAY_BUFFER, 0, horizon_verts.size() * sizeof(float), horizon_verts.data());
        glDrawArrays(GL_LINE_STRIP, 0, (int)(horizon_verts.size() / 2));
    }

    {
        float s = dpi_scale_;
        draw_text("- horizon -", width_ - 120.0f * s,
                  ((ndc_y_at_center) * 0.5f + 0.5f) * height_ - 16.0f * s,
                  1.0f, glm::vec3(0.30f, 0.35f, 0.45f));
    }

    // --- Batch ALL bearing ticks into one draw (pos2+color4) ---
    std::vector<float> tick_verts;
    tick_verts.reserve(36 * 12);

    for (int bearing = 0; bearing < 360; bearing += 10) {
        float diff = (float)bearing - center_bearing;
        while (diff > 180.0f) diff -= 360.0f;
        while (diff < -180.0f) diff += 360.0f;
        if (std::abs(diff) > arc_half + 5.0f) continue;

        double rad = glm::radians((double)bearing);
        glm::vec4 clip = vp * glm::vec4((float)(sin(rad) * horizon_dist),
                                         (float)(cos(rad) * horizon_dist), ground_z, 1.0f);
        if (clip.w <= 0.0f) continue;
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        if (ndc.x < -1.3f || ndc.x > 1.3f) continue;

        bool major = (bearing % 30 == 0);
        float tick_h = major ? 0.04f : 0.02f;
        float brightness = major ? 0.35f : 0.2f;
        float r = brightness * 0.6f, g = brightness * 0.7f, b = brightness;

        tick_verts.push_back(ndc.x); tick_verts.push_back(ndc.y);
        tick_verts.push_back(r); tick_verts.push_back(g); tick_verts.push_back(b); tick_verts.push_back(1.0f);
        tick_verts.push_back(ndc.x); tick_verts.push_back(ndc.y + tick_h);
        tick_verts.push_back(r); tick_verts.push_back(g); tick_verts.push_back(b); tick_verts.push_back(1.0f);
    }

    if (!tick_verts.empty()) {
        glUseProgram(terrain_shader_);
        glBindVertexArray(batch2d_vao_);
        glBindBuffer(GL_ARRAY_BUFFER, batch2d_vbo_);
        glBufferData(GL_ARRAY_BUFFER, tick_verts.size() * sizeof(float), tick_verts.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_LINES, 0, (GLsizei)(tick_verts.size() / 6));
    }

    // --- Major bearing labels (only 30-deg intervals, ~12 draw_text calls) ---
    float s = dpi_scale_;
    for (int bearing = 0; bearing < 360; bearing += 30) {
        float diff = (float)bearing - center_bearing;
        while (diff > 180.0f) diff -= 360.0f;
        while (diff < -180.0f) diff += 360.0f;
        if (std::abs(diff) > arc_half + 5.0f) continue;

        double rad = glm::radians((double)bearing);
        glm::vec4 clip = vp * glm::vec4((float)(sin(rad) * horizon_dist),
                                         (float)(cos(rad) * horizon_dist), ground_z, 1.0f);
        if (clip.w <= 0.0f) continue;
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        if (ndc.x < -1.3f || ndc.x > 1.3f) continue;

        float sx = (ndc.x * 0.5f + 0.5f) * width_;
        float sy = (ndc.y * 0.5f + 0.5f) * height_ + 0.04f * height_ * 0.5f + 6 * s;
        char buf[16];
        if (bearing == 0)        snprintf(buf, sizeof(buf), "N");
        else if (bearing == 90)  snprintf(buf, sizeof(buf), "E");
        else if (bearing == 180) snprintf(buf, sizeof(buf), "S");
        else if (bearing == 270) snprintf(buf, sizeof(buf), "W");
        else                     snprintf(buf, sizeof(buf), "%d", bearing);
        draw_text(buf, sx - strlen(buf) * 4.0f * s, sy, 1.0f, glm::vec3(0.35f, 0.40f, 0.55f));
    }

    // --- Room heading vertical reference line (1 draw call) ---
    {
        float ref_rad = glm::radians(room_heading);
        glm::vec4 clip_bot = vp * glm::vec4(sin(ref_rad) * horizon_dist,
                                              cos(ref_rad) * horizon_dist, ground_z, 1.0f);
        if (clip_bot.w > 0.0f) {
            glm::vec3 ndc_bot = glm::vec3(clip_bot) / clip_bot.w;
            float seg[] = { ndc_bot.x, -1.0f, ndc_bot.x, 1.0f };
            glUseProgram(marker_shader_);
            glUniformMatrix4fv(marker_uTransform_, 1, GL_FALSE, glm::value_ptr(identity));
            glUniform4f(marker_uColor_, 0.6f, 0.3f, 0.1f, 0.35f);
            glBindVertexArray(marker_vao_);
            glBindBuffer(GL_ARRAY_BUFFER, marker_vbo_);
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(seg), seg);
            glDrawArrays(GL_LINES, 0, 2);

            float sx = (ndc_bot.x * 0.5f + 0.5f) * width_;
            float sy = (ndc_bot.y * 0.5f + 0.5f) * height_ + 20 * dpi_scale_;
            char ref_buf[8];
            snprintf(ref_buf, sizeof(ref_buf), "%.0f", room_heading);
            draw_text(ref_buf, sx - 12 * dpi_scale_, sy, 1.0f, glm::vec3(0.6f, 0.3f, 0.1f));
        }
    }
}

// ---------------------------------------------------------------------------
// draw_terrain — GPU-side projection via enu3d_shader_ (pos3+color4)
//   Uploads ENU coordinates directly, VP matrix as uniform.
//   Cache only checks generation — camera rotation is free (just updates VP).
// ---------------------------------------------------------------------------

void Renderer::draw_terrain(const Terrain& terrain, const glm::mat4& view, const glm::mat4& proj) {
    if (!terrain.is_ready()) return;

    glm::mat4 vp = proj * view;
    uint32_t gen = terrain.generation();

    // Cache hit: terrain data unchanged — only update VP uniform
    if (terrain_cached_count_ > 0 && gen == terrain_cached_gen_) {
        glUseProgram(enu3d_shader_);
        glUniformMatrix4fv(enu3d_uVP_, 1, GL_FALSE, glm::value_ptr(vp));
        glBindVertexArray(terrain_vao_);
        glDrawArrays(GL_TRIANGLES, 0, terrain_cached_count_);
        return;
    }
    terrain_cached_gen_ = gen;

    const auto& pts = terrain.points();
    const auto& tris = terrain.visible_triangles();

    terrain_verts_.clear();
    terrain_verts_.reserve(tris.size() * 7);

    for (size_t i = 0; i + 2 < tris.size(); i += 3) {
        const glm::vec3& a = pts[tris[i]].enu;
        const glm::vec3& b = pts[tris[i+1]].enu;
        const glm::vec3& c = pts[tris[i+2]].enu;

        // Color based on elevation
        float avg_elev = (a.z + b.z + c.z) / 3.0f + 68.0f;
        float elev_factor = std::min(1.0f, std::max(0.0f, avg_elev / 300.0f));
        float r = 0.02f + 0.04f * elev_factor;
        float g = 0.03f + 0.05f * elev_factor;
        float bl = 0.05f + 0.06f * elev_factor;

        auto push = [&](const glm::vec3& v) {
            terrain_verts_.push_back(v.x); terrain_verts_.push_back(v.y); terrain_verts_.push_back(v.z);
            terrain_verts_.push_back(r); terrain_verts_.push_back(g); terrain_verts_.push_back(bl); terrain_verts_.push_back(0.85f);
        };
        push(a); push(b); push(c);
    }

    terrain_cached_count_ = (GLsizei)(terrain_verts_.size() / 7);
    if (terrain_cached_count_ == 0) return;

    glUseProgram(enu3d_shader_);
    glUniformMatrix4fv(enu3d_uVP_, 1, GL_FALSE, glm::value_ptr(vp));
    glBindVertexArray(terrain_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, terrain_vbo_);
    glBufferData(GL_ARRAY_BUFFER, terrain_verts_.size() * sizeof(float), terrain_verts_.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, terrain_cached_count_);
}

// ---------------------------------------------------------------------------
// draw_buildings — GPU-side projection via enu3d_shader_ (Phase 2)
//   Uploads 3D ENU positions directly (vec3 pos + vec4 color = 7 floats/vert)
//   VP matrix is set as a uniform, so cache only checks generation + theme
// ---------------------------------------------------------------------------

void Renderer::draw_buildings(const Buildings& buildings, const glm::mat4& view, const glm::mat4& proj) {
    if (!buildings.is_ready()) return;

    glm::mat4 vp = proj * view;
    uint32_t gen = buildings.generation();
    int theme = building_theme_;

    // Cache hit: geometry data and theme unchanged
    if (buildings_cached_count_ > 0 && gen == buildings_cached_gen_ &&
        (uint32_t)theme == buildings_cached_theme_) {
        glEnable(GL_DEPTH_TEST);
        glUseProgram(enu3d_shader_);
        glUniformMatrix4fv(enu3d_uVP_, 1, GL_FALSE, glm::value_ptr(vp));
        // Filled rooftops
        glBindVertexArray(buildings_vao_);
        glDrawArrays(GL_TRIANGLES, 0, buildings_cached_count_);
        // Edge outlines on top (polygon offset avoids z-fighting)
        if (buildings_edge_count_ > 0) {
            glEnable(GL_POLYGON_OFFSET_LINE);
            glPolygonOffset(-1.0f, -1.0f);
            glBindVertexArray(buildings_edge_vao_);
            glDrawArrays(GL_LINES, 0, buildings_edge_count_);
            glDisable(GL_POLYGON_OFFSET_LINE);
        }
        glDisable(GL_DEPTH_TEST);
        return;
    }
    buildings_cached_gen_ = gen;
    buildings_cached_theme_ = (uint32_t)theme;

    // Filled rooftop color themes
    struct ThemeColor { float r, g, b, alpha, height_influence; };
    static const ThemeColor themes[NUM_BUILDING_THEMES] = {
        {0.25f, 0.18f, 0.08f, 0.55f, 0.6f},  // 0: Amber
        {0.08f, 0.30f, 0.35f, 0.60f, 0.5f},  // 1: Neon
        {0.15f, 0.22f, 0.38f, 0.50f, 0.4f},  // 2: Blueprint
        {0.35f, 0.15f, 0.04f, 0.55f, 0.8f},  // 3: Ember
        {0.08f, 0.08f, 0.12f, 0.30f, 0.3f},  // 4: Ghost
        {0.05f, 0.28f, 0.08f, 0.50f, 0.7f},  // 5: Matrix
    };

    const ThemeColor& tc = themes[theme];
    const auto& bldgs = buildings.buildings();

    buildings_verts_.clear();
    buildings_edge_verts_.clear();

    for (const auto& b : bldgs) {
        if (b.enu_base.size() < 3) continue;
        int n = (int)b.enu_base.size();

        // Distance-based fade + LOD culling
        float cx = 0, cy = 0;
        for (const auto& v : b.enu_base) { cx += v.x; cy += v.y; }
        cx /= n; cy /= n;
        float dist = std::sqrt(cx * cx + cy * cy);
        if (dist > 6000.0f) continue;
        if (dist > 3000.0f && b.enu_height < 15.0f) continue;
        if (dist > 1500.0f && b.enu_height < 8.0f) continue;

        float dist_fade = std::max(0.15f, 1.0f - dist / 6000.0f);

        float h_factor = std::min(1.0f, b.enu_height / 50.0f);
        float bright = (1.0f - tc.height_influence) + tc.height_influence * (0.3f + 0.7f * h_factor);

        float fr = tc.r * bright * dist_fade;
        float fg = tc.g * bright * dist_fade;
        float fb = tc.b * bright * dist_fade;
        float fa = 1.0f;  // opaque so sky doesn't bleed through

        // Walls slightly darker than roof for face separation
        float wr = fr * 0.7f, wg = fg * 0.7f, wb = fb * 0.7f;

        float avg_z = 0;
        for (const auto& v : b.enu_base) avg_z += v.z;
        avg_z /= n;
        float roof_offset = b.enu_height;

        auto push_v = [&](float x, float y, float z, float r, float g, float bl) {
            buildings_verts_.push_back(x); buildings_verts_.push_back(y); buildings_verts_.push_back(z);
            buildings_verts_.push_back(r); buildings_verts_.push_back(g); buildings_verts_.push_back(bl); buildings_verts_.push_back(fa);
        };

        // --- Filled roof triangles (fan from centroid) ---
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            push_v(cx, cy, avg_z + roof_offset, fr, fg, fb);
            push_v(b.enu_base[i].x, b.enu_base[i].y, b.enu_base[i].z + roof_offset, fr, fg, fb);
            push_v(b.enu_base[j].x, b.enu_base[j].y, b.enu_base[j].z + roof_offset, fr, fg, fb);
        }

        // --- Wall quads (two triangles per face) so the sky doesn't show through ---
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            const auto& vi = b.enu_base[i];
            const auto& vj = b.enu_base[j];
            float zi = vi.z, zj = vj.z;
            float zit = zi + roof_offset, zjt = zj + roof_offset;
            // Triangle 1: bottom-i -> bottom-j -> top-j
            push_v(vi.x, vi.y, zi,  wr, wg, wb);
            push_v(vj.x, vj.y, zj,  wr, wg, wb);
            push_v(vj.x, vj.y, zjt, wr, wg, wb);
            // Triangle 2: bottom-i -> top-j -> top-i
            push_v(vi.x, vi.y, zi,  wr, wg, wb);
            push_v(vj.x, vj.y, zjt, wr, wg, wb);
            push_v(vi.x, vi.y, zit, wr, wg, wb);
        }

        // --- Roof edge outlines (brighter, only for visible/close buildings) ---
        float er = fr * 1.8f, eg = fg * 1.8f, eb = fb * 1.8f;
        float ea = fa * 0.7f;
        auto push_edge = [&](float x1, float y1, float z1, float x2, float y2, float z2) {
            buildings_edge_verts_.push_back(x1); buildings_edge_verts_.push_back(y1); buildings_edge_verts_.push_back(z1);
            buildings_edge_verts_.push_back(er); buildings_edge_verts_.push_back(eg); buildings_edge_verts_.push_back(eb); buildings_edge_verts_.push_back(ea);
            buildings_edge_verts_.push_back(x2); buildings_edge_verts_.push_back(y2); buildings_edge_verts_.push_back(z2);
            buildings_edge_verts_.push_back(er); buildings_edge_verts_.push_back(eg); buildings_edge_verts_.push_back(eb); buildings_edge_verts_.push_back(ea);
        };

        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            const auto& vi = b.enu_base[i];
            const auto& vj = b.enu_base[j];

            // Front-face test: only draw edges whose outward normal faces the observer (at origin)
            float dx = vj.x - vi.x, dy = vj.y - vi.y;
            // Candidate outward normal (perpendicular to edge)
            float nx = -dy, ny = dx;
            // Ensure it points away from centroid
            float mx = (vi.x + vj.x) * 0.5f, my = (vi.y + vj.y) * 0.5f;
            if (nx * (mx - cx) + ny * (my - cy) < 0) { nx = -nx; ny = -ny; }
            // Visible if normal points toward observer at origin
            bool front = (nx * (-mx) + ny * (-my)) > 0;

            // Roof edges: always draw (depth buffer handles occlusion)
            push_edge(vi.x, vi.y, vi.z + roof_offset,
                      vj.x, vj.y, vj.z + roof_offset);

            if (front) {
                // Vertical edges at both corners of this face
                push_edge(vi.x, vi.y, vi.z,
                          vi.x, vi.y, vi.z + roof_offset);
                // Ground edge
                push_edge(vi.x, vi.y, vi.z,
                          vj.x, vj.y, vj.z);
            }
        }
    }

    buildings_cached_count_ = (GLsizei)(buildings_verts_.size() / 7);
    buildings_edge_count_ = (GLsizei)(buildings_edge_verts_.size() / 7);

    glEnable(GL_DEPTH_TEST);
    glUseProgram(enu3d_shader_);
    glUniformMatrix4fv(enu3d_uVP_, 1, GL_FALSE, glm::value_ptr(vp));

    // Upload + draw filled rooftops
    if (buildings_cached_count_ > 0) {
        glBindVertexArray(buildings_vao_);
        glBindBuffer(GL_ARRAY_BUFFER, buildings_vbo_);
        glBufferData(GL_ARRAY_BUFFER, buildings_verts_.size() * sizeof(float), buildings_verts_.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, buildings_cached_count_);
    }

    // Upload + draw edge outlines (offset to sit on top of fills, occluded by closer buildings)
    if (buildings_edge_count_ > 0) {
        glEnable(GL_POLYGON_OFFSET_LINE);
        glPolygonOffset(-1.0f, -1.0f);
        glBindVertexArray(buildings_edge_vao_);
        glBindBuffer(GL_ARRAY_BUFFER, buildings_edge_vbo_);
        glBufferData(GL_ARRAY_BUFFER, buildings_edge_verts_.size() * sizeof(float), buildings_edge_verts_.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_LINES, 0, buildings_edge_count_);
        glDisable(GL_POLYGON_OFFSET_LINE);
    }

    glDisable(GL_DEPTH_TEST);
}

// ---------------------------------------------------------------------------
// draw_streets — GPU-side projection via enu3d_shader_ (Phase 2)
//   Uploads 3D ENU positions directly (vec3 pos + vec4 color = 7 floats/vert)
//   VP matrix is set as a uniform, so cache only checks generation
// ---------------------------------------------------------------------------

void Renderer::draw_streets(const Streets& streets, const glm::mat4& view, const glm::mat4& proj) {
    if (!streets.is_ready()) return;

    glm::mat4 vp = proj * view;
    uint32_t gen = streets.generation();

    // Cache hit: geometry data unchanged (VP is now a uniform, not baked in)
    if (streets_cached_count_ > 0 && gen == streets_cached_gen_) {
        glUseProgram(enu3d_shader_);
        glUniformMatrix4fv(enu3d_uVP_, 1, GL_FALSE, glm::value_ptr(vp));
        glBindVertexArray(streets_vao_);
        glDrawArrays(GL_LINES, 0, streets_cached_count_);
        return;
    }
    streets_cached_gen_ = gen;

    const auto& st_list = streets.streets();
    streets_verts_.clear();

    for (const auto& st : st_list) {
        if (st.enu_points.size() < 2) continue;

        // Color and alpha by road rank
        float bright, al;
        switch (st.rank) {
            case 0: bright = 0.5f; al = 0.45f; break;
            case 1: bright = 0.35f; al = 0.35f; break;
            default: bright = 0.2f; al = 0.25f; break;
        }
        float r = 0.08f * bright;
        float g = 0.12f * bright;
        float bl = 0.20f * bright;

        for (size_t i = 0; i + 1 < st.enu_points.size(); i++) {
            const glm::vec3& a = st.enu_points[i];
            const glm::vec3& b = st.enu_points[i + 1];

            // Distance cull (CPU-side, to avoid uploading far-away geometry)
            float mid_dist = std::sqrt((a.x + b.x) * (a.x + b.x) / 4.0f +
                                       (a.y + b.y) * (a.y + b.y) / 4.0f);
            if (mid_dist > 12000.0f) continue;
            float dist_fade = std::max(0.15f, 1.0f - mid_dist / 12000.0f);

            float fr = r * dist_fade, fg = g * dist_fade, fb = bl * dist_fade, fa = al * dist_fade;
            // Vertex A: pos3 + color4
            streets_verts_.push_back(a.x); streets_verts_.push_back(a.y); streets_verts_.push_back(a.z);
            streets_verts_.push_back(fr); streets_verts_.push_back(fg); streets_verts_.push_back(fb); streets_verts_.push_back(fa);
            // Vertex B: pos3 + color4
            streets_verts_.push_back(b.x); streets_verts_.push_back(b.y); streets_verts_.push_back(b.z);
            streets_verts_.push_back(fr); streets_verts_.push_back(fg); streets_verts_.push_back(fb); streets_verts_.push_back(fa);
        }
    }

    streets_cached_count_ = (GLsizei)(streets_verts_.size() / 7);
    if (streets_cached_count_ == 0) return;

    glUseProgram(enu3d_shader_);
    glUniformMatrix4fv(enu3d_uVP_, 1, GL_FALSE, glm::value_ptr(vp));
    glBindVertexArray(streets_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, streets_vbo_);
    glBufferData(GL_ARRAY_BUFFER, streets_verts_.size() * sizeof(float), streets_verts_.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_LINES, 0, streets_cached_count_);
}

// ---------------------------------------------------------------------------
// draw_landmarks — airport markers with labels
// ---------------------------------------------------------------------------

void Renderer::draw_landmarks(const ObserverConfig& observer,
                               const glm::mat4& view, const glm::mat4& proj) {
    glm::mat4 vp = proj * view;
    float s = dpi_scale_;

    struct Landmark {
        const char* name;
        double lat, lon, alt_m;
        glm::vec3 color;
    };

    Landmark landmarks[] = {
        {"SFO",     37.6213, -122.3790,   4.0, glm::vec3(1.0f, 0.6f, 0.2f)},
        {"OAK",     37.7213, -122.2208,   2.0, glm::vec3(0.8f, 0.5f, 0.8f)},
        {"SJC",     37.3626, -121.9291,  18.0, glm::vec3(0.5f, 0.8f, 0.5f)},
    };

    for (auto& lm : landmarks) {
        glm::dvec3 enu = geodetic_to_enu(lm.lat, lm.lon, lm.alt_m,
                                          observer.lat_deg, observer.lon_deg, observer.alt_m);

        double dist_km = glm::length(enu) / 1000.0;

        glm::vec4 clip = vp * glm::vec4((float)enu.x, (float)enu.y, (float)enu.z, 1.0f);
        if (clip.w <= 0.0f) continue;

        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        if (ndc.x < -1.5f || ndc.x > 1.5f) continue;

        float cx = ndc.x, cy = ndc.y;

        bool below_screen = cy < -1.0f;
        bool above_screen = cy > 1.0f;
        float draw_cy = std::max(-0.92f, std::min(0.92f, cy));

        glUseProgram(marker_shader_);
        glm::mat4 identity(1.0f);
        glUniformMatrix4fv(marker_uTransform_, 1, GL_FALSE, glm::value_ptr(identity));

        // Vertical reference line
        float line_top = std::min(draw_cy + 0.15f, 0.95f);
        float line_bot = std::max(draw_cy - 0.15f, -0.95f);
        float vline[] = { cx, line_bot, cx, line_top };
        glUniform4f(marker_uColor_, lm.color.r, lm.color.g, lm.color.b, 0.3f);
        glBindVertexArray(marker_vao_);
        glBindBuffer(GL_ARRAY_BUFFER, marker_vbo_);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vline), vline);
        glDrawArrays(GL_LINES, 0, 2);

        // Marker shape
        float msz = 0.012f;
        if (below_screen) {
            float tri[] = { cx, draw_cy - msz, cx - msz*0.7f, draw_cy + msz, cx + msz*0.7f, draw_cy + msz };
            glUniform4f(marker_uColor_, lm.color.r, lm.color.g, lm.color.b, 0.7f);
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(tri), tri);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        } else if (above_screen) {
            float tri[] = { cx, draw_cy + msz, cx - msz*0.7f, draw_cy - msz, cx + msz*0.7f, draw_cy - msz };
            glUniform4f(marker_uColor_, lm.color.r, lm.color.g, lm.color.b, 0.7f);
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(tri), tri);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        } else {
            float diamond[] = { cx, draw_cy+msz, cx+msz*0.7f, draw_cy, cx, draw_cy-msz, cx-msz*0.7f, draw_cy };
            glUniform4f(marker_uColor_, lm.color.r, lm.color.g, lm.color.b, 0.8f);
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(diamond), diamond);
            glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        }

        // Label
        float screen_x = (cx * 0.5f + 0.5f) * width_;
        float screen_y = (draw_cy * 0.5f + 0.5f) * height_;

        char buf[64];
        snprintf(buf, sizeof(buf), "%s %.1fkm", lm.name, dist_km);

        float label_y = below_screen ? screen_y + 14*s : screen_y - 16*s;
        draw_text(buf, screen_x - strlen(lm.name) * 4 * s, label_y, 1.0f, lm.color);
    }
}

// ---------------------------------------------------------------------------
// draw_altitude_scale — batched altitude reference ticks on right edge
// ---------------------------------------------------------------------------

void Renderer::draw_altitude_scale(const ObserverConfig& observer,
                                    const glm::mat4& view, const glm::mat4& proj) {
    glm::mat4 vp = proj * view;
    float s = dpi_scale_;
    float margin = 10.0f * s;
    float tick_len = 12.0f * s;
    float x_pos = width_ - margin;

    struct DistRef { float dist_km; const char* label; glm::vec3 color; };
    DistRef refs[] = {
        {10.0f,  "10km", glm::vec3(0.5f, 0.7f, 0.4f)},
        {30.0f,  "30km", glm::vec3(0.4f, 0.5f, 0.6f)},
        {100.0f, "100km", glm::vec3(0.3f, 0.3f, 0.5f)},
    };

    int alt_levels_ft[] = {1000, 2000, 5000, 10000, 15000, 20000, 25000, 30000, 35000, 40000};
    int num_levels = 10;

    float heading_rad = glm::radians((float)observer.heading_deg);
    float dir_e = sin(heading_rad);
    float dir_n = cos(heading_rad);

    // Batch all tick lines (pos2+color4) and collect label info
    std::vector<float> tick_verts;
    tick_verts.reserve(3 * num_levels * 12);

    struct AltLabel { float x, y; char text[16]; glm::vec3 color; };
    std::vector<AltLabel> labels;
    labels.reserve(3 * num_levels);

    for (int ri = 0; ri < 3; ri++) {
        float dist_m = refs[ri].dist_km * 1000.0f;
        float base_east = dir_e * dist_m;
        float base_north = dir_n * dist_m;
        float col_offset = ri * (tick_len + 40.0f * s);
        const glm::vec3& c = refs[ri].color;

        for (int li = 0; li < num_levels; li++) {
            float alt_m = alt_levels_ft[li] * 0.3048f;
            float up = alt_m - (float)observer.alt_m;

            glm::vec4 clip = vp * glm::vec4(base_east, base_north, up, 1.0f);
            if (clip.w <= 0.0f) continue;
            glm::vec3 ndc = glm::vec3(clip) / clip.w;
            if (ndc.y < -1.0f || ndc.y > 1.0f) continue;

            float tx = x_pos - col_offset;
            float lx0 = ((tx - tick_len) / width_) * 2.0f - 1.0f;
            float lx1 = (tx / width_) * 2.0f - 1.0f;

            tick_verts.push_back(lx0); tick_verts.push_back(ndc.y);
            tick_verts.push_back(c.r); tick_verts.push_back(c.g); tick_verts.push_back(c.b); tick_verts.push_back(0.5f);
            tick_verts.push_back(lx1); tick_verts.push_back(ndc.y);
            tick_verts.push_back(c.r); tick_verts.push_back(c.g); tick_verts.push_back(c.b); tick_verts.push_back(0.5f);

            AltLabel lbl;
            int alt_k = alt_levels_ft[li] / 1000;
            snprintf(lbl.text, sizeof(lbl.text), "%dk", alt_k);
            lbl.x = tx - tick_len - strlen(lbl.text) * 8.0f * s - 2*s;
            lbl.y = (ndc.y * 0.5f + 0.5f) * height_ - 4*s;
            lbl.color = c * 0.7f;
            labels.push_back(lbl);
        }
    }

    // One draw call for ALL altitude ticks
    if (!tick_verts.empty()) {
        glUseProgram(terrain_shader_);
        glBindVertexArray(batch2d_vao_);
        glBindBuffer(GL_ARRAY_BUFFER, batch2d_vbo_);
        glBufferData(GL_ARRAY_BUFFER, tick_verts.size() * sizeof(float), tick_verts.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_LINES, 0, (GLsizei)(tick_verts.size() / 6));
    }

    // Text labels (cached uniforms make these cheap)
    for (const auto& lbl : labels) {
        draw_text(lbl.text, lbl.x, lbl.y, 1.0f, lbl.color);
    }
    for (int ri = 0; ri < 3; ri++) {
        float col_offset = ri * (tick_len + 40.0f * s);
        draw_text(refs[ri].label, x_pos - col_offset - tick_len - 30*s,
                  height_ - 100*s, 1.0f, refs[ri].color);
    }
}
