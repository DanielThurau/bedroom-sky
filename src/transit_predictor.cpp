#include "transit_predictor.h"
#include "celestial.h"
#include <algorithm>
#include <cmath>

using celestial::AzEl;

namespace {

// Dead-reckon a plane forward by dt seconds, produce its geodetic state.
// Uses great-circle-flat math (good enough over the 10-min horizons we care about).
void dead_reckon(const TrackedPlane& p, double dt,
                 double& lat_out, double& lon_out, double& alt_out) {
    double heading_r = p.render_heading * celestial::DEG2RAD;
    double dist_m = p.render_velocity * dt;

    double dlat_m = dist_m * std::cos(heading_r);
    double dlon_m = dist_m * std::sin(heading_r);

    lat_out = p.render_lat + (dlat_m / EARTH_RADIUS_M) * celestial::RAD2DEG;
    lon_out = p.render_lon + (dlon_m /
        (EARTH_RADIUS_M * std::cos(p.render_lat * celestial::DEG2RAD))) * celestial::RAD2DEG;

    // Vertical rate (ft/min -> m/s)
    double vz_mps = p.render_vert_rate * 0.00508;
    alt_out = p.render_alt + vz_mps * dt;
}

}  // namespace

void TransitPredictor::update(
    const std::unordered_map<std::string, TrackedPlane>& planes,
    const ObserverConfig& observer,
    double utc_unix_s,
    double lookahead_s,
    double threshold_deg,
    double step_s)
{
    transits_.clear();

    if (lookahead_s <= 0 || threshold_deg <= 0) return;
    if (step_s <= 0) step_s = 2.0;

    for (const auto& kv : planes) {
        const std::string& id = kv.first;
        const TrackedPlane& plane = kv.second;
        if (plane.on_ground) continue;
        if (plane.render_velocity < 10.0) continue;  // parked / unreliable heading

        // Search for the closest approach to each body over the lookahead window.
        // Two-pass: coarse scan with step_s, then 0.25s refinement around the minimum.
        struct BestHit {
            double t;
            double sep;
            double body_az;
            double body_el;
        };
        BestHit best[2] = {
            { 0, 1e9, 0, 0 },
            { 0, 1e9, 0, 0 }
        };

        auto scan = [&](double t_start, double t_end, double step) {
            for (double t = t_start; t <= t_end; t += step) {
                double lat, lon, alt;
                dead_reckon(plane, t, lat, lon, alt);
                glm::dvec3 enu = geodetic_to_enu(lat, lon, alt,
                                                  observer.lat_deg,
                                                  observer.lon_deg,
                                                  observer.alt_m);
                if (enu.z < -200.0) continue;  // dropped below observer, skip
                AzEl plane_ae = celestial::enu_to_azel(enu);
                if (plane_ae.el_deg < -2.0) continue;  // below horizon

                double utc_at = utc_unix_s + t;
                AzEl sun = celestial::sun_azel(observer.lat_deg, observer.lon_deg, utc_at);
                AzEl moon = celestial::moon_azel(observer.lat_deg, observer.lon_deg, utc_at);

                for (int b = 0; b < 2; b++) {
                    const AzEl& body = (b == 0) ? sun : moon;
                    if (body.el_deg < 0.0) continue;  // below horizon, no transit possible
                    double sep = celestial::angular_sep_deg(
                        plane_ae.az_deg, plane_ae.el_deg,
                        body.az_deg, body.el_deg);
                    if (sep < best[b].sep) {
                        best[b] = { t, sep, body.az_deg, body.el_deg };
                    }
                }
            }
        };

        // Coarse
        scan(0.0, lookahead_s, step_s);

        // Refine each body near its coarse minimum
        for (int b = 0; b < 2; b++) {
            if (best[b].sep >= threshold_deg) continue;
            double t0 = std::max(0.0, best[b].t - step_s);
            double t1 = std::min(lookahead_s, best[b].t + step_s);
            BestHit saved = best[b];
            best[b].sep = 1e9;  // let refinement overwrite
            // Scan only this body at fine resolution
            for (double t = t0; t <= t1; t += 0.25) {
                double lat, lon, alt;
                dead_reckon(plane, t, lat, lon, alt);
                glm::dvec3 enu = geodetic_to_enu(lat, lon, alt,
                                                  observer.lat_deg,
                                                  observer.lon_deg,
                                                  observer.alt_m);
                AzEl plane_ae = celestial::enu_to_azel(enu);
                double utc_at = utc_unix_s + t;
                AzEl body_ae = (b == 0)
                    ? celestial::sun_azel(observer.lat_deg, observer.lon_deg, utc_at)
                    : celestial::moon_azel(observer.lat_deg, observer.lon_deg, utc_at);
                if (body_ae.el_deg < 0.0) continue;
                double sep = celestial::angular_sep_deg(
                    plane_ae.az_deg, plane_ae.el_deg,
                    body_ae.az_deg, body_ae.el_deg);
                if (sep < best[b].sep) {
                    best[b] = { t, sep, body_ae.az_deg, body_ae.el_deg };
                }
            }
            if (best[b].sep >= threshold_deg) {
                best[b] = saved;  // refinement didn't find anything better
            }
        }

        for (int b = 0; b < 2; b++) {
            if (best[b].sep > threshold_deg) continue;
            Transit t;
            t.plane_id = id;
            t.callsign = plane.callsign;
            t.body = (b == 0) ? CelestialBody::Sun : CelestialBody::Moon;
            t.time_to_transit_s = best[b].t;
            t.closest_sep_deg = best[b].sep;
            t.body_az_deg = best[b].body_az;
            t.body_el_deg = best[b].body_el;
            transits_.push_back(std::move(t));
        }
    }

    std::sort(transits_.begin(), transits_.end(),
              [](const Transit& a, const Transit& b) {
                  return a.time_to_transit_s < b.time_to_transit_s;
              });
}

const Transit* TransitPredictor::transit_for(const std::string& icao24) const {
    const Transit* best = nullptr;
    for (const auto& t : transits_) {
        if (t.plane_id != icao24) continue;
        if (!best || t.time_to_transit_s < best->time_to_transit_s) {
            best = &t;
        }
    }
    return best;
}
