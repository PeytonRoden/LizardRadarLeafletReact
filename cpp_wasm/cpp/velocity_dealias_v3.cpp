#include <string>
#include "velocity_dealias.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <queue>
#include <vector>

// ---------------------------------------------------------------------------
// Region-based velocity dealiasing, resolved by best-first (quality-guided)
// propagation across region boundaries.
//
// Doppler dealiasing is mathematically the same problem as 2*pi phase
// unwrapping, and the resolution strategy below borrows from that
// literature's "quality-guided path following" family (Goldstein-style
// branch cuts, Flynn minimum-discontinuity unwrapping): fold every region as
// soon as *any* already-resolved neighbor gives evidence for it, and use the
// amount of evidence only to decide processing *order*, never eligibility.
//
// The previous implementation required >=3 votes and >=3 distinct
// already-resolved boundary gates before a region could be folded at all.
// A tornadic couplet core is frequently only 1-4 gates across and 1-3 rays
// wide, so it routinely could not reach that quorum -- it fell through to
// the "unresolved island, fold = 0" fallback and was left aliased even
// though the field immediately around it dealiased correctly. This version
// removes the quorum: a region with one confident neighbor gets folded from
// that one neighbor, just later (lower priority) than a region with many.
// ---------------------------------------------------------------------------

namespace {

constexpr float VELOCITY_NAN = std::numeric_limits<float>::quiet_NaN();
constexpr int   MAX_FOLDS = 4;       // largest plausible jump, in Nyquist intervals (tune per radar)
constexpr float AZ_EPS = 0.01f;
constexpr float SEG_FRAC = 0.18f;    // intra-region continuity threshold, fraction of Nyquist
constexpr int   REPAIR_PASSES = 3;   // bounded local-consistency relaxation

inline int fold_estimate(float raw, float reference, float nyquist)
{
    return static_cast<int>(std::lround((reference - raw) / (2.0f * nyquist)));
}

struct Boundary { int gate; int neighbor; };

struct Region {
    std::vector<int> gates;
    std::vector<Boundary> boundary;   // edges from a gate in this region to a gate in another
    float nyquist = 0.0f;
    int fold = 0;
    int version = 0;
    bool resolved = false;
};

struct Candidate {
    int evidence;   // resolved-boundary-edge count behind this fold estimate: priority only
    int region;
    int fold;
    int version;
    bool operator<(const Candidate& other) const { return evidence < other.evidence; }
};

struct FoldEvidence {
    bool has_evidence;
    int fold;
    int evidence;
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
    // Group the packed (azimuth, dist, value) triplets into rays. This
    // plumbing is unchanged: it was not implicated in the tornado failure
    // and is correct as originally written.
    // ------------------------------------------------------------------
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
    // Densify into a ray x gate grid (NaN = missing gate) and connect
    // range/azimuth neighbors. Unchanged from the original.
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
    // Phase 1 -- Segmentation.
    //
    // Flood fill into regions of mutually consistent *raw* (still aliased)
    // value. The threshold (SEG_FRAC = 0.18, vs. the previous 0.25) is
    // deliberately tighter than "about half a Nyquist interval": a genuine
    // shear discontinuity -- the entire point of a TVS -- should fall
    // *between* two regions, not get smoothed into one. Small regions are
    // fine now; resolution below carries no minimum region size or edge
    // count, unlike before.
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
            for (const int neighbor : neighbors[gate]) {
                if (neighbor < 0 || labels[neighbor] >= 0 || nyq[neighbor] != region.nyquist) continue;
                if (std::fabs(grid[gate] - grid[neighbor]) > SEG_FRAC * region.nyquist) continue;
                labels[neighbor] = label;
                region.gates.push_back(neighbor);
            }
        }
    }
    if (regions.empty()) return;

    for (int t = 0; t < num_triplets; ++t) {
        if (labels[t] < 0) continue;
        for (const int neighbor : neighbors[t]) {
            if (neighbor >= 0 && labels[neighbor] >= 0 && labels[neighbor] != labels[t]) {
                regions[labels[t]].boundary.push_back({t, neighbor});
            }
        }
    }

    // ------------------------------------------------------------------
    // Phase 2 -- Best-first fold resolution.
    //
    // A region becomes an eligible candidate the moment it has >=1 resolved
    // neighbor. "evidence" (resolved boundary-edge count) only sets queue
    // priority, so well-supported regions are folded first and uncertainty
    // doesn't propagate ahead of confidence -- but nothing is ever refused
    // for lack of a quorum. This is exactly what lets a 2-gate TVS core get
    // folded correctly off a single trustworthy edge to the surrounding
    // flow, instead of being stranded as an "unresolved island".
    // ------------------------------------------------------------------
    std::priority_queue<Candidate> pq;
    size_t resolved_count = 0;

    auto best_fold = [&](int index) -> FoldEvidence {
        auto& region = regions[index];
        std::array<int, 2 * MAX_FOLDS + 1> votes{};
        int evidence = 0;
        for (const auto& edge : region.boundary) {
            const int nb_region = labels[edge.neighbor];
            if (!regions[nb_region].resolved) continue;
            const int k = fold_estimate(grid[edge.gate], grid[edge.neighbor], region.nyquist);
            if (std::abs(k) > MAX_FOLDS) continue;
            ++votes[k + MAX_FOLDS];
            ++evidence;
        }
        if (evidence == 0) return {false, 0, 0};
        const int best_bin = static_cast<int>(std::max_element(votes.begin(), votes.end()) - votes.begin());
        return {true, best_bin - MAX_FOLDS, evidence};
    };

    auto push_candidate = [&](int index) {
        auto& region = regions[index];
        if (region.resolved) return;
        const FoldEvidence fe = best_fold(index);
        if (!fe.has_evidence) return;
        ++region.version;
        pq.push({fe.evidence, index, fe.fold, region.version});
    };

    auto resolve = [&](int index, int fold) {
        auto& region = regions[index];
        region.fold = fold;
        region.resolved = true;
        ++resolved_count;
        for (const int gate : region.gates) grid[gate] += fold * 2.0f * region.nyquist;
        std::vector<int> touched;
        for (const auto& edge : region.boundary) {
            const int nb_region = labels[edge.neighbor];
            if (!regions[nb_region].resolved) touched.push_back(nb_region);
        }
        std::sort(touched.begin(), touched.end());
        touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
        for (const int t : touched) push_candidate(t);
    };

    // Seed order for disconnected pieces of the scan (data voids with no
    // boundary at all between them): take the largest region first, tie
    // broken toward the higher Nyquist, since that fragment is the least
    // likely to be uniformly folded relative to the rest of the volume.
    std::vector<int> seed_order(regions.size());
    std::iota(seed_order.begin(), seed_order.end(), 0);
    std::sort(seed_order.begin(), seed_order.end(), [&](int a, int b) {
        if (regions[a].gates.size() != regions[b].gates.size()) return regions[a].gates.size() > regions[b].gates.size();
        return regions[a].nyquist > regions[b].nyquist;
    });
    size_t seed_cursor = 0;
    auto seed_next_island = [&]() {
        while (seed_cursor < seed_order.size() && regions[seed_order[seed_cursor]].resolved) ++seed_cursor;
        if (seed_cursor < seed_order.size()) resolve(seed_order[seed_cursor], 0);
    };

    while (resolved_count < regions.size()) {
        if (pq.empty()) {
            seed_next_island();
            continue;
        }
        const Candidate top = pq.top();
        pq.pop();
        auto& region = regions[top.region];
        if (!region.resolved && top.version == region.version) resolve(top.region, top.fold);
    }

    // ------------------------------------------------------------------
    // Phase 3 -- Bounded local repair.
    //
    // Best-first propagation commits each region's fold using whichever
    // neighbors happened to resolve first; in a narrow, high-gradient
    // feature an early, low-evidence commitment can turn out to be locally
    // wrong even though it was the best information available at the time.
    // This relaxes every region a few times against *all* of its now-
    // resolved neighbors -- an ICM-style pass, the same style of local
    // energy minimization used for Markov-random-field phase unwrapping --
    // shifting a region's fold by +/-1 whenever that strictly reduces total
    // boundary mismatch. Bounded to a handful of passes and self-limiting:
    // a shift is only taken when it improves on the status quo, so this
    // cannot oscillate or run away.
    // ------------------------------------------------------------------
    for (int pass = 0; pass < REPAIR_PASSES; ++pass) {
        bool changed = false;
        for (size_t i = 0; i < regions.size(); ++i) {
            auto& region = regions[i];
            if (region.boundary.empty()) continue;
            double mismatch[3] = {0.0, 0.0, 0.0}; // shift -1, 0, +1
            int considered = 0;
            for (const auto& edge : region.boundary) {
                const int nb_region = labels[edge.neighbor];
                if (!regions[nb_region].resolved) continue;
                ++considered;
                for (int shift = -1; shift <= 1; ++shift) {
                    const float candidate_value = grid[edge.gate] + shift * 2.0f * region.nyquist;
                    mismatch[shift + 1] += std::fabs(candidate_value - grid[edge.neighbor]);
                }
            }
            if (considered == 0) continue;
            const int best_shift = static_cast<int>(std::min_element(mismatch, mismatch + 3) - mismatch) - 1;
            if (best_shift != 0 && mismatch[best_shift + 1] < 0.9 * mismatch[1]) {
                for (const int gate : region.gates) grid[gate] += best_shift * 2.0f * region.nyquist;
                region.fold += best_shift;
                changed = true;
            }
        }
        if (!changed) break;
    }

    // ------------------------------------------------------------------
    // Write corrected values back into the packed triplets.
    // ------------------------------------------------------------------
    for (size_t t = 0; t < count; ++t) packed[3 * t + 2] = grid[t];
}

} // namespace

void dealias_velocity_volume_v3(AllTilt& volume)
{
    for (auto& tilt : volume.Tilts) {
        dealias_tilt(tilt);
    }
}