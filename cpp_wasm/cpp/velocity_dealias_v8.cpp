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

namespace {

constexpr float NANV = std::numeric_limits<float>::quiet_NaN();
constexpr float TWO_PI = 6.2831853071795864769f;
constexpr int MAX_FOLD = 8;

// v8: conservative limits for the targeted hard-region overlay.
constexpr float HARD_SHEAR_CIRCULAR_FRAC = 0.50f;
constexpr float TVS_ABS_VELOCITY_FRAC = 0.45f;
constexpr float TVS_RAW_JUMP_FRAC = 0.85f;
constexpr size_t MAX_RESCUE_GATES_ABS = 4096;
constexpr float MAX_RESCUE_FRACTION = 0.035f;
constexpr float RESCUE_MIN_IMPROVEMENT = 0.30f;
constexpr int RESCUE_MIN_SUPPORT = 2;
constexpr float RESCUE_VAD_WEIGHT = 0.08f;
constexpr float RESCUE_EDGE_SCALE_FRAC = 0.38f;
constexpr int VAD_MIN_RAYS = 24;
constexpr float VAD_MIN_COVERAGE = 0.55f;
constexpr float VAD_MAX_RMSE_FRAC = 0.38f;
constexpr float VAD_MAX_AMPLITUDE_FRAC = 1.25f;

/*
 * Conservative tornado-aware velocity dealiasing (v8)
 *
 * Design:
 *   1. Build a real ray/gate graph, including the azimuth wrap only when the
 *      observed ray spacing says that a wrap edge is legitimate.
 *   2. Never throw away a gate merely because its raw velocity differs by a
 *      large amount. Large shear is valid meteorological information.
 *   3. Find high-confidence zero-crossing / locally coherent seeds.
 *   4. Unwrap outward with a quality-guided, multi-neighbor predictor.
 *   5. Revisit ambiguous gates using a second pass with a larger stencil.
 *   6. Use integer fold optimization on every accepted edge, rather than
 *      assuming that all neighboring velocities must be close.
 *   7. Keep the v2 solver as the baseline; a small tornado rescue overlay is
 *      permitted only inside a compact protected region.
 *   8. High-shear protection uses circular distance modulo 2*Nyquist, so a
 *      normal alias seam is not automatically labeled physical shear.
 *   9. No directional extrapolation is used across a hard region.
 *  10. VAD is used only as a weak absolute anchor for isolated background
 *      components; it never anchors a protected/tornadic core.
 *  11. The rescue has a strict gate budget and automatically disables itself
 *      when the protected fraction becomes too large.
 *
 * The v2 solution is therefore the default answer.
 */

struct RayInfo {
    size_t start = 0;
    size_t count = 0;
    float nyquist = NANV;
    float gate_spacing = NANV;
    float azimuth = NANV;
};

struct Node {
    float raw = NANV;
    float value = NANV;
    float nyq = NANV;
    float azimuth = NANV;
    float range = NANV;
    bool valid = false;
    bool solved = false;
    bool protected_shear = false;
    float confidence = 0.0f;
};

struct Edge {
    int a = -1;
    int b = -1;
    float geometric_weight = 1.0f;
};

struct QueueItem {
    float score;
    int node;
    int generation;

    bool operator<(const QueueItem& o) const {
        if (score != o.score) return score < o.score;
        return node > o.node;
    }
};

inline bool finite_positive(float x) {
    return std::isfinite(x) && x > 0.0f;
}

inline float wrap360(float x) {
    while (x < 0.0f) x += 360.0f;
    while (x >= 360.0f) x -= 360.0f;
    return x;
}

inline float az_distance(float a, float b) {
    float d = std::fabs(a - b);
    return std::min(d, 360.0f - d);
}

inline float interval(float nyq) {
    return 2.0f * nyq;
}

inline float folded_candidate(float raw, float reference, float nyq, int& fold) {
    if (!finite_positive(nyq) || !std::isfinite(raw) ||
        !std::isfinite(reference))
        return NANV;

    const float p = interval(nyq);
    const float kf = std::round((reference - raw) / p);
    const int k = static_cast<int>(kf);

    if (std::abs(k) > MAX_FOLD) return NANV;

    fold = k;
    return raw + static_cast<float>(k) * p;
}

inline float robust_median(std::vector<float>& x) {
    if (x.empty()) return NANV;
    const size_t m = x.size() / 2;
    std::nth_element(x.begin(), x.begin() + m, x.end());
    float v = x[m];
    if ((x.size() & 1u) == 0u) {
        std::nth_element(x.begin(), x.begin() + m - 1, x.end());
        v = 0.5f * (v + x[m - 1]);
    }
    return v;
}

inline float huber_cost(float residual, float scale)
{
    const float r = std::fabs(residual);
    const float d = std::max(scale, 1.0e-3f);
    if (r <= d)
        return 0.5f * r * r / d;
    return r - 0.5f * d;
}

inline float median_abs_deviation(const std::vector<float>& x, float center) {
    if (x.empty()) return 0.0f;
    std::vector<float> d;
    d.reserve(x.size());
    for (float v : x) d.push_back(std::fabs(v - center));
    return robust_median(d);
}

struct NeighborList {
    std::array<int, 4> n{{-1, -1, -1, -1}};
};

static bool valid_gate(const Node& n) {
    return n.valid && std::isfinite(n.raw) && finite_positive(n.nyq);
}

// Circular distance on the Doppler ambiguity interval. A seam near 2*Vn is
// close to zero in this metric and therefore does not look like physical shear.
static float circular_velocity_distance(float a, float b, float nyq)
{
    if (!std::isfinite(a) || !std::isfinite(b) || !finite_positive(nyq))
        return NANV;

    const float p = interval(nyq);
    float d = std::fmod(std::fabs(a - b), p);
    if (d > nyq) d = p - d;
    return d;
}

static void detect_protected_shear(
    std::vector<Node>& nodes,
    const std::vector<NeighborList>& graph)
{
    const int n = static_cast<int>(nodes.size());
    std::vector<unsigned char> core(n, 0);

    for (int i = 0; i < n; ++i)
    {
        if (!valid_gate(nodes[i])) continue;

        float max_circular_frac = 0.0f;
        bool strong_opposite = false;

        for (int j : graph[i].n)
        {
            if (j < 0 || !valid_gate(nodes[j])) continue;
            const float en = std::min(nodes[i].nyq, nodes[j].nyq);
            if (!(en > 0.0f)) continue;

            const float cd = circular_velocity_distance(
                nodes[i].raw, nodes[j].raw, en);
            if (std::isfinite(cd))
                max_circular_frac = std::max(max_circular_frac, cd / en);

            const bool opposite =
                (nodes[i].raw >= 0.0f && nodes[j].raw < 0.0f) ||
                (nodes[i].raw < 0.0f && nodes[j].raw >= 0.0f);

            if (opposite &&
                std::fabs(nodes[i].raw) >= TVS_ABS_VELOCITY_FRAC * en &&
                std::fabs(nodes[j].raw) >= TVS_ABS_VELOCITY_FRAC * en &&
                std::fabs(nodes[i].raw - nodes[j].raw) >= TVS_RAW_JUMP_FRAC * en)
                strong_opposite = true;
        }

        core[i] = static_cast<unsigned char>(
            max_circular_frac >= HARD_SHEAR_CIRCULAR_FRAC ||
            strong_opposite);
    }

    size_t core_count = 0;
    for (unsigned char x : core) core_count += x != 0;
    const float frac =
        n > 0 ? static_cast<float>(core_count) / static_cast<float>(n) : 1.0f;

    // Dilate only genuinely compact hard features. A scan-wide shear line gets
    // no dilation, because there may be no trustworthy exterior to anchor it.
    if (frac <= 0.10f)
    {
        std::vector<unsigned char> dilated = core;
        for (int i = 0; i < n; ++i)
        {
            if (!core[i]) continue;
            for (int j : graph[i].n)
                if (j >= 0) dilated[j] = 1;
        }
        core.swap(dilated);
    }

    for (int i = 0; i < n; ++i)
        nodes[i].protected_shear = core[i] != 0;
}

struct VADProfile
{
    bool valid = false;
    float a = 0.0f;
    float b = 0.0f;
    float c = 0.0f;
    float rmse = NANV;
    float amplitude = NANV;
};

static bool solve_3x3(double m[3][4], double x[3])
{
    for (int col = 0; col < 3; ++col)
    {
        int pivot = col;
        for (int r = col + 1; r < 3; ++r)
            if (std::fabs(m[r][col]) > std::fabs(m[pivot][col])) pivot = r;
        if (std::fabs(m[pivot][col]) < 1.0e-10) return false;

        if (pivot != col)
            for (int k = col; k < 4; ++k) std::swap(m[pivot][k], m[col][k]);

        const double div = m[col][col];
        for (int k = col; k < 4; ++k) m[col][k] /= div;

        for (int r = 0; r < 3; ++r)
        {
            if (r == col) continue;
            const double f = m[r][col];
            if (std::fabs(f) < 1.0e-14) continue;
            for (int k = col; k < 4; ++k) m[r][k] -= f * m[col][k];
        }
    }
    x[0] = m[0][3]; x[1] = m[1][3]; x[2] = m[2][3];
    return true;
}

static VADProfile build_vad_profile(
    const std::vector<Node>& nodes,
    const std::vector<RayInfo>& rays,
    const std::vector<NeighborList>& graph)
{
    VADProfile out;
    if (rays.size() < static_cast<size_t>(VAD_MIN_RAYS)) return out;

    struct Sample { float az, value, nyq; int count; };
    std::vector<Sample> samples;
    samples.reserve(rays.size());

    for (const RayInfo& ray : rays)
    {
        const size_t begin = ray.start;
        const size_t end = ray.start + ray.count;
        const size_t stride = std::max<size_t>(1, (ray.count + 39) / 40);
        std::vector<float> vals;
        vals.reserve(40);

        for (size_t g = begin; g < end; g += stride)
        {
            const Node& node = nodes[g];
            if (!valid_gate(node) || node.protected_shear) continue;
            if (std::fabs(node.raw) > 0.85f * node.nyq) continue;

            bool clean = true;
            for (int nb : graph[g].n)
            {
                if (nb < 0 || !valid_gate(nodes[nb])) continue;
                const float en = std::min(node.nyq, nodes[nb].nyq);
                const float cd = circular_velocity_distance(node.raw, nodes[nb].raw, en);
                if (std::isfinite(cd) && cd > 0.35f * en) { clean = false; break; }
            }
            if (clean) vals.push_back(node.raw);
        }

        if (vals.size() < 4) continue;
        const float med = robust_median(vals);
        if (std::isfinite(med))
            samples.push_back({ray.azimuth, med, ray.nyquist, static_cast<int>(vals.size())});
    }

    if (samples.size() < static_cast<size_t>(VAD_MIN_RAYS) ||
        static_cast<float>(samples.size()) / static_cast<float>(rays.size()) < VAD_MIN_COVERAGE)
        return VADProfile{};

    double x[3] = {0.0, 0.0, 0.0};
    for (int iter = 0; iter < 2; ++iter)
    {
        double m[3][4] = {{0,0,0,0},{0,0,0,0},{0,0,0,0}};
        for (const Sample& s : samples)
        {
            const double az = s.az * 3.14159265358979323846 / 180.0;
            const double f[3] = {1.0, std::cos(az), std::sin(az)};
            const double residual = s.value - (x[0] + x[1]*f[1] + x[2]*f[2]);
            const double scale = std::max(1.5, 0.25 * static_cast<double>(s.nyq));
            const double ar = std::fabs(residual);
            const double hw = ar <= scale ? 1.0 : scale / ar;
            const double w = hw * std::min(1.0, s.count / 12.0);
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) m[r][c] += w*f[r]*f[c];
                m[r][3] += w*f[r]*s.value;
            }
        }
        if (!solve_3x3(m, x)) return VADProfile{};
    }

    double sse = 0.0, wn = 0.0;
    for (const Sample& s : samples)
    {
        const double az = s.az * 3.14159265358979323846 / 180.0;
        const double pred = x[0] + x[1]*std::cos(az) + x[2]*std::sin(az);
        const double r = pred - s.value;
        const double w = std::min(1.0, s.count / 12.0);
        sse += w*r*r;
        wn += w;
    }
    if (wn <= 0.0) return VADProfile{};

    out.rmse = static_cast<float>(std::sqrt(sse / wn));
    out.amplitude = static_cast<float>(std::sqrt(x[1]*x[1] + x[2]*x[2]));
    const float nyq = samples[samples.size()/2].nyq;
    if (out.rmse > VAD_MAX_RMSE_FRAC * std::max(1.0f, nyq) ||
        out.amplitude > VAD_MAX_AMPLITUDE_FRAC * std::max(1.0f, nyq))
        return VADProfile{};

    out.valid = true;
    out.a = static_cast<float>(x[0]);
    out.b = static_cast<float>(x[1]);
    out.c = static_cast<float>(x[2]);
    return out;
}

static inline float vad_predict(const VADProfile& vad, float azimuth_deg)
{
    const float az = azimuth_deg * 3.14159265358979323846f / 180.0f;
    return vad.a + vad.b*std::cos(az) + vad.c*std::sin(az);
}

/*
 * Estimate a local physical velocity prediction from already solved nodes.
 *
 * We intentionally use a robust median rather than a mean. Around a tornado
 * one neighbor can be on the opposite side of a very strong shear zone and
 * should not drag the reference across the vortex.
 */
/*
 * Predict the value at i from solved neighbors. If two neighbors lie along
 * the same direction, estimate a first derivative and extrapolate it. This
 * prevents a real tornado shear layer from being flattened into a constant
 * velocity field.
 */
static float directional_prediction(
    int i,
    const std::vector<Node>& nodes,
    const std::vector<NeighborList>& graph,
    const std::vector<std::array<float, 4>>& distance)
{
    std::vector<float> predictions;

    for (int d = 0; d < 4; ++d) {
        const int j = graph[i].n[d];
        if (j < 0 || !nodes[j].solved) continue;

        float pred = nodes[j].value;

        // Opposite neighbor: use a linear derivative when both sides are known.
        const int od = (d == 0 ? 1 : d == 1 ? 0 : d == 2 ? 3 : 2);
        const int k = graph[j].n[od];

        if (k >= 0 && nodes[k].solved) {
            const float h1 = std::max(distance[i][d], 1e-3f);
            const float h2 = std::max(distance[j][od], 1e-3f);
            const float grad = (nodes[j].value - nodes[k].value) / (h1 + h2);
            pred = nodes[j].value + grad * h1;
        }

        predictions.push_back(pred);
    }

    if (predictions.empty()) return NANV;
    return robust_median(predictions);
}

/*
 * Score a candidate unfolded velocity.
 *
 * The loss is deliberately asymmetric:
 *   - small residuals are strongly preferred;
 *   - residuals up to roughly one Nyquist are still possible in a strong
 *     shear zone;
 *   - isolated huge residuals are rejected unless another independent
 *     neighbor confirms the same fold.
 */
static float candidate_cost(
    float candidate,
    float prediction,
    float nyq,
    float local_mad,
    int support)
{
    if (!std::isfinite(candidate) || !std::isfinite(prediction))
        return std::numeric_limits<float>::infinity();

    const float scale =
        std::max(1.5f, std::max(0.12f * nyq, 2.5f * local_mad));

    const float r = std::fabs(candidate - prediction);

    // Huber-like loss: don't catastrophically penalize legitimate shear.
    float loss;
    if (r <= scale) {
        loss = 0.5f * r * r / scale;
    } else {
        loss = r - 0.5f * scale;
    }

    // Independent support is extremely valuable around folds.
    if (support >= 2) loss *= 0.45f;
    else if (support == 1) loss *= 0.75f;

    return loss;
}

/*
 * Pick the fold for a node. The important difference from a simple
 * "nearest neighbor" dealiaser is that we evaluate ALL plausible folds and
 * require agreement from the surrounding solved field.
 */
static bool solve_node(
    int i,
    std::vector<Node>& nodes,
    const std::vector<NeighborList>& graph,
    const std::vector<std::array<float, 4>>& distance)
{
    if (!valid_gate(nodes[i])) return false;

    std::vector<float> refs;
    refs.reserve(8);

    for (int j : graph[i].n) {
        if (j >= 0 && nodes[j].solved)
            refs.push_back(nodes[j].value);
    }

    if (refs.empty()) return false;

    const float med = robust_median(refs);
    const float mad = median_abs_deviation(refs, med);
    const float prediction = directional_prediction(i, nodes, graph, distance);

    const float nyq = nodes[i].nyq;

    float best_cost = std::numeric_limits<float>::infinity();
    float second_cost = std::numeric_limits<float>::infinity();
    float best_value = NANV;
    int best_fold = 0;
    int best_support = 0;

    for (int k = -MAX_FOLD; k <= MAX_FOLD; ++k) {
        const float candidate =
            nodes[i].raw + static_cast<float>(k) * interval(nyq);

        if (!std::isfinite(candidate)) continue;

        int support = 0;
        float support_error = 0.0f;

        // Test this candidate against each solved neighbor using the neighbor
        // Nyquist. A neighbor need not have the same Nyquist.
        for (int j : graph[i].n) {
            if (j < 0 || !nodes[j].solved) continue;

            const float edge_nyq = std::min(nyq, nodes[j].nyq);
            const float diff = std::fabs(candidate - nodes[j].value);

            // A difference below one full folding interval is possible in a
            // genuine high-shear zone. What matters is whether the candidate
            // is also compatible with the local prediction.
            if (diff <= 1.10f * interval(edge_nyq))
                ++support;

            support_error +=
                std::min(diff, 2.0f * interval(edge_nyq));
        }

        float cost = 0.0f;

        if (std::isfinite(prediction)) {
            cost += candidate_cost(
                candidate, prediction, nyq, mad, support);
        } else {
            cost += support_error /
                    std::max(1, static_cast<int>(refs.size()));
        }

        // Prefer the smallest absolute fold only when the local evidence is
        // otherwise tied. This prevents arbitrary drift in disconnected data.
        cost += 0.015f * std::abs(k) * std::max(1.0f, nyq);

        if (cost < best_cost) {
            second_cost = best_cost;
            best_cost = cost;
            best_value = candidate;
            best_fold = k;
            best_support = support;
        } else if (cost < second_cost) {
            second_cost = cost;
        }
    }

    if (!std::isfinite(best_value)) return false;

    /*
     * Ambiguity test.
     *
     * A tornado can legitimately have large residuals, so don't use an
     * absolute velocity threshold here. Instead compare the best and second
     * best fold hypotheses. If they are almost identical in cost, defer the
     * gate until a second pass provides more information.
     */
    const float ambiguity =
        second_cost / std::max(best_cost, 1e-4f);

    if (best_support == 0) return false;

    if (best_support == 1 && ambiguity < 1.08f)
        return false;

    nodes[i].value = best_value;
    nodes[i].solved = true;

    // Confidence is deliberately continuous.
    const float support_score =
        std::min(1.0f, best_support / 3.0f);
    const float ambiguity_score =
        std::min(1.0f, std::max(0.0f, (ambiguity - 1.0f) / 0.5f));

    nodes[i].confidence =
        0.5f * support_score + 0.5f * ambiguity_score;

    (void)best_fold;
    return true;
}

/*
 * Find seeds using local continuity without assuming the storm is globally
 * centered on zero. Gates near a local zero crossing are particularly useful
 * because their absolute fold is usually identifiable from neighboring gates.
 *
 * We deliberately select MANY seeds. A single global seed is dangerous when
 * a tornado creates a disconnected high-shear region.
 */
static std::vector<int> find_seeds(
    const std::vector<Node>& nodes,
    const std::vector<NeighborList>& graph)
{
    struct Candidate {
        float q;
        int i;

        bool operator<(const Candidate& o) const {
            return q < o.q;
        }
    };

    std::vector<Candidate> candidates;

    for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
        if (!valid_gate(nodes[i])) continue;
        if (nodes[i].protected_shear) continue;

        int valid_neighbors = 0;
        float local_mean = 0.0f;
        std::vector<float> observed;

        for (int j : graph[i].n) {
            if (j < 0 || !valid_gate(nodes[j])) continue;
            ++valid_neighbors;
            observed.push_back(nodes[j].raw);
        }

        if (valid_neighbors == 0) continue;

        local_mean = robust_median(observed);
        const float mad = median_abs_deviation(observed, local_mean);

        // A seed should be locally coherent, but we do not require low
        // absolute velocity: a tornado can have a high-speed coherent lobe.
        const float residual = std::fabs(nodes[i].raw - local_mean);

        float q =
            2.0f * valid_neighbors
            - residual / std::max(2.0f, nodes[i].nyq * 0.5f)
            - mad / std::max(2.0f, nodes[i].nyq * 0.25f);

        // Mild preference for values near zero breaks the unavoidable global
        // folding ambiguity when several equally coherent solutions exist.
        q += 1.5f *
             std::exp(-std::fabs(nodes[i].raw) /
                      std::max(1.0f, 0.35f * nodes[i].nyq));

        candidates.push_back({q, i});
    }

    std::sort(candidates.begin(), candidates.end());

    std::vector<int> seeds;
    const int desired =
        std::max(8, static_cast<int>(candidates.size() / 500));

    // Spatially separate seeds so one smooth region doesn't consume all seeds.
    constexpr float MIN_AZ_SEED_DISTANCE = 2.0f;
    constexpr float MIN_RANGE_SEED_DISTANCE = 2.0f;

    for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
        const int i = it->i;

        bool too_close = false;
        for (int s : seeds) {
            if (std::fabs(nodes[i].range - nodes[s].range) <=
                    MIN_RANGE_SEED_DISTANCE &&
                az_distance(nodes[i].azimuth, nodes[s].azimuth) <=
                    MIN_AZ_SEED_DISTANCE) {
                too_close = true;
                break;
            }
        }

        if (!too_close)
            seeds.push_back(i);

        if (static_cast<int>(seeds.size()) >= desired)
            break;
    }

    return seeds;
}

static void dealias_tilt(SingleTilt& tilt)
{
    std::vector<float>& packed = tilt.Radials_VEL;

    if (packed.size() < 24 || packed.size() % 3 != 0)
        return;

    const size_t count = packed.size() / 3;
    if (count > static_cast<size_t>(
                    std::numeric_limits<int>::max()))
        return;

    /*
     * FIX 1:
     * Always establish a valid fallback Nyquist. The previous implementation
     * only used the fallback while constructing rays, which could leave an
     * already-populated VelocityRays array with NaN Nyquists.
     */
    const float fallback_nyq =
        static_cast<float>(tilt.vol_el_rad.rad.nyquist_vel) / 100.0f;

    if (!finite_positive(fallback_nyq))
        return;

    // ------------------------------------------------------------
    // Build ray descriptors.
    // ------------------------------------------------------------
    constexpr float AZ_EPS = 0.01f;

    std::vector<RayInfo> rays;

    if (!tilt.VelocityRays.empty()) {
        rays.reserve(tilt.VelocityRays.size());

        for (const auto& r : tilt.VelocityRays) {
            RayInfo x;
            x.start = r.start;
            x.count = r.count;
            x.nyquist =
                finite_positive(r.nyquist) ? r.nyquist : fallback_nyq;
            x.gate_spacing = r.gateSpacing;

            if (!finite_positive(x.gate_spacing))
                x.gate_spacing = tilt.gateSpacing;

            if (!finite_positive(x.gate_spacing))
                return;

            if (x.start >= count ||
                x.count == 0 ||
                x.count > count - x.start)
                return;

            x.azimuth = packed[3 * x.start];

            if (!std::isfinite(x.azimuth))
                return;

            x.azimuth = wrap360(x.azimuth);
            rays.push_back(x);
        }
    } else {
        size_t start = 0;

        while (start < count) {
            size_t end = start + 1;

            while (end < count) {
                const float a0 = packed[3 * (end - 1)];
                const float a1 = packed[3 * end];
                const float r0 = packed[3 * (end - 1) + 1];
                const float r1 = packed[3 * end + 1];

                if (!std::isfinite(a1) ||
                    !std::isfinite(r1))
                    break;

                if (std::fabs(a1 - a0) > AZ_EPS ||
                    r1 <= r0)
                    break;

                ++end;
            }

            RayInfo x;
            x.start = start;
            x.count = end - start;
            x.azimuth = wrap360(packed[3 * start]);
            x.nyquist = fallback_nyq;
            x.gate_spacing = tilt.gateSpacing;

            if (!finite_positive(x.gate_spacing)) {
                if (x.count >= 2) {
                    x.gate_spacing =
                        packed[3 * (start + 1) + 1] -
                        packed[3 * start + 1];
                }
            }

            if (!finite_positive(x.gate_spacing))
                return;

            rays.push_back(x);
            start = end;
        }
    }

    if (rays.size() < 4)
        return;

    /*
     * FIX 2:
     * Validate that rays cover the packed array contiguously.
     */
    size_t expected = 0;

    for (auto& r : rays) {
        if (r.start != expected ||
            r.count == 0 ||
            r.count > count - expected)
            return;

        expected += r.count;

        for (size_t t = r.start; t < r.start + r.count; ++t) {
            if (!std::isfinite(packed[3 * t + 1]))
                continue;

            if (t > r.start) {
                const float dr =
                    packed[3 * t + 1] -
                    packed[3 * (t - 1) + 1];

                if (std::isfinite(dr) &&
                    dr <= 0.0f)
                    return;
            }
        }
    }

    if (expected != count)
        return;

    // ------------------------------------------------------------
    // Nodes.
    // ------------------------------------------------------------
    std::vector<Node> nodes(count);

    for (size_t t = 0; t < count; ++t) {
        const size_t r = [&]() -> size_t {
            // Binary search over ray ranges.
            size_t lo = 0, hi = rays.size();
            while (lo + 1 < hi) {
                const size_t m = (lo + hi) / 2;
                if (rays[m].start <= t)
                    lo = m;
                else
                    hi = m;
            }
            return lo;
        }();

        nodes[t].raw = packed[3 * t + 2];
        nodes[t].value = nodes[t].raw;
        nodes[t].nyq = rays[r].nyquist;
        nodes[t].azimuth = rays[r].azimuth;
        nodes[t].range = packed[3 * t + 1];
        nodes[t].valid =
            std::isfinite(nodes[t].raw) &&
            std::isfinite(nodes[t].range) &&
            finite_positive(nodes[t].nyq);
    }

    // ------------------------------------------------------------
    // Build 4-neighbor graph.
    //
    // Unlike the old implementation, missing gates are simply missing edges;
    // they don't cause the entire dealiasing problem to collapse.
    // ------------------------------------------------------------
    std::vector<NeighborList> graph(count);
    std::vector<std::array<float, 4>> distance(
        count, {1.0f, 1.0f, 1.0f, 1.0f});

    auto connect = [&](int a, int b, int da, float dist) {
        if (a < 0 || b < 0 ||
            !valid_gate(nodes[a]) ||
            !valid_gate(nodes[b]))
            return;

        graph[a].n[da] = b;

        const int db =
            (da == 0 ? 1 :
             da == 1 ? 0 :
             da == 2 ? 3 : 2);

        graph[b].n[db] = a;

        distance[a][da] = std::max(dist, 1e-3f);
        distance[b][db] = std::max(dist, 1e-3f);
    };

    // Range neighbors.
    for (size_t r = 0; r < rays.size(); ++r) {
        const auto& ray = rays[r];

        for (size_t t = ray.start + 1;
             t < ray.start + ray.count;
             ++t) {

            const float dr =
                packed[3 * t + 1] -
                packed[3 * (t - 1) + 1];

            if (std::isfinite(dr) &&
                dr > 0.0f &&
                dr <= 1.5f * ray.gate_spacing) {

                connect(
                    static_cast<int>(t - 1),
                    static_cast<int>(t),
                    0,
                    dr);
            }
        }
    }

    /*
     * Azimuth neighbors.
     *
     * Determine the normal scan spacing from the median of observed gaps.
     * This prevents a sector boundary or missing radial from being treated as
     * a legitimate wrap edge.
     */
    std::vector<float> az_gaps;

    for (size_t r = 1; r < rays.size(); ++r) {
        const float g =
            rays[r].azimuth - rays[r - 1].azimuth;

        if (g > AZ_EPS && g < 10.0f)
            az_gaps.push_back(g);
    }

    float median_gap =
        az_gaps.empty() ? 1.0f : robust_median(az_gaps);

    const float max_normal_gap =
        std::max(1.5f, 1.8f * median_gap);

    for (size_t r = 1; r < rays.size(); ++r) {
        const float gap =
            rays[r].azimuth - rays[r - 1].azimuth;

        if (gap <= AZ_EPS ||
            gap > max_normal_gap)
            continue;

        const auto& a = rays[r - 1];
        const auto& b = rays[r];

        size_t ia = a.start;
        size_t ib = b.start;

        while (ia < a.start + a.count &&
               ib < b.start + b.count) {

            const float ra = packed[3 * ia + 1];
            const float rb = packed[3 * ib + 1];

            const float dr = ra - rb;

            if (std::fabs(dr) <= 0.5f * median_gap) {
                const float arc =
                    std::max(
                        0.001f,
                        0.5f * (ra + rb) *
                        gap * 3.14159265358979323846f /
                        180.0f);

                connect(
                    static_cast<int>(ia),
                    static_cast<int>(ib),
                    2,
                    arc);

                ++ia;
                ++ib;
            } else if (dr < 0.0f) {
                ++ia;
            } else {
                ++ib;
            }
        }
    }

    /*
     * Full-circle wrap.
     */
    if (rays.size() >= 8) {
        const float wrap_gap =
            rays.front().azimuth +
            360.0f -
            rays.back().azimuth;

        if (wrap_gap > AZ_EPS &&
            wrap_gap <= max_normal_gap) {

            const auto& a = rays.back();
            const auto& b = rays.front();

            size_t ia = a.start;
            size_t ib = b.start;

            while (ia < a.start + a.count &&
                   ib < b.start + b.count) {

                const float ra = packed[3 * ia + 1];
                const float rb = packed[3 * ib + 1];

                if (std::fabs(ra - rb) <=
                    0.5f * median_gap) {

                    const float arc =
                        std::max(
                            0.001f,
                            0.5f * (ra + rb) *
                            wrap_gap *
                            3.14159265358979323846f /
                            180.0f);

                    connect(
                        static_cast<int>(ia),
                        static_cast<int>(ib),
                        2,
                        arc);

                    ++ia;
                    ++ib;
                } else if (ra < rb) {
                    ++ia;
                } else {
                    ++ib;
                }
            }
        }
    }

    // ------------------------------------------------------------
    // v8 protection and optional absolute reference.
    // ------------------------------------------------------------
    // No polarimetric field is allowed to vote for a fold here. That keeps
    // this fast path deterministic even when auxiliary moments are sparse or
    // from a split cut.
    detect_protected_shear(nodes, graph);
    const VADProfile vad = build_vad_profile(nodes, rays, graph);

    // ------------------------------------------------------------
    // PASS 0: initialize many independent high-quality seeds.
    // ------------------------------------------------------------
    const std::vector<int> seeds =
        find_seeds(nodes, graph);

    for (int s : seeds) {
        nodes[s].value = nodes[s].raw;
        nodes[s].solved = true;
        nodes[s].confidence = 1.0f;
    }

    /*
     * Priority queue of candidate boundary nodes.
     */
    std::priority_queue<QueueItem> pq;
    std::vector<int> generation(count, 0);

    auto enqueue_neighbors = [&](int i) {
        for (int j : graph[i].n) {
            if (j < 0 || nodes[j].solved)
                continue;

            int solved_neighbors = 0;
            for (int k : graph[j].n) {
                if (k >= 0 && nodes[k].solved)
                    ++solved_neighbors;
            }

            if (solved_neighbors == 0)
                continue;

            /*
             * More solved neighbors = better candidate.
             * Local shear is not penalized heavily because it may be the
             * tornado signature itself.
             */
            float score =
                2.0f * solved_neighbors;

            std::vector<float> refs;
            for (int k : graph[j].n)
                if (k >= 0 && nodes[k].solved)
                    refs.push_back(nodes[k].value);

            if (!refs.empty()) {
                const float med = robust_median(refs);
                const float mad =
                    median_abs_deviation(refs, med);

                score +=
                    1.0f /
                    (1.0f + mad /
                     std::max(1.0f, nodes[j].nyq));
            }

            ++generation[j];
            pq.push({
                score,
                j,
                generation[j]
            });
        }
    };

    for (int s : seeds)
        enqueue_neighbors(s);

    /*
     * PASS 1: strict quality-guided growth.
     */
    while (!pq.empty()) {
        const QueueItem item = pq.top();
        pq.pop();

        const int i = item.node;

        if (nodes[i].solved ||
            item.generation != generation[i])
            continue;

        if (solve_node(i, nodes, graph, distance))
            enqueue_neighbors(i);
    }

    /*
     * PASS 2:
     *
     * Revisit every unsolved gate. At this point many more neighbors have
     * been solved, so gates that were ambiguous during pass 1 often become
     * unambiguous.
     */
    for (int iteration = 0; iteration < 4; ++iteration) {
        bool progress = false;

        for (int i = 0; i < static_cast<int>(count); ++i) {
            if (nodes[i].solved || !valid_gate(nodes[i]))
                continue;

            if (solve_node(i, nodes, graph, distance)) {
                progress = true;
            }
        }

        if (!progress)
            break;
    }

    /*
     * PASS 3:
     *
     * Isolated components need their own reference. For these components we
     * cannot know the absolute fold from velocity alone. The only physically
     * defensible local choice is the hypothesis that minimizes the internal
     * discontinuity. We therefore seed the component at its most coherent
     * gate and grow it exactly as above.
     */
    for (;;) {
        int seed = -1;
        float best_q = -std::numeric_limits<float>::infinity();

        for (int i = 0; i < static_cast<int>(count); ++i) {
            if (nodes[i].solved || !valid_gate(nodes[i]))
                continue;

            if (nodes[i].protected_shear)
                continue;

            int degree = 0;
            for (int j : graph[i].n)
                if (j >= 0 && !nodes[j].solved)
                    ++degree;

            if (degree == 0)
                continue;

            std::vector<float> raw_neighbors;

            for (int j : graph[i].n) {
                if (j >= 0 && !nodes[j].solved &&
                    valid_gate(nodes[j]))
                    raw_neighbors.push_back(nodes[j].raw);
            }

            const float med =
                raw_neighbors.empty()
                    ? nodes[i].raw
                    : robust_median(raw_neighbors);

            const float mad =
                raw_neighbors.empty()
                    ? 0.0f
                    : median_abs_deviation(raw_neighbors, med);

            const float q =
                static_cast<float>(degree)
                - std::fabs(nodes[i].raw - med) /
                      std::max(2.0f, nodes[i].nyq * 0.5f)
                - mad /
                      std::max(2.0f, nodes[i].nyq * 0.25f);

            if (q > best_q) {
                best_q = q;
                seed = i;
            }
        }

        if (seed < 0)
            break;

        nodes[seed].solved = true;

        int seed_fold = 0;
        if (vad.valid)
        {
            const float target = vad_predict(vad, nodes[seed].azimuth);
            const float p = interval(nodes[seed].nyq);
            const int k = std::max(-MAX_FOLD, std::min(MAX_FOLD,
                static_cast<int>(std::round((target - nodes[seed].raw) / p))));
            const float anchored = nodes[seed].raw + static_cast<float>(k) * p;
            if (std::fabs(anchored - target) <= 0.55f * nodes[seed].nyq)
                seed_fold = k;
        }

        nodes[seed].value =
            nodes[seed].raw + static_cast<float>(seed_fold) * interval(nodes[seed].nyq);
        nodes[seed].confidence =
            (vad.valid && seed_fold != 0) ? 0.40f : 0.25f;

        while (true) {
            bool progress = false;

            for (int i = 0; i < static_cast<int>(count); ++i) {
                if (nodes[i].solved || !valid_gate(nodes[i]))
                    continue;

                if (solve_node(i, nodes, graph, distance))
                    progress = true;
            }

            if (!progress)
                break;
        }
    }

    /*
     * FINAL CONSISTENCY PASS
     *
     * This is intentionally more conservative than the old v2 cleanup near
     * severe shear: protected gates AND their immediate neighbors are excluded.
     */
    for (int i = 0; i < static_cast<int>(count); ++i) {
        if (!nodes[i].solved || !valid_gate(nodes[i]))
            continue;
        if (nodes[i].protected_shear)
            continue;

        std::vector<float> refs;
        bool touches_protected = false;

        for (int j : graph[i].n) {
            if (j < 0 || !nodes[j].solved) continue;
            if (nodes[j].protected_shear) {
                touches_protected = true;
                continue;
            }
            refs.push_back(nodes[j].value);
        }

        if (touches_protected || refs.size() < 3)
            continue;

        const float prediction = robust_median(refs);
        const float mad = median_abs_deviation(refs, prediction);
        const float scale = std::max(
            1.5f,
            std::max(0.12f * nodes[i].nyq, 2.5f * mad));

        float best = nodes[i].value;
        float best_cost = std::numeric_limits<float>::infinity();

        for (int k = -MAX_FOLD; k <= MAX_FOLD; ++k) {
            const float candidate =
                nodes[i].raw + static_cast<float>(k) * interval(nodes[i].nyq);

            float cost = 0.0f;
            int support = 0;
            for (int j : graph[i].n) {
                if (j < 0 || !nodes[j].solved || nodes[j].protected_shear)
                    continue;
                const float en = std::min(nodes[i].nyq, nodes[j].nyq);
                const float d = std::fabs(candidate - nodes[j].value);
                cost += std::min(d, 2.0f * interval(en));
                if (d <= 0.50f * en) ++support;
            }

            if (support < 2) continue;
            cost += 0.35f * huber_cost(
                std::fabs(candidate - prediction), scale);

            if (cost < best_cost) {
                best_cost = cost;
                best = candidate;
            }
        }

        const float old_error = std::fabs(nodes[i].value - prediction);
        const float new_error = std::fabs(best - prediction);

        if (best != nodes[i].value &&
            new_error + 0.50f * std::max(1.0f, mad) < old_error)
            nodes[i].value = best;
    }


    // -----------------------------------------------------------------------
    // SURGICAL TORNADO / HIGH-SHEAR RESCUE
    // -----------------------------------------------------------------------
    // v2 remains the baseline. This overlay can move only a few protected
    // gates, and only when a +/-1 fold shift improves agreement with at least
    // two non-protected neighbors on both true graph axes. No extrapolation,
    // no whole-region shift, and no global re-centering are permitted here.
    {
        size_t valid_count = 0;
        size_t protected_count = 0;
        for (const Node& node : nodes) {
            if (!valid_gate(node)) continue;
            ++valid_count;
            protected_count += node.protected_shear ? 1u : 0u;
        }

        const size_t max_rescue = std::min(
            MAX_RESCUE_GATES_ABS,
            std::max<size_t>(1,
                static_cast<size_t>(
                    MAX_RESCUE_FRACTION *
                    static_cast<double>(std::max<size_t>(1, valid_count)))));

        // Do not "rescue" a field in which the hard mask itself dominates.
        if (protected_count <= max_rescue * 2 &&
            protected_count <= static_cast<size_t>(
                0.12f * static_cast<float>(std::max<size_t>(1, valid_count))))
        {
            size_t rescued = 0;

            for (int i = 0;
                 i < static_cast<int>(count) && rescued < max_rescue;
                 ++i)
            {
                if (!nodes[i].solved || !nodes[i].protected_shear)
                    continue;

                const float p = interval(nodes[i].nyq);
                if (!(p > 0.0f)) continue;

                double cost[3] = {0.0, 0.0, 0.0};
                int support[3] = {0, 0, 0};
                unsigned dirs[3] = {0u, 0u, 0u};

                for (int d = 0; d < 4; ++d)
                {
                    const int j = graph[i].n[d];
                    if (j < 0 || !nodes[j].solved || nodes[j].protected_shear)
                        continue;

                    const float en = std::min(nodes[i].nyq, nodes[j].nyq);
                    const float scale =
                        std::max(1.5f, RESCUE_EDGE_SCALE_FRAC * en);

                    for (int shift = -1; shift <= 1; ++shift)
                    {
                        const int q = shift + 1;
                        const float candidate =
                            nodes[i].value + static_cast<float>(shift) * p;
                        cost[q] += huber_cost(
                            candidate - nodes[j].value,
                            scale);

                        if (std::fabs(candidate - nodes[j].value) <= 0.55f * en) {
                            ++support[q];
                            dirs[q] |= 1u << static_cast<unsigned>(d);
                        }
                    }
                }

                // VAD only breaks ties. It cannot dominate a tornadic core.
                if (vad.valid)
                {
                    const float target = vad_predict(vad, nodes[i].azimuth);
                    for (int shift = -1; shift <= 1; ++shift)
                    {
                        const int q = shift + 1;
                        const float candidate =
                            nodes[i].value + static_cast<float>(shift) * p;
                        cost[q] += RESCUE_VAD_WEIGHT * huber_cost(
                            candidate - target,
                            std::max(2.0f, 0.40f * nodes[i].nyq));
                    }
                }

                int best_q = 1;
                for (int q = 0; q < 3; ++q)
                    if (cost[q] < cost[best_q]) best_q = q;

                const int shift = best_q - 1;
                if (shift == 0 || support[best_q] < RESCUE_MIN_SUPPORT)
                    continue;

                // Explicit axis types from direction bits; there is no
                // positional interpretation of a neighbor array here.
                int axis_types = 0;
                if (dirs[best_q] & ((1u << 0) | (1u << 1))) ++axis_types;
                if (dirs[best_q] & ((1u << 2) | (1u << 3))) ++axis_types;
                if (axis_types < 2)
                    continue;

                const double old_cost = cost[1];
                const double new_cost = cost[best_q];
                if (!(old_cost > 0.0) || !std::isfinite(static_cast<float>(new_cost)))
                    continue;

                const double improvement = (old_cost - new_cost) / old_cost;
                if (improvement < RESCUE_MIN_IMPROVEMENT)
                    continue;

                nodes[i].value += static_cast<float>(shift) * p;
                ++rescued;
            }
        }
    }

    /*
     * Absolute-fold ambiguity:
     *
     * Velocity-only dealiasing can never determine an arbitrary global
     * multiple of 2*Nyquist. We therefore DO NOT perform the old
     * component-wide "center fold" subtraction. That operation can shift a
     * small tornado component based on the dominant environmental fold.
     *
     * The seeds above establish the local zero/low-velocity reference.
     */

    // ------------------------------------------------------------
    // Write results.
    // Missing gates remain untouched.
    // ------------------------------------------------------------
    for (size_t t = 0; t < count; ++t) {
        if (nodes[t].solved &&
            std::isfinite(nodes[t].value)) {
            packed[3 * t + 2] = nodes[t].value;
        }
    }
}

} // namespace

void dealias_velocity_volume_v8(AllTilt& volume)
{
    for (auto& tilt : volume.Tilts)
        dealias_tilt(tilt);
}


