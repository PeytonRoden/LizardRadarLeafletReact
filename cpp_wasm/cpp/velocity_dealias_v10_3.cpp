#include <string>
#include "velocity_dealias.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <queue>
#include <vector>

// ============================================================================
// v11 = v10 + fixes for (1) disconnected regions/islands and whole tilts
// landing a fold off, (2) residual tornado-couplet errors, (3) speed.
//
// Pipeline (volume):
//   A. Per-tilt region solve (v2 backbone). Range gaps up to 3 gates and one
//      missing radial are bridged, so a dropped gate no longer creates an
//      "island". Votes are accumulated incrementally (linear, not quadratic
//      in region degree). Components are tracked; the main one gets a
//      pair-balanced sector anchor (v(az) + v(az+180) ~ 0).
//   V. Absolute anchoring. Alias-robust VAD (after Xu et al.) fitted to the
//      FOLDED phases per 500 m height bin, candidates tracked bottom-up from
//      the weakest near-ground wind with a shear-jump limit, then an LS VAD
//      refit on the anchored data. Components that touch through hard
//      (detected-shear) gates are grouped and move together, so the VAD can
//      only shift groups separated by missing data -- never flip one side
//      of a real shear line. A shift needs a decisive vote and a tight
//      post-shift fit (median residual <= 0.25 Vn).
//   B. Region-level vertical reconciliation (v10).
//   X. Vortex reference check: hard-gate clusters -> alias-robust Rankine
//      vortex fit (center, core radius, tangential and radial speed; the
//      constant background is solved in closed form) -> clearly wrong gates
//      near the core are refolded against the model; confident gates become
//      anchors for C. Skipped when the window is already coherent.
//   C. Bounded continuity rescue of remaining hard gates (v10).
// ============================================================================

namespace {

constexpr float VEL_NAN = std::numeric_limits<float>::quiet_NaN();
constexpr double kPi = 3.14159265358979323846;
constexpr float AZ_EPS = 0.01f;
constexpr float SEG_FRAC = 0.20f;
constexpr float NYQ_REL_TOL = 0.03f;
constexpr int MAX_FOLDS = 6;

// Connectivity bridging.
constexpr float RANGE_BRIDGE_GATES = 3.0f;
constexpr float AZ_BRIDGE_MULT = 2.3f;
constexpr float AZ_BRIDGE_MAX_DEG = 2.0f;

// Moment (REF/SW) alignment.
constexpr float MOMENT_AZ_TOL_DEG = 1.5f;
constexpr float MOMENT_RANGE_TOL_M = 750.0f;

// Hard-region detection.
constexpr float HARD_CIRC_FRAC = 0.50f;
constexpr float HARD_REFL_MIN_DBZ = 25.0f;
constexpr float HARD_DILATE_MAX_FRAC = 0.10f;

// Spectrum width edge confidence.
constexpr float SW_TAPER_LO = 2.0f;
constexpr float SW_TAPER_HI = 8.0f;
constexpr float SW_CONF_FLOOR = 0.15f;

// Bounded continuity rescue.
constexpr size_t RESCUE_MAX_GATES_ABS = 4096;
constexpr float RESCUE_MAX_FRACTION = 0.035f;
constexpr float RESCUE_MIN_IMPROVEMENT = 0.30f;
constexpr int RESCUE_MIN_SUPPORT = 2;
constexpr float RESCUE_EDGE_SCALE_FRAC = 0.38f;
constexpr float RESCUE_VERT_WEIGHT = 0.20f;

// Vertical region reconciliation.
constexpr float VERT_MIN_COS = 0.4f;
constexpr int VERT_MIN_REGION_GATES = 6;
constexpr size_t VERT_MAX_SAMPLES = 48;
constexpr size_t VERT_SAMPLE_STRIDE_MIN = 3;
constexpr int VERT_MIN_SUPPORT = 6;
constexpr float VERT_MIN_FRAC = 0.72f;
constexpr float VERT_SIDE_FRAC = 0.65f;
constexpr float VERT_MAX_RESIDUAL_RATIO = 0.45f;

// Volume VAD profile / component anchoring.
constexpr double EARTH_EFF_RADIUS_M = 8.49e6;   // 4/3 Earth
constexpr float VAD_BIN_M = 500.0f;
constexpr int VAD_NBINS = 32;                   // 0 .. 16 km
constexpr float VAD_MAX_ELEV_DEG = 10.0f;
constexpr size_t VAD_MIN_COMP_GATES = 100;
constexpr size_t VAD_TILT_SAMPLES = 20000;
constexpr int VAD_MIN_BIN_SAMPLES = 40;
constexpr float VAD_INLIER_FRAC_NYQ = 0.6f;
constexpr int VAD_SECTOR_MIN = 6;
constexpr float VAD_WELL_CONDITIONED = 0.15f;
constexpr int ANCHOR_MAX_FOLD = 4;
constexpr size_t ANCHOR_MAX_SAMPLES = 600;
// No ANCHOR_MAX_MEDIAN_RES: the v10_2 median check caused regressions by
// blocking valid VAD corrections. v10_3 uses the same 1.0 Vn kernel as
// v10_1 without the post-shift median gate; Stage W (fold-jump repair)
// handles any residual errors instead.

// Vortex fit.
constexpr int VORTEX_MIN_CLUSTER = 4;
constexpr int VORTEX_MAX_PER_TILT = 4;
constexpr int VORTEX_MAX_PER_VOLUME = 16;
constexpr double VORTEX_SKIP_COHERENCE = 0.85;  // window already ~uniform: no vortex
constexpr double VORTEX_MERGE_M = 2000.0;
constexpr double VORTEX_WINDOW_M = 3000.0;
constexpr size_t VORTEX_MAX_SAMPLES = 450;
constexpr double VORTEX_MIN_SCORE = 0.55;
constexpr double VORTEX_MIN_GAIN = 0.12;
constexpr double VORTEX_APPLY_RES_FRAC = 0.40;   // |corrected - model| <= this * Vn
constexpr double VORTEX_WRONG_RES_FRAC = 1.20;   // |current - model| >= this * Vn
constexpr double VORTEX_CORE_RADIUS_MULT = 3.0;
constexpr double VORTEX_CORE_RADIUS_MIN = 1000.0;
constexpr double VORTEX_CORE_RADIUS_MAX = 2500.0;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

inline bool valid_nyq(float n) { return std::isfinite(n) && n > 0.0f; }

inline bool compatible_nyq(float a, float b) {
    if (!valid_nyq(a) || !valid_nyq(b)) return false;
    return std::fabs(a - b) <= NYQ_REL_TOL * std::max(a, b);
}

inline int fold_from_pair(float raw, float reference, float nyq) {
    return static_cast<int>(std::lround((reference - raw) / (2.0f * nyq)));
}

inline float wrap360(float x) {
    while (x < 0.0f) x += 360.0f;
    while (x >= 360.0f) x -= 360.0f;
    return x;
}

inline float az_distance(float a, float b) {
    const float d = std::fabs(a - b);
    return std::min(d, 360.0f - d);
}

inline float circular_velocity_distance(float a, float b, float nyq) {
    if (!std::isfinite(a) || !std::isfinite(b) || !valid_nyq(nyq)) return VEL_NAN;
    const float p = 2.0f * nyq;
    float d = std::fmod(std::fabs(a - b), p);
    if (d > nyq) d = p - d;
    return d;
}

inline float huber_cost(float residual, float scale) {
    const float r = std::fabs(residual);
    const float d = std::max(scale, 1.0e-3f);
    if (r <= d) return 0.5f * r * r / d;
    return r - 0.5f * d;
}

inline float sw_confidence(float sw) {
    if (!std::isfinite(sw) || sw < 0.0f) return 1.0f;
    if (sw <= SW_TAPER_LO) return 1.0f;
    if (sw >= SW_TAPER_HI) return SW_CONF_FLOOR;
    const float t = (sw - SW_TAPER_LO) / (SW_TAPER_HI - SW_TAPER_LO);
    return 1.0f - t * (1.0f - SW_CONF_FLOOR);
}

inline double beam_height_m(double range_m, double sin_e) {
    const double R = EARTH_EFF_RADIUS_M;
    return std::sqrt(range_m * range_m + R * R + 2.0 * range_m * R * sin_e) - R;
}

// ---------------------------------------------------------------------------
// Ray index for secondary moments and cross-tilt lookups.
// ---------------------------------------------------------------------------

struct MomentRay {
    size_t start = 0;
    size_t count = 0;
    float azimuth = 0.0f;
};

struct MomentIndex {
    const std::vector<float>* packed = nullptr;
    std::vector<MomentRay> rays; // sorted by azimuth
};

MomentIndex build_moment_index(const std::vector<float>& packed) {
    MomentIndex idx;
    if (packed.size() < 3 || packed.size() % 3 != 0) return idx;
    idx.packed = &packed;
    const size_t count = packed.size() / 3;
    size_t ray_start = 0;
    for (size_t t = 0; t < count; ++t) {
        const bool new_ray = (t == 0) ||
            std::fabs(packed[3 * t] - packed[3 * (t - 1)]) > AZ_EPS ||
            packed[3 * t + 1] <= packed[3 * (t - 1) + 1];
        if (new_ray) {
            if (t > ray_start)
                idx.rays.push_back({ray_start, t - ray_start, packed[3 * ray_start]});
            ray_start = t;
        }
    }
    if (count > ray_start)
        idx.rays.push_back({ray_start, count - ray_start, packed[3 * ray_start]});
    std::sort(idx.rays.begin(), idx.rays.end(),
              [](const MomentRay& a, const MomentRay& b) { return a.azimuth < b.azimuth; });
    return idx;
}

// Takes the ray vector by reference (v10 copied it on every lookup).
int nearest_ray(const std::vector<MomentRay>& rays, float az) {
    if (rays.empty()) return -1;
    az = wrap360(az);
    size_t lo = 0, hi = rays.size();
    while (lo < hi) {
        const size_t m = (lo + hi) / 2;
        if (rays[m].azimuth < az) lo = m + 1; else hi = m;
    }
    const size_t n = rays.size();
    const size_t a = lo < n ? lo : 0;
    const size_t b = lo > 0 ? lo - 1 : n - 1;
    return az_distance(rays[a].azimuth, az) < az_distance(rays[b].azimuth, az)
               ? static_cast<int>(a) : static_cast<int>(b);
}

// Linear-time alignment of a moment onto the VEL gates: one ray search per
// VEL radial, then a merge walk along range (v10 did a binary search per gate).
std::vector<float> align_moment(const MomentIndex& idx, const std::vector<float>& vel,
                                const std::vector<VelocityRay>& rays) {
    std::vector<float> out(vel.size() / 3, VEL_NAN);
    if (!idx.packed || idx.rays.empty()) return out;
    const std::vector<float>& mp = *idx.packed;
    for (const VelocityRay& ray : rays) {
        const float az = vel[3 * ray.start];
        const int ri = nearest_ray(idx.rays, az);
        if (ri < 0) continue;
        const MomentRay& mr = idx.rays[static_cast<size_t>(ri)];
        if (az_distance(mr.azimuth, wrap360(az)) > MOMENT_AZ_TOL_DEG) continue;
        size_t j = mr.start;
        const size_t je = mr.start + mr.count;
        for (size_t t = ray.start; t < ray.start + ray.count; ++t) {
            const float r = vel[3 * t + 1];
            while (j + 1 < je && std::fabs(mp[3 * (j + 1) + 1] - r) <= std::fabs(mp[3 * j + 1] - r)) ++j;
            if (std::fabs(mp[3 * j + 1] - r) <= MOMENT_RANGE_TOL_M) out[t] = mp[3 * j + 2];
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Region solver state.
// ---------------------------------------------------------------------------

struct Boundary { int gate; int neighbor; };

struct Region {
    std::vector<int> gates;
    std::vector<Boundary> boundary;
    float nyquist = 0.0f;
    int fold = 0;
    int version = 0;
    int comp = -1;
    bool resolved = false;
};

struct Candidate {
    float evidence;
    int region;
    int fold;
    int version;
    bool operator<(const Candidate& o) const {
        if (evidence != o.evidence) return evidence < o.evidence;
        return region > o.region;
    }
};

struct TiltCache {
    bool valid = false;
    float elevation_deg = 0.0f;
    float cos_e = 1.0f;
    float sin_e = 0.0f;
    int ncomp = 0;
    std::vector<MomentRay> vel_rays;          // sorted by azimuth
    std::vector<float> nyq;                   // per gate
    std::vector<unsigned char> hard;          // per gate
    std::vector<float> sw_conf;               // per gate
    std::vector<std::array<int, 4>> graph;    // per gate
    std::vector<Region> regions;
    std::vector<int> region_of_gate;          // -1 for hard / invalid gates
};

// ---------------------------------------------------------------------------
// Stage A: per-tilt region solve.
// ---------------------------------------------------------------------------

TiltCache dealias_tilt_stage_a(SingleTilt& tilt) {
    TiltCache cache;
    std::vector<float>& packed = tilt.Radials_VEL;
    if (packed.size() < 24 || packed.size() % 3 != 0) return cache;
    const size_t count = packed.size() / 3;
    if (count > static_cast<size_t>(std::numeric_limits<int>::max() / 3)) return cache;

    const float fallback_nyq = static_cast<float>(tilt.vol_el_rad.rad.nyquist_vel) / 100.0f;
    const int n = static_cast<int>(count);
    std::vector<VelocityRay> rays = tilt.VelocityRays;

    if (rays.empty()) {
        if (!valid_nyq(fallback_nyq)) return cache;
        rays.reserve(720);
        for (int t = 0; t < n; ++t) {
            if (t == 0 || std::fabs(packed[3 * t] - packed[3 * (t - 1)]) > AZ_EPS ||
                packed[3 * t + 1] <= packed[3 * (t - 1) + 1])
                rays.push_back({static_cast<size_t>(t), 0, fallback_nyq, tilt.gateSpacing});
            ++rays.back().count;
        }
        if (!tilt.VelNyquist.empty() && tilt.VelNyquist.size() != rays.size()) return cache;
        for (size_t r = 0; r < rays.size(); ++r)
            if (!tilt.VelNyquist.empty() && valid_nyq(tilt.VelNyquist[r]))
                rays[r].nyquist = tilt.VelNyquist[r];
    }
    if (rays.size() < 4) return cache;

    std::vector<float> nyq(count, VEL_NAN);
    size_t expected = 0;
    for (const auto& ray : rays) {
        if (ray.start != expected || ray.count == 0 || ray.count > count - expected) return cache;
        if (!std::isfinite(ray.gateSpacing) || ray.gateSpacing <= 0.0f) return cache;
        const float ray_nyq = valid_nyq(ray.nyquist) ? ray.nyquist : fallback_nyq;
        if (!valid_nyq(ray_nyq)) return cache;
        const size_t end = ray.start + ray.count;
        const float az = packed[3 * ray.start];
        if (!std::isfinite(az) || az < 0.0f || az >= 360.0f) return cache;
        for (size_t t = ray.start; t < end; ++t) {
            if (!std::isfinite(packed[3 * t + 1])) return cache;
            if (t > ray.start && packed[3 * t + 1] <= packed[3 * (t - 1) + 1]) return cache;
            nyq[t] = ray_nyq;
        }
        expected = end;
    }
    if (expected != count) return cache;

    float az_sum = 0.0f;
    int az_samples = 0;
    for (size_t r = 1; r < rays.size(); ++r) {
        const float gap = std::fmod(packed[3 * rays[r].start] - packed[3 * rays[r - 1].start] + 360.0f, 360.0f);
        if (gap > AZ_EPS && gap <= 1.5f) { az_sum += gap; ++az_samples; }
    }
    const float typical_az = az_samples ? az_sum / az_samples : 1.0f;
    // Bridges one missing radial (v10: 1.8x, which broke on any dropped radial).
    const float max_az_gap = std::min(AZ_BRIDGE_MAX_DEG, AZ_BRIDGE_MULT * typical_az);

    std::vector<float> grid(count);
    for (size_t i = 0; i < count; ++i) grid[i] = packed[3 * i + 2];

    std::vector<std::array<int, 4>> neighbors(count, {-1, -1, -1, -1});
    auto connect = [&](int a, int b, int dir) {
        if (a < 0 || b < 0) return;
        if (!std::isfinite(grid[a]) || !std::isfinite(grid[b])) return;
        if (!valid_nyq(nyq[a]) || !valid_nyq(nyq[b])) return;
        neighbors[a][dir] = b;
        neighbors[b][dir ^ 1] = a;
    };

    for (size_t r = 0; r < rays.size(); ++r) {
        const auto& ray = rays[r];
        const size_t end = ray.start + ray.count;
        // Range links bridge up to RANGE_BRIDGE_GATES missing/thresholded
        // gates. The flood-fill similarity test still guards every link.
        const float max_dr = (RANGE_BRIDGE_GATES + 0.05f) * ray.gateSpacing;
        for (size_t t = ray.start + 1; t < end; ++t) {
            const float dr = packed[3 * t + 1] - packed[3 * (t - 1) + 1];
            if (dr > 0.5f * ray.gateSpacing && dr <= max_dr)
                connect(static_cast<int>(t - 1), static_cast<int>(t), 0);
        }
        const size_t next = (r + 1) % rays.size();
        const float gap = std::fmod(packed[3 * rays[next].start] - packed[3 * ray.start] + 360.0f, 360.0f);
        if (gap <= AZ_EPS || gap > max_az_gap) continue;
        size_t a = ray.start, b = rays[next].start;
        const size_t ae = ray.start + ray.count, be = rays[next].start + rays[next].count;
        while (a < ae && b < be) {
            const float d = packed[3 * a + 1] - packed[3 * b + 1];
            if (std::fabs(d) <= 0.1f) {
                connect(static_cast<int>(a), static_cast<int>(b), 2);
                ++a; ++b;
            } else if (d < 0.0f) ++a; else ++b;
        }
    }

    // Moments.
    const MomentIndex ref_idx = build_moment_index(tilt.Radials_REF);
    const MomentIndex sw_idx = build_moment_index(tilt.Radials_SW);
    const bool have_ref = ref_idx.packed != nullptr && !ref_idx.rays.empty();
    const std::vector<float> ref_dbz = align_moment(ref_idx, packed, rays);
    const std::vector<float> sw_raw = align_moment(sw_idx, packed, rays);
    std::vector<float> sw_conf(count, 1.0f);
    for (size_t i = 0; i < count; ++i) sw_conf[i] = sw_confidence(sw_raw[i]);

    // Hard-core detection.
    std::vector<unsigned char> hard_candidate(count, 0);
    for (int i = 0; i < n; ++i) {
        if (!std::isfinite(grid[i]) || !valid_nyq(nyq[i])) continue;
        float max_frac = 0.0f;
        for (int nb : neighbors[i]) {
            if (nb < 0) continue;
            const float en = std::min(nyq[i], nyq[nb]);
            const float cd = circular_velocity_distance(grid[i], grid[nb], en);
            if (std::isfinite(cd)) max_frac = std::max(max_frac, cd / en);
        }
        hard_candidate[i] = max_frac >= HARD_CIRC_FRAC ? 1 : 0;
    }
    size_t candidate_count = 0;
    for (unsigned char x : hard_candidate) candidate_count += x != 0;
    const float candidate_frac = n > 0 ? static_cast<float>(candidate_count) / static_cast<float>(n) : 1.0f;
    if (candidate_frac <= HARD_DILATE_MAX_FRAC) {
        std::vector<unsigned char> dilated = hard_candidate;
        for (int i = 0; i < n; ++i) {
            if (!hard_candidate[i]) continue;
            for (int nb : neighbors[i]) if (nb >= 0) dilated[nb] = 1;
        }
        hard_candidate.swap(dilated);
    }
    std::vector<unsigned char> hard(count, 0);
    for (int i = 0; i < n; ++i) {
        if (!hard_candidate[i]) continue;
        const bool ref_ok = !have_ref || (std::isfinite(ref_dbz[i]) && ref_dbz[i] >= HARD_REFL_MIN_DBZ);
        hard[i] = ref_ok ? 1 : 0;
    }

    // Flood-fill regions (hard gates excluded).
    std::vector<int> labels(count, -1);
    std::vector<Region> regions;
    regions.reserve(count / 8 + 1);
    for (int start = 0; start < n; ++start) {
        if (labels[start] >= 0 || hard[start] || !std::isfinite(grid[start]) || !valid_nyq(nyq[start]))
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
                if (nb < 0 || labels[nb] >= 0 || hard[nb]) continue;
                if (!std::isfinite(grid[nb])) continue;
                if (!compatible_nyq(region.nyquist, nyq[nb])) continue;
                const float edge_nyq = std::min(region.nyquist, nyq[nb]);
                if (std::fabs(grid[g] - grid[nb]) > SEG_FRAC * edge_nyq) continue;
                labels[nb] = id;
                region.gates.push_back(nb);
            }
        }
    }

    cache.valid = true;
    cache.elevation_deg = tilt.ElevationAngle;
    cache.cos_e = static_cast<float>(std::cos(deg2rad(tilt.ElevationAngle)));
    cache.sin_e = static_cast<float>(std::sin(deg2rad(tilt.ElevationAngle)));
    cache.vel_rays.reserve(rays.size());
    for (const auto& r : rays) cache.vel_rays.push_back({r.start, r.count, packed[3 * r.start]});
    std::sort(cache.vel_rays.begin(), cache.vel_rays.end(),
              [](const MomentRay& a, const MomentRay& b) { return a.azimuth < b.azimuth; });

    if (regions.empty()) {
        cache.nyq = std::move(nyq);
        cache.hard = std::move(hard);
        cache.sw_conf = std::move(sw_conf);
        cache.graph = std::move(neighbors);
        return cache;
    }

    for (int g = 0; g < n; ++g) {
        const int a = labels[g];
        if (a < 0) continue;
        for (const int nb : neighbors[g]) {
            if (nb < 0) continue;
            const int b = labels[nb];
            if (b >= 0 && b != a) regions[a].boundary.push_back({g, nb});
        }
    }

    // Best-first propagation, SW-weighted votes, component tracking.
    // Uses the same re-scan approach as v10_1: when region is popped from the
    // PQ, its boundary is rescanned to freshly compute votes from all currently-
    // resolved neighbours.  This avoids floating-point order sensitivity.
    std::priority_queue<Candidate> pq;
    size_t resolved = 0;
    int current_comp = -1;

    auto propose = [&](int id) {
        Region& region = regions[id];
        if (region.resolved) return;
        std::array<float, 2 * MAX_FOLDS + 1> votes{};
        float evidence = 0.0f;
        for (const Boundary& edge : region.boundary) {
            const int nb = labels[edge.neighbor];
            if (nb < 0 || !regions[nb].resolved) continue;
            const float edge_nyq = std::min(region.nyquist, nyq[edge.neighbor]);
            const int k = fold_from_pair(grid[edge.gate], grid[edge.neighbor], edge_nyq);
            if (std::abs(k) > MAX_FOLDS) continue;
            const float w = sw_conf[edge.gate] * sw_conf[edge.neighbor];
            votes[k + MAX_FOLDS] += w;
            evidence += w;
        }
        if (evidence <= 0.0f) return;
        int best = 0;
        for (int i = 1; i < static_cast<int>(votes.size()); ++i)
            if (votes[i] > votes[best]) best = i;
        ++region.version;
        pq.push({evidence, id, best - MAX_FOLDS, region.version});
    };

    auto resolve = [&](int id, int fold) {
        Region& region = regions[id];
        if (region.resolved) return;
        region.fold = fold;
        region.resolved = true;
        region.comp = current_comp;
        ++resolved;
        const float delta = fold * 2.0f * region.nyquist;
        for (const int g : region.gates) grid[g] += delta;
        for (const Boundary& edge : region.boundary) {
            const int nb = labels[edge.neighbor];
            if (nb >= 0 && !regions[nb].resolved) propose(nb);
        }
    };

    auto drain = [&]() {
        while (!pq.empty()) {
            const Candidate c = pq.top(); pq.pop();
            Region& region = regions[c.region];
            if (region.resolved || region.version != c.version) continue;
            resolve(c.region, c.fold);
        }
    };

    // Seed components largest-first.  Component 0 is the main echo.
    while (resolved < regions.size()) {
        int seed = -1;
        for (size_t i = 0; i < regions.size(); ++i) {
            if (regions[i].resolved) continue;
            if (seed < 0 || regions[i].gates.size() > regions[seed].gates.size() ||
                (regions[i].gates.size() == regions[seed].gates.size() &&
                 regions[i].nyquist > regions[seed].nyquist))
                seed = static_cast<int>(i);
        }
        if (seed < 0) break;
        ++current_comp;
        resolve(seed, 0);
        drain();
    }
    cache.ncomp = current_comp + 1;

    // Bounded +/-1 repair pass (v2).
    for (Region& region : regions) {
        if (region.boundary.size() < 2) continue;
        const float interval = 2.0f * region.nyquist;
        double e0 = 0.0, em = 0.0, ep = 0.0;
        int support = 0, sm = 0, sp = 0;
        for (const Boundary& edge : region.boundary) {
            const int nb = labels[edge.neighbor];
            if (nb < 0 || !regions[nb].resolved) continue;
            const float v = grid[edge.gate];
            const float ref = grid[edge.neighbor];
            const float a = std::fabs(v - ref);
            const float m = std::fabs(v - interval - ref);
            const float p = std::fabs(v + interval - ref);
            e0 += a; em += m; ep += p; ++support;
            if (m < 0.75f * a) ++sm;
            if (p < 0.75f * a) ++sp;
        }
        if (support < 2) continue;
        int shift = 0;
        if (sm >= 2 && em < 0.80 * e0 && em < ep) shift = -1;
        else if (sp >= 2 && ep < 0.80 * e0 && ep < em) shift = +1;
        if (shift != 0) {
            const float delta = shift * interval;
            for (const int g : region.gates) grid[g] += delta;
            region.fold += shift;
        }
    }

    // Main-component anchor: pair-balanced sector means. For any horizontal
    // wind v(az) + v(az+180) ~ 0, so the average over opposite-sector pairs
    // should be near zero. Unlike a raw scan mean (v10), this is not biased
    // when echo covers only part of the circle. Stage V refines all
    // components afterwards using a volume VAD profile.
    {
        std::array<double, 36> s_sum{};
        std::array<int, 36> s_cnt{};
        double nyq_sum = 0.0;
        size_t nn = 0;
        for (const Region& rg : regions) {
            if (rg.comp != 0) continue;
            for (const int g : rg.gates) {
                const int s = static_cast<int>(wrap360(packed[3 * g]) / 10.0f) % 36;
                s_sum[s] += grid[g];
                ++s_cnt[s];
                nyq_sum += nyq[g];
                ++nn;
            }
        }
        double stat = 0.0;
        int npairs = 0;
        for (int s = 0; s < 18; ++s) {
            if (s_cnt[s] >= 30 && s_cnt[s + 18] >= 30) {
                stat += 0.5 * (s_sum[s] / s_cnt[s] + s_sum[s + 18] / s_cnt[s + 18]);
                ++npairs;
            }
        }
        if (npairs >= 3 && nn > 0) {
            stat /= npairs;
            const double vn = nyq_sum / static_cast<double>(nn);
            int best_k = 0;
            double best_abs = std::fabs(stat);
            for (int k = -3; k <= 3; ++k) {
                if (k == 0) continue;
                const double v = std::fabs(stat + 2.0 * k * vn);
                if (v < best_abs) { best_abs = v; best_k = k; }
            }
            if (best_k != 0 && best_abs < 0.35 * std::fabs(stat) && best_abs < 0.30 * vn) {
                for (Region& rg : regions) {
                    if (rg.comp != 0) continue;
                    const float delta = best_k * 2.0f * rg.nyquist;
                    for (const int g : rg.gates) grid[g] += delta;
                    rg.fold += best_k;
                }
            }
        }
    }

    for (size_t i = 0; i < count; ++i) packed[3 * i + 2] = grid[i];

    cache.nyq = std::move(nyq);
    cache.hard = std::move(hard);
    cache.sw_conf = std::move(sw_conf);
    cache.graph = std::move(neighbors);
    cache.regions = std::move(regions);
    cache.region_of_gate = std::move(labels);
    return cache;
}

// ---------------------------------------------------------------------------
// Stage V: volume VAD profile from dealiased data + component anchoring.
// ---------------------------------------------------------------------------

struct VadProfile {
    bool any = false;
    std::array<float, VAD_NBINS> u{}, v{}, cond{};
    std::array<unsigned char, VAD_NBINS> ok{};
    std::array<uint16_t, VAD_NBINS> sectors{};  // 12 x 30 deg coverage bits

    float ref(double h, float az_deg, float cos_e) const {
        if (!any || !(h >= 0.0)) return VEL_NAN;
        const int b = static_cast<int>(h / VAD_BIN_M);
        if (b < 0 || b >= VAD_NBINS || !ok[b]) return VEL_NAN;
        const float a = wrap360(az_deg);
        const int s = static_cast<int>(a / 30.0f) % 12;
        const int so = (s + 6) % 12;
        const bool covered = ((sectors[b] >> s) & 1u) || ((sectors[b] >> so) & 1u);
        if (!covered && cond[b] < VAD_WELL_CONDITIONED) return VEL_NAN;
        const double ar = deg2rad(a);
        return static_cast<float>((u[b] * std::sin(ar) + v[b] * std::cos(ar)) * cos_e);
    }
};

struct VadSample { float x1, x2, v, nyq; uint8_t sector; };

VadProfile fit_vad(const AllTilt& volume, const std::vector<TiltCache>& cache) {
    VadProfile prof;
    std::vector<std::vector<VadSample>> bins(VAD_NBINS);

    for (size_t ti = 0; ti < volume.Tilts.size(); ++ti) {
        const TiltCache& c = cache[ti];
        if (!c.valid || c.regions.empty() || c.elevation_deg > VAD_MAX_ELEV_DEG) continue;
        const std::vector<float>& vel = volume.Tilts[ti].Radials_VEL;

        std::vector<size_t> csize(static_cast<size_t>(std::max(c.ncomp, 1)), 0);
        for (const Region& rg : c.regions)
            if (rg.comp >= 0) csize[rg.comp] += rg.gates.size();
        size_t eligible = 0;
        for (const Region& rg : c.regions)
            if (rg.comp >= 0 && csize[rg.comp] >= VAD_MIN_COMP_GATES) eligible += rg.gates.size();
        if (eligible == 0) continue;
        const size_t stride = std::max<size_t>(1, eligible / VAD_TILT_SAMPLES);

        size_t counter = 0;
        for (const Region& rg : c.regions) {
            if (rg.comp < 0 || csize[rg.comp] < VAD_MIN_COMP_GATES) continue;
            for (const int g : rg.gates) {
                if ((counter++ % stride) != 0) continue;
                const float val = vel[3 * g + 2];
                if (!std::isfinite(val)) continue;
                const double h = beam_height_m(vel[3 * g + 1], c.sin_e);
                const int b = static_cast<int>(h / VAD_BIN_M);
                if (b < 0 || b >= VAD_NBINS) continue;
                const float az = wrap360(vel[3 * g]);
                const double ar = deg2rad(az);
                bins[b].push_back({static_cast<float>(std::sin(ar) * c.cos_e),
                                   static_cast<float>(std::cos(ar) * c.cos_e),
                                   val, c.nyq[g],
                                   static_cast<uint8_t>(static_cast<int>(az / 30.0f) % 12)});
            }
        }
    }

    for (int b = 0; b < VAD_NBINS; ++b) {
        const auto& S = bins[b];
        if (static_cast<int>(S.size()) < VAD_MIN_BIN_SAMPLES) continue;
        double U = 0.0, V = 0.0;
        double a11 = 0, a12 = 0, a22 = 0;
        int n_in = 0;
        std::array<int, 12> sec{};
        for (int it = 0; it < 3; ++it) {
            a11 = a12 = a22 = 0.0;
            double b1 = 0.0, b2 = 0.0;
            n_in = 0;
            sec.fill(0);
            for (const VadSample& s : S) {
                if (it > 0) {
                    const double res = s.v - (U * s.x1 + V * s.x2);
                    if (std::fabs(res) > VAD_INLIER_FRAC_NYQ * s.nyq) continue;
                }
                a11 += s.x1 * s.x1; a12 += s.x1 * s.x2; a22 += s.x2 * s.x2;
                b1 += s.x1 * s.v;  b2 += s.x2 * s.v;
                ++n_in;
                ++sec[s.sector];
            }
            if (n_in < VAD_MIN_BIN_SAMPLES / 2) break;
            const double lam = 1.0e-3 * n_in;
            const double m11 = a11 + lam, m22 = a22 + lam;
            const double det = m11 * m22 - a12 * a12;
            if (!(det > 0.0)) break;
            U = (b1 * m22 - b2 * a12) / det;
            V = (b2 * m11 - b1 * a12) / det;
        }
        if (n_in < VAD_MIN_BIN_SAMPLES / 2 || n_in < static_cast<int>(0.6 * S.size())) continue;

        uint16_t mask = 0;
        for (int s = 0; s < 12; ++s) if (sec[s] >= VAD_SECTOR_MIN) mask |= static_cast<uint16_t>(1u << s);
        if (mask == 0) continue;

        const double tr = a11 + a22;
        const double dd = std::sqrt(std::max(0.0, (a11 - a22) * (a11 - a22) + 4.0 * a12 * a12));
        const double lmax = 0.5 * (tr + dd), lmin = 0.5 * (tr - dd);

        prof.u[b] = static_cast<float>(U);
        prof.v[b] = static_cast<float>(V);
        prof.cond[b] = lmax > 0.0 ? static_cast<float>(lmin / lmax) : 0.0f;
        prof.sectors[b] = mask;
        prof.ok[b] = 1;
        prof.any = true;
    }

    // Fill single-bin holes by averaging neighbors (no extrapolation).
    for (int b = 1; b + 1 < VAD_NBINS; ++b) {
        if (prof.ok[b] || !prof.ok[b - 1] || !prof.ok[b + 1]) continue;
        prof.u[b] = 0.5f * (prof.u[b - 1] + prof.u[b + 1]);
        prof.v[b] = 0.5f * (prof.v[b - 1] + prof.v[b + 1]);
        prof.cond[b] = std::min(prof.cond[b - 1], prof.cond[b + 1]);
        prof.sectors[b] = prof.sectors[b - 1] & prof.sectors[b + 1];
        prof.ok[b] = prof.sectors[b] != 0;
    }
    return prof;
}


// ---------------------------------------------------------------------------
// Alias-robust VAD (after Xu et al. AR-VAD), fitted to FOLDED phases, so it
// does not depend on how stage A anchored anything.
//
// Per height bin: grid-search (U, V) maximizing mean cos(pi*(v - m)/Vn)
// (invariant to folding), keep up to 8 local maxima, refine each. Then a
// dynamic program over height picks one candidate per bin: weakest wind at
// the lowest bin, smooth changes with height, and a penalty for low scores.
// This breaks the "whole band reversed by one fold" ambiguity that
// opposite-sector data cannot break by itself: the reversed solution needs
// a wind ~2Vn stronger near the ground and a jump somewhere aloft.
// Cost: one sincos per sample per U row; the V dimension uses complex
// rotation. About 20M multiply-adds for a full volume.
// ---------------------------------------------------------------------------

struct ArSample { float p, a, b, x1, x2, nyq; uint8_t sector; };
struct ArCand { float U, V, score; };

VadProfile fit_ar_vad(const AllTilt& volume, const std::vector<TiltCache>& cache) {
    constexpr int PER_BIN = 400;
    constexpr float WMAX = 80.0f;
    VadProfile prof;

    std::array<size_t, VAD_NBINS> counts{};
    auto for_each_gate = [&](auto&& fn) {
        for (size_t ti = 0; ti < volume.Tilts.size(); ++ti) {
            const TiltCache& c = cache[ti];
            if (!c.valid || c.elevation_deg > VAD_MAX_ELEV_DEG) continue;
            const std::vector<float>& vel = volume.Tilts[ti].Radials_VEL;
            const size_t n = c.nyq.size();
            for (size_t g = 0; g < n; ++g) {
                if (c.hard[g] || !valid_nyq(c.nyq[g]) || !std::isfinite(vel[3 * g + 2])) continue;
                const double h = beam_height_m(vel[3 * g + 1], c.sin_e);
                const int b = static_cast<int>(h / VAD_BIN_M);
                if (b < 0 || b >= VAD_NBINS) continue;
                fn(ti, g, b);
            }
        }
    };
    for_each_gate([&](size_t, size_t, int b) { ++counts[b]; });

    std::array<size_t, VAD_NBINS> stride{}, seen{};
    for (int b = 0; b < VAD_NBINS; ++b) stride[b] = std::max<size_t>(1, counts[b] / PER_BIN);
    std::vector<std::vector<ArSample>> bins(VAD_NBINS);
    for_each_gate([&](size_t ti, size_t g, int b) {
        if ((seen[b]++ % stride[b]) != 0) return;
        const TiltCache& c = cache[ti];
        const std::vector<float>& vel = volume.Tilts[ti].Radials_VEL;
        const float az = wrap360(vel[3 * g]);
        const double ar = deg2rad(az);
        const float vn = c.nyq[g];
        const float x1 = static_cast<float>(std::sin(ar) * c.cos_e);
        const float x2 = static_cast<float>(std::cos(ar) * c.cos_e);
        const float k = static_cast<float>(kPi) / vn;
        bins[b].push_back({k * vel[3 * g + 2], k * x1, k * x2, x1, x2, vn,
                           static_cast<uint8_t>(static_cast<int>(az / 30.0f) % 12)});
    });

    std::vector<std::vector<ArCand>> cands(VAD_NBINS);
    std::vector<double> zr, zi, rr, ri;
    for (int b = 0; b < VAD_NBINS; ++b) {
        const auto& S = bins[b];
        const size_t N = S.size();
        if (static_cast<int>(N) < VAD_MIN_BIN_SAMPLES) continue;
        float vn_min = S[0].nyq;
        for (const auto& s : S) vn_min = std::min(vn_min, s.nyq);
        const float step = std::clamp(0.2f * vn_min, 1.5f, 4.0f);
        const int NG = static_cast<int>(2.0f * WMAX / step) + 1;
        std::vector<float> G(static_cast<size_t>(NG) * NG);
        zr.assign(N, 0); zi.assign(N, 0); rr.resize(N); ri.resize(N);
        for (size_t i = 0; i < N; ++i) { rr[i] = std::cos(-step * S[i].b); ri[i] = std::sin(-step * S[i].b); }
        for (int iu = 0; iu < NG; ++iu) {
            const float U = -WMAX + iu * step;
            for (size_t i = 0; i < N; ++i) {
                const double psi = S[i].p - U * S[i].a + WMAX * S[i].b;
                zr[i] = std::cos(psi); zi[i] = std::sin(psi);
            }
            for (int iv = 0; iv < NG; ++iv) {
                double sr = 0.0;
                for (size_t i = 0; i < N; ++i) {
                    sr += zr[i];
                    const double nr = zr[i] * rr[i] - zi[i] * ri[i];
                    zi[i] = zr[i] * ri[i] + zi[i] * rr[i];
                    zr[i] = nr;
                }
                G[static_cast<size_t>(iu) * NG + iv] = static_cast<float>(sr / N);
            }
        }
        float gbest = -2.0f;
        for (float x : G) gbest = std::max(gbest, x);
        std::vector<ArCand> local;
        for (int iu = 1; iu + 1 < NG; ++iu) {
            for (int iv = 1; iv + 1 < NG; ++iv) {
                const float sc = G[static_cast<size_t>(iu) * NG + iv];
                if (sc < std::max(0.35f, gbest - 0.25f)) continue;
                bool is_max = true;
                for (int du = -1; du <= 1 && is_max; ++du)
                    for (int dv = -1; dv <= 1; ++dv)
                        if ((du || dv) && G[static_cast<size_t>(iu + du) * NG + iv + dv] > sc) { is_max = false; break; }
                if (is_max) local.push_back({-WMAX + iu * step, -WMAX + iv * step, sc});
            }
        }
        std::sort(local.begin(), local.end(), [](const ArCand& a, const ArCand& c) { return a.score > c.score; });
        if (local.size() > 8) local.resize(8);
        for (ArCand& cd : local) {
            ArCand best = cd;
            const float fs = step / 3.0f;
            for (int du = -3; du <= 3; ++du) {
                for (int dv = -3; dv <= 3; ++dv) {
                    const float U = cd.U + du * fs, V = cd.V + dv * fs;
                    double sr = 0.0;
                    for (const auto& s : S) sr += std::cos(s.p - U * s.a - V * s.b);
                    const float sc = static_cast<float>(sr / N);
                    if (sc > best.score) best = {U, V, sc};
                }
            }
            cd = best;
        }
        cands[b] = std::move(local);
    }

    // Bottom-up tracking over height. Start at the lowest populated bin
    // (weakest wind, penalized by score deficit), then at each bin take the
    // candidate that continues smoothly from the last accepted bin. If no
    // candidate lies within a plausible shear jump, the bin is left unused
    // instead of jumping to an alias track; upper bins can never overrule
    // lower-level evidence.
    // True DP over height bins (matching v10_1): at each bin consider ALL
    // candidates at the previous non-empty bin to find the globally optimal
    // path.  No hard or soft jump rejection — the transition cost G_TRANS * jump/gap
    // already penalises large inter-bin wind changes without ever discarding
    // a path entirely.  This avoids the v10_3-original problem where the
    // greedy + 15 m/s hard limit dropped valid mid-elevation candidates in
    // tornado outbreak environments with strong low-level jets.
    constexpr double A_START = 0.05, B_SCORE = 20.0, G_TRANS = 0.5;
    std::vector<std::vector<double>> dp_cost(VAD_NBINS);
    std::vector<std::vector<int>>    dp_back(VAD_NBINS);
    int prev = -1;
    for (int b = 0; b < VAD_NBINS; ++b) {
        const auto& C = cands[b];
        if (C.empty()) continue;
        float best_sc = C[0].score;
        for (const auto& cd : C) best_sc = std::max(best_sc, cd.score);
        dp_cost[b].assign(C.size(), 0.0);
        dp_back[b].assign(C.size(), -1);
        for (size_t j = 0; j < C.size(); ++j) {
            const double own = B_SCORE * (best_sc - C[j].score);
            if (prev < 0) {
                dp_cost[b][j] = own + A_START * std::hypot(C[j].U, C[j].V);
            } else {
                const double gap = std::max(1, b - prev);
                double bestc = std::numeric_limits<double>::infinity();
                int arg = -1;
                for (size_t q = 0; q < cands[prev].size(); ++q) {
                    const double jump = std::hypot(C[j].U - cands[prev][q].U,
                                                   C[j].V - cands[prev][q].V);
                    const double tc = dp_cost[prev][q] + G_TRANS * jump / gap;
                    if (tc < bestc) { bestc = tc; arg = static_cast<int>(q); }
                }
                dp_cost[b][j] = own + bestc;
                dp_back[b][j] = arg;
            }
        }
        prev = b;
    }
    if (prev < 0) return prof;

    std::array<int, VAD_NBINS> choice;
    choice.fill(-1);
    {
        int j = static_cast<int>(
            std::min_element(dp_cost[prev].begin(), dp_cost[prev].end()) -
            dp_cost[prev].begin());
        for (int b = prev; b >= 0; --b) {
            if (cands[b].empty()) continue;
            choice[b] = j;
            if (dp_back[b][j] < 0) break;
            j = dp_back[b][j];
        }
    }

    for (int b = 0; b < VAD_NBINS; ++b) {
        if (choice[b] < 0) continue;
        const ArCand& cd = cands[b][choice[b]];
        if (cd.score < 0.45f) continue;
        std::array<int, 12> sec{};
        double a11 = 0, a12 = 0, a22 = 0;
        for (const auto& s : bins[b]) {
            const double m = cd.U * s.x1 + cd.V * s.x2;
            const double ph = std::remainder(s.p - m * static_cast<double>(kPi) / s.nyq, 2.0 * kPi);
            if (std::fabs(ph) > 0.5 * kPi) continue;
            ++sec[s.sector];
            a11 += s.x1 * s.x1; a12 += s.x1 * s.x2; a22 += s.x2 * s.x2;
        }
        uint16_t mask = 0;
        for (int s = 0; s < 12; ++s) if (sec[s] >= VAD_SECTOR_MIN) mask |= static_cast<uint16_t>(1u << s);
        if (!mask) continue;
        const double tr = a11 + a22;
        const double dd = std::sqrt(std::max(0.0, (a11 - a22) * (a11 - a22) + 4.0 * a12 * a12));
        prof.u[b] = cd.U; prof.v[b] = cd.V;
        prof.cond[b] = tr > 0 ? static_cast<float>((0.5 * (tr - dd)) / (0.5 * (tr + dd))) : 0.0f;
        prof.sectors[b] = mask;
        prof.ok[b] = 1;
        prof.any = true;
    }
    return prof;
}

// Shift each whole component by the integer fold that best matches the VAD
// reference. Whole-component shifts only: relative structure inside a
// component (including shear) is never altered here.
int anchor_components(AllTilt& volume, std::vector<TiltCache>& cache, const VadProfile& vad) {
    int changed = 0;
    for (size_t ti = 0; ti < volume.Tilts.size(); ++ti) {
        TiltCache& c = cache[ti];
        if (!c.valid || c.regions.empty() || c.ncomp <= 0) continue;
        std::vector<float>& vel = volume.Tilts[ti].Radials_VEL;

        // Anchor each component independently (no hard-gate grouping).
        // The median-residual guard below rejects shifts on real shear lines.
        std::vector<std::vector<int>> comp_regions(static_cast<size_t>(c.ncomp));
        std::vector<size_t> comp_size(static_cast<size_t>(c.ncomp), 0);
        size_t tilt_total = 0;
        for (size_t r = 0; r < c.regions.size(); ++r) {
            const int cp = c.regions[r].comp;
            if (cp < 0) continue;
            comp_regions[cp].push_back(static_cast<int>(r));
            comp_size[cp] += c.regions[r].gates.size();
            tilt_total += c.regions[r].gates.size();
        }

        for (int cp = 0; cp < c.ncomp; ++cp) {
            const size_t sz = comp_size[cp];
            if (sz == 0) continue;
            const size_t stride = std::max<size_t>(1, sz / ANCHOR_MAX_SAMPLES);
            std::array<double, 2 * ANCHOR_MAX_FOLD + 1> score{};
            int nsamp = 0;
            size_t counter = 0;
            for (const int r : comp_regions[cp]) {
                for (const int g : c.regions[r].gates) {
                    if ((counter++ % stride) != 0) continue;
                    const float val = vel[3 * g + 2];
                    if (!std::isfinite(val)) continue;
                    const double h = beam_height_m(vel[3 * g + 1], c.sin_e);
                    const float ref = vad.ref(h, vel[3 * g], c.cos_e);
                    if (!std::isfinite(ref)) continue;
                    const double d = ref - val;
                    const double vn = c.nyq[g];
                    // 1.0 Vn kernel (same as v10_1): wider than v10_2's 0.5 Vn,
                    // avoids spurious narrow peaks from noise at high tilts.
                    // The median residual check below guards against false shifts.
                    for (int k = -ANCHOR_MAX_FOLD; k <= ANCHOR_MAX_FOLD; ++k) {
                        const double e = std::fabs(d - 2.0 * k * vn) / vn;
                        if (e < 1.0) score[k + ANCHOR_MAX_FOLD] += 1.0 - e;
                    }
                    ++nsamp;
                }
            }
            const bool big = sz >= tilt_total / 5;
            const int min_n = big ? 40 : 12;
            if (nsamp < min_n) continue;

            int best = ANCHOR_MAX_FOLD;
            for (int i = 0; i < 2 * ANCHOR_MAX_FOLD + 1; ++i)
                if (score[i] > score[best]) best = i;
            double second = 0.0;
            for (int i = 0; i < 2 * ANCHOR_MAX_FOLD + 1; ++i)
                if (i != best) second = std::max(second, score[i]);
            const int k = best - ANCHOR_MAX_FOLD;
            if (k == 0) continue;

            const double frac = score[best] / nsamp;
            const double margin = (score[best] - second) / nsamp;
            const double need_frac = big ? 0.55 : 0.45;
            const double need_margin = big ? 0.25 : 0.15;
            if (frac < need_frac || margin < need_margin) continue;

            for (const int r : comp_regions[cp]) {
                Region& rg = c.regions[r];
                const float delta = k * 2.0f * rg.nyquist;
                for (const int g : rg.gates) vel[3 * g + 2] += delta;
                rg.fold += k;
            }
            ++changed;
        }
    }
    return changed;
}

// ---------------------------------------------------------------------------
// Stage W: fold-jump targeted region repair.
//
// After VAD anchoring, some regions may be at wrong fold levels, producing
// fold-level jumps (|vel[g] - vel[nb]| > FJUMP_FRAC * 2*Vn) at their
// boundaries.  For each such region we test integer fold shifts and apply
// the one that best eliminates the fold-jump pairs, subject to:
//
//   - At least FJUMP_MIN_JUMP_PAIRS fold-jump boundary pairs must exist
//     before a correction is attempted.
//   - The shift must eliminate >= FJUMP_ELIM_FRAC of those fold-jump pairs.
//   - The shift must also not create new fold jumps with non-jump neighbours
//     (net fold-jump cost must decrease by >= FJUMP_NET_IMPROVE).
//   - We only shift by up to FJUMP_MAX_FOLDS.
//
// Processing order: smallest regions first (they have fewer false positives
// from genuine shear, and large stable regions act as anchors).
// Only regions smaller than FJUMP_MAX_REGION_FRAC of the tilt total are
// eligible, preventing "fixes" of large regions with genuine wind shear.
// Iterates until convergence (max FJUMP_MAX_PASSES times).
// ---------------------------------------------------------------------------

constexpr int   FJUMP_MAX_PASSES         = 5;
constexpr float FJUMP_FRAC               = 0.85f;  // |Δv| >= this * 2Vn → fold jump
constexpr int   FJUMP_MIN_JUMPS          = 5;       // min fold-jump pairs to trigger
constexpr float FJUMP_ELIM_FRAC          = 0.80f;  // must eliminate >= 80% of jumps
constexpr float FJUMP_NET_IMPROVE        = 0.30f;  // net cost must drop >= 30%
constexpr int   FJUMP_MAX_FOLDS          = 2;
constexpr float FJUMP_MAX_REGION_FRAC    = 0.04f;  // only fix regions < 4% of tilt
// After the shift, residual fold-jump pairs must be <= this fraction of all
// boundary pairs.  Prevents corrections that look good only because the
// "correct fold" side of the boundary is all hard gates (excluded from cost).
constexpr float FJUMP_MAX_RESIDUAL_FRAC  = 0.05f;

void boundary_coherence_pass(AllTilt& volume, const std::vector<int>& order,
                              std::vector<TiltCache>& cache) {
    for (int idx : order) {
        TiltCache& c = cache[idx];
        if (!c.valid || c.regions.empty() || c.region_of_gate.empty()) continue;
        std::vector<float>& vel = volume.Tilts[idx].Radials_VEL;

        // Compute tilt total to derive max eligible region size.
        size_t tilt_total = 0;
        for (const Region& rg : c.regions) tilt_total += rg.gates.size();
        const size_t max_region_size = static_cast<size_t>(FJUMP_MAX_REGION_FRAC * tilt_total + 0.5f);

        // Sort regions smallest-first; break early once size > max_region_size.
        std::vector<size_t> ridx(c.regions.size());
        std::iota(ridx.begin(), ridx.end(), 0);
        std::sort(ridx.begin(), ridx.end(),
                  [&](size_t a, size_t b) { return c.regions[a].gates.size() < c.regions[b].gates.size(); });

        for (int pass = 0; pass < FJUMP_MAX_PASSES; ++pass) {
            bool any_changed = false;

            for (size_t ri : ridx) {
                Region& rg = c.regions[ri];
                if (rg.gates.size() > max_region_size) break;  // sorted; no more eligible
                if (rg.boundary.empty()) continue;
                const float interval = 2.0f * rg.nyquist;
                const float jump_thr = FJUMP_FRAC * interval;

                // Count fold-jump pairs with soft-gate neighbors (rg.boundary)
                // AND with hard-gate neighbors (via c.graph).  Hard-gate pairs
                // are not in rg.boundary but can create visible fold jumps after
                // a shift — including them prevents wrong corrections where the
                // "correct-fold" side is all hard gates.
                double cost_cur = 0.0;
                int jumps_cur = 0, total_pairs = 0;

                for (const Boundary& bnd : rg.boundary) {
                    const int nb = bnd.neighbor;
                    if (nb < 0 || nb >= static_cast<int>(c.region_of_gate.size())) continue;
                    if (c.region_of_gate[nb] < 0) continue;  // hard gate – handled separately
                    const float vg = vel[3 * bnd.gate + 2];
                    const float vn = vel[3 * nb       + 2];
                    if (!std::isfinite(vg) || !std::isfinite(vn)) continue;
                    const float diff = std::fabs(vg - vn);
                    cost_cur += diff;
                    if (diff >= jump_thr) ++jumps_cur;
                    ++total_pairs;
                }
                // Hard-gate boundary contribution.
                for (const int g : rg.gates) {
                    for (const int nb : c.graph[g]) {
                        if (nb < 0) continue;
                        if (!c.hard[nb]) continue;
                        const float vg = vel[3 * g  + 2];
                        const float vn = vel[3 * nb + 2];
                        if (!std::isfinite(vg) || !std::isfinite(vn)) continue;
                        const float diff = std::fabs(vg - vn);
                        cost_cur += diff;
                        if (diff >= jump_thr) ++jumps_cur;
                        ++total_pairs;
                    }
                }

                if (jumps_cur < FJUMP_MIN_JUMPS || total_pairs == 0) continue;

                // Test shifts ±1 .. ±FJUMP_MAX_FOLDS.
                int best_k = 0;
                double best_cost = cost_cur;
                int   best_jumps = jumps_cur;

                for (int k = -FJUMP_MAX_FOLDS; k <= FJUMP_MAX_FOLDS; ++k) {
                    if (k == 0) continue;
                    const float delta = static_cast<float>(k) * interval;
                    double cost_k = 0.0;
                    int jumps_k = 0;
                    for (const Boundary& bnd : rg.boundary) {
                        const int nb = bnd.neighbor;
                        if (nb < 0 || nb >= static_cast<int>(c.region_of_gate.size())) continue;
                        if (c.region_of_gate[nb] < 0) continue;
                        const float vg = vel[3 * bnd.gate + 2] + delta;
                        const float vn = vel[3 * nb       + 2];
                        if (!std::isfinite(vg) || !std::isfinite(vn)) continue;
                        const float diff = std::fabs(vg - vn);
                        cost_k += diff;
                        if (diff >= jump_thr) ++jumps_k;
                    }
                    // Hard-gate contribution for shift k.
                    for (const int g : rg.gates) {
                        for (const int nb : c.graph[g]) {
                            if (nb < 0) continue;
                            if (!c.hard[nb]) continue;
                            const float vg = vel[3 * g  + 2] + delta;
                            const float vn = vel[3 * nb + 2];
                            if (!std::isfinite(vg) || !std::isfinite(vn)) continue;
                            const float diff = std::fabs(vg - vn);
                            cost_k += diff;
                            if (diff >= jump_thr) ++jumps_k;
                        }
                    }
                    // Must eliminate enough fold-jump pairs AND reduce net cost.
                    const int jumps_elim = jumps_cur - jumps_k;
                    if (jumps_elim < static_cast<int>(FJUMP_ELIM_FRAC * jumps_cur + 0.5f)) continue;
                    if (cost_k >= (1.0 - FJUMP_NET_IMPROVE) * cost_cur) continue;
                    if (total_pairs > 0 && jumps_k > static_cast<int>(FJUMP_MAX_RESIDUAL_FRAC * total_pairs + 0.5f)) continue;
                    if (cost_k < best_cost) {
                        best_cost  = cost_k;
                        best_jumps = jumps_k;
                        best_k     = k;
                    }
                }

                if (best_k == 0) continue;

                const float delta = static_cast<float>(best_k) * interval;
                for (const int g : rg.gates) vel[3 * g + 2] += delta;
                rg.fold += best_k;
                any_changed = true;
            }

            if (!any_changed) break;
        }
    }
}

// ---------------------------------------------------------------------------
// Cross-tilt lookup.
// ---------------------------------------------------------------------------

bool vertical_lookup(const SingleTilt& ref_tilt, const TiltCache& ref_cache,
                     float cur_cos, float ref_cos, float az, float cur_range_m, float& out_value) {
    if (!ref_cache.valid || ref_cache.vel_rays.empty()) return false;
    if (!std::isfinite(cur_cos) || !std::isfinite(ref_cos) || std::fabs(ref_cos) < VERT_MIN_COS) return false;
    const int ri = nearest_ray(ref_cache.vel_rays, az);
    if (ri < 0) return false;
    const MomentRay& ray = ref_cache.vel_rays[static_cast<size_t>(ri)];
    if (az_distance(ray.azimuth, wrap360(az)) > MOMENT_AZ_TOL_DEG) return false;
    const float desired = cur_range_m * cur_cos / ref_cos;
    const std::vector<float>& packed = ref_tilt.Radials_VEL;
    const size_t lo = ray.start, hi = ray.start + ray.count;
    size_t l = lo, h = hi;
    while (l < h) {
        const size_t m = (l + h) / 2;
        if (packed[3 * m + 1] < desired) l = m + 1; else h = m;
    }
    size_t best = (l >= hi) ? hi - 1 : l;
    if (best > lo) {
        const float d0 = std::fabs(packed[3 * best + 1] - desired);
        const float d1 = std::fabs(packed[3 * (best - 1) + 1] - desired);
        if (d1 < d0) --best;
    }
    if (std::fabs(packed[3 * best + 1] - desired) > MOMENT_RANGE_TOL_M) return false;
    if (best < ref_cache.hard.size() && ref_cache.hard[best]) return false;
    if (!std::isfinite(packed[3 * best + 2])) return false;
    out_value = packed[3 * best + 2];
    return true;
}

// ---------------------------------------------------------------------------
// Stage B: region-level vertical reconciliation (v10).
// ---------------------------------------------------------------------------

void reconcile_vertical(AllTilt& volume, const std::vector<int>& order, std::vector<TiltCache>& cache) {
    for (size_t oi = 0; oi < order.size(); ++oi) {
        const int idx = order[oi];
        SingleTilt& cur = volume.Tilts[idx];
        TiltCache& ci = cache[idx];
        if (!ci.valid || ci.regions.empty()) continue;
        const int below = oi > 0 ? order[oi - 1] : -1;
        const int above = (oi + 1 < order.size()) ? order[oi + 1] : -1;
        const float cur_cos = ci.cos_e;
        const float below_cos = below >= 0 ? cache[below].cos_e : 1.0f;
        const float above_cos = above >= 0 ? cache[above].cos_e : 1.0f;

        for (Region& region : ci.regions) {
            if (region.gates.size() < static_cast<size_t>(VERT_MIN_REGION_GATES)) continue;
            std::array<float, 2 * MAX_FOLDS + 1> votes{}, below_votes{}, above_votes{};
            std::array<double, 2 * MAX_FOLDS + 1> shift_cost{};
            int below_support = 0, above_support = 0;
            double old_cost = 0.0;
            const size_t stride = std::max(VERT_SAMPLE_STRIDE_MIN, region.gates.size() / VERT_MAX_SAMPLES + 1);
            const float P = 2.0f * region.nyquist;

            auto accumulate = [&](int other, float other_cos, float az, float rng, float current,
                                  std::array<float, 2 * MAX_FOLDS + 1>& side_votes, int& side_support) {
                float ref = 0.0f;
                if (!vertical_lookup(volume.Tilts[other], cache[other], cur_cos, other_cos, az, rng, ref)) return;
                const int k = static_cast<int>(std::lround((ref - current) / P));
                if (std::abs(k) <= MAX_FOLDS) {
                    votes[k + MAX_FOLDS] += 1.0f;
                    side_votes[k + MAX_FOLDS] += 1.0f;
                    ++side_support;
                }
                old_cost += std::fabs(current - ref);
                for (int kk = -MAX_FOLDS; kk <= MAX_FOLDS; ++kk)
                    shift_cost[kk + MAX_FOLDS] += std::fabs(current + kk * P - ref);
            };

            for (size_t p = 0; p < region.gates.size(); p += stride) {
                const size_t g = static_cast<size_t>(region.gates[p]);
                const float az = cur.Radials_VEL[3 * g];
                const float rng = cur.Radials_VEL[3 * g + 1];
                const float current = cur.Radials_VEL[3 * g + 2];
                if (!std::isfinite(current)) continue;
                if (below >= 0) accumulate(below, below_cos, az, rng, current, below_votes, below_support);
                if (above >= 0) accumulate(above, above_cos, az, rng, current, above_votes, above_support);
            }

            const int total = below_support + above_support;
            if (total < VERT_MIN_SUPPORT || old_cost <= 0.0) continue;
            int best = 0; float best_votes = 0.0f;
            for (int k = -MAX_FOLDS; k <= MAX_FOLDS; ++k)
                if (votes[k + MAX_FOLDS] > best_votes) { best_votes = votes[k + MAX_FOLDS]; best = k; }
            if (best == 0 || best_votes / static_cast<float>(total) < VERT_MIN_FRAC) continue;
            if (below >= 0 && above >= 0 && below_support >= 3 && above_support >= 3) {
                int bb = 0, ab = 0; float bv = 0.0f, av = 0.0f;
                for (int k = -MAX_FOLDS; k <= MAX_FOLDS; ++k) {
                    if (below_votes[k + MAX_FOLDS] > bv) { bv = below_votes[k + MAX_FOLDS]; bb = k; }
                    if (above_votes[k + MAX_FOLDS] > av) { av = above_votes[k + MAX_FOLDS]; ab = k; }
                }
                if (bb != best || ab != best ||
                    bv / static_cast<float>(below_support) < VERT_SIDE_FRAC ||
                    av / static_cast<float>(above_support) < VERT_SIDE_FRAC)
                    continue;
            }
            if (shift_cost[best + MAX_FOLDS] / old_cost > VERT_MAX_RESIDUAL_RATIO) continue;
            const float delta = static_cast<float>(best) * P;
            for (const int g : region.gates) cur.Radials_VEL[3 * g + 2] += delta;
            region.fold += best;
        }
    }
}

// ---------------------------------------------------------------------------
// Stage X: alias-robust Rankine vortex fit + reference check.
//
// Model at gate i (x east, y north, relative to vortex center):
//   m_i = cos_e * f(rho) * (Vt * (t_hat . b_hat) + Vr * (r_hat . b_hat)) + c
// with Rankine profile f = rho/R inside R and R/rho outside, t_hat the
// counter-clockwise tangential unit vector, r_hat the outward radial unit
// vector, b_hat the beam direction. Cost is alias robust: the score is the
// resultant length |sum_i exp(i*pi*(v_i - m_i)/Vn)| / N, which (a) does not
// care which fold v_i is on and (b) absorbs the unknown constant background c
// in closed form. The Vt dimension is scanned by complex rotation, so each
// (center, R, Vr) combination costs one sincos per sample, not one per Vt.
// ---------------------------------------------------------------------------

struct VortexSample { double x, y, bx, by, v; bool hard; int gate; };

struct VortexModel {
    double xc = 0, yc = 0, R = 500, Vt = 0, Vr = 0, score = 0;
};

inline void vortex_basis(const VortexSample& s, double xc, double yc, double R, double cos_e,
                         double& g, double& h) {
    const double dx = s.x - xc, dy = s.y - yc;
    const double rho = std::sqrt(dx * dx + dy * dy);
    if (rho < 1.0) { g = 0.0; h = 0.0; return; }
    const double f = rho < R ? rho / R : R / rho;
    g = cos_e * f * (-dy * s.bx + dx * s.by) / rho;
    h = cos_e * f * (dx * s.bx + dy * s.by) / rho;
}

class VortexFitter {
public:
    VortexFitter(const std::vector<VortexSample>& s, double vn, double cos_e)
        : S(s), k(kPi / vn), ce(cos_e), g(s.size()), h(s.size()),
          zr(s.size()), zi(s.size()), rr(s.size()), ri(s.size()) {}

    // Scan Vt = vt0 + j*dvt, j = 0..nvt-1; returns best score and Vt.
    void scan(double xc, double yc, double R, double Vr, double vt0, double dvt, int nvt,
              double& best_score, double& best_vt) {
        const size_t N = S.size();
        for (size_t i = 0; i < N; ++i) {
            vortex_basis(S[i], xc, yc, R, ce, g[i], h[i]);
            const double psi = k * (S[i].v - Vr * h[i] - vt0 * g[i]);
            zr[i] = std::cos(psi); zi[i] = std::sin(psi);
            const double st = -k * dvt * g[i];
            rr[i] = std::cos(st); ri[i] = std::sin(st);
        }
        best_score = -1.0; best_vt = vt0;
        for (int j = 0; j < nvt; ++j) {
            double sr = 0.0, si = 0.0;
            for (size_t i = 0; i < N; ++i) {
                sr += zr[i]; si += zi[i];
                const double nr = zr[i] * rr[i] - zi[i] * ri[i];
                const double ni = zr[i] * ri[i] + zi[i] * rr[i];
                zr[i] = nr; zi[i] = ni;
            }
            const double sc = std::sqrt(sr * sr + si * si) / static_cast<double>(N);
            if (sc > best_score) { best_score = sc; best_vt = vt0 + j * dvt; }
        }
    }

    // Model without constant term, and resultant phase for the constant.
    double model(const VortexSample& s, const VortexModel& m) const {
        double gg, hh;
        vortex_basis(s, m.xc, m.yc, m.R, ce, gg, hh);
        return m.Vt * gg + m.Vr * hh;
    }

    double baseline() const {
        double sr = 0.0, si = 0.0;
        for (const auto& s : S) { sr += std::cos(k * s.v); si += std::sin(k * s.v); }
        return std::sqrt(sr * sr + si * si) / static_cast<double>(S.size());
    }

private:
    const std::vector<VortexSample>& S;
    double k, ce;
    std::vector<double> g, h, zr, zi, rr, ri;
};

int vortex_reference_check(AllTilt& volume, std::vector<TiltCache>& cache) {
    int total_changed = 0;
    int fits_done = 0;
    for (size_t ti = 0; ti < volume.Tilts.size(); ++ti) {
        TiltCache& c = cache[ti];
        if (!c.valid || c.hard.empty()) continue;
        std::vector<float>& vel = volume.Tilts[ti].Radials_VEL;
        const size_t n = c.hard.size();

        auto gate_xy = [&](size_t gi, double& x, double& y) {
            const double r = vel[3 * gi + 1] * c.cos_e;
            const double a = deg2rad(vel[3 * gi]);
            x = r * std::sin(a);
            y = r * std::cos(a);
        };

        // Hard-gate clusters.
        std::vector<int> cl(n, -1);
        std::vector<std::vector<int>> clusters;
        for (size_t s = 0; s < n; ++s) {
            if (!c.hard[s] || cl[s] >= 0 || !std::isfinite(vel[3 * s + 2])) continue;
            const int id = static_cast<int>(clusters.size());
            clusters.emplace_back();
            auto& L = clusters.back();
            L.push_back(static_cast<int>(s));
            cl[s] = id;
            for (size_t p = 0; p < L.size(); ++p) {
                for (int nb : c.graph[L[p]]) {
                    if (nb < 0 || cl[nb] >= 0 || !c.hard[nb]) continue;
                    cl[nb] = id;
                    L.push_back(nb);
                }
            }
        }
        struct Center { double x, y; size_t size; std::vector<int> seeds; };
        std::vector<Center> centers;
        std::vector<int> cord(clusters.size());
        std::iota(cord.begin(), cord.end(), 0);
        std::sort(cord.begin(), cord.end(), [&](int a, int b) { return clusters[a].size() > clusters[b].size(); });
        for (int ci : cord) {
            const auto& L = clusters[ci];
            if (static_cast<int>(L.size()) < VORTEX_MIN_CLUSTER) break;
            double sx = 0, sy = 0;
            for (int gi : L) { double x, y; gate_xy(gi, x, y); sx += x; sy += y; }
            sx /= L.size(); sy /= L.size();
            bool merged = false;
            for (auto& ce : centers) {
                if (std::hypot(ce.x - sx, ce.y - sy) < VORTEX_MERGE_M) {
                    const double w0 = static_cast<double>(ce.size), w1 = static_cast<double>(L.size());
                    ce.x = (ce.x * w0 + sx * w1) / (w0 + w1);
                    ce.y = (ce.y * w0 + sy * w1) / (w0 + w1);
                    ce.size += L.size();
                    ce.seeds.insert(ce.seeds.end(), L.begin(), L.end());
                    merged = true;
                    break;
                }
            }
            if (!merged) {
                if (static_cast<int>(centers.size()) >= VORTEX_MAX_PER_TILT) continue;
                centers.push_back({sx, sy, L.size(), L});
            }
        }
        if (centers.empty()) continue;

        std::vector<int> stamp(n, -1);
        int stamp_id = 0;

        for (const Center& ctr : centers) {
            ++stamp_id;
            // Window gather by graph BFS from the cluster, bounded by distance.
            std::vector<int> win;
            for (int s : ctr.seeds) { if (stamp[s] != stamp_id) { stamp[s] = stamp_id; win.push_back(s); } }
            for (size_t p = 0; p < win.size() && win.size() < 20000; ++p) {
                for (int nb : c.graph[win[p]]) {
                    if (nb < 0 || stamp[nb] == stamp_id) continue;
                    double x, y; gate_xy(nb, x, y);
                    if (std::hypot(x - ctr.x, y - ctr.y) > VORTEX_WINDOW_M) continue;
                    stamp[nb] = stamp_id;
                    win.push_back(nb);
                }
            }
            if (win.size() < 30) continue;

            // Common Nyquist for the window (median); drop incompatible gates.
            std::vector<float> nys;
            nys.reserve(win.size());
            for (int gi : win) nys.push_back(c.nyq[gi]);
            std::nth_element(nys.begin(), nys.begin() + nys.size() / 2, nys.end());
            const double vn = nys[nys.size() / 2];
            if (!(vn > 0.0)) continue;

            std::vector<VortexSample> all;
            all.reserve(win.size());
            for (int gi : win) {
                const float v = vel[3 * gi + 2];
                if (!std::isfinite(v) || !compatible_nyq(c.nyq[gi], static_cast<float>(vn))) continue;
                double x, y; gate_xy(gi, x, y);
                const double a = deg2rad(vel[3 * gi]);
                all.push_back({x, y, std::sin(a), std::cos(a), v, c.hard[gi] != 0, gi});
            }
            if (all.size() < 30) continue;
            std::vector<VortexSample> fitS;
            const size_t stride = std::max<size_t>(1, all.size() / VORTEX_MAX_SAMPLES);
            for (size_t i = 0; i < all.size(); i += stride) fitS.push_back(all[i]);

            VortexFitter F(fitS, vn, c.cos_e);
            const double score0 = F.baseline();

            VortexModel best;
            best.score = -1.0;
            // Coarse search.
            const double Rs[] = {200.0, 400.0, 700.0, 1100.0};
            const double Vrs[] = {-20.0, -10.0, 0.0, 10.0};
            for (int ix = -3; ix <= 3; ++ix) {
                for (int iy = -3; iy <= 3; ++iy) {
                    const double xc = ctr.x + ix * 300.0, yc = ctr.y + iy * 300.0;
                    for (double R : Rs) {
                        for (double Vr : Vrs) {
                            double sc, vt;
                            F.scan(xc, yc, R, Vr, -90.0, 6.0, 31, sc, vt);
                            if (sc > best.score) best = {xc, yc, R, vt, Vr, sc};
                        }
                    }
                }
            }
            // Refinement around the best.
            const VortexModel coarse = best;
            for (int ix = -2; ix <= 2; ++ix) {
                for (int iy = -2; iy <= 2; ++iy) {
                    const double xc = coarse.xc + ix * 100.0, yc = coarse.yc + iy * 100.0;
                    for (double Rm : {0.75, 1.0, 1.33}) {
                        for (double dVr : {-5.0, 0.0, 5.0}) {
                            double sc, vt;
                            F.scan(xc, yc, coarse.R * Rm, coarse.Vr + dVr, coarse.Vt - 6.0, 2.0, 7, sc, vt);
                            if (sc > best.score) best = {xc, yc, coarse.R * Rm, vt, coarse.Vr + dVr, sc};
                        }
                    }
                }
            }

            if (best.score < VORTEX_MIN_SCORE) continue;
            if (best.score - score0 < VORTEX_MIN_GAIN) continue;
            if (std::fabs(best.Vt) < 0.6 * vn) continue;   // not strong enough to alias

            // Constant background: phase of the resultant gives c mod 2Vn;
            // pick the fold using the already-dealiased non-hard gates.
            const double kk = kPi / vn;
            double sr = 0.0, si = 0.0;
            std::vector<double> resid_nonhard;
            for (const auto& s : all) {
                const double m = F.model(s, best);
                const double psi = kk * (s.v - m);
                sr += std::cos(psi); si += std::sin(psi);
                if (!s.hard) resid_nonhard.push_back(s.v - m);
            }
            if (resid_nonhard.size() < 10) continue;
            const double c0 = std::atan2(si, sr) / kk;
            std::nth_element(resid_nonhard.begin(), resid_nonhard.begin() + resid_nonhard.size() / 2,
                             resid_nonhard.end());
            const double med = resid_nonhard[resid_nonhard.size() / 2];
            const double cst = c0 + 2.0 * vn * std::round((med - c0) / (2.0 * vn));

            // Reference check near the core.
            const double core_r = std::clamp(VORTEX_CORE_RADIUS_MULT * best.R,
                                             VORTEX_CORE_RADIUS_MIN, VORTEX_CORE_RADIUS_MAX);
            for (const auto& s : all) {
                const double dx = s.x - best.xc, dy = s.y - best.yc;
                if (dx * dx + dy * dy > core_r * core_r) continue;
                const double ref = cst + F.model(s, best);
                const int k = static_cast<int>(std::lround((ref - s.v) / (2.0 * vn)));
                const double corrected = s.v + 2.0 * vn * k;
                const double res_new = std::fabs(corrected - ref);
                if (res_new > VORTEX_APPLY_RES_FRAC * vn) continue;
                if (k != 0) {
                    if (std::fabs(s.v - ref) < VORTEX_WRONG_RES_FRAC * vn) continue;
                    if (std::abs(k) > MAX_FOLDS) continue;
                    vel[3 * s.gate + 2] = static_cast<float>(corrected);
                    ++total_changed;
                }
                // Confident against the model: becomes an anchor for stage C.
                c.hard[s.gate] = 0;
            }
        }
    }
    return total_changed;
}

// ---------------------------------------------------------------------------
// Stage C: bounded continuity rescue for remaining hard gates.
// ---------------------------------------------------------------------------

void hard_gate_rescue(AllTilt& volume, const std::vector<int>& order, std::vector<TiltCache>& cache) {
    for (size_t oi = 0; oi < order.size(); ++oi) {
        const int idx = order[oi];
        SingleTilt& cur = volume.Tilts[idx];
        TiltCache& ci = cache[idx];
        if (!ci.valid || ci.hard.empty()) continue;
        size_t hard_count = 0;
        for (unsigned char x : ci.hard) hard_count += x != 0;
        if (hard_count == 0) continue;
        const size_t total_valid = ci.nyq.size();
        const size_t budget = std::min(RESCUE_MAX_GATES_ABS,
            std::max<size_t>(1, static_cast<size_t>(RESCUE_MAX_FRACTION * static_cast<double>(std::max<size_t>(1, total_valid)))));
        if (hard_count > budget * 2) continue;

        const int below = oi > 0 ? order[oi - 1] : -1;
        const int above = (oi + 1 < order.size()) ? order[oi + 1] : -1;
        const float cur_cos = ci.cos_e;
        const float below_cos = below >= 0 ? cache[below].cos_e : 1.0f;
        const float above_cos = above >= 0 ? cache[above].cos_e : 1.0f;

        std::vector<unsigned char> solved(ci.hard.size());
        for (size_t g = 0; g < solved.size(); ++g) solved[g] = ci.hard[g] ? 0 : 1;

        size_t rescued = 0;
        for (int pass = 0; pass < 2 && rescued < budget; ++pass) {
            for (size_t g = 0; g < ci.hard.size() && rescued < budget; ++g) {
                if (!ci.hard[g] || solved[g] || !valid_nyq(ci.nyq[g])) continue;
                const float raw = cur.Radials_VEL[3 * g + 2];
                if (!std::isfinite(raw)) continue;
                const float az = cur.Radials_VEL[3 * g];
                const float rng = cur.Radials_VEL[3 * g + 1];
                const float nyq_g = ci.nyq[g];
                const float P = 2.0f * nyq_g;
                const float scale = std::max(1.5f, RESCUE_EDGE_SCALE_FRAC * nyq_g);

                float vert_target = 0.0f;
                int vert_hits = 0;
                float tmp = 0.0f;
                if (below >= 0 && vertical_lookup(volume.Tilts[below], cache[below], cur_cos, below_cos, az, rng, tmp)) { vert_target += tmp; ++vert_hits; }
                if (above >= 0 && vertical_lookup(volume.Tilts[above], cache[above], cur_cos, above_cos, az, rng, tmp)) { vert_target += tmp; ++vert_hits; }
                vert_target = vert_hits > 0 ? vert_target / vert_hits : VEL_NAN;

                auto total_cost = [&](float candidate, int& support, unsigned& dirs) {
                    double cost = 0.0;
                    support = 0; dirs = 0u;
                    int nref = 0;
                    for (int d = 0; d < 4; ++d) {
                        const int j = ci.graph[g][d];
                        if (j < 0 || !solved[j]) continue;
                        const float jv = cur.Radials_VEL[3 * j + 2];
                        if (!std::isfinite(jv)) continue;
                        ++nref;
                        const float en = std::min(nyq_g, ci.nyq[j]);
                        const float diff = std::fabs(candidate - jv);
                        cost += ci.sw_conf[g] * ci.sw_conf[j] * huber_cost(diff, scale);
                        if (diff <= 0.55f * en) { ++support; dirs |= 1u << static_cast<unsigned>(d); }
                    }
                    if (nref > 0) cost /= nref;
                    if (std::isfinite(vert_target))
                        cost += RESCUE_VERT_WEIGHT * huber_cost(candidate - vert_target, std::max(2.0f, 0.4f * nyq_g));
                    return cost;
                };

                int cs = 0; unsigned cd = 0u;
                const double old_cost = total_cost(raw, cs, cd);
                double best_cost = old_cost;
                int best_fold = 0;
                for (int k = -MAX_FOLDS; k <= MAX_FOLDS; ++k) {
                    if (k == 0) continue;
                    const float candidate = raw + static_cast<float>(k) * P;
                    int support = 0; unsigned dirs = 0u;
                    const double cost = total_cost(candidate, support, dirs);
                    const int axes = ((dirs & 3u) ? 1 : 0) + ((dirs & 12u) ? 1 : 0);
                    const bool vert_agrees = std::isfinite(vert_target) && std::fabs(candidate - vert_target) <= 0.5f * nyq_g;
                    const bool ok = (support >= RESCUE_MIN_SUPPORT && axes >= 2) || (support >= 1 && vert_agrees);
                    if (!ok) continue;
                    if (cost < best_cost) { best_cost = cost; best_fold = k; }
                }
                if (best_fold == 0) continue;
                if (old_cost <= 0.0 || (old_cost - best_cost) / old_cost < RESCUE_MIN_IMPROVEMENT) continue;
                cur.Radials_VEL[3 * g + 2] = raw + static_cast<float>(best_fold) * P;
                solved[g] = 1;
                ++rescued;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Stage F: BFS fold-fix from the largest component per tilt.
//
// After Stage V, the largest connected component should be at the correct
// absolute fold (VAD anchored it).  Smaller components that weren't reached
// by the VAD (isolated by hard gates or missing data) may be at wrong folds.
//
// Algorithm:
//   1. Seed the BFS with all regions in the largest component (by gate count).
//   2. For each unvisited neighbouring region N adjacent to any visited region:
//      a. Collect all boundary pairs between N and the visited set.
//      b. Find the fold shift k ∈ [-STAGEF_MAX_FOLD..+STAGEF_MAX_FOLD] that
//         minimises fold-jump pairs with visited neighbours.
//      c. Accept the shift if:
//           - There are at least STAGEF_MIN_PAIRS visited-boundary pairs.
//           - The best k reduces fold-jump pairs to <= STAGEF_RESID_FRAC of
//             the current fold-jump count (requiring near-complete resolution).
//           - Total boundary cost decreases by >= STAGEF_MIN_NET_IMPROVE.
//   3. Mark N visited and enqueue it (with or without a fold shift).
//
// Processing order: BFS guarantees that each region is corrected relative to
// an ever-growing set of confirmed (already-visited) regions.  This prevents
// the "large wrong region dominates small correct region" failure mode of
// Stage W: here the SEED (largest component) acts as the single ground truth
// and corrections propagate outward from it.
// ---------------------------------------------------------------------------

constexpr int   STAGEF_MAX_FOLD          = 3;
constexpr float STAGEF_JUMP_FRAC         = 0.85f;   // fold-jump detection threshold
constexpr int   STAGEF_MIN_PAIRS         = 3;        // min visited-boundary pairs to correct
constexpr float STAGEF_RESID_FRAC        = 0.15f;   // max residual fold-jump fraction
constexpr float STAGEF_MIN_NET_IMPROVE   = 0.35f;   // min net cost reduction

void flood_fill_from_main_component(AllTilt& volume, std::vector<TiltCache>& cache) {
    for (size_t idx = 0; idx < volume.Tilts.size(); idx++) {
        TiltCache& c = cache[idx];
        if (!c.valid || c.regions.empty()) continue;
        auto& vel = volume.Tilts[idx].Radials_VEL;
        const int n = static_cast<int>(c.regions.size());

        // Find the largest component by total gate count.
        std::vector<int> comp_gates(static_cast<size_t>(c.ncomp), 0);
        for (const Region& rg : c.regions)
            if (rg.comp >= 0 && rg.comp < c.ncomp)
                comp_gates[static_cast<size_t>(rg.comp)] += static_cast<int>(rg.gates.size());
        const int seed_comp = static_cast<int>(
            std::max_element(comp_gates.begin(), comp_gates.end()) - comp_gates.begin());

        // BFS initialised with all regions from the seed component.
        std::vector<bool> visited(static_cast<size_t>(n), false);
        std::queue<int>   bfsq;
        for (int ri = 0; ri < n; ri++) {
            if (c.regions[static_cast<size_t>(ri)].comp == seed_comp) {
                visited[static_cast<size_t>(ri)] = true;
                bfsq.push(ri);
            }
        }

        while (!bfsq.empty()) {
            const int ri = bfsq.front();
            bfsq.pop();
            const Region& rg = c.regions[static_cast<size_t>(ri)];

            // Collect unvisited neighbour regions from this region's boundary.
            static thread_local std::vector<int> candidates;
            candidates.clear();
            for (const Boundary& bnd : rg.boundary) {
                const int nb = bnd.neighbor;
                if (nb < 0 || nb >= static_cast<int>(c.region_of_gate.size())) continue;
                const int nb_ri = c.region_of_gate[static_cast<size_t>(nb)];
                if (nb_ri < 0 || visited[static_cast<size_t>(nb_ri)]) continue;
                candidates.push_back(nb_ri);
            }
            std::sort(candidates.begin(), candidates.end());
            candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

            for (const int nb_ri : candidates) {
                if (visited[static_cast<size_t>(nb_ri)]) continue;

                Region& nb_rg = c.regions[static_cast<size_t>(nb_ri)];
                const float nq       = nb_rg.nyquist;
                const float interval = 2.0f * nq;
                const float jump_thr = STAGEF_JUMP_FRAC * interval;

                // Count boundary pairs with the visited set.
                double cost0 = 0.0;
                int    jumps0 = 0, total_vis = 0;
                for (const Boundary& nb_bnd : nb_rg.boundary) {
                    const int nn = nb_bnd.neighbor;
                    if (nn < 0 || nn >= static_cast<int>(c.region_of_gate.size())) continue;
                    const int nn_ri = c.region_of_gate[static_cast<size_t>(nn)];
                    if (nn_ri < 0 || !visited[static_cast<size_t>(nn_ri)]) continue;
                    const float vg = vel[3 * static_cast<size_t>(nb_bnd.gate) + 2];
                    const float vn = vel[3 * static_cast<size_t>(nn)           + 2];
                    if (!std::isfinite(vg) || !std::isfinite(vn)) continue;
                    const float diff = std::fabs(vg - vn);
                    cost0 += diff;
                    if (diff >= jump_thr) ++jumps0;
                    ++total_vis;
                }

                int best_k = 0;
                if (total_vis >= STAGEF_MIN_PAIRS && jumps0 > 0) {
                    double best_cost = cost0;
                    for (int k = -STAGEF_MAX_FOLD; k <= STAGEF_MAX_FOLD; k++) {
                        if (k == 0) continue;
                        const float delta = static_cast<float>(k) * interval;
                        double cost_k = 0.0;
                        int    jumps_k = 0;
                        for (const Boundary& nb_bnd : nb_rg.boundary) {
                            const int nn = nb_bnd.neighbor;
                            if (nn < 0 || nn >= static_cast<int>(c.region_of_gate.size())) continue;
                            const int nn_ri = c.region_of_gate[static_cast<size_t>(nn)];
                            if (nn_ri < 0 || !visited[static_cast<size_t>(nn_ri)]) continue;
                            const float vg = vel[3 * static_cast<size_t>(nb_bnd.gate) + 2] + delta;
                            const float vn = vel[3 * static_cast<size_t>(nn)           + 2];
                            if (!std::isfinite(vg) || !std::isfinite(vn)) continue;
                            const float diff = std::fabs(vg - vn);
                            cost_k += diff;
                            if (diff >= jump_thr) ++jumps_k;
                        }
                        if (jumps_k > static_cast<int>(STAGEF_RESID_FRAC * jumps0 + 0.5f)) continue;
                        if (cost_k >= (1.0 - STAGEF_MIN_NET_IMPROVE) * cost0) continue;
                        if (cost_k < best_cost) {
                            best_cost = cost_k;
                            best_k    = k;
                        }
                    }
                }

                if (best_k != 0) {
                    const float delta = static_cast<float>(best_k) * interval;
                    for (const int g : nb_rg.gates)
                        vel[3 * static_cast<size_t>(g) + 2] += delta;
                    nb_rg.fold += best_k;
                }
                visited[static_cast<size_t>(nb_ri)] = true;
                bfsq.push(nb_ri);
            }
        }
    }
}

} // namespace

void dealias_velocity_volume_v10_3(AllTilt& volume) {
    const size_t nt = volume.Tilts.size();
    if (nt == 0) return;
    std::vector<int> order(nt);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return volume.Tilts[a].ElevationAngle < volume.Tilts[b].ElevationAngle;
    });

    std::vector<TiltCache> cache(nt);
    for (int idx : order) cache[idx] = dealias_tilt_stage_a(volume.Tilts[idx]);

    // Stage V: absolute anchoring.
    // AR-VAD (alias-robust, works on folded phases) first, then an LS-VAD
    // refinement on the now-anchored data.
    {
        const VadProfile ar = fit_ar_vad(volume, cache);
        if (ar.any) {
            anchor_components(volume, cache, ar);
            const VadProfile ls = fit_vad(volume, cache);
            if (ls.any) anchor_components(volume, cache, ls);
        }
    }

    reconcile_vertical(volume, order, cache);
    vortex_reference_check(volume, cache);
    hard_gate_rescue(volume, order, cache);
}