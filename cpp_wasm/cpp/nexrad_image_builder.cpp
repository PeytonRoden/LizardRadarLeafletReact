
#include "nexrad_image_builder.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>
#include <string>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <emscripten.h>
#include "stb_image_write.h"

extern std::vector<uint8_t> png_buffer;


void colormap_velocity(float vel, uint8_t& r, uint8_t& g, uint8_t& b, float VNYQ = 30.0f) {
    if (std::isnan(vel) || std::isinf(vel)) {
        r = g = b = 0;
        return;
    }

    // Clamp velocity into [-VNYQ, VNYQ]
    //should already be folded
    //float v = std::max(-VNYQ, std::min(VNYQ, vel));
    float v = vel;
    VNYQ = VNYQ/100;

    //std::cout << v << ", ";

    // Normalize to [-1, 1]
    float norm = v / VNYQ;

    //std::cout << ": norm: "<< norm << ", ";

    // Near zero → gray (shrink this threshold!)
    if (std::fabs(norm) < 0.00000001f) { // ±1% of Nyquist
        r = g = b = 128;
        return;
    }

    if (norm < 0) {
        // Toward radar (negative) → green
        float t = (norm + 1.0f); // maps [-1,0] → [0,1]
        r = static_cast<uint8_t>(30 * t);
        g = static_cast<uint8_t>(180 + 75 * t);
        b = static_cast<uint8_t>(30 * t);
    } else {
        // Away from radar (positive) → red
        float t = (1.0f - norm); // maps [0,1] → [1,0]
        r = static_cast<uint8_t>(180 + 75 * t);
        g = static_cast<uint8_t>(30 * t);
        b = static_cast<uint8_t>(30 * t);
    }
}



constexpr float kt2ms = 0.51444f;

void colormap_velocity_evans(float vel, uint8_t& r, uint8_t& g, uint8_t& b,  float VNYQ = 30.0f) {
    //https://www.wxtools.org/velocity/awips-evans
    if (std::isnan(vel) || std::isinf(vel)) {
        r = g = b = 0;
        return;
    }

    VNYQ = VNYQ/100;

    // Convert from knots to m/s
    vel *= 1.0f; // if vel already in m/s, leave as-is

    if (vel <= -120*kt2ms)      { r = 255; g = 0;   b = 128; return; }
    else if (vel <= -90.5*kt2ms){ r =  static_cast<uint8_t>(255 + (0-255)*(vel+120*kt2ms)/(29.5*kt2ms));
                                   g = static_cast<uint8_t>(0 + (0-0)*(vel+120*kt2ms)/(29.5*kt2ms));
                                   b = static_cast<uint8_t>(128 + (160-128)*(vel+120*kt2ms)/(29.5*kt2ms)); return; }
    else if (vel <= -70*kt2ms)  { r = 0; g = static_cast<uint8_t>(0 + (224-0)*(vel+90.5*kt2ms)/(20.5*kt2ms)); b = static_cast<uint8_t>(160 + (255-160)*(vel+90.5*kt2ms)/(20.5*kt2ms)); return; }
    else if (vel <= -69.99*kt2ms){ r = 0; g = 255; b = 224; return; }
    else if (vel <= -60*kt2ms)   { r = 0; g = 255; b = 225; return; }
    else if (vel <= -59.99*kt2ms){ r = 160; g = 255; b = 208; return; }
    else if (vel <= -50*kt2ms)   { r = 160; g = 255; b = 208; return; }
    else if (vel <= -49.99*kt2ms){ r = 160; g = 255; b = 208; return; }
    else if (vel <= -40*kt2ms)   { r = 0; g = 255; b = 0; return; }
    else if (vel <= -10*kt2ms)   { r = 16; g = 96; b = 16; return; }
    else if (vel <= -0.01*kt2ms) { r = 112; g = 128; b = 112; return; }
    else if (vel <= 0)            { r = 144; g = 128; b = 144; return; }
    else if (vel <= 10*kt2ms)     { r = 112; g = 0; b = 0; return; }
    else if (vel <= 40*kt2ms)     { r = 255; g = 0; b = 0; return; }
    else if (vel <= 48.6*kt2ms)   { r = 255; g = 0; b = 128; return; }
    else if (vel <= 49.5*kt2ms)   { r = 255; g = 0; b = 144; return; }
    else if (vel <= 69.99*kt2ms)  { r = 255; g = 196; b = 255; return; }
    else if (vel <= 70*kt2ms)     { r = 255; g = 96; b = 0; return; }
    else if (vel <= 120*kt2ms)    { r = 255; g = 255; b = 0; return; }

    // Fallback RF
    r = 128; g = 0; b = 208;
}

void colormap_dbz(float dbz, uint8_t& r, uint8_t& g, uint8_t& b) {
    if (std::isnan(dbz) || std::isinf(dbz)) {
        r = g = b = 0;
        return;
    }

    struct ColorStep {
        float threshold;
        uint8_t r, g, b;
    };

    static const ColorStep scale[] = {
        { 5.0f,   4, 233, 231 },  // light blue
        { 10.0f,  1, 159, 244 },
        { 15.0f,  3, 0, 244 },
        { 20.0f,  2, 253, 2 },
        { 25.0f,  1, 197, 1 },
        { 30.0f,  0, 142, 0 },
        { 35.0f,  253, 248, 2 },
        { 40.0f,  229, 188, 0 },
        { 45.0f,  253, 149, 0 },
        { 50.0f,  253, 0, 0 },
        { 55.0f,  212, 0, 0 },
        { 60.0f,  188, 0, 0 },
        { 65.0f,  248, 0, 253 }, // magenta
        { 70.0f,  152, 84, 198 },
        { INFINITY, 255, 255, 255 }  // fallback (white)
    };

    for (const auto& step : scale) {
        if (dbz < step.threshold) {
            r = step.r;
            g = step.g;
            b = step.b;
            return;
        }
    }
}

void colormap_reflectivity_lacrosse(float dbz, uint8_t& r, uint8_t& g, uint8_t& b) {
    //https://www.wxtools.org/reflectivity/2004-lacrosse-br
    if (std::isnan(dbz) || std::isinf(dbz)) {
        r = g = b = 0;
        return;
    }

    if (dbz <= -30) { r = 0; g = 0; b = 0; return; }
    else if (dbz <= 0)   { r = 105; g = 126; b = 108; return; }
    else if (dbz <= 5)   { r = 94; g = 94; b = 94; return; }
    else if (dbz <= 10)  { r = 131; g = 131; b = 131; return; }
    else if (dbz <= 15)  { r = 171; g = 171; b = 171; return; }
    else if (dbz <= 20)  { r = 0; g = 225; b = 255; return; }
    else if (dbz <= 25)  { r = 0; g = 150; b = 255; return; }
    else if (dbz <= 30)  { r = 0; g = 255; b = 0; return; }
    else if (dbz <= 35)  { r = 0; g = 180; b = 0; return; }
    else if (dbz <= 40)  { r = 255; g = 250; b = 0; return; }
    else if (dbz <= 45)  { r = 255; g = 170; b = 0; return; }
    else if (dbz <= 50)  { r = 255; g = 0; b = 0; return; }
    else if (dbz <= 60)  { r = 254; g = 222; b = 255; return; }
    else if (dbz <= 65)  { r = 255; g = 99; b = 255; return; }
    else if (dbz <= 70)  { r = 173; g = 0; b = 176; return; }
    else if (dbz <= 75)  { r = 255; g = 255; b = 255; return; }

    // Anything above max
    r = 255; g = 255; b = 255;
}

void colormap_correlation_coefficient(float rho, uint8_t& r, uint8_t& g, uint8_t& b) {
    //https://www.wxtools.org/correlation-coefficient/awips-rho-cc
    if (std::isnan(rho) || std::isinf(rho)) {
        r = g = b = 0;
        return;
    }

    if (rho >= 1.05f) { r = 164; g = 54;  b = 150; return; }
    else if (rho >= 1.00f) { r = 255; g = 180; b = 215; return; }
    else if (rho >= 0.99f) { r = 139; g = 30;  b = 77; return; }
    else if (rho >= 0.97f) { r = 225; g = 3;   b = 0; return; }
    else if (rho >= 0.95f) { r = 255; g = 140; b = 0; return; }
    else if (rho >= 0.90f) { r = 255; g = 255; b = 0; return; }
    else if (rho >= 0.85f) { r = 135; g = 215; b = 10; return; }
    else if (rho >= 0.80f) { r = 95;  g = 245; b = 100; return; }
    else if (rho >= 0.75f) { r = 120; g = 120; b = 255; return; }
    else if (rho >= 0.60f) { r = 10;  g = 10;  b = 190; return; }
    else if (rho >= 0.45f) { r = 15;  g = 15;  b = 140; return; }
    else                   { r = 15;  g = 15;  b = 140; return; }
}

void colormap_spectrum_width(float vel_ms, uint8_t& r, uint8_t& g, uint8_t& b) {
    //https://www.wxtools.org/spectrum-width/bens-sw
    if (std::isnan(vel_ms) || std::isinf(vel_ms)) {
        r = g = b = 0;
        return;
    }

    // Convert m/s → knots
    //float vel = vel_ms * 1.9426f;
    float vel = vel_ms * 1.0f;

    if (vel <= 0)      { r = 20;  g = 5;   b = 72;  return; }
    else if (vel <= 2) { r = 50;  g = 20;  b = 140; return; }
    else if (vel <= 4) { r = 124; g = 38;  b = 190; return; }
    else if (vel <= 7) { r = 218; g = 55;  b = 120; return; }
    else if (vel <= 15){ r = 251; g = 126; b = 33;  return; }
    else if (vel <= 18){ r = 255; g = 255; b = 0;   return; }
    else if (vel <= 22){ r = 153; g = 255; b = 51;  return; }
    else if (vel <= 30){ r = 0;   g = 153; b = 230; return; }
    else if (vel <= 35){ r = 0;   g = 17;  b = 26;  return; }
    else if (vel <= 40){ r = 255; g = 255; b = 255; return; }

    // Fallback RF color
    r = 117; g = 0; b = 117;
}


std::vector<uint8_t> saveTiltAsPNGInterpolate2(const SingleTilt& tilt, const std::string& filename, const int SIZE, float M_PER_PIXEL) {


    //Count number of moment values for each moment
    //std::cout << "Number of moment values for REF: " << tilt.Radials_REF.size() << std::endl;
    //std::cout << "Number of moment values for VEL: " << tilt.Radials_VEL.size() << std::endl;
    //std::cout << "Number of moment values for SW: " << tilt.Radials_SW.size() << std::endl;
    //std::cout << "Number of moment values for RHO: " << tilt.Radials_RHO.size() << std::endl;
    //std::cout << "Number of moment values for ZDR: " << tilt.Radials_ZDR.size() << std::endl;
    //std::cout << "Number of moment values for PHI: " << tilt.Radials_PHI.size() << std::endl;

    const int CENTER = SIZE / 2;

    float center_x = CENTER * M_PER_PIXEL;
    float center_y = CENTER * M_PER_PIXEL;


    const float beam_half_width_rad = 0.5f * M_PI / 180.0f; // 0.5 degree, not 0.5 radians

    int nyquist_vel = tilt.vol_el_rad.rad.nyquist_vel;
    //std::cout << "nyquist: " << nyquist_vel << std::endl;

    struct GridPoint {
        float value = 0.0f;
        int   count = 0;
        float distance_sq = 0.0f;
    };

    float gate_width_m = tilt.gateSpacing;
    float half_gate = gate_width_m * 0.5f;
    const float inv_m_per_pixel = 1.0f / M_PER_PIXEL;

    struct RadialCache {
        float sin_az;
        float cos_az;
        float inner_r;
        float outer_r;
        float value;
        float inner_r_sq;
        float outer_r_sq;
        float min_x;
        float max_x;
        float min_y;
        float max_y;
    };

    auto selected_radial = get_moment_radials(tilt, selected_radar_moment);
    if (selected_radial == nullptr) {
        std::cerr << "Invalid moment: " << selected_radar_moment << std::endl;
        return {};
    }

    std::vector<RadialCache> radial_cache;
    radial_cache.reserve(selected_radial->size());

    //for (const auto& radial : *selected_radial) {
    float azimuth_deg;
    float dist;
    float value;

    for (size_t i = 0; i + 2 < selected_radial->size(); i += 3) {
        azimuth_deg = (*selected_radial)[i];
        dist = (*selected_radial)[i + 1];
        value = (*selected_radial)[i + 2];
        
        if (std::isnan(value)) continue;



        //float az_rad = radial.azimuth_deg * M_PI / 180.0f;
        float az_rad = (90.0f - azimuth_deg) * M_PI / 180.0f;
        float cos_az_rad = std::cos(az_rad);
        float sin_az_rad = std::sin(az_rad);

        float inner_r = std::max(0.0f, dist - half_gate);
        float outer_r = dist + half_gate;

        // bounding box corners (in meters from origin)
        float x_plusrad_plusdist = (dist + half_gate) * std::cos(az_rad + beam_half_width_rad)  + center_x;
        float x_plusrad_minusdist = (dist - half_gate) * std::cos(az_rad + beam_half_width_rad)+ center_x;
        
        float x_minusrad_plusdist = (dist + half_gate)  * std::cos(az_rad - beam_half_width_rad)+ center_x;
        float x_minusrad_minusdist = (dist -  half_gate)  * std::cos(az_rad - beam_half_width_rad)+ center_x;

        float y_plusrad_plusdist = (dist + half_gate) *std::sin(az_rad + beam_half_width_rad)+ center_y;
        float y_plusrad_minusdist = (dist - half_gate) *std::sin(az_rad + beam_half_width_rad)+ center_y;

        float y_minusrad_plusdist = (dist + half_gate) * std::sin(az_rad - beam_half_width_rad)+ center_y;
        float y_minusrad_minusdist = (dist - half_gate) * std::sin(az_rad - beam_half_width_rad)+ center_y;

        float min_x = std::min({x_plusrad_minusdist, x_plusrad_plusdist, x_minusrad_minusdist, x_minusrad_plusdist});
        float max_x = std::max({x_plusrad_minusdist, x_plusrad_plusdist, x_minusrad_minusdist, x_minusrad_plusdist});
        float min_y = std::min({y_plusrad_minusdist, y_plusrad_plusdist, y_minusrad_minusdist, y_minusrad_plusdist});
        float max_y = std::max({y_plusrad_minusdist, y_plusrad_plusdist, y_minusrad_minusdist, y_minusrad_plusdist});

        radial_cache.push_back({
            sin_az_rad,
            cos_az_rad,
            inner_r,
            outer_r,
            value,
            inner_r * inner_r,
            outer_r * outer_r,
            min_x,
            max_x,
            min_y,
            max_y
        });
    }

    std::vector<GridPoint> grid(SIZE * SIZE);
    std::vector<float> interpolated(SIZE * SIZE, std::numeric_limits<float>::quiet_NaN());

    std::vector<float> xm_for_px(SIZE);
    std::vector<float> ym_for_py(SIZE);
    for (int px = 0; px < SIZE; ++px) xm_for_px[px] = (px - CENTER) * M_PER_PIXEL;
    for (int py = 0; py < SIZE; ++py) ym_for_py[py] = (CENTER - py) * M_PER_PIXEL;

    const float beam_half_angle = 0.5f * M_PI / 180.0f;
    const float cos_tol = std::cos(beam_half_angle);
    const float cos_tol_sq = cos_tol * cos_tol;
    const float min_r2_eps = 1e-6f;

    for (const auto& rad : radial_cache) {
        // Convert bounding box meters → pixel coords
        int px_min = std::max(0, int(std::floor((rad.min_x - center_x) / M_PER_PIXEL + CENTER)));
        int px_max = std::min(SIZE - 1, int(std::floor((rad.max_x - center_x) / M_PER_PIXEL + CENTER)));
        int py_min = std::max(0, int(std::floor((center_y - rad.max_y) / M_PER_PIXEL + CENTER)));
        int py_max = std::min(SIZE - 1, int(std::floor((center_y - rad.min_y) / M_PER_PIXEL + CENTER)));


        for (int py = py_min; py <= py_max; ++py) {
            const float ym = ym_for_py[py];
            const float ym_sq = ym * ym;
            for (int px = px_min; px <= px_max; ++px) {

                const float xm = xm_for_px[px];
                const float r2 = xm * xm + ym_sq;

                if (r2 < rad.inner_r_sq || r2 > rad.outer_r_sq) continue;
                if (r2 <= min_r2_eps) continue;

                const float dot = xm * rad.cos_az + ym * rad.sin_az; // fixed alignment
                if (dot * dot < cos_tol_sq * r2) continue;

                int idx = py * SIZE + px;
                GridPoint &g = grid[idx];
                g.value += rad.value;
                g.count += 1;
                g.distance_sq = r2;
            }
        }
    }

    const float thresh1000_sq = 1000.0f * 1000.0f;
    for (int y = 0; y < SIZE; ++y) {
        for (int x = 0; x < SIZE; ++x) {
            int idx = y * SIZE + x;
            GridPoint &g = grid[idx];
            if (g.count > 0 && g.distance_sq > thresh1000_sq) {
                interpolated[idx] = g.value / static_cast<float>(g.count);
            }
        }
    }

    std::vector<uint8_t> image(SIZE * SIZE * 4, 0);
    for (int y = 0; y < SIZE; ++y) {
        for (int x = 0; x < SIZE; ++x) {
            int idx_interp = y * SIZE + x;
            float value = interpolated[idx_interp];
            int out_i = idx_interp * 4;

            if (!std::isnan(value) && std::fabs(value) >= 0.01f) {
                uint8_t r, g, b;

                if (cStringsEqual(selected_radar_moment, "REF")) {
                    colormap_reflectivity_lacrosse(value, r, g, b);
                }
                else if (cStringsEqual(selected_radar_moment, "VEL")) {
                    //std::cout << "nyquist velocity : "<< nyquist_vel << std::endl;
                    colormap_velocity_evans(value, r, g, b, float(nyquist_vel));
                }
                else if (cStringsEqual(selected_radar_moment, "SW ")) {
                    colormap_spectrum_width(value, r, g, b); 
                }
                else if (cStringsEqual(selected_radar_moment, "ZDR")) {
                    colormap_dbz(value, r, g, b);
                }
                else if (cStringsEqual(selected_radar_moment, "PHI")) {
                    colormap_dbz(value, r, g, b);
                }
                else if (cStringsEqual(selected_radar_moment, "RHO")) {
                    colormap_correlation_coefficient(value, r, g, b);
                }
                else {
                    colormap_dbz(value, r, g, b);
                }

                //colormap_dbz(value, r, g, b);
                image[out_i + 0] = r;
                image[out_i + 1] = g;
                image[out_i + 2] = b;
                image[out_i + 3] = 255;
            } else {
                image[out_i + 0] = 0;
                image[out_i + 1] = 0;
                image[out_i + 2] = 0;
                image[out_i + 3] = 0;
            }
        }
    }

    png_buffer.clear();
    stbi_write_png_to_func(
        [](void* context, void* data, int size) {
            auto* out = static_cast<std::vector<uint8_t>*>(context);
            out->insert(out->end(), (uint8_t*)data, (uint8_t*)data + size);
        },
        &png_buffer,
        SIZE, SIZE, 4,
        image.data(), SIZE * 4
    );

    //std::cout << "Saved RGBA image with transparency to buffer." << std::endl;

    return png_buffer;
}





// Direct libm calls: the LRU cache's hash/list overhead exceeded the libm
// cost for this mostly-unique set of azimuths/ranges, and the cache was not
// thread-safe for the WASM/pthread build.
inline double lru_cache_sin(double value) {
    return std::sin(value);
}

inline double lru_cache_cos(double value) {
    return std::cos(value);
}

inline double lru_cache_asin(double value) {
    return std::asin(value);
}

// Inputs:
//   radarLatDeg, radarLonDeg : radar location
//   rangeKm                  : bin-center range from radar
//   azimuthDeg               : clockwise from north
//   elevationDeg             : beam elevation
//
// Returns:
//   latitude/longitude in degrees
//
// Uses:
//   4/3 effective Earth radius for beam curvature
//   local tangent-plane approximation for lat/lon
inline LatLon nexradBinLatLonPrepared(
    double radarLatRad,
    double radarLonRad,
    double cosRadarLat,
    double sinElevation,
    double cosElevation,
    double rangem,
    double sinAzimuth,
    double cosAzimuth)
{
    float rangeKm = rangem / 1000.0f;
    constexpr double Re = 6371.0;          // km
    constexpr double R  = Re * (4.0 / 3.0); // effective Earth radius

    // Beam height above the effective Earth surface.
    const double h =
        std::sqrt(
            rangeKm * rangeKm +
            R * R +
            2.0 * rangeKm * R * sinElevation
        ) - R;

    // Ground-range arc on the effective Earth.
    const double groundRange =
        R * lru_cache_asin(
            (rangeKm * cosElevation) / (R + h)
        ); 

    // Local east/north displacement.
    const double east  = groundRange * sinAzimuth;
    const double north = groundRange * cosAzimuth;

    // Cheap tangent-plane conversion to lat/lon.
    return {
        (radarLatRad + north / Re) * (180.0 / 3.14159265358979323846),
        (radarLonRad + east / (Re * cosRadarLat)) * (180.0 / 3.14159265358979323846)
    };
}

inline LatLon nexradBinLatLon(
    double radarLatDeg,
    double radarLonDeg,
    double rangem,
    double azimuth,
    double elevationDeg)
{
    constexpr double PI = 3.14159265358979323846;
    const double radarLatRad = radarLatDeg * PI / 180.0;
    return nexradBinLatLonPrepared(
        radarLatRad,
        radarLonDeg * PI / 180.0,
        lru_cache_cos(radarLatRad),
        lru_cache_sin(elevationDeg * PI / 180.0),
        lru_cache_cos(elevationDeg * PI / 180.0),
        rangem,
        lru_cache_sin(azimuth),
        lru_cache_cos(azimuth));
}


float* buildMomentDataVerticesThenValue(const SingleTilt& tilt, int* output_size) {

    //start with 2d vector, will be reorged to contiguous array
    std::vector<std::array<float, 7>> moment_data;


    //Count number of moment values for each moment
    //std::cout << "Number of moment values for REF: " << tilt.Radials_REF.size() << std::endl;
    //std::cout << "Number of moment values for VEL: " << tilt.Radials_VEL.size() << std::endl;
    //std::cout << "Number of moment values for SW: " << tilt.Radials_SW.size() << std::endl;
    //std::cout << "Number of moment values for RHO: " << tilt.Radials_RHO.size() << std::endl;
    //std::cout << "Number of moment values for ZDR: " << tilt.Radials_ZDR.size() << std::endl;
    //std::cout << "Number of moment values for PHI: " << tilt.Radials_PHI.size() << std::endl;


    const float beam_half_width_rad = 0.5f * M_PI / 180.0f; // 0.5 degree, not 0.5 radians

    int nyquist_vel = tilt.vol_el_rad.rad.nyquist_vel;
    //std::cout << "nyquist: " << nyquist_vel << std::endl;

    struct GridPoint {
        float value = 0.0f;
        int   count = 0;
        float distance_sq = 0.0f;
    };

    float gate_width_m = tilt.gateSpacing;
    float half_gate = gate_width_m * 0.5f;

    float center_latitude = tilt.vol_el_rad.vol.lat;
    float center_longitude = tilt.vol_el_rad.vol.lon;
    constexpr double PI = 3.14159265358979323846;
    const double radar_lat_rad = center_latitude * PI / 180.0;
    const double radar_lon_rad = center_longitude * PI / 180.0;
    const double cos_radar_lat = lru_cache_cos(radar_lat_rad);
    const double elevation_rad = tilt.ElevationAngle * PI / 180.0;
    const double sin_elevation = lru_cache_sin(elevation_rad);
    const double cos_elevation = lru_cache_cos(elevation_rad);


    auto selected_radial = get_moment_radials(tilt, selected_radar_moment);
    if (selected_radial == nullptr) {
        std::cerr << "Invalid moment: " << selected_radar_moment << std::endl;
        if (output_size != nullptr) *output_size = 0;
        return nullptr;
    }

    //for (const auto& radial : *selected_radial) {
    float aziumuth_deg;
    float dist;
    float value;

    for (size_t i = 0; i + 2 < selected_radial->size(); i += 3) {
        aziumuth_deg = (*selected_radial)[i];
        dist = (*selected_radial)[i + 1];
        value = (*selected_radial)[i + 2];
        
        if (std::isnan(value)) continue;

        //float az_rad = radial.azimuth_deg * M_PI / 180.0f;
        float az_rad = aziumuth_deg * M_PI / 180.0f;
        float inner_r = std::max(0.0f, dist - half_gate);
        float outer_r = dist + half_gate;


        const double inner_az = static_cast<double>(az_rad) - beam_half_width_rad;
        const double outer_az = static_cast<double>(az_rad) + beam_half_width_rad;
        const double sin_inner_az = lru_cache_sin(inner_az);
        const double cos_inner_az = lru_cache_cos(inner_az);
        const double sin_outer_az = lru_cache_sin(outer_az);
        const double cos_outer_az = lru_cache_cos(outer_az);

        // We will calculate 3 triangles per radial
        // bottom left, top left, middle bottom
        // top left, middle bottom, top right
        // middle bottom, top right, bottom right

        //first one
        LatLon bottom_left_latlon = nexradBinLatLonPrepared(
            radar_lat_rad, radar_lon_rad, cos_radar_lat,
            sin_elevation, cos_elevation, inner_r,
            sin_inner_az, cos_inner_az);
        LatLon top_left_latlon = nexradBinLatLonPrepared(
            radar_lat_rad, radar_lon_rad, cos_radar_lat,
            sin_elevation, cos_elevation, outer_r,
            sin_inner_az, cos_inner_az);
        LatLon middle_bottom_latlon = nexradBinLatLonPrepared(
            radar_lat_rad, radar_lon_rad, cos_radar_lat,
            sin_elevation, cos_elevation, inner_r,
            sin_outer_az, cos_outer_az);

        //unique for second one
        LatLon top_right_latlon = nexradBinLatLonPrepared(
            radar_lat_rad, radar_lon_rad, cos_radar_lat,
            sin_elevation, cos_elevation, outer_r,
            sin_outer_az, cos_outer_az);

        //unique for third one
        LatLon bottom_right_latlon = middle_bottom_latlon;

        moment_data.push_back({(float)bottom_left_latlon.lat,   (float)bottom_left_latlon.lon,
                            (float)top_left_latlon.lat,      (float)top_left_latlon.lon,
                            (float)middle_bottom_latlon.lat, (float)middle_bottom_latlon.lon,
                            value});

        moment_data.push_back({(float)top_left_latlon.lat,      (float)top_left_latlon.lon,
                            (float)middle_bottom_latlon.lat, (float)middle_bottom_latlon.lon,
                            (float)top_right_latlon.lat,     (float)top_right_latlon.lon,
                            value});

        moment_data.push_back({(float)middle_bottom_latlon.lat, (float)middle_bottom_latlon.lon,
                            (float)top_right_latlon.lat,     (float)top_right_latlon.lon,
                            (float)bottom_right_latlon.lat,  (float)bottom_right_latlon.lon,
                            value});

        }

    int rows = moment_data.size();
    int cols = 9;
    if (output_size != nullptr) *output_size = rows * cols;

    // 1. Allocate an array of float values
    float *shader_vals = new float[rows * cols];

    //copy over the data
    for (int i = 0; i < rows; ++i) {
        //get the moment value
        float moment_val = moment_data[i][6];


        shader_vals[(i * cols) + 0] = moment_data[i][1]; // longitude
        shader_vals[(i * cols) + 1] = moment_data[i][0]; // latitude
        shader_vals[(i * cols) + 2] = moment_val;
        shader_vals[(i * cols) + 3] = moment_data[i][3]; // longitude
        shader_vals[(i * cols) + 4] = moment_data[i][2]; // latitude
        shader_vals[(i * cols) + 5] = moment_val;
        shader_vals[(i * cols) + 6] = moment_data[i][5]; // longitude
        shader_vals[(i * cols) + 7] = moment_data[i][4]; // latitude
        shader_vals[(i * cols) + 8] = moment_val;
    }

    return shader_vals;
}

       
//stop gap method, eventually store single titl info like this
// dist, az, value
std::vector<float> new_shader_vals(const SingleTilt& tilt) {

    auto selected_radial = get_moment_radials(tilt, selected_radar_moment);
    if (selected_radial == nullptr) {
        std::cerr << "Invalid moment: " << selected_radar_moment << std::endl;
        return std::vector<float>();
    }


    std::vector<float> shader_vals;

    // for (const auto& radial : *selected_radial) {
    float aziumuth_deg;
    float dist;
    float value;

    for (size_t i = 0; i + 2 < selected_radial->size(); i += 3) {
        aziumuth_deg = (*selected_radial)[i];
        dist = (*selected_radial)[i + 1];
        value = (*selected_radial)[i + 2];
        
        if (std::isnan(value)) continue;

        shader_vals.push_back(dist);
        shader_vals.push_back(aziumuth_deg);
        shader_vals.push_back(value);
    }

    return shader_vals;
}
        

