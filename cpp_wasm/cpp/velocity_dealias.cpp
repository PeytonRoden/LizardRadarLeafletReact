#include <string>
#include "velocity_dealias.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <queue>
#include <vector>

namespace {

constexpr float VELOCITY_NAN = std::numeric_limits<float>::quiet_NaN();

// Choose the alias of `velocity` (velocity + 2*k*nyquist) that is closest to
// an already-dealiased reference. Jumps of more than MAX_FOLDS intervals are
// rejected as physically implausible.
inline float dealias_to_reference(float velocity, float reference, float nyquist)
{
    constexpr float MAX_FOLDS = 3.0f;
    const float interval = 2.0f * nyquist;
    const float k = std::round((reference - velocity) / interval);
    return std::fabs(k) <= MAX_FOLDS ? velocity + k * interval : VELOCITY_NAN;
}

struct Seed {
    float quality;
    int index; // ray * numGates + gate
    int version;
    bool operator<(const Seed& other) const {
        return quality != other.quality ? quality < other.quality : index > other.index;
    }
};

struct Boundary {
    int gate;
    int neighbor;
};

struct Region {
    std::vector<int> gates;
    std::vector<Boundary> boundary;
    double sum = 0.0;
    float nyquist = 0.0f;
    int fold = 0;
    int version = 0;
    bool claimed = false;
};

void dealias_tilt(SingleTilt& tilt)
{
    std::vector<float>& packed = tilt.Radials_VEL;
    if (packed.size() < 24 || packed.size() % 3 != 0) return;
    const size_t count = packed.size() / 3;
    if (count > static_cast<size_t>(std::numeric_limits<int>::max() / 3)) return;

    const float fallback_nyq =
        static_cast<float>(tilt.vol_el_rad.rad.nyquist_vel) / 100.0f;

    // ------------------------------------------------------------------
    // Group the packed (azimuth, dist, value) triplets into rays.
    // Triplets are appended radial-by-radial in parse order, so an
    // azimuth change marks a ray boundary.
    // ------------------------------------------------------------------
    constexpr float AZ_EPS = 0.01f;
    const int num_triplets = static_cast<int>(count);
    std::vector<VelocityRay> rays = tilt.VelocityRays;
    if (rays.empty()) {
        for (int t = 0; t < num_triplets; ++t) {
            if (t == 0 || std::fabs(packed[3 * t] - packed[3 * (t - 1)]) > AZ_EPS ||
                packed[3 * t + 1] <= packed[3 * (t - 1) + 1]) {
                rays.push_back({static_cast<size_t>(t), 0, fallback_nyq, tilt.gateSpacing});
            }
            ++rays.back().count;
        }
        if (!tilt.VelNyquist.empty() && tilt.VelNyquist.size() != rays.size()) return;
        for (size_t r = 0; r < rays.size(); ++r) {
            if (!tilt.VelNyquist.empty()) rays[r].nyquist = tilt.VelNyquist[r];
        }
    }
    if (rays.size() < 4) return;

    // Per-ray Nyquist stays aligned with ray groups as long as one entry
    // was pushed per VEL radial with >= 1 valid gate; otherwise use the
    // tilt-level value everywhere.
    size_t expected_start = 0;
    std::vector<float> nyq(count, VELOCITY_NAN);
    for (const auto& ray : rays) {
        if (ray.start != expected_start || ray.count == 0 || ray.count > count - expected_start) return;
        expected_start += ray.count;
        if (!std::isfinite(ray.gateSpacing) || ray.gateSpacing <= 0.0f) return;
        if (!std::isfinite(packed[3 * ray.start]) || packed[3 * ray.start] < 0.0f || packed[3 * ray.start] >= 360.0f) return;
        for (size_t t = ray.start; t < expected_start; ++t) {
            if (!std::isfinite(packed[3 * t + 1]) || (t > ray.start && packed[3 * t + 1] <= packed[3 * (t - 1) + 1])) return;
            if (std::isfinite(ray.nyquist) && ray.nyquist > 0.0f) nyq[t] = ray.nyquist;
        }
    }
    if (expected_start != count) return;

    // Full-circle scans wrap around in azimuth; sector scans do not.
    auto az_gap = [&](size_t a, size_t b) {
        return std::fmod(packed[3 * rays[b].start] - packed[3 * rays[a].start] + 360.0f, 360.0f);
    };
    std::vector<float> az_steps;
    for (size_t r = 1; r < rays.size(); ++r) {
        const float gap = az_gap(r - 1, r);
        if (gap > AZ_EPS && gap <= 1.5f) az_steps.push_back(gap);
    }
    float max_gap = 0.0f;
    if (!az_steps.empty()) {
        std::sort(az_steps.begin(), az_steps.end());
        max_gap = std::min(1.5f, 1.5f * az_steps[az_steps.size() / 2]);
    }

    // ------------------------------------------------------------------
    // Densify into a ray x gate grid (NaN = missing gate).
    // ------------------------------------------------------------------
    std::vector<float> grid(count);
    std::vector<std::array<int, 4>> neighbors(count, {-1, -1, -1, -1});
    for (size_t t = 0; t < count; ++t) grid[t] = packed[3 * t + 2];
    auto connect = [&](int a, int b, int direction) {
        if (!std::isfinite(grid[a]) || !std::isfinite(grid[b]) || !std::isfinite(nyq[a]) || !std::isfinite(nyq[b])) return;
        neighbors[a][direction] = b;
        neighbors[b][direction + 1] = a;
    };
    for (size_t r = 0; r < rays.size(); ++r) {
        const auto& ray = rays[r];
        const size_t end = ray.start + ray.count;
        for (size_t t = ray.start + 1; t < end; ++t) {
            if (std::fabs(packed[3 * t + 1] - packed[3 * (t - 1) + 1] - ray.gateSpacing) <= 0.1f) {
                connect(static_cast<int>(t - 1), static_cast<int>(t), 0);
            }
        }
        const size_t next = (r + 1) % rays.size();
        const float gap = az_gap(r, next);
        if (gap <= AZ_EPS || gap > max_gap) continue;
        const auto& other = rays[next];
        size_t a = ray.start, b = other.start;
        while (a < end && b < other.start + other.count) {
            const float diff = packed[3 * a + 1] - packed[3 * b + 1];
            if (std::fabs(diff) <= 0.1f) {
                connect(static_cast<int>(a++), static_cast<int>(b++), 2);
            } else if (diff < 0.0f) {
                ++a;
            } else {
                ++b;
            }
        }
    }

    // ------------------------------------------------------------------
    // Quality-weighted region growing. Static quality = number of valid
    // 4-neighbors (range +/-1 gate, azimuth +/-1 ray), so growth expands
    // from the most embedded gates and delays crossing data holes.
    // Isolated islands are grown from their own observed values.
    // ------------------------------------------------------------------
    std::vector<int> labels(count, -1);
    std::vector<Region> regions;
    for (int t = 0; t < num_triplets; ++t) {
        if (labels[t] >= 0 || !std::isfinite(grid[t]) || !std::isfinite(nyq[t])) continue;
        const int label = static_cast<int>(regions.size());
        regions.emplace_back();
        auto& region = regions.back();
        region.nyquist = nyq[t];
        region.gates.push_back(t);
        labels[t] = label;
        for (size_t cursor = 0; cursor < region.gates.size(); ++cursor) {
            const int gate = region.gates[cursor];
            region.sum += grid[gate];
            for (const int neighbor : neighbors[gate]) {
                if (neighbor < 0 || labels[neighbor] >= 0 || nyq[neighbor] != region.nyquist ||
                    std::fabs(grid[gate] - grid[neighbor]) > 0.25f * region.nyquist) continue;
                labels[neighbor] = label;
                region.gates.push_back(neighbor);
            }
        }
    }
    if (regions.empty()) return;
    for (int t = 0; t < num_triplets; ++t) {
        if (labels[t] < 0) continue;
        for (const int neighbor : neighbors[t]) {
            if (neighbor >= 0 && labels[neighbor] != labels[t]) regions[labels[t]].boundary.push_back({t, neighbor});
        }
    }
    std::vector<int> by_quality(regions.size());            // valid indices, best quality first
    std::iota(by_quality.begin(), by_quality.end(), 0);
    std::sort(by_quality.begin(), by_quality.end(), [&](int a, int b) {
        if (regions[a].nyquist != regions[b].nyquist) return regions[a].nyquist > regions[b].nyquist;
        if (regions[a].gates.size() != regions[b].gates.size()) return regions[a].gates.size() > regions[b].gates.size();
        const double ma = std::fabs(regions[a].sum), mb = std::fabs(regions[b].sum);
        return ma != mb ? ma < mb : a < b;
    });
    std::priority_queue<Seed> pq;           // growth frontier only

    // claimed[i] == 1 once gate i has received its final dealiased value.
    // A gate is assigned exactly once (by the first frontier neighbor that
    // reaches it) and queued exactly once; pops then expand unconditionally.
    // Without this, a gate reachable from two sides of a fold boundary gets
    // re-folded by whichever neighbor pops later.
    std::vector<char> claimed(count, 0);
    std::vector<int> parent(regions.size());
    std::iota(parent.begin(), parent.end(), 0);
    auto root = [&](int index) {
        while (parent[index] != index) {
            parent[index] = parent[parent[index]];
            index = parent[index];
        }
        return index;
    };
    auto boundary_reference = [&](int gate, int neighbor) {
        for (int direction = 0; direction < 4; ++direction) {
            if (neighbors[gate][direction] != neighbor) continue;
            const int behind = neighbors[neighbor][direction];
            if (behind < 0 || !claimed[behind] || root(labels[behind]) != root(labels[neighbor])) break;
            const float gradient = grid[neighbor] - grid[behind];
            const float limit = std::min(nyq[neighbor], nyq[behind]);
            if (std::fabs(gradient) > 0.5f * limit) break;
            const int third = neighbors[behind][direction];
            if (third < 0 || !claimed[third] || root(labels[third]) != root(labels[neighbor]) ||
                std::fabs(gradient - (grid[behind] - grid[third])) > 0.2f * limit) break;
            return grid[neighbor] + gradient;
        }
        return grid[neighbor];
    };
    auto propose = [&](int index) {
        auto& region = regions[index];
        ++region.version;
        std::array<int, 7> votes{};
        std::array<std::vector<int>, 7> source_gates, target_gates;
        std::vector<float> references;
        for (const auto& edge : region.boundary) {
            if (claimed[edge.neighbor]) references.push_back(boundary_reference(edge.gate, edge.neighbor));
        }
        if (references.empty()) return;
        std::sort(references.begin(), references.end());
        const float texture = references[3 * (references.size() - 1) / 4] - references[(references.size() - 1) / 4];
        int total = 0;
        for (const auto& edge : region.boundary) {
            if (!claimed[edge.neighbor]) continue;
            const float reference = boundary_reference(edge.gate, edge.neighbor);
            const float corrected = dealias_to_reference(grid[edge.gate], reference, region.nyquist);
            const float edge_nyquist = std::min(region.nyquist, nyq[edge.neighbor]);
            const float tolerance = std::min(edge_nyquist, 0.3f * edge_nyquist + texture);
            if (!std::isfinite(corrected) || std::fabs(corrected - reference) > tolerance) continue;
            ++total;
            const int fold = static_cast<int>(std::lround((corrected - grid[edge.gate]) / (2.0f * region.nyquist)));
            const int bin = fold + 3;
            ++votes[bin];
            source_gates[bin].push_back(edge.neighbor);
            target_gates[bin].push_back(edge.gate);
        }
        const int best = static_cast<int>(std::max_element(votes.begin(), votes.end()) - votes.begin());
        if (votes[best] < 3 || votes[best] < 0.8f * total) return;
        auto distinct = [](std::vector<int>& gates) {
            std::sort(gates.begin(), gates.end());
            return std::unique(gates.begin(), gates.end()) - gates.begin();
        };
        if (distinct(source_gates[best]) < 3 ||
            distinct(target_gates[best]) < static_cast<std::ptrdiff_t>(std::min(size_t{3}, region.gates.size()))) return;
        region.fold = best - 3;
        pq.push({static_cast<float>(votes[best]), index, region.version});
    };

    // When the frontier drains, the best remaining gate seeds a new
    // (internally consistent) island from its observed value.
    size_t cursor = 0;
    auto push_next_island_seed = [&]() {
        while (cursor < by_quality.size() && regions[by_quality[cursor]].claimed) ++cursor;
        if (cursor < by_quality.size()) {
            const int index = by_quality[cursor];
            regions[index].fold = 0;
            pq.push({0.0f, index, ++regions[index].version});
        }
    };
    push_next_island_seed();
    while (!pq.empty()) {
        const Seed seed = pq.top();
        pq.pop();
        auto& region = regions[seed.index];
        if (!region.claimed && seed.version == region.version) {
            region.claimed = true;
            for (const int gate : region.gates) {
                grid[gate] += region.fold * 2.0f * region.nyquist;
                claimed[gate] = 1;
            }
            std::vector<int> targets;
            for (const auto& edge : region.boundary) {
                const int label = labels[edge.neighbor];
                if (!regions[label].claimed) {
                    targets.push_back(label);
                } else if (seed.quality > 0.0f && std::fabs(grid[edge.gate] - grid[edge.neighbor]) <=
                           0.3f * std::min(region.nyquist, nyq[edge.neighbor])) {
                    parent[root(label)] = root(seed.index);
                }
            }
            std::sort(targets.begin(), targets.end());
            targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
            for (const int target : targets) propose(target);
        }
        if (pq.empty()) push_next_island_seed();
    }

    std::vector<std::array<size_t, 7>> fold_counts(regions.size());
    std::vector<bool> uniform_nyquist(regions.size(), true);
    for (size_t i = 0; i < regions.size(); ++i) {
        const int component = root(static_cast<int>(i));
        fold_counts[component][regions[i].fold + 3] += regions[i].gates.size();
        if (regions[i].nyquist != regions[component].nyquist) uniform_nyquist[component] = false;
    }
    std::vector<int> center_fold(regions.size(), 0);
    for (size_t i = 0; i < regions.size(); ++i) {
        if (!uniform_nyquist[i]) continue;
        const size_t total = std::accumulate(fold_counts[i].begin(), fold_counts[i].end(), size_t{0});
        size_t cumulative = 0;
        for (int bin = 0; bin < 7 && total > 0; ++bin) {
            cumulative += fold_counts[i][bin];
            if (cumulative > total / 2) {
                center_fold[i] = bin - 3;
                break;
            }
        }
        for (int bin = 0; bin < 7; ++bin) {
            if (fold_counts[i][bin] && std::abs(bin - 3 - center_fold[i]) > 3) {
                center_fold[i] = 0;
                break;
            }
        }
    }
    for (size_t i = 0; i < regions.size(); ++i) {
        const float correction = center_fold[root(static_cast<int>(i))] * 2.0f * regions[i].nyquist;
        for (const int gate : regions[i].gates) grid[gate] -= correction;
    }

    // ------------------------------------------------------------------
    // Write corrected values back into the packed triplets.
    // ------------------------------------------------------------------
    for (size_t t = 0; t < count; ++t) packed[3 * t + 2] = grid[t];
}

} // namespace

void dealias_velocity_volume(AllTilt& volume)
{
    for (auto& tilt : volume.Tilts) {
        dealias_tilt(tilt);
    }
}
