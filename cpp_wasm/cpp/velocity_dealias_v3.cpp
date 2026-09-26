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

// ============================================================================
// Velocity Dealiasing v3
//
// Design:
//   - Keeps v2's aggressive best-first region propagation.
//   - NO confidence veto: evidence always produces a propagation candidate.
//   - Candidate folds are scored using the entire resolved boundary.
//   - Robust Huber-like boundary cost prevents one bad edge dominating.
//   - Candidate margin is metadata / queue priority only.
//   - No global min-|V| anchoring.
//   - No high-shear / TVS veto.
//   - Vertical information is intentionally not hard-wired here; v3 focuses
//     on making the 2-D single-tilt inference reliable first.
//
// The fundamental model is:
//     observed_velocity = true_velocity modulo 2 * Nyquist
//
// A region is internally continuous, so it receives one integer fold.
// ============================================================================

constexpr float VEL_NAN = std::numeric_limits<float>::quiet_NaN();

constexpr float AZ_EPS = 0.01f;
constexpr float NYQ_REL_TOL = 0.03f;

// Initial segmentation threshold. This is deliberately less aggressive than
// v2's 0.20 Nyquist split. It can be tuned after looking at real scans.
constexpr float SEG_FRAC = 0.40f;

// Neighbor range matching.
constexpr float RANGE_TOL = 0.10f;

// Candidate fold search. NEXRAD tornado cases can require several folds.
constexpr int MAX_FOLDS = 6;

// Robust boundary loss. Residuals below this fraction of Nyquist are treated
// approximately quadratically; large residuals become approximately linear.
constexpr float HUBER_FRAC = 0.50f;

// A boundary edge whose Nyquist is incompatible with the region is ignored.
inline bool valid_nyq(float n)
{
    return std::isfinite(n) && n > 0.0f;
}

inline bool compatible_nyq(float a, float b)
{
    if (!valid_nyq(a) || !valid_nyq(b))
        return false;

    return std::fabs(a - b) <=
           NYQ_REL_TOL * std::max(a, b);
}

inline int fold_from_pair(float raw, float reference, float nyq)
{
    return static_cast<int>(
        std::lround((reference - raw) / (2.0f * nyq)));
}

inline float huber_cost(float residual, float scale)
{
    residual = std::fabs(residual);

    const float d = std::max(scale, 1e-3f);

    if (residual <= d)
        return 0.5f * residual * residual / d;

    return residual - 0.5f * d;
}

struct Boundary {
    int gate;
    int neighbor;
};

struct Region {
    std::vector<int> gates;
    std::vector<Boundary> boundary;

    float nyquist = 0.0f;

    int fold = 0;
    int version = 0;

    bool resolved = false;

    // Diagnostics only. These NEVER veto propagation.
    float best_cost = std::numeric_limits<float>::infinity();
    float second_cost = std::numeric_limits<float>::infinity();
    float confidence = 0.0f;
    int evidence = 0;
};

struct Candidate {
    int evidence = 0;
    int region = -1;
    int fold = 0;
    int version = 0;

    // Larger is better. Used only to determine propagation order.
    float priority = 0.0f;

    bool operator<(const Candidate& o) const
    {
        if (priority != o.priority)
            return priority < o.priority;

        if (evidence != o.evidence)
            return evidence < o.evidence;

        return region > o.region;
    }
};

// -----------------------------------------------------------------------------
// Score all integer fold hypotheses for one unresolved region.
//
// We compare the candidate-corrected boundary value against the already
// resolved neighbor. The entire boundary participates.
//
// IMPORTANT:
//   This function does not decide whether the result is "safe enough".
//   It always returns the best available hypothesis.
// -----------------------------------------------------------------------------

struct FoldScore {
    int fold = 0;
    float cost = std::numeric_limits<float>::infinity();
};

struct FoldEvaluation {
    FoldScore best;
    FoldScore second;

    int evidence = 0;
    int best_support = 0;
    int second_support = 0;
};

FoldEvaluation evaluate_region(
    const Region& region,
    const std::vector<int>& labels,
    const std::vector<Region>& regions,
    const std::vector<float>& grid,
    const std::vector<float>& nyq)
{
    FoldEvaluation result;

    std::array<float, 2 * MAX_FOLDS + 1> cost{};
    std::array<int, 2 * MAX_FOLDS + 1> support{};

    cost.fill(0.0f);
    support.fill(0);

    for (const Boundary& edge : region.boundary) {
        const int nb = labels[edge.neighbor];

        if (nb < 0 || !regions[nb].resolved)
            continue;

        if (!std::isfinite(grid[edge.gate]) ||
            !std::isfinite(grid[edge.neighbor]))
            continue;

        const float edge_nyq =
            std::min(region.nyquist, nyq[edge.neighbor]);

        if (!valid_nyq(edge_nyq))
            continue;

        if (!compatible_nyq(region.nyquist, nyq[edge.neighbor]))
            continue;

        ++result.evidence;

        const float raw = grid[edge.gate];
        const float reference = grid[edge.neighbor];

        const float huber_scale =
            std::max(1.0f, HUBER_FRAC * edge_nyq);

        for (int k = -MAX_FOLDS; k <= MAX_FOLDS; ++k) {
            const int idx = k + MAX_FOLDS;

            const float corrected =
                raw + static_cast<float>(k) * 2.0f * region.nyquist;

            const float residual =
                corrected - reference;

            cost[idx] += huber_cost(residual, huber_scale);

            // "Support" means the corrected value is at least reasonably
            // close to the neighboring resolved field.
            if (std::fabs(residual) <= 0.50f * edge_nyq)
                ++support[idx];
        }
    }

    if (result.evidence == 0)
        return result;

    int best_idx = 0;

    for (int i = 1; i < static_cast<int>(cost.size()); ++i) {
        if (cost[i] < cost[best_idx])
            best_idx = i;
    }

    int second_idx = -1;

    for (int i = 0; i < static_cast<int>(cost.size()); ++i) {
        if (i == best_idx)
            continue;

        if (second_idx < 0 || cost[i] < cost[second_idx])
            second_idx = i;
    }

    result.best.fold = best_idx - MAX_FOLDS;
    result.best.cost = cost[best_idx];
    result.best_support = support[best_idx];

    if (second_idx >= 0) {
        result.second.fold = second_idx - MAX_FOLDS;
        result.second.cost = cost[second_idx];
        result.second_support = support[second_idx];
    }

    return result;
}

// -----------------------------------------------------------------------------
// Per-tilt dealiasing.
// -----------------------------------------------------------------------------

void dealias_tilt(SingleTilt& tilt)
{
    std::vector<float>& packed = tilt.Radials_VEL;

    if (packed.size() < 24 || packed.size() % 3 != 0)
        return;

    const size_t count = packed.size() / 3;

    if (count >
        static_cast<size_t>(
            std::numeric_limits<int>::max() / 3))
        return;

    const float fallback_nyq =
        static_cast<float>(
            tilt.vol_el_rad.rad.nyquist_vel) / 100.0f;

    const int n = static_cast<int>(count);

    std::vector<VelocityRay> rays = tilt.VelocityRays;

    // ------------------------------------------------------------------------
    // Ray construction / validation.
    // ------------------------------------------------------------------------

    if (rays.empty()) {
        if (!valid_nyq(fallback_nyq))
            return;

        rays.reserve(720);

        for (int t = 0; t < n; ++t) {
            if (t == 0 ||
                std::fabs(
                    packed[3 * t] -
                    packed[3 * (t - 1)]) > AZ_EPS ||
                packed[3 * t + 1] <=
                    packed[3 * (t - 1) + 1]) {

                rays.push_back({
                    static_cast<size_t>(t),
                    0,
                    fallback_nyq,
                    tilt.gateSpacing
                });
            }

            ++rays.back().count;
        }

        if (!tilt.VelNyquist.empty() &&
            tilt.VelNyquist.size() != rays.size())
            return;

        for (size_t r = 0; r < rays.size(); ++r) {
            if (!tilt.VelNyquist.empty() &&
                valid_nyq(tilt.VelNyquist[r])) {

                rays[r].nyquist =
                    tilt.VelNyquist[r];
            }
        }
    }

    if (rays.size() < 4)
        return;

    // Always use the tilt Nyquist as fallback.
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
            valid_nyq(ray.nyquist)
                ? ray.nyquist
                : fallback_nyq;

        if (!valid_nyq(ray_nyq))
            return;

        const size_t end =
            ray.start + ray.count;

        const float az =
            packed[3 * ray.start];

        if (!std::isfinite(az) ||
            az < 0.0f ||
            az >= 360.0f)
            return;

        for (size_t t = ray.start; t < end; ++t) {
            if (!std::isfinite(
                    packed[3 * t + 1]))
                return;

            if (t > ray.start &&
                packed[3 * t + 1] <=
                    packed[3 * (t - 1) + 1])
                return;

            nyq[t] = ray_nyq;
        }

        expected = end;
    }

    if (expected != count)
        return;

    // ------------------------------------------------------------------------
    // Estimate normal azimuth spacing.
    // ------------------------------------------------------------------------

    float az_sum = 0.0f;
    int az_samples = 0;

    for (size_t r = 1; r < rays.size(); ++r) {
        const float gap = std::fmod(
            packed[3 * rays[r].start] -
            packed[3 * rays[r - 1].start] +
            360.0f,
            360.0f);

        if (gap > AZ_EPS && gap <= 1.5f) {
            az_sum += gap;
            ++az_samples;
        }
    }

    const float typical_az =
        az_samples
            ? az_sum / az_samples
            : 1.0f;

    const float max_az_gap =
        std::min(1.5f, 1.8f * typical_az);

    // ------------------------------------------------------------------------
    // Grid + 4-neighbor graph.
    // ------------------------------------------------------------------------

    std::vector<float> grid(count);

    for (size_t i = 0; i < count; ++i)
        grid[i] = packed[3 * i + 2];

    std::vector<std::array<int, 4>> neighbors(
        count,
        {-1, -1, -1, -1});

    auto connect = [&](int a, int b, int dir) {
        if (a < 0 || b < 0)
            return;

        if (!std::isfinite(grid[a]) ||
            !std::isfinite(grid[b]))
            return;

        if (!valid_nyq(nyq[a]) ||
            !valid_nyq(nyq[b]))
            return;

        neighbors[a][dir] = b;
        neighbors[b][dir ^ 1] = a;
    };

    for (size_t r = 0; r < rays.size(); ++r) {
        const auto& ray = rays[r];
        const size_t end =
            ray.start + ray.count;

        // Range neighbors.
        for (size_t t = ray.start + 1;
             t < end;
             ++t) {

            const float dr =
                packed[3 * t + 1] -
                packed[3 * (t - 1) + 1];

            if (std::fabs(
                    dr - ray.gateSpacing) <=
                RANGE_TOL) {

                connect(
                    static_cast<int>(t - 1),
                    static_cast<int>(t),
                    0);
            }
        }

        // Adjacent azimuth rays.
        const size_t next =
            (r + 1) % rays.size();

        const float gap = std::fmod(
            packed[3 * rays[next].start] -
            packed[3 * ray.start] +
            360.0f,
            360.0f);

        if (gap <= AZ_EPS ||
            gap > max_az_gap)
            continue;

        size_t a = ray.start;
        size_t b = rays[next].start;

        const size_t ae =
            ray.start + ray.count;

        const size_t be =
            rays[next].start +
            rays[next].count;

        while (a < ae && b < be) {
            const float d =
                packed[3 * a + 1] -
                packed[3 * b + 1];

            if (std::fabs(d) <= RANGE_TOL) {
                connect(
                    static_cast<int>(a),
                    static_cast<int>(b),
                    2);

                ++a;
                ++b;
            }
            else if (d < 0.0f) {
                ++a;
            }
            else {
                ++b;
            }
        }
    }

    // ------------------------------------------------------------------------
    // Phase 1: raw connected components.
    //
    // A region is locally continuous. A boundary is where the observed
    // velocities differ too much to belong to the same branch.
    // ------------------------------------------------------------------------

    std::vector<int> labels(
        count,
        -1);

    std::vector<Region> regions;

    regions.reserve(
        count / 8 + 1);

    for (int start = 0;
         start < n;
         ++start) {

        if (labels[start] >= 0 ||
            !std::isfinite(grid[start]) ||
            !valid_nyq(nyq[start]))
            continue;

        const int id =
            static_cast<int>(
                regions.size());

        regions.emplace_back();

        Region& region =
            regions.back();

        region.nyquist =
            nyq[start];

        region.gates.push_back(start);
        labels[start] = id;

        for (size_t p = 0;
             p < region.gates.size();
             ++p) {

            const int g =
                region.gates[p];

            for (const int nb :
                 neighbors[g]) {

                if (nb < 0 ||
                    labels[nb] >= 0)
                    continue;

                if (!std::isfinite(
                        grid[nb]))
                    continue;

                if (!compatible_nyq(
                        region.nyquist,
                        nyq[nb]))
                    continue;

                const float edge_nyq =
                    std::min(
                        region.nyquist,
                        nyq[nb]);

                if (std::fabs(
                        grid[g] - grid[nb]) >
                    SEG_FRAC * edge_nyq)
                    continue;

                labels[nb] = id;
                region.gates.push_back(nb);
            }
        }
    }

    if (regions.empty())
        return;

    // ------------------------------------------------------------------------
    // Boundary list.
    // ------------------------------------------------------------------------

    for (int g = 0;
         g < n;
         ++g) {

        const int a =
            labels[g];

        if (a < 0)
            continue;

        for (const int nb :
             neighbors[g]) {

            if (nb < 0)
                continue;

            const int b =
                labels[nb];

            if (b >= 0 && b != a)
                regions[a].boundary.push_back(
                    {g, nb});
        }
    }

    // ------------------------------------------------------------------------
    // Phase 2: best-first propagation.
    //
    // IMPORTANT DIFFERENCE FROM v5/v6:
    //
    // There is NO acceptance threshold.
    //
    // If one resolved boundary edge exists, a fold candidate is generated.
    // Confidence only controls ordering.
    // ------------------------------------------------------------------------

    std::priority_queue<Candidate> pq;

    size_t resolved = 0;

    auto propose = [&](int id) {
        Region& region =
            regions[id];

        if (region.resolved)
            return;

        const FoldEvaluation eval =
            evaluate_region(
                region,
                labels,
                regions,
                grid,
                nyq);

        if (eval.evidence <= 0)
            return;

        region.best_cost =
            eval.best.cost;

        region.second_cost =
            eval.second.cost;

        region.evidence =
            eval.evidence;

        const float margin =
            (std::isfinite(eval.second.cost) &&
             eval.second.cost >
                 1e-6f)
                ? std::clamp(
                      (eval.second.cost -
                       eval.best.cost) /
                      eval.second.cost,
                      0.0f,
                      1.0f)
                : 1.0f;

        region.confidence = margin;

        ++region.version;

        // Stronger evidence and larger margin go first.
        // This is ONLY queue priority.
        const float priority =
            static_cast<float>(
                eval.evidence) *
                (0.25f + 0.75f * margin);

        pq.push({
            eval.evidence,
            id,
            eval.best.fold,
            region.version,
            priority
        });
    };

    auto resolve = [&](int id, int fold) {
        Region& region =
            regions[id];

        if (region.resolved)
            return;

        region.fold = fold;
        region.resolved = true;
        ++resolved;

        const float delta =
            static_cast<float>(fold) *
            2.0f *
            region.nyquist;

        for (const int g :
             region.gates)
            grid[g] += delta;

        for (const Boundary& edge :
             region.boundary) {

            const int nb =
                labels[edge.neighbor];

            if (nb >= 0 &&
                !regions[nb].resolved)
                propose(nb);
        }
    };

    // ------------------------------------------------------------------------
    // Initial reference for a disconnected scan.
    //
    // This is a reference, not a physical claim that zero-fold is globally
    // correct. Absolute anchoring is fundamentally unavailable from one
    // isolated velocity field.
    // ------------------------------------------------------------------------

    int seed = -1;

    for (size_t i = 0;
         i < regions.size();
         ++i) {

        if (seed < 0 ||
            regions[i].gates.size() >
                regions[seed].gates.size() ||
            (regions[i].gates.size() ==
                 regions[seed].gates.size() &&
             regions[i].nyquist >
                 regions[seed].nyquist)) {

            seed =
                static_cast<int>(i);
        }
    }

    resolve(seed, 0);

    // ------------------------------------------------------------------------
    // Connected propagation.
    // ------------------------------------------------------------------------

    while (!pq.empty()) {
        const Candidate c =
            pq.top();

        pq.pop();

        Region& region =
            regions[c.region];

        if (region.resolved ||
            region.version !=
                c.version)
            continue;

        resolve(
            c.region,
            c.fold);
    }

    // ------------------------------------------------------------------------
    // Disconnected islands.
    //
    // Each disconnected component needs an arbitrary reference. Keep v2's
    // largest-island fold-0 behavior rather than inventing a global anchor.
    // ------------------------------------------------------------------------

    while (resolved < regions.size()) {
        seed = -1;

        for (size_t i = 0;
             i < regions.size();
             ++i) {

            if (regions[i].resolved)
                continue;

            if (seed < 0 ||
                regions[i].gates.size() >
                    regions[seed].gates.size()) {

                seed =
                    static_cast<int>(i);
            }
        }

        if (seed < 0)
            break;

        resolve(seed, 0);

        while (!pq.empty()) {
            const Candidate c =
                pq.top();

            pq.pop();

            Region& region =
                regions[c.region];

            if (region.resolved ||
                region.version !=
                    c.version)
                continue;

            resolve(
                c.region,
                c.fold);
        }
    }

    // ------------------------------------------------------------------------
    // Phase 3: bounded local repair.
    //
    // Repair is deliberately conservative. Unlike propagation, this phase
    // CAN reject a correction because it is modifying an already-resolved
    // region rather than establishing a new fold hypothesis.
    //
    // We test +/- one fold against all resolved boundary neighbors.
    // ------------------------------------------------------------------------

    for (Region& region :
         regions) {

        if (region.boundary.size() < 2)
            continue;

        const float interval =
            2.0f *
            region.nyquist;

        double e0 = 0.0;
        double em = 0.0;
        double ep = 0.0;

        int support = 0;
        int sm = 0;
        int sp = 0;

        for (const Boundary& edge :
             region.boundary) {

            const int nb =
                labels[edge.neighbor];

            if (nb < 0 ||
                !regions[nb].resolved)
                continue;

            const float v =
                grid[edge.gate];

            const float ref =
                grid[edge.neighbor];

            if (!std::isfinite(v) ||
                !std::isfinite(ref))
                continue;

            const float a =
                std::fabs(v - ref);

            const float m =
                std::fabs(
                    v - interval - ref);

            const float p =
                std::fabs(
                    v + interval - ref);

            e0 += a;
            em += m;
            ep += p;

            ++support;

            if (m < 0.75f * a)
                ++sm;

            if (p < 0.75f * a)
                ++sp;
        }

        if (support < 2)
            continue;

        int shift = 0;

        if (sm >= 2 &&
            em < 0.80 * e0 &&
            em < ep) {

            shift = -1;
        }
        else if (sp >= 2 &&
                 ep < 0.80 * e0 &&
                 ep < em) {

            shift = +1;
        }

        if (shift != 0) {
            const float delta =
                static_cast<float>(shift) *
                interval;

            for (const int g :
                 region.gates) {

                grid[g] += delta;
            }

            region.fold += shift;
        }
    }

    // ------------------------------------------------------------------------
    // Output.
    // ------------------------------------------------------------------------

    for (size_t i = 0;
         i < count;
         ++i) {

        packed[3 * i + 2] =
            grid[i];
    }
}

} // namespace

void dealias_velocity_volume_v3(AllTilt& volume)
{
    for (auto& tilt :
         volume.Tilts) {

        dealias_tilt(tilt);
    }
}
