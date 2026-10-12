#include "renderer.h"
#include "json_helpers.h"
#include <cmath>
#include <cstring>
#include <thread>
#include <mutex>

// ---------------------------------------------------------------------------
// Async aircraft detail cache — fetches enriched data from /v2/hex/{icao}
// ---------------------------------------------------------------------------

struct AircraftDetail {
    std::string origin;
    std::string destination;
    bool loaded = false;
    bool loading = false;
};

static std::mutex s_detail_mutex;
static std::unordered_map<std::string, AircraftDetail> s_detail_cache;

// Helper: find a JSON object { ... } starting from pos, return start/end
static bool find_json_obj(const std::string& j, size_t pos, size_t& obj_start, size_t& obj_end) {
    obj_start = j.find('{', pos);
    if (obj_start == std::string::npos) return false;
    int depth = 1;
    obj_end = obj_start + 1;
    while (obj_end < j.size() && depth > 0) {
        if (j[obj_end] == '{') depth++;
        else if (j[obj_end] == '}') depth--;
        obj_end++;
    }
    return depth == 0;
}

// Parse airport info from a nested JSON object like "origin":{"iata_code":"SFO","municipality":"San Francisco",...}
static std::string parse_airport(const std::string& j, const char* key) {
    std::string needle = std::string("\"") + key + "\":";
    size_t pos = j.find(needle);
    if (pos == std::string::npos) return "";

    size_t obj_start, obj_end;
    if (!find_json_obj(j, pos + needle.size(), obj_start, obj_end)) return "";

    std::string iata = json_value(j, obj_start, obj_end, "iata_code");
    std::string city = json_value(j, obj_start, obj_end, "municipality");
    std::string name = json_value(j, obj_start, obj_end, "name");

    if (!iata.empty() && !city.empty()) return iata + " (" + city + ")";
    if (!iata.empty()) return iata;
    if (!city.empty()) return city;
    if (!name.empty()) return name;
    return "";
}

static void fetch_detail_thread(std::string icao24, std::string callsign) {
    AircraftDetail detail;
    detail.loaded = true;
    detail.loading = false;

    // Look up route by callsign via adsbdb.com (free, no auth)
    if (!callsign.empty()) {
        CURL* curl = curl_easy_init();
        if (curl) {
            std::string url = "https://api.adsbdb.com/v0/callsign/" + callsign;
            std::string response;

            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "bedroom-sky/1.0");

            CURLcode res = curl_easy_perform(curl);
            curl_easy_cleanup(curl);

            if (res == CURLE_OK && !response.empty()) {
                // adsbdb response: {"response":{"flightroute":{"origin":{...},"destination":{...},...}}}
                detail.origin = parse_airport(response, "origin");
                detail.destination = parse_airport(response, "destination");
            }
        }
    }

    std::lock_guard<std::mutex> lock(s_detail_mutex);
    s_detail_cache[icao24] = std::move(detail);
}

// ---------------------------------------------------------------------------

void Renderer::draw_aircraft(const std::unordered_map<std::string, TrackedPlane>& planes,
                              const ObserverConfig& observer,
                              const glm::mat4& view,
                              const glm::mat4& proj,
                              double now,
                              const Config& cfg,
                              const TransitPredictor* transits) {
    glm::mat4 vp = proj * view;

    // Get cursor position for hover detection (NDC space)
    double mx_win, my_win;
    glfwGetCursorPos(window_, &mx_win, &my_win);
    float cursor_ndc_x = ((float)mx_win * dpi_scale_ / (float)width_) * 2.0f - 1.0f;
    float cursor_ndc_y = 1.0f - ((float)my_win * dpi_scale_ / (float)height_) * 2.0f;
    bool check_hover = !show_settings_ && !show_help_;
    hover_active_ = false;
    float best_hover_dist2 = 1e9f;

    // Clear persistent batch buffers (keeps allocated capacity across frames)
    ac_trail_verts_.clear();
    ac_pred_verts_.clear();
    ac_fill_verts_.clear();
    ac_outline_verts_.clear();
    ac_transit_verts_.clear();

    // Reserve based on plane count to avoid re-allocation
    size_t n = planes.size();
    ac_fill_verts_.reserve(n * 90);     // bg-rect (6) + center-dot (6) + arrow (3) verts × 6 floats
    ac_outline_verts_.reserve(n * 96);  // 8 bracket segments × 2 verts × 6 floats
    float aspect = (float)width_ / (float)height_;
    ac_trail_verts_.reserve(n * 60 * 12);
    ac_pred_verts_.reserve(n * 8 * 12);

    // Deferred label info — drawn after batched geometry
    struct LabelInfo {
        float x, y, scale;
        std::string text;
        glm::vec3 color;
    };
    std::vector<LabelInfo> labels;
    labels.reserve(n);

    for (const auto& [id, plane] : planes) {
        if (plane.on_ground) continue;

        // Altitude filter
        double alt_ft = plane.render_alt * 3.28084;
        if (alt_ft < cfg.min_alt_ft || alt_ft > cfg.max_alt_ft) continue;

        // Staleness fade
        double stale = plane.staleness(now);
        float fade = 1.0f;
        if (stale > cfg.fade_start_s) {
            if (stale > cfg.fade_end_s) fade = 0.0f;
            else fade = 1.0f - (float)((stale - cfg.fade_start_s) / (cfg.fade_end_s - cfg.fade_start_s));
        }
        if (fade <= 0.0f) continue;

        const glm::dvec3& enu = plane.enu;
        double dist_km = std::hypot(enu.x, enu.y) / 1000.0;
        if (dist_km < cfg.min_dist_km || dist_km > cfg.max_dist_km) continue;

        // Project to clip space
        glm::vec4 clip = vp * glm::vec4((float)enu.x, (float)enu.y, (float)enu.z, 1.0f);
        if (clip.w <= 0.0f) continue;
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        if (ndc.x < -1.2f || ndc.x > 1.2f || ndc.y < -1.2f || ndc.y > 1.2f) continue;

        float close_factor = std::max(0.1f, (float)(1.0 - std::min(1.0, dist_km / cfg.max_dist_km)));
        float brightness = 0.4f + 0.6f * (float)sqrt(close_factor);
        float alpha = brightness * fade;
        float sz = cfg.marker_size * (0.5f + close_factor * 1.5f);
        float cx = ndc.x, cy = ndc.y;

        // Bracket-box color from acceleration (preserves the existing accel signal)
        float accel_factor = std::min(1.0f, std::max(-1.0f, plane.render_accel / 2.0f));
        float mr, mg, mb;
        if (accel_factor >= 0.0f) {
            mr = (0.1f + 0.5f * accel_factor) * brightness;
            mg = (0.5f + 0.5f * brightness) + 0.3f * accel_factor * brightness;
            mb = (0.8f - 0.3f * accel_factor) * brightness;
        } else {
            float d = -accel_factor;
            mr = (0.1f + 0.7f * d) * brightness;
            mg = (0.5f + 0.5f * brightness) * (1.0f - 0.6f * d);
            mb = (0.8f * (1.0f - 0.7f * d)) * brightness;
        }

        // --- Bounding box (aspect-corrected so it's visually square in screen px) ---
        float box_h = sz * 1.5f;
        float box_w = box_h / aspect;
        float bracket_x = box_w * 0.35f;
        float bracket_y = box_h * 0.35f;
        float left  = cx - box_w, right_ = cx + box_w;
        float bot   = cy - box_h, top_   = cy + box_h;

        // Faint backing fill (very subtle "selected target" hint)
        float bg_alpha = alpha * 0.12f;
        auto push_bg = [&](float x, float y) {
            ac_fill_verts_.push_back(x); ac_fill_verts_.push_back(y);
            ac_fill_verts_.push_back(mr); ac_fill_verts_.push_back(mg);
            ac_fill_verts_.push_back(mb); ac_fill_verts_.push_back(bg_alpha);
        };
        push_bg(left, bot);   push_bg(right_, bot);  push_bg(right_, top_);
        push_bg(left, bot);   push_bg(right_, top_); push_bg(left, top_);

        // Center dot — small bright square at exact plane position
        float dot_h = sz * 0.18f;
        float dot_w = dot_h / aspect;
        float dr = 0.95f * brightness, dg = 1.0f * brightness, db_ = 0.95f * brightness;
        auto push_dot = [&](float x, float y) {
            ac_fill_verts_.push_back(x); ac_fill_verts_.push_back(y);
            ac_fill_verts_.push_back(dr); ac_fill_verts_.push_back(dg);
            ac_fill_verts_.push_back(db_); ac_fill_verts_.push_back(alpha);
        };
        push_dot(cx - dot_w, cy - dot_h); push_dot(cx + dot_w, cy - dot_h); push_dot(cx + dot_w, cy + dot_h);
        push_dot(cx - dot_w, cy - dot_h); push_dot(cx + dot_w, cy + dot_h); push_dot(cx - dot_w, cy + dot_h);

        // Corner brackets — 8 short line segments
        auto push_line = [&](float x0, float y0, float x1, float y1) {
            ac_outline_verts_.push_back(x0); ac_outline_verts_.push_back(y0);
            ac_outline_verts_.push_back(mr); ac_outline_verts_.push_back(mg);
            ac_outline_verts_.push_back(mb); ac_outline_verts_.push_back(alpha);
            ac_outline_verts_.push_back(x1); ac_outline_verts_.push_back(y1);
            ac_outline_verts_.push_back(mr); ac_outline_verts_.push_back(mg);
            ac_outline_verts_.push_back(mb); ac_outline_verts_.push_back(alpha);
        };
        push_line(left,   bot,  left + bracket_x, bot);
        push_line(left,   bot,  left,             bot + bracket_y);
        push_line(right_, bot,  right_ - bracket_x, bot);
        push_line(right_, bot,  right_,           bot + bracket_y);
        push_line(right_, top_, right_ - bracket_x, top_);
        push_line(right_, top_, right_,           top_ - bracket_y);
        push_line(left,   top_, left + bracket_x, top_);
        push_line(left,   top_, left,             top_ - bracket_y);

        // Climb / descent indicator — line-stroked chevrons that match the bracket
        // stroke. Number of chevrons (1 or 2) and their size scale with vertical rate.
        // Subtle color tint (green = climbing, red = descending). Hidden when level.
        if (std::abs(plane.render_vert_rate) > 100.0f) {
            bool climbing = plane.render_vert_rate > 0;
            float vr_mag = std::min(1.0f, std::abs(plane.render_vert_rate) / 1500.0f);
            int n_chev = (std::abs(plane.render_vert_rate) > 900.0f) ? 2 : 1;

            float ch_w = box_w * 0.42f;
            float ch_h = box_h * (0.18f + 0.22f * vr_mag);
            float ch_cx = right_ + box_w * 0.5f;
            float ch_gap = ch_h * 0.55f;
            // Stack of chevrons, "pointing" toward the rate direction
            float dir = climbing ? 1.0f : -1.0f;
            float vr_r = climbing ? 0.45f : 0.95f;
            float vr_g = climbing ? 0.95f : 0.45f;
            float vr_b = climbing ? 0.55f : 0.45f;

            auto push_seg = [&](float x0, float y0, float x1, float y1) {
                ac_outline_verts_.push_back(x0); ac_outline_verts_.push_back(y0);
                ac_outline_verts_.push_back(vr_r * brightness);
                ac_outline_verts_.push_back(vr_g * brightness);
                ac_outline_verts_.push_back(vr_b * brightness);
                ac_outline_verts_.push_back(alpha);
                ac_outline_verts_.push_back(x1); ac_outline_verts_.push_back(y1);
                ac_outline_verts_.push_back(vr_r * brightness);
                ac_outline_verts_.push_back(vr_g * brightness);
                ac_outline_verts_.push_back(vr_b * brightness);
                ac_outline_verts_.push_back(alpha);
            };

            for (int k = 0; k < n_chev; k++) {
                // Chevron k offset along the direction of motion from center
                float center_y = cy + dir * (ch_h * 0.5f + k * ch_gap);
                float tip_y  = center_y + dir * (ch_h * 0.5f);
                float base_y = center_y - dir * (ch_h * 0.5f);
                push_seg(ch_cx - ch_w, base_y, ch_cx, tip_y);
                push_seg(ch_cx + ch_w, base_y, ch_cx, tip_y);
            }
        }

        // --- Transit ring (if this plane is about to cross the sun or moon) ---
        if (transits) {
            const Transit* tr = transits->transit_for(plane.icao24);
            if (tr && tr->time_to_transit_s <= 120.0) {
                float ttt = (float)std::max(0.0, tr->time_to_transit_s);
                // Ring shrinks toward the plane as transit approaches
                float ring_r = sz * (1.8f + (ttt / 120.0f) * 2.2f);
                // Pulse once per second, stronger as transit nears
                float pulse = 0.5f + 0.5f * sinf((float)now * 6.28318f);
                float urgency = 1.0f - ttt / 120.0f;  // 0..1
                float ring_alpha = alpha * (0.35f + 0.65f * pulse) * (0.6f + 0.4f * urgency);

                float tr_r, tr_g, tr_b;
                if (tr->body == CelestialBody::Sun) {
                    tr_r = 1.0f; tr_g = 0.80f; tr_b = 0.25f;
                } else {
                    tr_r = 0.70f; tr_g = 0.85f; tr_b = 1.00f;
                }

                const int N = 28;
                float prev_x = cx + ring_r, prev_y = cy;
                for (int i = 1; i <= N; i++) {
                    float a = (float)i / N * 6.28318f;
                    float nx = cx + ring_r * cosf(a);
                    float ny = cy + ring_r * sinf(a);
                    ac_transit_verts_.push_back(prev_x); ac_transit_verts_.push_back(prev_y);
                    ac_transit_verts_.push_back(tr_r); ac_transit_verts_.push_back(tr_g);
                    ac_transit_verts_.push_back(tr_b); ac_transit_verts_.push_back(ring_alpha);
                    ac_transit_verts_.push_back(nx); ac_transit_verts_.push_back(ny);
                    ac_transit_verts_.push_back(tr_r); ac_transit_verts_.push_back(tr_g);
                    ac_transit_verts_.push_back(tr_b); ac_transit_verts_.push_back(ring_alpha);
                    prev_x = nx;
                    prev_y = ny;
                }
            }
        }

        // --- Batch trail segments ---
        if (plane.history_count > 1 && cfg.trail_opacity > 0.0f) {
            float prev_nx = cx, prev_ny = cy;
            float max_trail_age = (float)cfg.trail_length_s;

            for (int h = 0; h < plane.history_count - 1; h++) {
                auto pt = plane.get_history(h);
                float age = (float)(now - pt.time);
                if (age > max_trail_age) break;

                glm::dvec3 h_enu = geodetic_to_enu(pt.lat, pt.lon, pt.alt,
                    observer.lat_deg, observer.lon_deg, observer.alt_m);
                glm::vec4 h_clip = vp * glm::vec4((float)h_enu.x, (float)h_enu.y, (float)h_enu.z, 1.0f);
                if (h_clip.w <= 0.0f) { prev_nx = -999; continue; }
                glm::vec3 h_ndc = glm::vec3(h_clip) / h_clip.w;

                if (prev_nx > -99 && h > 0) {
                    float trail_alpha = (1.0f - age / max_trail_age) * cfg.trail_opacity * alpha;
                    ac_trail_verts_.push_back(prev_nx); ac_trail_verts_.push_back(prev_ny);
                    ac_trail_verts_.push_back(0.15f); ac_trail_verts_.push_back(0.4f);
                    ac_trail_verts_.push_back(0.5f); ac_trail_verts_.push_back(trail_alpha);
                    ac_trail_verts_.push_back(h_ndc.x); ac_trail_verts_.push_back(h_ndc.y);
                    ac_trail_verts_.push_back(0.15f); ac_trail_verts_.push_back(0.4f);
                    ac_trail_verts_.push_back(0.5f); ac_trail_verts_.push_back(trail_alpha);
                }
                prev_nx = h_ndc.x;
                prev_ny = h_ndc.y;
            }
        }

        // --- Batch prediction segments (ENU-space, no geodetic conversion) ---
        if (plane.render_velocity > 5.0f) {
            float prev_px = cx, prev_py = cy;
            float pred_total = (float)cfg.predict_length_s;
            int pred_steps = 8;
            float pred_step_sec = pred_total / pred_steps;
            float heading_rad = glm::radians(plane.render_heading);
            float ve = plane.render_velocity * sinf(heading_rad);
            float vn = plane.render_velocity * cosf(heading_rad);

            for (int p = 1; p <= pred_steps; p++) {
                float dt = p * pred_step_sec;
                glm::vec4 p_clip = vp * glm::vec4(
                    (float)enu.x + ve * dt,
                    (float)enu.y + vn * dt,
                    (float)enu.z, 1.0f);
                if (p_clip.w <= 0.0f) break;
                glm::vec3 p_ndc = glm::vec3(p_clip) / p_clip.w;

                float pred_alpha = (1.0f - expf(-dt / 30.0f)) * alpha * 0.6f;
                ac_pred_verts_.push_back(prev_px); ac_pred_verts_.push_back(prev_py);
                ac_pred_verts_.push_back(0.8f); ac_pred_verts_.push_back(0.5f);
                ac_pred_verts_.push_back(0.1f); ac_pred_verts_.push_back(pred_alpha);
                ac_pred_verts_.push_back(p_ndc.x); ac_pred_verts_.push_back(p_ndc.y);
                ac_pred_verts_.push_back(0.8f); ac_pred_verts_.push_back(0.5f);
                ac_pred_verts_.push_back(0.1f); ac_pred_verts_.push_back(pred_alpha);

                prev_px = p_ndc.x;
                prev_py = p_ndc.y;
            }
        }

        // --- Hover detection ---
        if (check_hover) {
            float hd_x = cursor_ndc_x - cx;
            float hd_y = cursor_ndc_y - cy;
            float hd2 = hd_x * hd_x + hd_y * hd_y;
            float hover_radius = sz * 2.5f;
            if (hd2 < hover_radius * hover_radius && hd2 < best_hover_dist2) {
                best_hover_dist2 = hd2;
                hover_active_ = true;
                hover_screen_x_ = (ndc.x * 0.5f + 0.5f) * width_;
                hover_screen_y_ = (ndc.y * 0.5f + 0.5f) * height_;
                hover_callsign_ = plane.callsign.empty() ? plane.icao24 : plane.callsign;
                hover_icao24_ = plane.icao24;
                hover_operator_ = plane.ac_operator;
                hover_type_ = plane.ac_type;
                hover_reg_ = plane.registration;
                hover_alt_ft_ = alt_ft;
                hover_velocity_ = plane.render_velocity;
                hover_heading_ = plane.render_heading;
                hover_dist_km_ = dist_km;
                hover_vert_rate_ = plane.render_vert_rate;

                // Check detail cache for origin/destination
                hover_origin_.clear();
                hover_destination_.clear();
                hover_detail_loading_ = false;
                {
                    std::lock_guard<std::mutex> lock(s_detail_mutex);
                    auto it = s_detail_cache.find(plane.icao24);
                    if (it != s_detail_cache.end()) {
                        if (it->second.loaded) {
                            hover_origin_ = it->second.origin;
                            hover_destination_ = it->second.destination;
                        } else {
                            hover_detail_loading_ = true;
                        }
                    } else {
                        // First hover on this aircraft — start async fetch
                        s_detail_cache[plane.icao24].loading = true;
                        hover_detail_loading_ = true;
                        std::string cs = plane.callsign;
                        // Trim trailing spaces from callsign
                        while (!cs.empty() && cs.back() == ' ') cs.pop_back();
                        std::thread(fetch_detail_thread, plane.icao24, cs).detach();
                    }
                }
            }
        }

        // --- Save label for deferred text rendering ---
        float screen_x = (ndc.x * 0.5f + 0.5f) * width_;
        float screen_y = (ndc.y * 0.5f + 0.5f) * height_;

        std::string label = plane.callsign.empty() ? plane.icao24 : plane.callsign;
        char alt_str[96];
        if (dist_km < 1.0)
            snprintf(alt_str, sizeof(alt_str), " %.0fft %.0fkt %.0fm",
                     alt_ft, plane.render_velocity * 1.94384, dist_km * 1000.0);
        else
            snprintf(alt_str, sizeof(alt_str), " %.0fft %.0fkt %.1fkm",
                     alt_ft, plane.render_velocity * 1.94384, dist_km);
        label += alt_str;

        // Offset label to clear the bounding box + chevron indicator region
        float marker_right_px = box_w * 1.15f * (float)width_;
        labels.push_back({
            screen_x + marker_right_px + 4 * dpi_scale_,
            screen_y - 4 * dpi_scale_,
            cfg.label_scale,
            std::move(label),
            glm::vec3(0.15f * brightness, (0.6f + 0.4f * brightness) * alpha, 0.85f * brightness * alpha)
        });
    }

    // === BATCH DRAW — 4 draw calls for ALL aircraft geometry ===
    glUseProgram(terrain_shader_);
    glBindVertexArray(batch2d_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, batch2d_vbo_);

    // 1. Trails (behind everything)
    if (!ac_trail_verts_.empty()) {
        glBufferData(GL_ARRAY_BUFFER, ac_trail_verts_.size() * sizeof(float),
                     ac_trail_verts_.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_LINES, 0, (GLsizei)(ac_trail_verts_.size() / 6));
    }
    // 2. Predictions
    if (!ac_pred_verts_.empty()) {
        glBufferData(GL_ARRAY_BUFFER, ac_pred_verts_.size() * sizeof(float),
                     ac_pred_verts_.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_LINES, 0, (GLsizei)(ac_pred_verts_.size() / 6));
    }
    // 3. Diamond fills
    if (!ac_fill_verts_.empty()) {
        glBufferData(GL_ARRAY_BUFFER, ac_fill_verts_.size() * sizeof(float),
                     ac_fill_verts_.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(ac_fill_verts_.size() / 6));
    }
    // 4. Diamond outlines
    if (!ac_outline_verts_.empty()) {
        glBufferData(GL_ARRAY_BUFFER, ac_outline_verts_.size() * sizeof(float),
                     ac_outline_verts_.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_LINES, 0, (GLsizei)(ac_outline_verts_.size() / 6));
    }
    // 5. Transit rings (on top so they're always visible)
    if (!ac_transit_verts_.empty()) {
        glBufferData(GL_ARRAY_BUFFER, ac_transit_verts_.size() * sizeof(float),
                     ac_transit_verts_.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_LINES, 0, (GLsizei)(ac_transit_verts_.size() / 6));
    }

    // Text labels
    for (const auto& lbl : labels) {
        draw_text(lbl.text, lbl.x, lbl.y, lbl.scale, lbl.color);
    }

    // === Landing notifications (unchanged, only fires for a few planes) ===
    float s = dpi_scale_;
    float notif_y = height_ * 0.75f;
    for (const auto& [id, plane] : planes) {
        if (!plane.landed || plane.landed_airport.empty()) continue;

        double age = now - plane.landed_time;
        if (age > 15.0) continue;

        float notif_alpha = (age < 1.0) ? (float)age :
                            (age > 12.0) ? (float)(1.0 - (age - 12.0) / 3.0) : 1.0f;
        notif_alpha = std::max(0.0f, std::min(1.0f, notif_alpha));
        float flash = (age < 2.0) ? 0.5f + 0.5f * (float)cos(age * 6.0) : 0.0f;

        std::string callsign = plane.callsign.empty() ? plane.icao24 : plane.callsign;
        char notif_buf[128];
        snprintf(notif_buf, sizeof(notif_buf), "LANDED  %s  ->  %s",
                 callsign.c_str(), plane.landed_airport.c_str());

        float text_w = strlen(notif_buf) * 8.0f * s;
        float bar_x = (width_ - text_w) * 0.5f - 10*s;
        float bar_w = text_w + 20*s;
        float bar_h = 20.0f * s;
        draw_filled_rect(bar_x, notif_y - 4*s, bar_w, bar_h,
                         glm::vec4(0.1f + flash*0.2f, 0.05f, 0.0f, notif_alpha * 0.8f));
        draw_text(notif_buf, (width_ - text_w) * 0.5f, notif_y, 1.0f,
                  glm::vec3(1.0f, 0.6f + flash*0.4f, 0.2f) * notif_alpha);

        notif_y -= 28.0f * s;
    }
}

// ---------------------------------------------------------------------------
// draw_hover_tooltip — aircraft info panel, drawn on top of everything
// ---------------------------------------------------------------------------

void Renderer::draw_hover_tooltip() {
    if (!hover_active_) return;

    float s = dpi_scale_;
    float pad = 10.0f * s;
    float line_h = 15.0f * s;

    // Build tooltip lines: {text, color}
    struct TipLine { char text[128]; glm::vec3 color; };
    TipLine lines[10];
    int num_lines = 0;

    // Line 1: Callsign (bright)
    snprintf(lines[num_lines].text, sizeof(lines[0].text), "%s", hover_callsign_.c_str());
    lines[num_lines].color = glm::vec3(0.1f, 1.0f, 0.8f);
    num_lines++;

    // Line 2: Operator (if available)
    if (!hover_operator_.empty()) {
        snprintf(lines[num_lines].text, sizeof(lines[0].text), "%s", hover_operator_.c_str());
        lines[num_lines].color = glm::vec3(0.65f, 0.70f, 0.80f);
        num_lines++;
    }

    // Line 3: Type / Registration (if available)
    if (!hover_type_.empty() || !hover_reg_.empty()) {
        if (!hover_type_.empty() && !hover_reg_.empty())
            snprintf(lines[num_lines].text, sizeof(lines[0].text), "%s / %s",
                     hover_type_.c_str(), hover_reg_.c_str());
        else if (!hover_type_.empty())
            snprintf(lines[num_lines].text, sizeof(lines[0].text), "%s", hover_type_.c_str());
        else
            snprintf(lines[num_lines].text, sizeof(lines[0].text), "%s", hover_reg_.c_str());
        lines[num_lines].color = glm::vec3(0.55f, 0.60f, 0.70f);
        num_lines++;
    }

    // Line: Origin → Destination (if available)
    if (!hover_origin_.empty() || !hover_destination_.empty()) {
        if (!hover_origin_.empty() && !hover_destination_.empty())
            snprintf(lines[num_lines].text, sizeof(lines[0].text), "%s -> %s",
                     hover_origin_.c_str(), hover_destination_.c_str());
        else if (!hover_origin_.empty())
            snprintf(lines[num_lines].text, sizeof(lines[0].text), "from %s", hover_origin_.c_str());
        else
            snprintf(lines[num_lines].text, sizeof(lines[0].text), "to %s", hover_destination_.c_str());
        lines[num_lines].color = glm::vec3(1.0f, 0.75f, 0.3f);
        num_lines++;
    } else if (hover_detail_loading_) {
        snprintf(lines[num_lines].text, sizeof(lines[0].text), "loading route...");
        lines[num_lines].color = glm::vec3(0.4f, 0.4f, 0.5f);
        num_lines++;
    }

    // Line: ICAO hex (dim)
    snprintf(lines[num_lines].text, sizeof(lines[0].text), "ICAO %s", hover_icao24_.c_str());
    lines[num_lines].color = glm::vec3(0.4f, 0.42f, 0.5f);
    num_lines++;

    // Line 5: Altitude + vertical rate
    const char* vr_sym = (hover_vert_rate_ > 200) ? " ^" :
                          (hover_vert_rate_ < -200) ? " v" : "";
    snprintf(lines[num_lines].text, sizeof(lines[0].text),
             "FL%.0f  (%.0f ft)%s", hover_alt_ft_ / 100.0, hover_alt_ft_, vr_sym);
    lines[num_lines].color = glm::vec3(0.5f, 0.7f, 0.5f);
    num_lines++;

    // Line 6: Speed + heading
    snprintf(lines[num_lines].text, sizeof(lines[0].text),
             "%.0f kt  HDG %.0f", hover_velocity_ * 1.94384, hover_heading_);
    lines[num_lines].color = glm::vec3(0.5f, 0.55f, 0.65f);
    num_lines++;

    // Line 7: Distance
    snprintf(lines[num_lines].text, sizeof(lines[0].text), "%.1f km away", hover_dist_km_);
    lines[num_lines].color = glm::vec3(0.45f, 0.50f, 0.60f);
    num_lines++;

    // Calculate tooltip dimensions
    float max_text_w = 0;
    for (int i = 0; i < num_lines; i++) {
        float w = strlen(lines[i].text) * 8.0f * s;
        if (w > max_text_w) max_text_w = w;
    }
    float tooltip_w = max_text_w + pad * 2;
    float tooltip_h = num_lines * line_h + pad * 2;

    // Position: to the right of the aircraft, vertically centered
    float tx = hover_screen_x_ + 28 * s;
    float ty = hover_screen_y_ - tooltip_h * 0.5f;

    // If it would go off the right edge, flip to left side
    if (tx + tooltip_w > width_) tx = hover_screen_x_ - 28 * s - tooltip_w;
    // Clamp vertically
    if (ty < 4 * s) ty = 4 * s;
    if (ty + tooltip_h > height_ - 4 * s) ty = height_ - 4 * s - tooltip_h;

    // Background
    draw_filled_rect(tx, ty, tooltip_w, tooltip_h,
                     glm::vec4(0.04f, 0.06f, 0.10f, 0.93f));

    // Border
    float bx0 = (tx / width_) * 2.0f - 1.0f;
    float by0 = (ty / height_) * 2.0f - 1.0f;
    float bx1 = ((tx + tooltip_w) / width_) * 2.0f - 1.0f;
    float by1 = ((ty + tooltip_h) / height_) * 2.0f - 1.0f;
    float border[] = { bx0,by0, bx1,by0, bx1,by1, bx0,by1 };
    glUseProgram(marker_shader_);
    glm::mat4 identity(1.0f);
    glUniformMatrix4fv(marker_uTransform_, 1, GL_FALSE, glm::value_ptr(identity));
    glUniform4f(marker_uColor_, 0.1f, 0.5f, 0.6f, 0.8f);
    glBindVertexArray(marker_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, marker_vbo_);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(border), border);
    glDrawArrays(GL_LINE_LOOP, 0, 4);

    // Text lines (top to bottom)
    for (int i = 0; i < num_lines; i++) {
        draw_text(lines[i].text,
                  tx + pad,
                  ty + tooltip_h - pad - line_h * (i + 1),
                  1.0f, lines[i].color);
    }
}
