// compare_dealias.cpp
//
// Build:  make compare_dealias
// Usage:  ./build/compare_dealias LEVEL2_FILE ELEVATION_OR_ALL OUTPUT.csv
//
// Runs v10_1, v10_2 and v10_3 dealiasers on the requested tilt(s), writes one
// CSV row per velocity gate:
//
//   tilt, elevation, collect_ms, ray, azimuth, range, nyquist, raw, v10_1, v10_2, v10_3
//
// Pass "all" as the second argument to export every tilt.
// Prints per-tilt timing and inter-version differences to stdout.
//
#include "cpp/velocity_dealias_v10_1.h"
#include "cpp/velocity_dealias_v10_2.h"
#include "cpp/velocity_dealias_v10_3.h"
#include "cpp/velocity_dealias_pyart_region.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

// Declared in nexrad_image_builder.cpp / main.cpp
std::pair<std::vector<size_t>, std::vector<int>>
find_bzip2_block_offsets(const uint8_t*, size_t);

void unzip_process_ldm_worker(size_t, const size_t*, const int*,
                               const uint8_t*, size_t, AllTilt&);

AllTilt combine_all_tilts_from_thread_results(std::vector<AllTilt>&);

using Clock = std::chrono::steady_clock;
static double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "Usage: compare_dealias LEVEL2_FILE ELEVATION_OR_ALL OUTPUT.csv\n"
                  << "       ELEVATION_OR_ALL: a float like 0.5 or the literal \"all\"\n";
        return 1;
    }

    std::ifstream input(argv[1], std::ios::binary);
    if (!input) { std::cerr << "Cannot open " << argv[1] << '\n'; return 1; }
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(input)), {});
    input.close();

    const auto blocks = find_bzip2_block_offsets(data.data(), data.size());
    std::vector<AllTilt> results(blocks.first.size());
    for (size_t i = 0; i < results.size(); ++i)
        unzip_process_ldm_worker(i, blocks.first.data(), blocks.second.data(),
                                 data.data(), data.size(), results[i]);

    const AllTilt volume = combine_all_tilts_from_thread_results(results);

    const bool all = std::string(argv[2]) == "all";
    const float elevation = all ? 0.0f : std::stof(argv[2]);

    // Print tilt inventory and find the closest-elevation tilt.
    int selected = -1;
    for (size_t i = 0; i < volume.Tilts.size(); ++i) {
        const auto& tilt = volume.Tilts[i];
        std::cout << "tilt " << i
                  << "  el=" << tilt.ElevationAngle
                  << "  rays=" << tilt.VelocityRays.size()
                  << "  vel_gates=" << tilt.Radials_VEL.size() / 3
                  << "  ref_gates=" << tilt.Radials_REF.size() / 3
                  << "  ms=" << tilt.msg_31.collect_ms << '\n';
        if (tilt.Radials_VEL.empty()) continue;
        if (selected < 0 ||
            std::fabs(tilt.ElevationAngle - elevation) <
            std::fabs(volume.Tilts[selected].ElevationAngle - elevation))
            selected = static_cast<int>(i);
    }
    if (selected < 0) { std::cerr << "No velocity tilts found\n"; return 1; }
    if (!all) std::cout << "\nSelected tilt " << selected
                        << " (el=" << volume.Tilts[selected].ElevationAngle << ")\n";

    // Snapshot raw values before any dealiasing.
    std::vector<std::vector<float>> raw(volume.Tilts.size());
    for (size_t i = 0; i < volume.Tilts.size(); ++i)
        raw[i] = volume.Tilts[i].Radials_VEL;

    // Run v10_1.
    AllTilt vol1 = volume;
    auto t0 = Clock::now();
    dealias_velocity_volume_v10_1(vol1);
    const double ms1 = ms_since(t0);

    // Run v10_2.
    AllTilt vol2 = volume;
    t0 = Clock::now();
    dealias_velocity_volume_v10_2(vol2);
    const double ms2 = ms_since(t0);

    // Run v10_3.
    AllTilt vol3 = volume;
    t0 = Clock::now();
    dealias_velocity_volume_v10_3(vol3);
    const double ms3 = ms_since(t0);

    // Run pyart_region.
    AllTilt vol_pa = volume;
    t0 = Clock::now();
    dealias_velocity_volume_pyart_region(vol_pa);
    const double ms_pa = ms_since(t0);

    std::cout << "\nv10_1:       " << ms1  << " ms\n"
              << "v10_2:       " << ms2  << " ms\n"
              << "v10_3:       " << ms3  << " ms\n"
              << "pyart_region:" << ms_pa << " ms\n\n";

    // Write CSV.
    std::ofstream output(argv[3]);
    if (!output) { std::cerr << "Cannot open output: " << argv[3] << '\n'; return 1; }
    output << "tilt,elevation,collect_ms,ray,azimuth,range,nyquist,raw,v10_1,v10_2,v10_3,pyart_region\n"
           << std::fixed << std::setprecision(6);

    const size_t first = all ? 0 : static_cast<size_t>(selected);
    const size_t last  = all ? volume.Tilts.size() : first + 1;

    size_t total_gates = 0, total_diff = 0;
    for (size_t i = first; i < last; ++i) {
        const auto& tilt = volume.Tilts[i];
        if (tilt.VelocityRays.empty()) continue;
        size_t tilt_diff = 0, tilt_gates = 0;
        for (size_t r = 0; r < tilt.VelocityRays.size(); ++r) {
            const auto& ray = tilt.VelocityRays[r];
            for (size_t g = ray.start; g < ray.start + ray.count; ++g) {
                const float rv  = raw[i][3 * g + 2];
                const float v1  = vol1.Tilts[i].Radials_VEL[3 * g + 2];
                const float v2  = vol2.Tilts[i].Radials_VEL[3 * g + 2];
                const float v3  = vol3.Tilts[i].Radials_VEL[3 * g + 2];
                const float vpa = vol_pa.Tilts[i].Radials_VEL[3 * g + 2];
                output << i << ',' << tilt.ElevationAngle << ',' << tilt.msg_31.collect_ms
                       << ',' << r
                       << ',' << raw[i][3 * g]        // azimuth
                       << ',' << raw[i][3 * g + 1]    // range
                       << ',' << ray.nyquist
                       << ',' << rv
                       << ',' << v1
                       << ',' << v2
                       << ',' << v3
                       << ',' << vpa << '\n';
                const bool d12 = std::isfinite(v1) && std::isfinite(v2) && std::fabs(v1 - v2) > 0.05f;
                const bool d13 = std::isfinite(v1) && std::isfinite(v3) && std::fabs(v1 - v3) > 0.05f;
                tilt_diff  += d12;
                ++tilt_gates;
                (void)d13;
            }
        }
        std::cout << "tilt " << i
                  << "  el=" << tilt.ElevationAngle
                  << "  gates=" << tilt_gates
                  << "  v10_1 vs v10_2 differ=" << tilt_diff << '\n';
        total_gates += tilt_gates;
        total_diff  += tilt_diff;
    }
    std::cout << "\ntotal gates=" << total_gates
              << "  v10_1 vs v10_2 differ=" << total_diff << '\n';

    return output.good() ? 0 : 1;
}
