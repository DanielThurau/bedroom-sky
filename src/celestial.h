#pragma once
#include <glm/glm.hpp>
#include <cmath>
#include <ctime>

// Sun and moon position math.
//
// Inputs: observer lat/lon in degrees, UTC time as Unix seconds (double).
// Outputs: azimuth (degrees clockwise from North) and elevation (degrees above horizon).
//
// Sun:  NOAA low-precision (accurate to ~0.01 deg).
// Moon: abbreviated Meeus (accurate to ~0.3 deg — plenty for 1-deg transit windows).
//
// Header-only, pure functions. No external deps beyond glm + cmath.

namespace celestial {

constexpr double PI = 3.14159265358979323846;
constexpr double DEG2RAD = PI / 180.0;
constexpr double RAD2DEG = 180.0 / PI;

struct AzEl {
    double az_deg;   // 0=N, 90=E, 180=S, 270=W
    double el_deg;   // 0=horizon, 90=zenith, negative=below horizon
};

// Unix seconds -> Julian Date
inline double unix_to_jd(double unix_s) {
    return 2440587.5 + unix_s / 86400.0;
}

// Wrap x into [0, 360)
inline double wrap360(double x) {
    double y = std::fmod(x, 360.0);
    if (y < 0) y += 360.0;
    return y;
}

// Angular separation between two (az, el) points on the sphere, in degrees
inline double angular_sep_deg(double az1_deg, double el1_deg,
                               double az2_deg, double el2_deg) {
    double a1 = az1_deg * DEG2RAD;
    double e1 = el1_deg * DEG2RAD;
    double a2 = az2_deg * DEG2RAD;
    double e2 = el2_deg * DEG2RAD;
    double c = std::sin(e1) * std::sin(e2) +
               std::cos(e1) * std::cos(e2) * std::cos(a1 - a2);
    if (c > 1.0) c = 1.0;
    if (c < -1.0) c = -1.0;
    return std::acos(c) * RAD2DEG;
}

// Convert (az, el) to ENU unit direction vector
inline glm::dvec3 azel_to_enu(double az_deg, double el_deg) {
    double a = az_deg * DEG2RAD;
    double e = el_deg * DEG2RAD;
    return glm::dvec3(
        std::sin(a) * std::cos(e),  // east
        std::cos(a) * std::cos(e),  // north
        std::sin(e)                  // up
    );
}

// Given right ascension and declination (both in degrees) and observer lat/lon + JD,
// return az/el as seen by the observer.
inline AzEl equatorial_to_azel(double ra_deg, double dec_deg,
                                double observer_lat_deg, double observer_lon_deg,
                                double jd) {
    double n = jd - 2451545.0;
    // Greenwich mean sidereal time (degrees)
    double gmst_deg = wrap360(280.46061837 + 360.98564736629 * n);
    // Local sidereal time
    double lst_deg = wrap360(gmst_deg + observer_lon_deg);
    // Hour angle
    double ha_deg = lst_deg - ra_deg;

    double lat = observer_lat_deg * DEG2RAD;
    double dec = dec_deg * DEG2RAD;
    double ha  = ha_deg * DEG2RAD;

    double sin_el = std::sin(lat) * std::sin(dec) +
                    std::cos(lat) * std::cos(dec) * std::cos(ha);
    if (sin_el > 1.0) sin_el = 1.0;
    if (sin_el < -1.0) sin_el = -1.0;
    double el = std::asin(sin_el);
    double cos_el = std::cos(el);

    double az;
    if (cos_el < 1e-9) {
        az = 0.0;  // at zenith; azimuth undefined
    } else {
        double cos_az = (std::sin(dec) - sin_el * std::sin(lat)) /
                        (cos_el * std::cos(lat));
        if (cos_az > 1.0) cos_az = 1.0;
        if (cos_az < -1.0) cos_az = -1.0;
        az = std::acos(cos_az);
        // sin(ha) > 0 means body is west of meridian => az in (180, 360)
        if (std::sin(ha) > 0) az = 2.0 * PI - az;
    }

    return { wrap360(az * RAD2DEG), el * RAD2DEG };
}

// Sun position — NOAA low-precision.
inline AzEl sun_azel(double observer_lat_deg, double observer_lon_deg,
                     double utc_unix_s) {
    double jd = unix_to_jd(utc_unix_s);
    double n = jd - 2451545.0;

    double L = wrap360(280.460 + 0.9856474 * n);   // mean longitude
    double g = wrap360(357.528 + 0.9856003 * n);   // mean anomaly

    double g_r = g * DEG2RAD;
    double lambda = L + 1.915 * std::sin(g_r) + 0.020 * std::sin(2.0 * g_r);
    double eps = 23.439 - 0.0000004 * n;

    double lambda_r = lambda * DEG2RAD;
    double eps_r = eps * DEG2RAD;

    double ra = std::atan2(std::cos(eps_r) * std::sin(lambda_r), std::cos(lambda_r));
    double dec = std::asin(std::sin(eps_r) * std::sin(lambda_r));

    return equatorial_to_azel(wrap360(ra * RAD2DEG), dec * RAD2DEG,
                              observer_lat_deg, observer_lon_deg, jd);
}

// Moon position — abbreviated Meeus (Chapter 47).
// Also returns phase fraction illuminated [0..1] via output param.
inline AzEl moon_azel(double observer_lat_deg, double observer_lon_deg,
                      double utc_unix_s, double* phase_illum = nullptr) {
    double jd = unix_to_jd(utc_unix_s);
    double T = (jd - 2451545.0) / 36525.0;

    // Fundamental arguments (degrees)
    double Lp = wrap360(218.3164477 + 481267.88123421 * T);   // moon mean longitude
    double D  = wrap360(297.8501921 + 445267.1114034 * T);    // mean elongation
    double M  = wrap360(357.5291092 + 35999.0502909 * T);     // sun mean anomaly
    double Mp = wrap360(134.9633964 + 477198.8675055 * T);    // moon mean anomaly
    double F  = wrap360(93.2720950 + 483202.0175233 * T);     // argument of latitude

    double D_r  = D  * DEG2RAD;
    double M_r  = M  * DEG2RAD;
    double Mp_r = Mp * DEG2RAD;
    double F_r  = F  * DEG2RAD;

    // Ecliptic longitude correction (abbreviated — top 6 terms from Meeus Table 47.A)
    double dlon = 6.289 * std::sin(Mp_r);
    dlon += -1.274 * std::sin(Mp_r - 2.0 * D_r);
    dlon +=  0.658 * std::sin(2.0 * D_r);
    dlon += -0.186 * std::sin(M_r);
    dlon += -0.059 * std::sin(2.0 * Mp_r - 2.0 * D_r);
    dlon += -0.057 * std::sin(Mp_r - 2.0 * D_r + M_r);

    // Ecliptic latitude (abbreviated top 6 terms from Meeus Table 47.B)
    double beta = 5.128 * std::sin(F_r);
    beta +=  0.281 * std::sin(Mp_r + F_r);
    beta += -0.278 * std::sin(F_r - Mp_r);
    beta += -0.173 * std::sin(F_r - 2.0 * D_r);
    beta +=  0.055 * std::sin(Mp_r + F_r - 2.0 * D_r);
    beta += -0.046 * std::sin(Mp_r - F_r - 2.0 * D_r);

    double lambda = Lp + dlon;
    double lambda_r = lambda * DEG2RAD;
    double beta_r = beta * DEG2RAD;
    double eps = 23.439 - 0.0000004 * (jd - 2451545.0);
    double eps_r = eps * DEG2RAD;

    // Ecliptic -> equatorial
    double ra = std::atan2(std::sin(lambda_r) * std::cos(eps_r) -
                           std::tan(beta_r) * std::sin(eps_r),
                           std::cos(lambda_r));
    double dec = std::asin(std::sin(beta_r) * std::cos(eps_r) +
                           std::cos(beta_r) * std::sin(eps_r) * std::sin(lambda_r));

    if (phase_illum) {
        // Illuminated fraction ~= (1 - cos(phase_angle)) / 2
        // phase_angle ≈ 180° - D - 6.289°*sin(Mp) + 2.1°*sin(M)  (Meeus 48.4 simplified)
        double pa_deg = 180.0 - D - 6.289 * std::sin(Mp_r) + 2.100 * std::sin(M_r);
        double pa_r = pa_deg * DEG2RAD;
        *phase_illum = (1.0 - std::cos(pa_r)) * 0.5;
    }

    return equatorial_to_azel(wrap360(ra * RAD2DEG), dec * RAD2DEG,
                              observer_lat_deg, observer_lon_deg, jd);
}

// Given an ENU position relative to observer, compute its (az, el) as seen from observer.
inline AzEl enu_to_azel(const glm::dvec3& enu) {
    double horiz = std::sqrt(enu.x * enu.x + enu.y * enu.y);
    double el = std::atan2(enu.z, horiz) * RAD2DEG;
    double az = std::atan2(enu.x, enu.y) * RAD2DEG;  // atan2(east, north) = compass bearing
    return { wrap360(az), el };
}

}  // namespace celestial
