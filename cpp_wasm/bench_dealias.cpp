#include "cpp/velocity_dealias.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
#include <utility>
#include <vector>

std::pair<std::vector<size_t>, std::vector<int>> find_bzip2_block_offsets(const uint8_t*, size_t);
void unzip_process_ldm_worker(size_t, const size_t*, const int*, const uint8_t*, size_t, AllTilt&);
AllTilt combine_all_tilts_from_thread_results(std::vector<AllTilt>&);

using Clock = std::chrono::steady_clock;

static double ms_since(Clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "Usage: bench_dealias LEVEL2_FILE\n";
        return 1;
    }
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) return 1;
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(input)), {});
    const auto blocks = find_bzip2_block_offsets(data.data(), data.size());

    // Mirror the real pipeline: 8 threads over bz2 blocks.
    auto t0 = Clock::now();
    std::vector<AllTilt> results(blocks.first.size());
    std::vector<std::thread> threads;
    constexpr size_t MAX_THREADS = 8;
    for (size_t i = 0; i < blocks.first.size(); ++i) {
        threads.emplace_back(unzip_process_ldm_worker, i, blocks.first.data(),
                             blocks.second.data(), data.data(), data.size(),
                             std::ref(results[i]));
        if (threads.size() == MAX_THREADS || i == blocks.first.size() - 1) {
            for (auto& t : threads) t.join();
            threads.clear();
        }
    }
    const double parse_ms = ms_since(t0);

    t0 = Clock::now();
    AllTilt volume = combine_all_tilts_from_thread_results(results);
    const double combine_ms = ms_since(t0);

    size_t total_gates = 0;
    for (const auto& tilt : volume.Tilts) total_gates += tilt.Radials_VEL.size() / 3;

    // Run dealiasing 3x to check consistency; first run is representative.
    for (int run = 0; run < 3; ++run) {
        AllTilt copy = volume;
        t0 = Clock::now();
        dealias_velocity_volume(copy);
        std::cout << "dealias run " << run << ": " << ms_since(t0) << " ms\n";
    }

    std::cout << "\nparse (8 threads): " << parse_ms << " ms\n"
              << "combine:           " << combine_ms << " ms\n"
              << "tilts:             " << volume.Tilts.size() << '\n'
              << "vel gates:         " << total_gates << '\n';
    return 0;
}
