#include <string>
#include "velocity_dealias.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <queue>
#include <utility>
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
    int proposed_by = -1;
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

static void dealias_tilt_v2_core(SingleTilt& tilt)
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

    // v9 fix: honor per-ray Nyquist values even when VelocityRays was already
    // populated. The old v2 path only applied VelNyquist for fallback rays.
    if (!tilt.VelNyquist.empty() && tilt.VelNyquist.size() == rays.size()) {
        for (size_t r = 0; r < rays.size(); ++r) {
            if (valid_nyq(tilt.VelNyquist[r]))
                rays[r].nyquist = tilt.VelNyquist[r];
        }
    }

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

        const float delta =
            fold * 2.0f * region.nyquist;

        for (const int g : region.gates)
            grid[g] += delta;

        for (const Boundary& edge : region.boundary) {
            const int nb = labels[edge.neighbor];
            if (nb >= 0 && !regions[nb].resolved &&
                regions[nb].proposed_by != id) {
                regions[nb].proposed_by = id;
                propose(nb);
            }
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
    // Seed each remaining component from a size-ordered heap so an
    // island costs O(log R) instead of a full O(R) rescan per island.
    // ---------------------------------------------------------------
    std::priority_queue<std::pair<size_t, int>> islands;

    for (int i = 0; i < static_cast<int>(regions.size()); ++i)
        if (!regions[i].resolved)
            islands.emplace(regions[i].gates.size(), i);

    while (!islands.empty()) {
        seed = islands.top().second;
        islands.pop();

        if (regions[seed].resolved) continue;

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




// ===========================================================================
// v9: fast v2 baseline + guarded 3-D branch reconciliation + local rescue
// ===========================================================================
// Design is informed by the 4DD, R2D2, and UNRAVEL approaches:
//   * independently solve each tilt first;
//   * use adjacent elevations to resolve integer Nyquist-branch ambiguity;
//   * never use vertical data as a smoothing operation;
//   * keep high-shear/tornadic regions out of whole-region vertical shifts;
//   * use a bounded local rescue only inside the hard/shear mask.
//
// The deliberate difference from a chained 4DD-style implementation is that
// the vertical reference for a cut comes from independently solved neighbors.
// This prevents a single bad cut from becoming an absolute prior for the whole
// volume. A disconnected component can remain globally ambiguous, which is
// physically unavoidable without temporal/model/sounding information.
// ===========================================================================

namespace {

constexpr float V9_PI = 3.14159265358979323846f;
constexpr float V9_AZ_EPS = 0.01f;
constexpr float V9_NYQ_TOL = 0.03f;
constexpr float V9_SEG_FRAC = 0.20f;
constexpr int V9_MAX_FOLD = 6;

constexpr float V9_HARD_CIRC_FRAC = 0.50f;
constexpr float V9_TVS_ABS_FRAC = 0.45f;
constexpr float V9_TVS_JUMP_FRAC = 0.85f;
constexpr float V9_HARD_DILATE_FRAC = 0.08f;

constexpr int V9_VERTICAL_MIN_SUPPORT = 6;
constexpr float V9_VERTICAL_MIN_FRAC = 0.72f;
constexpr float V9_VERTICAL_SIDE_FRAC = 0.67f;
constexpr float V9_VERTICAL_SINGLE_SIDE_FRAC = 0.80f;
constexpr float V9_VERTICAL_MAX_RESIDUAL_RATIO = 0.45f;
constexpr int V9_VERTICAL_WINDOW = 2;
constexpr float V9_VERTICAL_AZ_TOL_DEG = 1.5f;
constexpr float V9_VERTICAL_RANGE_TOL_MULT = 2.0f;
constexpr size_t V9_VERTICAL_SAMPLE_STRIDE = 4;
constexpr size_t V9_VERTICAL_MAX_SAMPLES = 192;

constexpr size_t V9_RESCUE_MAX_ABS = 4096;
constexpr float V9_RESCUE_MAX_FRAC = 0.035f;
constexpr float V9_RESCUE_MIN_IMPROVEMENT = 0.30f;
constexpr float V9_RESCUE_SCALE_FRAC = 0.38f;

struct V9Ray {
    size_t start = 0;
    size_t count = 0;
    float azimuth = 0.0f;
    float nyquist = 0.0f;
    float spacing = 0.0f;
};

struct V9Region {
    std::vector<int> gates;
    size_t hard_count = 0;
};

struct V9Info {
    std::vector<float> baseline;
    std::vector<float> raw;
    std::vector<float> nyq;
    std::vector<V9Ray> rays;
    std::vector<std::array<int,4>> graph;
    std::vector<unsigned char> hard;
    std::vector<V9Region> regions;
};

inline bool v9_valid_nyq(float x) {
    return std::isfinite(x) && x > 0.0f;
}

inline float v9_wrap(float x) {
    x = std::fmod(x, 360.0f);
    return x < 0.0f ? x + 360.0f : x;
}

inline float v9_az_distance(float a, float b) {
    const float d = std::fabs(a - b);
    return std::min(d, 360.0f - d);
}

inline float v9_circular_distance(float a, float b, float nyq) {
    if (!std::isfinite(a) || !std::isfinite(b) || !v9_valid_nyq(nyq))
        return std::numeric_limits<float>::quiet_NaN();
    const float p = 2.0f * nyq;
    float d = std::fmod(std::fabs(a - b), p);
    if (d > nyq) d = p - d;
    return d;
}

inline float v9_principal(float v, float nyq) {
    if (!std::isfinite(v) || !v9_valid_nyq(nyq))
        return std::numeric_limits<float>::quiet_NaN();
    const float p = 2.0f * nyq;
    return v - p * std::floor((v + nyq) / p);
}

static std::vector<V9Ray> v9_build_rays(const SingleTilt& tilt) {
    const auto& packed = tilt.Radials_VEL;
    const size_t n = packed.size() / 3;
    std::vector<V9Ray> rays;
    if (n == 0) return rays;

    if (!tilt.VelocityRays.empty()) {
        rays.reserve(tilt.VelocityRays.size());
        for (const auto& src : tilt.VelocityRays) {
            if (src.start >= n || src.count == 0 || src.count > n - src.start)
                return {};
            V9Ray r;
            r.start = src.start;
            r.count = src.count;
            r.azimuth = v9_wrap(packed[3 * src.start]);
            r.nyquist = v9_valid_nyq(src.nyquist)
                ? src.nyquist : tilt.vol_el_rad.rad.nyquist_vel / 100.0f;
            r.spacing = v9_valid_nyq(src.gateSpacing)
                ? src.gateSpacing : tilt.gateSpacing;
            if (!v9_valid_nyq(r.spacing) && r.count >= 2)
                r.spacing = packed[3 * (r.start + 1) + 1] - packed[3 * r.start + 1];
            if (!v9_valid_nyq(r.nyquist) || !v9_valid_nyq(r.spacing)) return {};
            rays.push_back(r);
        }
    } else {
        const float fallback = tilt.vol_el_rad.rad.nyquist_vel / 100.0f;
        if (!v9_valid_nyq(fallback)) return {};
        size_t s = 0;
        while (s < n) {
            size_t e = s + 1;
            while (e < n) {
                const float a0 = packed[3 * (e - 1)];
                const float a1 = packed[3 * e];
                const float r0 = packed[3 * (e - 1) + 1];
                const float r1 = packed[3 * e + 1];
                if (!std::isfinite(a1) || !std::isfinite(r1) ||
                    std::fabs(a1 - a0) > V9_AZ_EPS || r1 <= r0)
                    break;
                ++e;
            }
            V9Ray r;
            r.start = s;
            r.count = e - s;
            r.azimuth = v9_wrap(packed[3 * s]);
            r.nyquist = fallback;
            r.spacing = tilt.gateSpacing;
            if (!v9_valid_nyq(r.spacing) && r.count >= 2)
                r.spacing = packed[3 * (s + 1) + 1] - packed[3 * s + 1];
            if (!v9_valid_nyq(r.spacing)) return {};
            rays.push_back(r);
            s = e;
        }
    }

    if (!tilt.VelNyquist.empty() && tilt.VelNyquist.size() == rays.size()) {
        for (size_t r = 0; r < rays.size(); ++r)
            if (v9_valid_nyq(tilt.VelNyquist[r])) rays[r].nyquist = tilt.VelNyquist[r];
    }

    size_t expected = 0;
    for (const auto& r : rays) {
        if (r.start != expected || r.count == 0 || !v9_valid_nyq(r.nyquist)) return {};
        for (size_t g = r.start; g < r.start + r.count; ++g) {
            if (!std::isfinite(packed[3 * g + 1])) return {};
            if (g > r.start && packed[3 * g + 1] <= packed[3 * (g - 1) + 1]) return {};
        }
        expected += r.count;
    }
    return expected == n ? rays : std::vector<V9Ray>{};
}

static void v9_build_graph(const std::vector<float>& packed,
                           const std::vector<V9Ray>& rays,
                           std::vector<std::array<int,4>>& graph) {
    const size_t n = packed.size() / 3;
    graph.assign(n, std::array<int,4>{{-1,-1,-1,-1}});
    auto connect = [&](int a, int b, int dir) {
        if (a < 0 || b < 0 || a == b) return;
        const int opp = dir == 0 ? 1 : dir == 1 ? 0 : dir == 2 ? 3 : 2;
        graph[a][dir] = b;
        graph[b][opp] = a;
    };

    for (const auto& r : rays) {
        for (size_t g = r.start + 1; g < r.start + r.count; ++g) {
            const float dr = packed[3 * g + 1] - packed[3 * (g - 1) + 1];
            if (std::isfinite(dr) && dr > 0.0f && dr <= 1.30f * r.spacing)
                connect(static_cast<int>(g - 1), static_cast<int>(g), 0);
        }
    }

    std::vector<float> gaps;
    gaps.reserve(rays.size());
    for (size_t r = 1; r < rays.size(); ++r) {
        const float gap = rays[r].azimuth - rays[r - 1].azimuth;
        if (gap > V9_AZ_EPS && gap < 10.0f) gaps.push_back(gap);
    }
    float med_gap = 1.0f;
    if (!gaps.empty()) {
        std::sort(gaps.begin(), gaps.end());
        med_gap = gaps[gaps.size() / 2];
    }
    const float max_gap = std::max(1.5f, 1.8f * med_gap);

    auto link = [&](size_t ra, size_t rb, float gap) {
        if (gap <= V9_AZ_EPS || gap > max_gap) return;
        const auto& a = rays[ra];
        const auto& b = rays[rb];
        const float tol = 0.75f * std::max(a.spacing, b.spacing);
        size_t ia = a.start, ib = b.start;
        while (ia < a.start + a.count && ib < b.start + b.count) {
            const float dr = packed[3 * ia + 1] - packed[3 * ib + 1];
            if (std::fabs(dr) <= tol) {
                connect(static_cast<int>(ia), static_cast<int>(ib), 2);
                ++ia; ++ib;
            } else if (dr < 0.0f) {
                ++ia;
            } else {
                ++ib;
            }
        }
    };
    for (size_t r = 1; r < rays.size(); ++r)
        link(r - 1, r, rays[r].azimuth - rays[r - 1].azimuth);
    if (rays.size() >= 8) {
        const float wrap = rays.front().azimuth + 360.0f - rays.back().azimuth;
        if (wrap > V9_AZ_EPS && wrap <= max_gap)
            link(rays.size() - 1, 0, wrap);
    }
}

static std::vector<unsigned char> v9_detect_hard(
    const std::vector<float>& raw,
    const std::vector<float>& nyq,
    const std::vector<std::array<int,4>>& graph) {
    const size_t n = raw.size();
    std::vector<unsigned char> hard(n, 0);
    for (size_t i = 0; i < n; ++i) {
        if (!std::isfinite(raw[i]) || !v9_valid_nyq(nyq[i])) continue;
        float max_cd = 0.0f;
        bool opposite = false;
        for (int j : graph[i]) {
            if (j < 0 || !std::isfinite(raw[j]) || !v9_valid_nyq(nyq[j])) continue;
            const float en = std::min(nyq[i], nyq[j]);
            const float cd = v9_circular_distance(raw[i], raw[j], en);
            if (std::isfinite(cd)) max_cd = std::max(max_cd, cd / en);
            const bool sign_change = (raw[i] >= 0.0f && raw[j] < 0.0f) ||
                                     (raw[i] < 0.0f && raw[j] >= 0.0f);
            if (sign_change &&
                std::fabs(raw[i]) >= V9_TVS_ABS_FRAC * en &&
                std::fabs(raw[j]) >= V9_TVS_ABS_FRAC * en &&
                std::fabs(raw[i] - raw[j]) >= V9_TVS_JUMP_FRAC * en)
                opposite = true;
        }
        hard[i] = static_cast<unsigned char>(max_cd >= V9_HARD_CIRC_FRAC || opposite);
    }
    size_t hc = 0;
    for (unsigned char x : hard) hc += x != 0;
    if (n > 0 && hc > 0 && hc <= static_cast<size_t>(V9_HARD_DILATE_FRAC * n)) {
        std::vector<unsigned char> d = hard;
        for (size_t i = 0; i < n; ++i) if (hard[i])
            for (int j : graph[i]) if (j >= 0) d[j] = 1;
        hard.swap(d);
    }
    return hard;
}

static void v9_build_regions(
    const std::vector<float>& raw,
    const std::vector<float>& nyq,
    const std::vector<std::array<int,4>>& graph,
    const std::vector<unsigned char>& hard,
    std::vector<V9Region>& regions) {
    const int n = static_cast<int>(raw.size());
    regions.clear();
    regions.reserve(static_cast<size_t>(n / 16 + 1));
    std::vector<int> label(n, -1);
    for (int s = 0; s < n; ++s) {
        if (label[s] >= 0 || !std::isfinite(raw[s]) || !v9_valid_nyq(nyq[s])) continue;
        const int id = static_cast<int>(regions.size());
        regions.emplace_back();
        std::vector<int> stack;
        stack.reserve(32);
        stack.push_back(s);
        label[s] = id;
        while (!stack.empty()) {
            const int g = stack.back();
            stack.pop_back();
            regions[id].gates.push_back(g);
            regions[id].hard_count += hard[g] ? 1u : 0u;
            for (int nb : graph[g]) {
                if (nb < 0 || label[nb] >= 0 || !std::isfinite(raw[nb]) || !v9_valid_nyq(nyq[nb])) continue;
                const float en = std::min(nyq[g], nyq[nb]);
                if (std::fabs(nyq[g] - nyq[nb]) > V9_NYQ_TOL * std::max(nyq[g], nyq[nb])) continue;
                if (std::fabs(raw[g] - raw[nb]) > V9_SEG_FRAC * en) continue;
                label[nb] = id;
                stack.push_back(nb);
            }
        }
    }
}

static int v9_nearest_ray(const std::vector<V9Ray>& rays, float az) {
    if (rays.empty()) return -1;
    az = v9_wrap(az);
    int lo = 0, hi = static_cast<int>(rays.size());
    while (lo < hi) {
        const int m = (lo + hi) / 2;
        if (rays[m].azimuth < az) lo = m + 1;
        else hi = m;
    }
    const int n = static_cast<int>(rays.size());
    const int a = lo < n ? lo : 0;
    const int b = lo > 0 ? lo - 1 : n - 1;
    return v9_az_distance(rays[a].azimuth, az) < v9_az_distance(rays[b].azimuth, az) ? a : b;
}

static bool v9_vertical_reference(
    const std::vector<float>& current,
    size_t gate,
    float current_cos,
    float current_spacing,
    const std::vector<float>& reference_packed,
    const std::vector<float>& reference_velocity,
    const std::vector<V9Ray>& reference_rays,
    float reference_cos,
    const std::vector<unsigned char>& reference_hard,
    float& median_value) {
    if (reference_rays.empty() || gate >= current.size() / 3 ||
        !std::isfinite(current_cos) || !std::isfinite(reference_cos) ||
        std::fabs(reference_cos) < 0.5f)
        return false;
    const float az = current[3 * gate];
    const int rr = v9_nearest_ray(reference_rays, az);
    if (rr < 0 || v9_az_distance(reference_rays[rr].azimuth, az) > V9_VERTICAL_AZ_TOL_DEG)
        return false;
    const V9Ray& ray = reference_rays[rr];
    const float current_range = current[3 * gate + 1];
    if (!std::isfinite(current_range)) return false;

    const float desired_range = current_range * current_cos / reference_cos;
    size_t lo = ray.start, hi = ray.start + ray.count;
    size_t left = lo, right = hi;
    while (left < right) {
        const size_t m = (left + right) / 2;
        if (reference_packed[3 * m + 1] < desired_range) left = m + 1;
        else right = m;
    }
    size_t best = left >= hi ? hi - 1 : left;
    if (best > lo) {
        const float d0 = std::fabs(reference_packed[3 * best + 1] - desired_range);
        const float d1 = std::fabs(reference_packed[3 * (best - 1) + 1] - desired_range);
        if (d1 < d0) --best;
    }

    const float tol = std::max(180.0f,
        V9_VERTICAL_RANGE_TOL_MULT * std::max(current_spacing, ray.spacing));
    std::array<float,5> vals{};
    int n = 0;
    for (int d = -V9_VERTICAL_WINDOW; d <= V9_VERTICAL_WINDOW; ++d) {
        const long q = static_cast<long>(best) + d;
        if (q < static_cast<long>(lo) || q >= static_cast<long>(hi)) continue;
        const size_t g = static_cast<size_t>(q);
        if (g >= reference_hard.size() || reference_hard[g] || !std::isfinite(reference_velocity[g])) continue;
        if (std::fabs(reference_packed[3 * g + 1] - desired_range) > tol) continue;
        vals[n++] = reference_velocity[g];
    }
    if (n == 0) return false;
    for (int i = 1; i < n; ++i) {
        const float key = vals[i];
        int j = i;
        while (j > 0 && vals[j - 1] > key) { vals[j] = vals[j - 1]; --j; }
        vals[j] = key;
    }
    median_value = (n & 1) ? vals[n / 2] : 0.5f * (vals[n / 2 - 1] + vals[n / 2]);
    return true;
}

static V9Info v9_prepare(const SingleTilt& tilt) {
    V9Info out;
    const size_t n = tilt.Radials_VEL.size() / 3;
    out.rays = v9_build_rays(tilt);
    if (out.rays.empty()) return out;
    out.nyq.assign(n, std::numeric_limits<float>::quiet_NaN());
    for (const auto& r : out.rays)
        for (size_t g = r.start; g < r.start + r.count; ++g)
            out.nyq[g] = r.nyquist;
    out.baseline.resize(n, std::numeric_limits<float>::quiet_NaN());
    out.raw.resize(n, std::numeric_limits<float>::quiet_NaN());
    for (size_t g = 0; g < n; ++g) {
        const float corrected = tilt.Radials_VEL[3 * g + 2];
        out.baseline[g] = corrected;
        out.raw[g] = v9_principal(corrected, out.nyq[g]);
    }
    v9_build_graph(tilt.Radials_VEL, out.rays, out.graph);
    out.hard = v9_detect_hard(out.raw, out.nyq, out.graph);
    v9_build_regions(out.raw, out.nyq, out.graph, out.hard, out.regions);
    return out;
}

static void v9_reconcile_vertical(
    AllTilt& volume,
    const std::vector<int>& order,
    const std::vector<V9Info>& info) {
    for (size_t oi = 0; oi < order.size(); ++oi) {
        const int idx = order[oi];
        SingleTilt& cur = volume.Tilts[idx];
        const V9Info& ci = info[idx];
        if (ci.rays.empty() || ci.regions.empty()) continue;
        const int below = oi > 0 ? order[oi - 1] : -1;
        const int above = oi + 1 < order.size() ? order[oi + 1] : -1;
        const float cur_cos = std::cos(cur.ElevationAngle * V9_PI / 180.0f);
        const float below_cos = below >= 0 ? std::cos(volume.Tilts[below].ElevationAngle * V9_PI / 180.0f) : 1.0f;
        const float above_cos = above >= 0 ? std::cos(volume.Tilts[above].ElevationAngle * V9_PI / 180.0f) : 1.0f;

        for (const auto& region : ci.regions) {
            if (region.hard_count > 0 || region.gates.size() < static_cast<size_t>(V9_VERTICAL_MIN_SUPPORT))
                continue;

            std::array<int,13> votes{};
            std::array<int,13> below_votes{};
            std::array<int,13> above_votes{};
            std::array<double,13> shift_cost{};
            int below_support = 0, above_support = 0;
            double old_cost = 0.0;
            size_t samples = 0;
            const size_t stride = std::max<size_t>(V9_VERTICAL_SAMPLE_STRIDE,
                region.gates.size() / V9_VERTICAL_MAX_SAMPLES + 1);

            for (size_t p = 0; p < region.gates.size(); p += stride) {
                const size_t g = static_cast<size_t>(region.gates[p]);
                const float current = cur.Radials_VEL[3 * g + 2];
                if (!std::isfinite(current) || !v9_valid_nyq(ci.nyq[g])) continue;
                ++samples;
                const float P = 2.0f * ci.nyq[g];

                if (below >= 0) {
                    float ref = 0.0f;
                    if (v9_vertical_reference(cur.Radials_VEL, g, cur_cos, ci.rays[0].spacing,
                                              volume.Tilts[below].Radials_VEL, info[below].baseline, info[below].rays,
                                              below_cos, info[below].hard, ref)) {
                        const int k = static_cast<int>(std::lround((ref - current) / P));
                        if (std::abs(k) <= V9_MAX_FOLD) { ++votes[k + 6]; ++below_votes[k + 6]; ++below_support; }
                        old_cost += std::fabs(current - ref);
                        for (int kk = -V9_MAX_FOLD; kk <= V9_MAX_FOLD; ++kk)
                            shift_cost[kk + 6] += std::fabs(current + kk * P - ref);
                    }
                }
                if (above >= 0) {
                    float ref = 0.0f;
                    if (v9_vertical_reference(cur.Radials_VEL, g, cur_cos, ci.rays[0].spacing,
                                              volume.Tilts[above].Radials_VEL, info[above].baseline, info[above].rays,
                                              above_cos, info[above].hard, ref)) {
                        const int k = static_cast<int>(std::lround((ref - current) / P));
                        if (std::abs(k) <= V9_MAX_FOLD) { ++votes[k + 6]; ++above_votes[k + 6]; ++above_support; }
                        old_cost += std::fabs(current - ref);
                        for (int kk = -V9_MAX_FOLD; kk <= V9_MAX_FOLD; ++kk)
                            shift_cost[kk + 6] += std::fabs(current + kk * P - ref);
                    }
                }
                if (samples >= V9_VERTICAL_MAX_SAMPLES) break;
            }

            const int total = below_support + above_support;
            if (total < V9_VERTICAL_MIN_SUPPORT || old_cost <= 0.0) continue;
            int best = 0, best_votes = 0;
            for (int k = -V9_MAX_FOLD; k <= V9_MAX_FOLD; ++k)
                if (votes[k + 6] > best_votes) { best_votes = votes[k + 6]; best = k; }
            if (best == 0 || static_cast<float>(best_votes) / total < V9_VERTICAL_MIN_FRAC) continue;

            if (below >= 0 && above >= 0 && below_support >= 3 && above_support >= 3) {
                int bbest = 0, bv = 0, abest = 0, av = 0;
                for (int k = -V9_MAX_FOLD; k <= V9_MAX_FOLD; ++k) {
                    if (below_votes[k + 6] > bv) { bv = below_votes[k + 6]; bbest = k; }
                    if (above_votes[k + 6] > av) { av = above_votes[k + 6]; abest = k; }
                }
                if (bbest != best || abest != best ||
                    static_cast<float>(bv) / below_support < V9_VERTICAL_SIDE_FRAC ||
                    static_cast<float>(av) / above_support < V9_VERTICAL_SIDE_FRAC)
                    continue;
            } else {
                const bool below_is_stronger = below_support >= above_support;
                const int side_support = below_is_stronger ? below_support : above_support;
                const auto& side_votes = below_is_stronger ? below_votes : above_votes;
                if (static_cast<float>(side_votes[best + 6]) / std::max(1, side_support) < V9_VERTICAL_SINGLE_SIDE_FRAC)
                    continue;
            }

            if (shift_cost[best + 6] / old_cost > V9_VERTICAL_MAX_RESIDUAL_RATIO) continue;
            const float shift = static_cast<float>(best) * 2.0f;
            for (int g : region.gates)
                cur.Radials_VEL[3 * g + 2] += shift * ci.nyq[g];
        }
    }
}

static float v9_directional_prediction(
    size_t i,
    const std::vector<float>& value,
    const std::vector<unsigned char>& solved,
    const std::vector<std::array<int,4>>& graph) {
    std::array<float,4> p{};
    int n = 0;
    for (int d = 0; d < 4; ++d) {
        const int j = graph[i][d];
        if (j < 0 || !solved[j] || !std::isfinite(value[3 * j + 2])) continue;
        float pred = value[3 * j + 2];
        const int od = d == 0 ? 1 : d == 1 ? 0 : d == 2 ? 3 : 2;
        const int k = graph[j][od];
        if (k >= 0 && solved[k] && std::isfinite(value[3 * k + 2]))
            pred += 0.5f * (value[3 * j + 2] - value[3 * k + 2]);
        p[n++] = pred;
    }
    if (n == 0) return std::numeric_limits<float>::quiet_NaN();
    for (int i2 = 1; i2 < n; ++i2) {
        const float key = p[i2];
        int j = i2;
        while (j > 0 && p[j - 1] > key) { p[j] = p[j - 1]; --j; }
        p[j] = key;
    }
    return (n & 1) ? p[n / 2] : 0.5f * (p[n / 2 - 1] + p[n / 2]);
}

static void v9_local_hard_rescue(
    AllTilt& volume,
    const std::vector<int>& order,
    const std::vector<V9Info>& info) {
    for (size_t oi = 0; oi < order.size(); ++oi) {
        const int idx = order[oi];
        SingleTilt& tilt = volume.Tilts[idx];
        const V9Info& in = info[idx];
        if (in.raw.empty()) continue;
        size_t hard_count = 0;
        for (unsigned char x : in.hard) hard_count += x != 0;
        if (hard_count == 0) continue;
        const size_t budget = std::min(V9_RESCUE_MAX_ABS,
            std::max<size_t>(1, static_cast<size_t>(V9_RESCUE_MAX_FRAC * in.raw.size())));
        if (hard_count > 2 * budget) continue;

        std::vector<unsigned char> solved(in.hard.size(), 0);
        for (size_t g = 0; g < solved.size(); ++g)
            solved[g] = !in.hard[g] && std::isfinite(tilt.Radials_VEL[3 * g + 2]);

        const int below = oi > 0 ? order[oi - 1] : -1;
        const int above = oi + 1 < order.size() ? order[oi + 1] : -1;
        const float cur_cos = std::cos(tilt.ElevationAngle * V9_PI / 180.0f);
        const float below_cos = below >= 0 ? std::cos(volume.Tilts[below].ElevationAngle * V9_PI / 180.0f) : 1.0f;
        const float above_cos = above >= 0 ? std::cos(volume.Tilts[above].ElevationAngle * V9_PI / 180.0f) : 1.0f;
        const float cur_spacing = in.rays.empty() ? tilt.gateSpacing : in.rays[0].spacing;

        size_t rescued = 0;
        for (int pass = 0; pass < 3 && rescued < budget; ++pass) {
            for (size_t g = 0; g < in.raw.size() && rescued < budget; ++g) {
                if (!in.hard[g] || solved[g] || !std::isfinite(in.raw[g]) || !v9_valid_nyq(in.nyq[g])) continue;
                const float prediction = v9_directional_prediction(g, tilt.Radials_VEL, solved, in.graph);
                if (!std::isfinite(prediction)) continue;

                int nref = 0;
                unsigned base_dirs = 0u;
                for (int d = 0; d < 4; ++d) {
                    const int j = in.graph[g][d];
                    if (j >= 0 && solved[j] && std::isfinite(tilt.Radials_VEL[3 * j + 2])) {
                        ++nref;
                        base_dirs |= 1u << static_cast<unsigned>(d);
                    }
                }
                if (nref < 1) continue;
                const int base_axes = ((base_dirs & 3u) ? 1 : 0) + ((base_dirs & 12u) ? 1 : 0);
                if (pass == 0 && (nref < 2 || base_axes < 2)) continue;

                float vertical_target = std::numeric_limits<float>::quiet_NaN();
                if (below >= 0) {
                    float v = 0.0f;
                    if (v9_vertical_reference(tilt.Radials_VEL, g, cur_cos, cur_spacing,
                                               volume.Tilts[below].Radials_VEL, info[below].baseline, info[below].rays,
                                               below_cos, info[below].hard, v)) vertical_target = v;
                }
                if (above >= 0) {
                    float v = 0.0f;
                    if (v9_vertical_reference(tilt.Radials_VEL, g, cur_cos, cur_spacing,
                                               volume.Tilts[above].Radials_VEL, info[above].baseline, info[above].rays,
                                               above_cos, info[above].hard, v))
                        vertical_target = std::isfinite(vertical_target) ? 0.5f * (vertical_target + v) : v;
                }

                const float nyq = in.nyq[g];
                const float P = 2.0f * nyq;
                const float current = tilt.Radials_VEL[3 * g + 2];
                const float scale = std::max(1.5f, V9_RESCUE_SCALE_FRAC * nyq);
                auto pred_loss = [&](float v) {
                    const float r = std::fabs(v - prediction);
                    return r <= scale ? 0.5f * r * r / scale : r - 0.5f * scale;
                };
                auto total_cost = [&](float candidate, int& support, unsigned& dirs) {
                    double cost = 0.75 * pred_loss(candidate);
                    double edge_cost = 0.0;
                    support = 0;
                    dirs = 0u;
                    for (int d = 0; d < 4; ++d) {
                        const int j = in.graph[g][d];
                        if (j < 0 || !solved[j] || !std::isfinite(tilt.Radials_VEL[3 * j + 2])) continue;
                        const float en = std::min(in.nyq[g], in.nyq[j]);
                        const float diff = std::fabs(candidate - tilt.Radials_VEL[3 * j + 2]);
                        edge_cost += std::min(diff, 4.0f * en);
                        if (diff <= 0.55f * en) {
                            ++support;
                            dirs |= 1u << static_cast<unsigned>(d);
                        }
                    }
                    cost += 0.25 * edge_cost / std::max(1, nref);
                    if (std::isfinite(vertical_target)) {
                        const float s = std::max(2.0f, 0.40f * nyq);
                        const float r = std::fabs(candidate - vertical_target);
                        cost += 0.08 * (r <= s ? 0.5f * r * r / s : r - 0.5f * s);
                    }
                    return cost;
                };

                int current_support = 0; unsigned current_dirs = 0u;
                const double old_cost = total_cost(current, current_support, current_dirs);
                double best_cost = old_cost;
                int best_fold = static_cast<int>(std::lround((current - in.raw[g]) / P));
                int best_support = current_support;
                unsigned best_dirs = current_dirs;

                for (int k = -V9_MAX_FOLD; k <= V9_MAX_FOLD; ++k) {
                    if (k == best_fold) continue;
                    const float candidate = in.raw[g] + k * P;
                    int support = 0; unsigned dirs = 0u;
                    const double cost = total_cost(candidate, support, dirs);
                    const int axes = ((dirs & 3u) ? 1 : 0) + ((dirs & 12u) ? 1 : 0);
                    const int min_support = pass == 0 ? 2 : 2;
                    if (support < min_support || (pass == 0 && axes < 2)) continue;
                    if (cost < best_cost) {
                        best_cost = cost;
                        best_fold = k;
                        best_support = support;
                        best_dirs = dirs;
                    }
                }
                const int axes = ((best_dirs & 3u) ? 1 : 0) + ((best_dirs & 12u) ? 1 : 0);
                if (best_support < 2 || axes < 2) continue;
                if (old_cost <= 0.0 || (old_cost - best_cost) / old_cost < V9_RESCUE_MIN_IMPROVEMENT) continue;

                tilt.Radials_VEL[3 * g + 2] = in.raw[g] + best_fold * P;
                solved[g] = 1;
                ++rescued;
            }
        }
    }
}

} // namespace

void dealias_velocity_volume_v9(AllTilt& volume) {
    const size_t nt = volume.Tilts.size();
    if (nt == 0) return;

    std::vector<int> order(nt);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return volume.Tilts[a].ElevationAngle < volume.Tilts[b].ElevationAngle;
    });

    // Stage 1: the fast, well-tested v2 region solver on every tilt separately.
    for (int idx : order)
        dealias_tilt_v2_core(volume.Tilts[idx]);

    // Stage 2: recover the original folded observations modulo Nyquist and build
    // geometry/quality metadata for the vertical and local-harvest stages.
    std::vector<V9Info> info(nt);
    for (int idx : order)
        info[idx] = v9_prepare(volume.Tilts[idx]);

    // Stage 3: 3-D branch correction. Only quiet regions may move as a whole.
    v9_reconcile_vertical(volume, order, info);

    // Stage 4: bounded high-shear/tornado rescue.
    v9_local_hard_rescue(volume, order, info);
}
