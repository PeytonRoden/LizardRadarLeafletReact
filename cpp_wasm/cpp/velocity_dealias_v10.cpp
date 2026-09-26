#include <string>
#include "velocity_dealias.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <queue>
#include <vector>

// ============================================================================
// v10: region-based dealiasing (v2's fast backbone) + reflectivity/spectrum-
// width gated hard-core (tornado/meso) handling + cheap O(regions) multi-tilt
// vertical reconciliation.
//
// Design goals, in priority order:
//   1. Never be slower than "one linear pass building the graph/regions, plus
//      a handful of additional linear or O(regions) passes." No per-gate
//      global convergence loop (that's what made v8 slow), no rebuilding the
//      ray/graph/region structures from scratch a second time (that's what
//      made v9 slow).
//   2. Never let a single noisy edge or gate flip a large coherent region.
//      Fold decisions for the "easy" 95%+ of the field are made per REGION
//      (flood-filled, boundary-vote), exactly like v2. This is the property
//      that made v2 stable, and it is preserved unchanged here.
//   3. Give the hard cases (tornado/meso, high local shear) a separate,
//      tightly bounded, budget-capped treatment instead of forcing the same
//      per-gate iterative solver over the whole field (that's what made v8
//      flip large regions -- an early wrong per-gate decision could poison
//      everything solved from it before any global check existed).
//   4. Use reflectivity (REF) to keep the "hard region" classifier from
//      being triggered by noise: a shear spike is only treated as a
//      protected tornadic/meso signature if there is real echo there.
//      Use spectrum width (SW) as a soft per-edge confidence weight, not a
//      hard filter, since it is noisier and less diagnostic than REF.
//   5. Use multi-tilt continuity as a REGION-level (not gate-level) sanity
//      check between resolved regions on adjacent tilts. This is cheap
//      (O(regions), not O(gates)), and because it only ever shifts a whole
//      already-internally-consistent region by a whole number of folds, it
//      cannot introduce the kind of fine-grained flip a per-gate solver can.
//
// Moment alignment caveat: REF/SW may be sampled at different gate spacing
// and (depending on VCP/ingest) a different range origin than VEL, and this
// header does not currently carry each moment's own first-gate offset -- see
// build_moment_index()/moment_lookup() below. Matching is nearest-neighbor
// in (azimuth, range) with a generous tolerance. If you have access to each
// moment's first_gate/gate_spacing at parse time, threading that through
// will tighten this meaningfully; recommend checking that before trusting
// REF-based gating quantitatively near steep range gradients.
// ============================================================================

namespace {

constexpr float VEL_NAN = std::numeric_limits<float>::quiet_NaN();
constexpr float AZ_EPS = 0.01f;
constexpr float SEG_FRAC = 0.20f;
constexpr float NYQ_REL_TOL = 0.03f;
constexpr int MAX_FOLDS = 6;

// Moment (REF/SW) to VEL-grid matching tolerances.
constexpr float MOMENT_AZ_TOL_DEG = 1.5f;
constexpr float MOMENT_RANGE_TOL_M = 750.0f;

// Hard-region (protected tornadic/meso core) detection.
constexpr float HARD_CIRC_FRAC = 0.50f;      // v8-style circular-distance/Nyquist fraction
constexpr float HARD_REFL_MIN_DBZ = 25.0f;   // require real echo to trust a hard signature
constexpr float HARD_DILATE_MAX_FRAC = 0.10f;

// Spectrum-width based edge-confidence taper (tune against your own radar's
// SW climatology -- these are reasonable starting points, not calibrated).
constexpr float SW_TAPER_LO = 2.0f;
constexpr float SW_TAPER_HI = 8.0f;
constexpr float SW_CONF_FLOOR = 0.15f;

// Bounded hard-gate rescue.
constexpr size_t RESCUE_MAX_GATES_ABS = 4096;
constexpr float RESCUE_MAX_FRACTION = 0.035f;
constexpr float RESCUE_MIN_IMPROVEMENT = 0.30f;
constexpr int RESCUE_MIN_SUPPORT = 2;
constexpr float RESCUE_EDGE_SCALE_FRAC = 0.38f;
constexpr float RESCUE_VERT_WEIGHT = 0.20f;

// Multi-tilt (vertical) region reconciliation.
constexpr float VERT_MIN_COS = 0.4f;
constexpr int VERT_MIN_REGION_GATES = 6;
constexpr size_t VERT_MAX_SAMPLES = 48;
constexpr size_t VERT_SAMPLE_STRIDE_MIN = 3;
constexpr int VERT_MIN_SUPPORT = 6;
constexpr float VERT_MIN_FRAC = 0.72f;
constexpr float VERT_SIDE_FRAC = 0.65f;
constexpr float VERT_MAX_RESIDUAL_RATIO = 0.45f;


inline bool solve_3x3(double m[3][4], double x[3]) {
    for (int col = 0; col < 3; ++col) {
        int pivot = col;
        for (int r = col + 1; r < 3; ++r)
            if (std::fabs(m[r][col]) > std::fabs(m[pivot][col])) pivot = r;
        if (std::fabs(m[pivot][col]) < 1.0e-10) return false;
        if (pivot != col) for (int k = col; k < 4; ++k) std::swap(m[pivot][k], m[col][k]);
        const double div = m[col][col];
        for (int k = col; k < 4; ++k) m[col][k] /= div;
        for (int r = 0; r < 3; ++r) {
            if (r == col) continue;
            const double f = m[r][col];
            if (std::fabs(f) < 1.0e-14) continue;
            for (int k = col; k < 4; ++k) m[r][k] -= f * m[col][k];
        }
    }
    x[0] = m[0][3]; x[1] = m[1][3]; x[2] = m[2][3];
    return true;
}

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

// Circular distance on the Doppler ambiguity interval: a seam near 2*Vn
// reads as near-zero here, so a normal alias jump does not look like real
// shear. (Same idea as v8's detect_protected_shear.)
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

// Spectrum width -> edge confidence in [SW_CONF_FLOOR, 1.0]. Missing SW data
// means "no reason to distrust this edge" -> full confidence.
inline float sw_confidence(float sw) {
    if (!std::isfinite(sw) || sw < 0.0f) return 1.0f;
    if (sw <= SW_TAPER_LO) return 1.0f;
    if (sw >= SW_TAPER_HI) return SW_CONF_FLOOR;
    const float t = (sw - SW_TAPER_LO) / (SW_TAPER_HI - SW_TAPER_LO);
    return 1.0f - t * (1.0f - SW_CONF_FLOOR);
}

// ---------------------------------------------------------------------------
// Generic (azimuth, range) nearest-neighbor index for matching a secondary
// moment (REF, SW) or a neighboring tilt's VEL array onto the current VEL
// grid. Built once per array, O(n log n); each lookup is O(log rays +
// log gates-in-ray).
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
        const bool new_ray =
            (t == 0) ||
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

int nearest_moment_ray(const MomentIndex& idx, float az) {
    if (idx.rays.empty()) return -1;
    az = wrap360(az);
    size_t lo = 0, hi = idx.rays.size();
    while (lo < hi) {
        const size_t m = (lo + hi) / 2;
        if (idx.rays[m].azimuth < az) lo = m + 1; else hi = m;
    }
    const size_t n = idx.rays.size();
    const size_t a = lo < n ? lo : 0;
    const size_t b = lo > 0 ? lo - 1 : n - 1;
    return az_distance(idx.rays[a].azimuth, az) < az_distance(idx.rays[b].azimuth, az)
               ? static_cast<int>(a) : static_cast<int>(b);
}

// Returns NaN if no gate found within tolerance.
float moment_lookup(const MomentIndex& idx, float az, float range_m,
                     float az_tol_deg, float range_tol_m) {
    if (!idx.packed || idx.rays.empty()) return VEL_NAN;
    const int ri = nearest_moment_ray(idx, az);
    if (ri < 0) return VEL_NAN;
    const MomentRay& ray = idx.rays[static_cast<size_t>(ri)];
    if (az_distance(ray.azimuth, wrap360(az)) > az_tol_deg) return VEL_NAN;

    const std::vector<float>& packed = *idx.packed;
    const size_t lo = ray.start, hi = ray.start + ray.count;
    size_t l = lo, h = hi;
    while (l < h) {
        const size_t m = (l + h) / 2;
        if (packed[3 * m + 1] < range_m) l = m + 1; else h = m;
    }
    size_t best = (l >= hi) ? hi - 1 : l;
    if (best > lo) {
        const float d0 = std::fabs(packed[3 * best + 1] - range_m);
        const float d1 = std::fabs(packed[3 * (best - 1) + 1] - range_m);
        if (d1 < d0) --best;
    }
    if (std::fabs(packed[3 * best + 1] - range_m) > range_tol_m) return VEL_NAN;
    return packed[3 * best + 2];
}

// ---------------------------------------------------------------------------
// Region-based solver state (same shape as v2).
// ---------------------------------------------------------------------------

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
    float evidence;
    int region;
    int fold;
    int version;

    bool operator<(const Candidate& o) const {
        if (evidence != o.evidence) return evidence < o.evidence;
        return region > o.region;
    }
};

// Per-tilt cache kept around after the 2D solve so stage B (vertical
// reconciliation) and stage C (hard-gate rescue) don't have to rebuild the
// ray/graph/region structures from scratch (that duplication is what made
// v9 expensive).
struct TiltCache {
    bool valid = false;
    float elevation_cos = 1.0f;
    std::vector<MomentRay> vel_rays;      // sorted by azimuth, for cross-tilt lookups
    std::vector<float> nyq;               // per gate
    std::vector<unsigned char> hard;      // per gate
    std::vector<std::array<int, 4>> graph; // per gate 4-neighbor graph
    std::vector<Region> regions;          // resolved normal regions (hard gates excluded)
};

// ---------------------------------------------------------------------------
// Stage A: single-tilt 2D solve. Mirrors v2's flood-fill + boundary-vote
// core, with two additions: (1) gates flagged "hard" (protected tornadic/
// meso core, confirmed by reflectivity) are excluded from normal region
// growth and left for the bounded rescue in stage C; (2) boundary votes are
// weighted by spectrum-width-derived edge confidence instead of counted
// uniformly.
// ---------------------------------------------------------------------------

TiltCache dealias_tilt_stage_a(SingleTilt& tilt) {
    TiltCache cache;

    std::vector<float>& packed = tilt.Radials_VEL;
    if (packed.size() < 24 || packed.size() % 3 != 0) return cache;

    const size_t count = packed.size() / 3;
    if (count > static_cast<size_t>(std::numeric_limits<int>::max() / 3)) return cache;

    const float fallback_nyq =
        static_cast<float>(tilt.vol_el_rad.rad.nyquist_vel) / 100.0f;

    const int n = static_cast<int>(count);
    std::vector<VelocityRay> rays = tilt.VelocityRays;

    // --- Ray construction / validation (identical logic to v2) -------------
    if (rays.empty()) {
        if (!valid_nyq(fallback_nyq)) return cache;
        rays.reserve(720);
        for (int t = 0; t < n; ++t) {
            if (t == 0 ||
                std::fabs(packed[3 * t] - packed[3 * (t - 1)]) > AZ_EPS ||
                packed[3 * t + 1] <= packed[3 * (t - 1) + 1]) {
                rays.push_back({static_cast<size_t>(t), 0, fallback_nyq, tilt.gateSpacing});
            }
            ++rays.back().count;
        }
        if (!tilt.VelNyquist.empty() && tilt.VelNyquist.size() != rays.size())
            return cache;
        for (size_t r = 0; r < rays.size(); ++r)
            if (!tilt.VelNyquist.empty() && valid_nyq(tilt.VelNyquist[r]))
                rays[r].nyquist = tilt.VelNyquist[r];
    }

    if (rays.size() < 4) return cache;

    std::vector<float> nyq(count, VEL_NAN);
    size_t expected = 0;

    for (const auto& ray : rays) {
        if (ray.start != expected || ray.count == 0 || ray.count > count - expected)
            return cache;
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
    const float max_az_gap = std::min(1.5f, 1.8f * typical_az);

    // --- Grid + 4-neighbor graph (identical to v2) --------------------------
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

        for (size_t t = ray.start + 1; t < end; ++t) {
            const float dr = packed[3 * t + 1] - packed[3 * (t - 1) + 1];
            if (std::fabs(dr - ray.gateSpacing) <= 0.1f)
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

    // --- Moment alignment: REF (hard-region confirmation) and SW (edge
    //     confidence). Built once, O(n log n); missing moments degrade
    //     gracefully (empty index -> lookups return NaN -> treated as
    //     "no information", never as "fail").
    // -------------------------------------------------------------------
    const MomentIndex ref_idx = build_moment_index(tilt.Radials_REF);
    const MomentIndex sw_idx = build_moment_index(tilt.Radials_SW);
    const bool have_ref = ref_idx.packed != nullptr && !ref_idx.rays.empty();

    std::vector<float> ref_dbz(count, VEL_NAN);
    std::vector<float> sw_conf(count, 1.0f);
    for (size_t i = 0; i < count; ++i) {
        const float az = packed[3 * i];
        const float rng = packed[3 * i + 1];
        if (have_ref)
            ref_dbz[i] = moment_lookup(ref_idx, az, rng, MOMENT_AZ_TOL_DEG, MOMENT_RANGE_TOL_M);
        const float sw = moment_lookup(sw_idx, az, rng, MOMENT_AZ_TOL_DEG, MOMENT_RANGE_TOL_M);
        sw_conf[i] = sw_confidence(sw);
    }

    // --- Hard-core (protected tornadic/meso) detection ----------------------
    // A gate is a *shear candidate* if its circular (mod 2*Nyquist) distance
    // to some neighbor is large relative to Nyquist -- this is what
    // distinguishes real high-shear discontinuities from ordinary alias
    // seams. It only becomes *hard* (excluded from normal flood-fill and
    // handed to the bounded rescue) if reflectivity confirms real echo
    // there, so an isolated noise spike doesn't consume rescue budget or get
    // special-cased.
    std::vector<unsigned char> hard_candidate(count, 0);
    for (int i = 0; i < n; ++i) {
        if (!std::isfinite(grid[i]) || !valid_nyq(nyq[i])) continue;
        float max_frac = 0.0f;
        for (int nb : neighbors[i]) {
            if (nb < 0 || !std::isfinite(grid[nb]) || !valid_nyq(nyq[nb])) continue;
            const float en = std::min(nyq[i], nyq[nb]);
            if (!(en > 0.0f)) continue;
            const float cd = circular_velocity_distance(grid[i], grid[nb], en);
            if (std::isfinite(cd)) max_frac = std::max(max_frac, cd / en);
        }
        hard_candidate[i] = max_frac >= HARD_CIRC_FRAC ? 1 : 0;
    }

    size_t candidate_count = 0;
    for (unsigned char x : hard_candidate) candidate_count += x != 0;
    const float candidate_frac =
        n > 0 ? static_cast<float>(candidate_count) / static_cast<float>(n) : 1.0f;

    // Only dilate a genuinely compact feature (a real localized core). A
    // scan-wide shear line gets no dilation and, below, no reflectivity
    // rescue either -- there may be no trustworthy exterior to anchor it,
    // and it is exactly the failure mode that made v8 mislabel broad shear
    // as "protected."
    if (candidate_frac <= HARD_DILATE_MAX_FRAC) {
        std::vector<unsigned char> dilated = hard_candidate;
        for (int i = 0; i < n; ++i) {
            if (!hard_candidate[i]) continue;
            for (int nb : neighbors[i])
                if (nb >= 0) dilated[nb] = 1;
        }
        hard_candidate.swap(dilated);
    }

    std::vector<unsigned char> hard(count, 0);
    for (int i = 0; i < n; ++i) {
        if (!hard_candidate[i]) continue;
        const bool ref_confirms = !have_ref || (std::isfinite(ref_dbz[i]) && ref_dbz[i] >= HARD_REFL_MIN_DBZ);
        hard[i] = ref_confirms ? 1 : 0;
    }

    // --- Phase 1: flood-fill regions, skipping hard gates entirely --------
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

    if (regions.empty()) {
        // Nothing resolvable as normal regions (e.g. entire tilt is hard or
        // empty). Leave data untouched; cache stays minimal but still
        // records hard/nyq/graph for a possible stage C attempt.
        cache.valid = true;
        cache.nyq = std::move(nyq);
        cache.hard = std::move(hard);
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

    // --- Phase 2: best-first propagation, SW-confidence-weighted votes ----
    std::priority_queue<Candidate> pq;
    size_t resolved = 0;

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
        ++resolved;

        const float delta = fold * 2.0f * region.nyquist;
        for (const int g : region.gates) grid[g] += delta;

        for (const Boundary& edge : region.boundary) {
            const int nb = labels[edge.neighbor];
            if (nb >= 0 && !regions[nb].resolved) propose(nb);
        }
    };

    int seed = -1;
    for (size_t i = 0; i < regions.size(); ++i) {
        if (seed < 0 ||
            regions[i].gates.size() > regions[seed].gates.size() ||
            (regions[i].gates.size() == regions[seed].gates.size() &&
             regions[i].nyquist > regions[seed].nyquist))
            seed = static_cast<int>(i);
    }
    resolve(seed, 0);

    while (!pq.empty()) {
        const Candidate c = pq.top(); pq.pop();
        Region& region = regions[c.region];
        if (region.resolved || region.version != c.version) continue;
        resolve(c.region, c.fold);
    }

    while (resolved < regions.size()) {
        seed = -1;
        for (size_t i = 0; i < regions.size(); ++i) {
            if (regions[i].resolved) continue;
            if (seed < 0 || regions[i].gates.size() > regions[seed].gates.size())
                seed = static_cast<int>(i);
        }
        if (seed < 0) break;
        resolve(seed, 0);

        while (!pq.empty()) {
            const Candidate c = pq.top(); pq.pop();
            Region& region = regions[c.region];
            if (region.resolved || region.version != c.version) continue;
            resolve(c.region, c.fold);
        }
    }

    // --- Bounded repair pass (identical to v2) ------------------------------
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


    // --- Global fold anchor: break the "which contiguous blob is fold zero"
    // ambiguity that a pure region-growing solver otherwise inherits from
    // whichever region happens to be largest. That heuristic silently fails
    // whenever environmental wind speed exceeds Nyquist over more than half
    // the ring: the aliased side can then be the bigger blob, and the whole
    // tilt gets anchored to the wrong absolute reference.
    //
    // The fix relies on a clean physical invariant rather than a curve fit:
    // for ANY uniform horizontal wind, the azimuthal mean of true radial
    // velocity over a full 360 degree sweep is exactly zero (sin/cos each
    // integrate to zero over a full period), independent of wind speed. A
    // VAD-style harmonic fit's own free constant term would silently absorb
    // a wrong global shift and can't be used to detect one; the raw mean is
    // not degenerate this way, so it's what we anchor on. This is a single
    // integer applied to the WHOLE tilt -- it cannot flip one region
    // relative to another, only move the whole already self-consistent
    // field by a whole number of folds.
    {
        double sum = 0.0;
        double typical_nyq = 0.0;
        size_t n_samples = 0;
        for (size_t i = 0; i < count; ++i) {
            if (hard[i] || !std::isfinite(grid[i]) || !valid_nyq(nyq[i])) continue;
            sum += grid[i];
            typical_nyq += nyq[i];
            ++n_samples;
        }

        if (n_samples >= 40) {
            typical_nyq /= static_cast<double>(n_samples);
            const double mean0 = sum / static_cast<double>(n_samples);

            constexpr int ANCHOR_RANGE = 3;
            double best_abs_mean = std::fabs(mean0);
            int best_k = 0;

            for (int k = -ANCHOR_RANGE; k <= ANCHOR_RANGE; ++k) {
                if (k == 0) continue;
                const double mean_k = mean0 + static_cast<double>(k) * 2.0 * typical_nyq;
                if (std::fabs(mean_k) < best_abs_mean) {
                    best_abs_mean = std::fabs(mean_k);
                    best_k = k;
                }
            }

            // Only shift if doing so both (a) clears a solid margin over
            // leaving it alone and (b) lands the corrected mean somewhere
            // physically small -- otherwise trust the region solver's
            // relative structure and leave the (unavoidable, inherent)
            // absolute ambiguity alone rather than guess on noisy/chaotic
            // fields.
            if (best_k != 0 &&
                best_abs_mean < 0.35 * std::fabs(mean0) &&
                best_abs_mean < 0.30 * typical_nyq) {
                for (size_t i = 0; i < count; ++i) {
                    if (hard[i] || !std::isfinite(grid[i]) || !valid_nyq(nyq[i])) continue;
                    grid[i] += static_cast<float>(best_k) * 2.0f * nyq[i];
                }
                for (Region& region : regions) region.fold += best_k;
            }
        }
    }

    for (size_t i = 0; i < count; ++i) packed[3 * i + 2] = grid[i];

    // --- Populate cache for stage B / stage C ------------------------------
    cache.valid = true;
    cache.elevation_cos = std::cos(static_cast<float>(deg2rad(tilt.ElevationAngle)));
    cache.vel_rays.reserve(rays.size());
    for (const auto& r : rays)
        cache.vel_rays.push_back({r.start, r.count, packed[3 * r.start]});
    std::sort(cache.vel_rays.begin(), cache.vel_rays.end(),
              [](const MomentRay& a, const MomentRay& b) { return a.azimuth < b.azimuth; });
    cache.nyq = std::move(nyq);
    cache.hard = std::move(hard);
    cache.graph = std::move(neighbors);
    cache.regions = std::move(regions);

    return cache;
}

// ---------------------------------------------------------------------------
// Cross-tilt (azimuth, range) -> value lookup used by both stage B
// (vertical region reconciliation) and stage C (hard-gate rescue's vertical
// prior). Range is projected by the ratio of cosines of elevation so that a
// lookup at a given slant range on tilt A compares against roughly the same
// ground range on tilt B, following the same approximation v9 used.
// ---------------------------------------------------------------------------

bool vertical_lookup(const SingleTilt& ref_tilt, const TiltCache& ref_cache,
                      float cur_cos, float ref_cos, float az, float cur_range_m,
                      float& out_value) {
    if (!ref_cache.valid || ref_cache.vel_rays.empty()) return false;
    if (!std::isfinite(cur_cos) || !std::isfinite(ref_cos) || std::fabs(ref_cos) < VERT_MIN_COS)
        return false;

    const int ri = nearest_moment_ray(MomentIndex{&ref_tilt.Radials_VEL, ref_cache.vel_rays}, az);
    if (ri < 0) return false;
    const MomentRay& ray = ref_cache.vel_rays[static_cast<size_t>(ri)];
    if (az_distance(ray.azimuth, wrap360(az)) > MOMENT_AZ_TOL_DEG) return false;

    const float desired_range = cur_range_m * cur_cos / ref_cos;
    const std::vector<float>& packed = ref_tilt.Radials_VEL;

    const size_t lo = ray.start, hi = ray.start + ray.count;
    size_t l = lo, h = hi;
    while (l < h) {
        const size_t m = (l + h) / 2;
        if (packed[3 * m + 1] < desired_range) l = m + 1; else h = m;
    }
    size_t best = (l >= hi) ? hi - 1 : l;
    if (best > lo) {
        const float d0 = std::fabs(packed[3 * best + 1] - desired_range);
        const float d1 = std::fabs(packed[3 * (best - 1) + 1] - desired_range);
        if (d1 < d0) --best;
    }
    if (std::fabs(packed[3 * best + 1] - desired_range) > MOMENT_RANGE_TOL_M) return false;
    if (best < ref_cache.hard.size() && ref_cache.hard[best]) return false; // don't anchor on an unresolved reference gate
    if (!std::isfinite(packed[3 * best + 2])) return false;

    out_value = packed[3 * best + 2];
    return true;
}

// ---------------------------------------------------------------------------
// Stage B: vertical (multi-tilt) region reconciliation. Operates on whole
// regions, O(regions * sample_size) total, and only ever shifts a region by
// a whole number of folds when both the vote fraction and the residual
// improvement clear a strict bar. Hard/unresolved gates are never touched
// here -- they are handled separately in stage C, after this stage has
// finished calibrating the smooth background around them.
// ---------------------------------------------------------------------------

void reconcile_vertical(AllTilt& volume, const std::vector<int>& order,
                         std::vector<TiltCache>& cache) {
    for (size_t oi = 0; oi < order.size(); ++oi) {
        const int idx = order[oi];
        SingleTilt& cur = volume.Tilts[idx];
        TiltCache& ci = cache[idx];
        if (!ci.valid || ci.regions.empty()) continue;

        const int below = oi > 0 ? order[oi - 1] : -1;
        const int above = (oi + 1 < order.size()) ? order[oi + 1] : -1;
        const float cur_cos = ci.elevation_cos;
        const float below_cos = below >= 0 ? cache[below].elevation_cos : 1.0f;
        const float above_cos = above >= 0 ? cache[above].elevation_cos : 1.0f;

        for (Region& region : ci.regions) {
            if (region.gates.size() < static_cast<size_t>(VERT_MIN_REGION_GATES)) continue;

            std::array<float, 2 * MAX_FOLDS + 1> votes{};
            std::array<float, 2 * MAX_FOLDS + 1> below_votes{};
            std::array<float, 2 * MAX_FOLDS + 1> above_votes{};
            std::array<double, 2 * MAX_FOLDS + 1> shift_cost{};
            int below_support = 0, above_support = 0;
            double old_cost = 0.0;

            const size_t stride = std::max(VERT_SAMPLE_STRIDE_MIN,
                                            region.gates.size() / VERT_MAX_SAMPLES + 1);

            for (size_t p = 0; p < region.gates.size(); p += stride) {
                const size_t g = static_cast<size_t>(region.gates[p]);
                const float az = cur.Radials_VEL[3 * g];
                const float rng = cur.Radials_VEL[3 * g + 1];
                const float current = cur.Radials_VEL[3 * g + 2];
                if (!std::isfinite(current)) continue;
                const float P = 2.0f * region.nyquist;

                if (below >= 0) {
                    float ref = 0.0f;
                    if (vertical_lookup(volume.Tilts[below], cache[below], cur_cos, below_cos, az, rng, ref)) {
                        const int k = static_cast<int>(std::lround((ref - current) / P));
                        if (std::abs(k) <= MAX_FOLDS) {
                            votes[k + MAX_FOLDS] += 1.0f;
                            below_votes[k + MAX_FOLDS] += 1.0f;
                            ++below_support;
                        }
                        old_cost += std::fabs(current - ref);
                        for (int kk = -MAX_FOLDS; kk <= MAX_FOLDS; ++kk)
                            shift_cost[kk + MAX_FOLDS] += std::fabs(current + kk * P - ref);
                    }
                }
                if (above >= 0) {
                    float ref = 0.0f;
                    if (vertical_lookup(volume.Tilts[above], cache[above], cur_cos, above_cos, az, rng, ref)) {
                        const int k = static_cast<int>(std::lround((ref - current) / P));
                        if (std::abs(k) <= MAX_FOLDS) {
                            votes[k + MAX_FOLDS] += 1.0f;
                            above_votes[k + MAX_FOLDS] += 1.0f;
                            ++above_support;
                        }
                        old_cost += std::fabs(current - ref);
                        for (int kk = -MAX_FOLDS; kk <= MAX_FOLDS; ++kk)
                            shift_cost[kk + MAX_FOLDS] += std::fabs(current + kk * P - ref);
                    }
                }
            }

            const int total = below_support + above_support;
            if (total < VERT_MIN_SUPPORT || old_cost <= 0.0) continue;

            int best = 0; float best_votes = 0.0f;
            for (int k = -MAX_FOLDS; k <= MAX_FOLDS; ++k)
                if (votes[k + MAX_FOLDS] > best_votes) { best_votes = votes[k + MAX_FOLDS]; best = k; }
            if (best == 0 || best_votes / static_cast<float>(total) < VERT_MIN_FRAC) continue;

            if (below >= 0 && above >= 0 && below_support >= 3 && above_support >= 3) {
                int bbest = 0, abest = 0; float bv = 0.0f, av = 0.0f;
                for (int k = -MAX_FOLDS; k <= MAX_FOLDS; ++k) {
                    if (below_votes[k + MAX_FOLDS] > bv) { bv = below_votes[k + MAX_FOLDS]; bbest = k; }
                    if (above_votes[k + MAX_FOLDS] > av) { av = above_votes[k + MAX_FOLDS]; abest = k; }
                }
                if (bbest != best || abest != best ||
                    bv / static_cast<float>(below_support) < VERT_SIDE_FRAC ||
                    av / static_cast<float>(above_support) < VERT_SIDE_FRAC)
                    continue;
            }

            if (shift_cost[best + MAX_FOLDS] / old_cost > VERT_MAX_RESIDUAL_RATIO) continue;

            const float delta = static_cast<float>(best) * 2.0f * region.nyquist;
            for (const int g : region.gates) cur.Radials_VEL[3 * g + 2] += delta;
            region.fold += best;
        }
    }
}

// ---------------------------------------------------------------------------
// Stage C: bounded hard-gate (tornadic/meso core) rescue. Runs after stage B
// so the surrounding "normal" field it references is already
// vertically-calibrated. Budget-capped exactly like v8/v9's rescue; a gate
// that cannot clear the support/axis-diversity/improvement bars is left
// untouched (raw, aliased) rather than guessed -- consistent with the
// mesocyclone-targeted-dealiasing literature's practice of leaving vortex
// core ambiguity as a gap rather than filling it wrong.
// ---------------------------------------------------------------------------

void hard_gate_rescue(AllTilt& volume, const std::vector<int>& order,
                       std::vector<TiltCache>& cache) {
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
        if (hard_count > budget * 2) continue; // field is dominated by "hard" -- don't trust the classification enough to rescue

        const int below = oi > 0 ? order[oi - 1] : -1;
        const int above = (oi + 1 < order.size()) ? order[oi + 1] : -1;
        const float cur_cos = ci.elevation_cos;
        const float below_cos = below >= 0 ? cache[below].elevation_cos : 1.0f;
        const float above_cos = above >= 0 ? cache[above].elevation_cos : 1.0f;

        // Build REF/SW indices once for this tilt (same as stage A -- cheap,
        // and stage A didn't cache them since they're only needed again
        // here).
        const MomentIndex sw_idx = build_moment_index(cur.Radials_SW);

        std::vector<unsigned char> solved = ci.hard; // solved[g]==0 means "not hard" (already fine) or "rescued"
        for (size_t g = 0; g < solved.size(); ++g) solved[g] = ci.hard[g] ? 0 : 1;

        size_t rescued = 0;
        for (int pass = 0; pass < 2 && rescued < budget; ++pass) {
            for (size_t g = 0; g < ci.hard.size() && rescued < budget; ++g) {
                if (!ci.hard[g] || solved[g]) continue;
                if (!valid_nyq(ci.nyq[g])) continue;

                const float raw = cur.Radials_VEL[3 * g + 2];
                if (!std::isfinite(raw)) continue;
                const float az = cur.Radials_VEL[3 * g];
                const float rng = cur.Radials_VEL[3 * g + 1];
                const float nyq_g = ci.nyq[g];
                const float P = 2.0f * nyq_g;
                const float scale = std::max(1.5f, RESCUE_EDGE_SCALE_FRAC * nyq_g);

                float vert_target = VEL_NAN;
                int vert_hits = 0;
                if (below >= 0) {
                    float v = 0.0f;
                    if (vertical_lookup(volume.Tilts[below], cache[below], cur_cos, below_cos, az, rng, v)) {
                        vert_target = std::isfinite(vert_target) ? vert_target + v : v; ++vert_hits;
                    }
                }
                if (above >= 0) {
                    float v = 0.0f;
                    if (vertical_lookup(volume.Tilts[above], cache[above], cur_cos, above_cos, az, rng, v)) {
                        vert_target = std::isfinite(vert_target) ? vert_target + v : v; ++vert_hits;
                    }
                }
                if (vert_hits > 0) vert_target /= static_cast<float>(vert_hits);

                auto edge_conf = [&](int gate_idx) {
                    const float sw = moment_lookup(sw_idx, cur.Radials_VEL[3 * gate_idx],
                                                    cur.Radials_VEL[3 * gate_idx + 1],
                                                    MOMENT_AZ_TOL_DEG, MOMENT_RANGE_TOL_M);
                    return sw_confidence(sw);
                };

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
                        const float w = edge_conf(static_cast<int>(g)) * edge_conf(j);
                        cost += w * huber_cost(diff, scale);
                        if (diff <= 0.55f * en) { ++support; dirs |= 1u << static_cast<unsigned>(d); }
                    }
                    if (nref > 0) cost /= nref;
                    if (std::isfinite(vert_target))
                        cost += RESCUE_VERT_WEIGHT * huber_cost(candidate - vert_target, std::max(2.0f, 0.4f * nyq_g));
                    return cost;
                };

                int cur_support = 0; unsigned cur_dirs = 0u;
                const double old_cost = total_cost(raw, cur_support, cur_dirs);

                double best_cost = old_cost;
                int best_fold = 0;
                int best_support = cur_support;
                unsigned best_dirs = cur_dirs;

                for (int k = -MAX_FOLDS; k <= MAX_FOLDS; ++k) {
                    if (k == 0) continue;
                    const float candidate = raw + static_cast<float>(k) * P;
                    int support = 0; unsigned dirs = 0u;
                    const double cost = total_cost(candidate, support, dirs);
                    const int axes = ((dirs & 3u) ? 1 : 0) + ((dirs & 12u) ? 1 : 0);
                    const bool vert_agrees = std::isfinite(vert_target) &&
                                              std::fabs(candidate - vert_target) <= 0.5f * nyq_g;
                    const bool spatially_ok = (support >= RESCUE_MIN_SUPPORT && axes >= 2) ||
                                               (support >= 1 && vert_agrees);
                    if (!spatially_ok) continue;
                    if (cost < best_cost) {
                        best_cost = cost; best_fold = k; best_support = support; best_dirs = dirs;
                    }
                }

                (void)best_support; (void)best_dirs;
                if (best_fold == 0) continue;
                if (old_cost <= 0.0 || (old_cost - best_cost) / old_cost < RESCUE_MIN_IMPROVEMENT) continue;

                cur.Radials_VEL[3 * g + 2] = raw + static_cast<float>(best_fold) * P;
                solved[g] = 1;
                ++rescued;
            }
        }
    }
}

} // namespace

void dealias_velocity_volume_v10(AllTilt& volume) {
    const size_t nt = volume.Tilts.size();
    if (nt == 0) return;

    std::vector<int> order(nt);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return volume.Tilts[a].ElevationAngle < volume.Tilts[b].ElevationAngle;
    });

    // Stage A: fast per-tilt 2D solve (region flood-fill + boundary vote),
    // REF/SW-aware hard-core exclusion. O(n log n) per tilt, no rebuilt
    // work later.
    std::vector<TiltCache> cache(nt);
    for (int idx : order)
        cache[idx] = dealias_tilt_stage_a(volume.Tilts[idx]);

    // Stage B: O(regions) multi-tilt sanity check/correction on the normal
    // (already internally-consistent) regions.
    reconcile_vertical(volume, order, cache);

    // Stage C: bounded rescue for the reflectivity-confirmed hard cores,
    // now referencing a vertically-calibrated background.
    hard_gate_rescue(volume, order, cache);
}