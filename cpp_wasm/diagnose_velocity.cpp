#include "cpp/velocity_dealias.h"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>

std::pair<std::vector<size_t>, std::vector<int>> find_bzip2_block_offsets(const uint8_t*, size_t);
void unzip_process_ldm_worker(size_t, const size_t*, const int*, const uint8_t*, size_t, AllTilt&);
AllTilt combine_all_tilts_from_thread_results(std::vector<AllTilt>&);

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "Usage: diagnose_velocity LEVEL2_FILE ELEVATION_OR_ALL OUTPUT.csv\n";
        return 1;
    }
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) return 1;
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(input)), {});
    const auto blocks = find_bzip2_block_offsets(data.data(), data.size());
    std::vector<AllTilt> results(blocks.first.size());
    for (size_t i = 0; i < results.size(); ++i) {
        unzip_process_ldm_worker(i, blocks.first.data(), blocks.second.data(), data.data(), data.size(), results[i]);
    }
    AllTilt volume = combine_all_tilts_from_thread_results(results);
    const bool all = std::string(argv[2]) == "all";
    const float elevation = all ? 0.0f : std::stof(argv[2]);
    int selected = -1;
    for (size_t i = 0; i < volume.Tilts.size(); ++i) {
        const auto& tilt = volume.Tilts[i];
        std::cout << "tilt " << i << " elevation=" << tilt.ElevationAngle << " rays=" << tilt.VelocityRays.size()
                  << " gates=" << tilt.Radials_VEL.size() / 3 << " reflectivity=" << tilt.Radials_REF.size() / 3
                  << " ms=" << tilt.msg_31.collect_ms << '\n';
        if (tilt.Radials_VEL.empty()) continue;
        if (selected < 0 || std::fabs(tilt.ElevationAngle - elevation) < std::fabs(volume.Tilts[selected].ElevationAngle - elevation)) selected = i;
    }
    if (selected < 0) return 1;
    std::vector<std::vector<float>> raw(volume.Tilts.size());
    for (size_t i = 0; i < volume.Tilts.size(); ++i) raw[i] = volume.Tilts[i].Radials_VEL;
    dealias_velocity_volume(volume);
    std::ofstream output(argv[3]);
    if (!output) return 1;
    output << "tilt,elevation,collect_ms,ray,azimuth,range,nyquist,raw,corrected\n" << std::setprecision(9);
    const size_t first = all ? 0 : static_cast<size_t>(selected);
    const size_t last = all ? volume.Tilts.size() : first + 1;
    for (size_t i = first; i < last; ++i) {
        const auto& tilt = volume.Tilts[i];
        size_t changed = 0;
        for (size_t r = 0; r < tilt.VelocityRays.size(); ++r) {
            const auto& ray = tilt.VelocityRays[r];
            for (size_t g = ray.start; g < ray.start + ray.count; ++g) {
                output << i << ',' << tilt.ElevationAngle << ',' << tilt.msg_31.collect_ms << ',' << r << ','
                       << raw[i][3 * g] << ',' << raw[i][3 * g + 1] << ',' << ray.nyquist << ','
                       << raw[i][3 * g + 2] << ',' << tilt.Radials_VEL[3 * g + 2] << '\n';
                changed += std::fabs(raw[i][3 * g + 2] - tilt.Radials_VEL[3 * g + 2]) > 0.01f;
            }
        }
        std::cout << "tilt " << i << " corrected " << changed << '/' << raw[i].size() / 3 << " gates\n";
    }
    return output ? 0 : 1;
}
