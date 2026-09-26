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

constexpr float VEL_NAN = std::numeric_limits<float>::quiet_NaN();
constexpr float AZ_EPS = 0.01f;
constexpr float SEG_FRAC = 0.20f;
constexpr float NYQ_REL_TOL = 0.03f;
constexpr int MAX_FOLDS = 6;

struct Boundary { int gate; int neighbor; };

struct Region {
    std::vector<int> gates;
    std::vector<Boundary> boundary;
    float nyquist = 0.0f;
    int fold = 0;
    int version = 0;
    bool resolved = false;
};

struct Candidate {
    int evidence;
    int region;
    int fold;
    int version;

    bool operator<(const Candidate& o) const {
        if (evidence != o.evidence) return evidence < o.evidence;
        return region > o.region;
    }
};

inline bool valid_nyq(float n) {
    return std::isfinite(n) && n > 0.0f;
}

inline bool compatible_nyq(float a, float b) {
    if (!valid_nyq(a) || !valid_nyq(b)) return false;
    return std::fabs(a - b) <= NYQ_REL_TOL * std::max(a, b);
}

inline int fold_from_pair(float raw, float reference, float nyq) {
    return static_cast<int>(
        std::lround((reference - raw) / (2.0f * nyq)));
}

void dealias_tilt(SingleTilt& tilt)
{
    std::vector<float>& packed = tilt.Radials_VEL;
    if (packed.size() < 24 || packed.size() % 3 != 0) return;

    const size_t count = packed.size() / 3;
    if (count > static_cast<size_t>(
            std::numeric_limits<int>::max() / 3))
        return;

    const float fallback_nyq =
        static_cast<float>(tilt.vol_el_rad.rad.nyquist_vel) / 100.0f;

    const int n = static_cast<int>(count);
    std::vector<VelocityRay> rays = tilt.VelocityRays;

    // ---------------------------------------------------------------
    // Ray construction / validation.
    // ---------------------------------------------------------------
    if (rays.empty()) {
        if (!valid_nyq(fallback_nyq)) return;

        rays.reserve(720);

        for (int t = 0; t < n; ++t) {
            if (t == 0 ||
                std::fabs(packed[3*t] - packed[3*(t-1)]) > AZ_EPS ||
                packed[3*t+1] <= packed[3*(t-1)+1]) {
                rays.push_back({
                    static_cast<size_t>(t), 0,
                    fallback_nyq, tilt.gateSpacing
                });
            }
            ++rays.back().count;
        }

        if (!tilt.VelNyquist.empty() &&
            tilt.VelNyquist.size() != rays.size())
            return;

        for (size_t r = 0; r < rays.size(); ++r)
            if (!tilt.VelNyquist.empty() &&
                valid_nyq(tilt.VelNyquist[r]))
                rays[r].nyquist = tilt.VelNyquist[r];
    }

    if (rays.size() < 4) return;

    // Always use the tilt Nyquist as fallback even when VelocityRays
    // already exists. This fixes a real failure mode in the old code.
    std::vector<float> nyq(count, VEL_NAN);
    size_t expected = 0;

    for (const auto& ray : rays) {
        if (ray.start != expected ||
            ray.count == 0 ||
            ray.count > count - expected)
            return;

        if (!std::isfinite(ray.gateSpacing) ||
            ray.gateSpacing <= 0.0f)
            return;

        const float ray_nyq =
            valid_nyq(ray.nyquist) ? ray.nyquist : fallback_nyq;

        if (!valid_nyq(ray_nyq)) return;

        const size_t end = ray.start + ray.count;
        const float az = packed[3 * ray.start];

        if (!std::isfinite(az) || az < 0.0f || az >= 360.0f)
            return;

        for (size_t t = ray.start; t < end; ++t) {
            if (!std::isfinite(packed[3*t+1])) return;
            if (t > ray.start &&
                packed[3*t+1] <= packed[3*(t-1)+1])
                return;
            nyq[t] = ray_nyq;
        }

        expected = end;
    }

    if (expected != count) return;

    // Estimate normal ray spacing without sorting.
    float az_sum = 0.0f;
    int az_samples = 0;

    for (size_t r = 1; r < rays.size(); ++r) {
        const float gap = std::fmod(
            packed[3*rays[r].start] -
            packed[3*rays[r-1].start] + 360.0f, 360.0f);

        if (gap > AZ_EPS && gap <= 1.5f) {
            az_sum += gap;
            ++az_samples;
        }
    }

    const float typical_az =
        az_samples ? az_sum / az_samples : 1.0f;

    const float max_az_gap =
        std::min(1.5f, 1.8f * typical_az);

    // ---------------------------------------------------------------
    // Grid + 4-neighbor graph.
    // ---------------------------------------------------------------
    std::vector<float> grid(count);
    for (size_t i = 0; i < count; ++i)
        grid[i] = packed[3*i+2];

    std::vector<std::array<int,4>> neighbors(
        count, {-1,-1,-1,-1});

    auto connect = [&](int a, int b, int dir) {
        if (a < 0 || b < 0) return;
        if (!std::isfinite(grid[a]) ||
            !std::isfinite(grid[b])) return;
        if (!valid_nyq(nyq[a]) ||
            !valid_nyq(nyq[b])) return;

        neighbors[a][dir] = b;
        neighbors[b][dir ^ 1] = a;
    };

    for (size_t r = 0; r < rays.size(); ++r) {
        const auto& ray = rays[r];
        const size_t end = ray.start + ray.count;

        // Range neighbors.
        for (size_t t = ray.start + 1; t < end; ++t) {
            const float dr =
                packed[3*t+1] - packed[3*(t-1)+1];

            if (std::fabs(dr - ray.gateSpacing) <= 0.1f)
                connect(static_cast<int>(t-1),
                        static_cast<int>(t), 0);
        }

        // Adjacent azimuth rays.
        const size_t next = (r + 1) % rays.size();

        const float gap = std::fmod(
            packed[3*rays[next].start] -
            packed[3*ray.start] + 360.0f, 360.0f);

        if (gap <= AZ_EPS || gap > max_az_gap) continue;

        size_t a = ray.start;
        size_t b = rays[next].start;
        const size_t ae = ray.start + ray.count;
        const size_t be = rays[next].start + rays[next].count;

        while (a < ae && b < be) {
            const float d =
                packed[3*a+1] - packed[3*b+1];

            if (std::fabs(d) <= 0.1f) {
                connect(static_cast<int>(a),
                        static_cast<int>(b), 2);
                ++a; ++b;
            } else if (d < 0.0f) {
                ++a;
            } else {
                ++b;
            }
        }
    }

    // ---------------------------------------------------------------
    // Phase 1: raw connected components.
    //
    // A strong shear surface becomes a region boundary. We deliberately
    // do not use expensive local regressions here.
    // ---------------------------------------------------------------
    std::vector<int> labels(count, -1);
    std::vector<Region> regions;
    regions.reserve(count / 8 + 1);

    for (int start = 0; start < n; ++start) {
        if (labels[start] >= 0 ||
            !std::isfinite(grid[start]) ||
            !valid_nyq(nyq[start]))
            continue;

        const int id = static_cast<int>(regions.size());
        regions.emplace_back();

        Region& region = regions.back();
        region.nyquist = nyq[start];
        region.gates.push_back(start);
        labels[start] = id;

        for (size_t p = 0; p < region.gates.size(); ++p) {
            const int g = region.gates[p];

            for (const int nb : neighbors[g]) {
                if (nb < 0 || labels[nb] >= 0) continue;
                if (!std::isfinite(grid[nb])) continue;
                if (!compatible_nyq(region.nyquist, nyq[nb]))
                    continue;

                const float edge_nyq =
                    std::min(region.nyquist, nyq[nb]);

                if (std::fabs(grid[g] - grid[nb]) >
                    SEG_FRAC * edge_nyq)
                    continue;

                labels[nb] = id;
                region.gates.push_back(nb);
            }
        }
    }

    if (regions.empty()) return;

    // Boundary list.
    for (int g = 0; g < n; ++g) {
        const int a = labels[g];
        if (a < 0) continue;

        for (const int nb : neighbors[g]) {
            if (nb < 0) continue;

            const int b = labels[nb];
            if (b >= 0 && b != a)
                regions[a].boundary.push_back({g, nb});
        }
    }

    // ---------------------------------------------------------------
    // Phase 2: best-first propagation.
    //
    // One edge is sufficient for a tiny tornado core. Multiple edges
    // increase priority, so well-supported regions resolve first.
    // ---------------------------------------------------------------
    std::priority_queue<Candidate> pq;
    size_t resolved = 0;

    auto propose = [&](int id) {
        Region& region = regions[id];
        if (region.resolved) return;

        std::array<int, 2*MAX_FOLDS+1> votes{};
        int evidence = 0;

        for (const Boundary& edge : region.boundary) {
            const int nb = labels[edge.neighbor];

            if (nb < 0 || !regions[nb].resolved)
                continue;

            const float edge_nyq =
                std::min(region.nyquist,
                         nyq[edge.neighbor]);

            const int k = fold_from_pair(
                grid[edge.gate],
                grid[edge.neighbor],
                edge_nyq);

            if (std::abs(k) > MAX_FOLDS)
                continue;

            ++votes[k + MAX_FOLDS];
            ++evidence;
        }

        if (evidence == 0) return;

        int best = 0;
        for (int i = 1; i < static_cast<int>(votes.size()); ++i)
            if (votes[i] > votes[best])
                best = i;

        ++region.version;
        pq.push({
            evidence,
            id,
            best - MAX_FOLDS,
            region.version
        });
    };

    auto resolve = [&](int id, int fold) {
        Region& region = regions[id];
        if (region.resolved) return;

        region.fold = fold;
        region.resolved = true;
        ++resolved;

        const float delta =
            fold * 2.0f * region.nyquist;

        for (const int g : region.gates)
            grid[g] += delta;

        for (const Boundary& edge : region.boundary) {
            const int nb = labels[edge.neighbor];
            if (nb >= 0 && !regions[nb].resolved)
                propose(nb);
        }
    };

    // Initial reference for a disconnected scan.
    int seed = -1;

    for (size_t i = 0; i < regions.size(); ++i) {
        if (seed < 0 ||
            regions[i].gates.size() > regions[seed].gates.size() ||
            (regions[i].gates.size() == regions[seed].gates.size() &&
             regions[i].nyquist > regions[seed].nyquist))
            seed = static_cast<int>(i);
    }

    resolve(seed, 0);

    // Connected propagation.
    while (!pq.empty()) {
        const Candidate c = pq.top();
        pq.pop();

        Region& region = regions[c.region];

        if (region.resolved ||
            region.version != c.version)
            continue;

        resolve(c.region, c.fold);
    }

    // ---------------------------------------------------------------
    // Disconnected islands.
    // Avoid sorting all regions. Find the largest remaining island,
    // which is O(number of regions) only when an island exists.
    // ---------------------------------------------------------------
    while (resolved < regions.size()) {
        seed = -1;

        for (size_t i = 0; i < regions.size(); ++i) {
            if (regions[i].resolved) continue;

            if (seed < 0 ||
                regions[i].gates.size() > regions[seed].gates.size())
                seed = static_cast<int>(i);
        }

        if (seed < 0) break;

        resolve(seed, 0);

        while (!pq.empty()) {
            const Candidate c = pq.top();
            pq.pop();

            Region& region = regions[c.region];

            if (region.resolved ||
                region.version != c.version)
                continue;

            resolve(c.region, c.fold);
        }
    }

    // ---------------------------------------------------------------
    // One bounded repair pass.
    //
    // Only +/- one fold is tested, and a correction requires >=2
    // supporting edges plus a large error reduction. This is cheap and
    // catches propagation mistakes without the expensive iterative
    // optimization used in the slower version.
    // ---------------------------------------------------------------
    for (Region& region : regions) {
        if (region.boundary.size() < 2) continue;

        const float interval =
            2.0f * region.nyquist;

        double e0 = 0.0;
        double em = 0.0;
        double ep = 0.0;

        int support = 0;
        int sm = 0;
        int sp = 0;

        for (const Boundary& edge : region.boundary) {
            const int nb = labels[edge.neighbor];
            if (nb < 0 || !regions[nb].resolved)
                continue;

            const float v = grid[edge.gate];
            const float ref = grid[edge.neighbor];

            const float a = std::fabs(v - ref);
            const float m = std::fabs(v - interval - ref);
            const float p = std::fabs(v + interval - ref);

            e0 += a;
            em += m;
            ep += p;
            ++support;

            if (m < 0.75f * a) ++sm;
            if (p < 0.75f * a) ++sp;
        }

        if (support < 2) continue;

        int shift = 0;

        if (sm >= 2 && em < 0.80 * e0 && em < ep)
            shift = -1;
        else if (sp >= 2 && ep < 0.80 * e0 && ep < em)
            shift = +1;

        if (shift != 0) {
            const float delta = shift * interval;

            for (const int g : region.gates)
                grid[g] += delta;

            region.fold += shift;
        }
    }

    for (size_t i = 0; i < count; ++i)
        packed[3*i+2] = grid[i];
}

} // namespace


void dealias_velocity_volume_v2(AllTilt& volume)
{
    for (auto& tilt : volume.Tilts)
        dealias_tilt(tilt);
}
