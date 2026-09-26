#include <string>
#include "velocity_dealias.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <queue>
#include <vector>

// ---------------------------------------------------------------------------
// Velocity Dealiasing v7.0
//
// Tornado-preserving region-based Doppler velocity dealiasing.
//
// Main changes from v6.2:
//   1. NO global min-|V| component anchoring.
//      Absolute fold ambiguity is never resolved by assuming velocities
//      should be centered around zero.
//
//   2. Explicit low-to-high elevation ordering.
//
//   3. Prefer same-cut RHOHV; split-cut RHOHV is only a fallback.
//
//   4. Nyquist compatibility uses a tolerance instead of exact float
//      equality.
//
//   5. Vertical and same-tilt evidence use separate robust residual scales.
//
//   6. Fold confidence uses hypothesis margin, absolute residual quality,
//      evidence support, and region size.
//
//   7. Ambiguous regions are allowed to remain raw rather than receiving
//      an arbitrary fold-0 anchor.
//
//   8. Conservative repair never modifies high-shear / TVS-like regions.
//
//   9. Vertical continuity is evidence, not an unconditional truth.
//
//   10. TDS/RHOHV modifies uncertainty only; it never votes for a fold.
//
// Complexity remains approximately O(n log n) per tilt due to the
// best-first region propagation. Alignment passes are O(n).
// ---------------------------------------------------------------------------

namespace {

constexpr float VELOCITY_NAN =
    std::numeric_limits<float>::quiet_NaN();

constexpr int MAX_FOLD = 6;
constexpr float AZ_EPS = 0.01f;

// Fast-path region segmentation. High-shear gates are excluded before this
// stage, so the threshold can be substantially more permissive than v2/v3.
constexpr float SEG_FRAC = 0.55f;

// R2D2-style anomalous-shear detector. This is deliberately separate from
// the region segmentation threshold. A large jump may be either aliasing or
// real meteorological shear, so the surrounding area is protected first.
constexpr float HIGH_SHEAR_FRAC = 0.80f;
constexpr int HIGH_SHEAR_BUFFER_HOPS = 2; // approximately a 5x5 neighborhood

// Hard-region acceptance. Core high-shear gates need independent support.
constexpr int HARD_MIN_SUPPORT_CORE = 2;
constexpr int HARD_MIN_AXIS_TYPES_CORE = 2;
constexpr float HARD_MIN_CONFIDENCE = 0.18f;
constexpr float HARD_RELAX_MARGIN = 0.12f;
constexpr float HARD_CORE_EDGE_WEIGHT = 0.30f;
constexpr float HARD_BUFFER_EDGE_WEIGHT = 0.70f;

constexpr int REPAIR_PASSES = 2;

// Evidence weights.
constexpr float VERTICAL_WEIGHT = 1.25f;

// Cross-tilt ground-range matching tolerance.
constexpr float VERTICAL_KEY_TOL_M = 300.0f;

// Split-cut matching.
constexpr float SPLIT_CUT_ANGLE_EPS = 0.10f;
constexpr long long SPLIT_CUT_TIME_EPS_MS = 30000LL;

// Polarimetric quality.
constexpr float RHO_TDS_THRESHOLD = 0.80f;

// TDS widens uncertainty; it does NOT increase confidence.
constexpr float RHO_TDS_SCALE_MULTIPLIER = 1.8f;

// Fold acceptance.
constexpr float CONFIDENCE_ACCEPT = 0.20f;
constexpr float CONFIDENCE_STRONG = 0.55f;

// Weak-reflectivity protection.
constexpr float MIN_REF_FRACTION_FOR_FOLD = 0.30f;
constexpr int MIN_HORIZONTAL_SUPPORT_FOR_FOLD = 3;
constexpr int MIN_VERTICAL_SUPPORT_FOR_FOLD = 2;
constexpr size_t MIN_REGION_SIZE_FOR_FOLD = 4;

// Repair.
constexpr float REPAIR_HYSTERESIS = 0.80f;

// Effective Earth radius for beam geometry.
constexpr double EARTH_RADIUS_EFFECTIVE_M =
    6371000.0 * 4.0 / 3.0;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

inline float wrap360(float x)
{
    x = std::fmod(x, 360.0f);
    return x < 0.0f ? x + 360.0f : x;
}

inline float az_distance(float a, float b)
{
    const float d = std::fabs(a - b);
    return std::min(d, 360.0f - d);
}

inline bool compatible_nyquist(float a, float b)
{
    if (!std::isfinite(a) || !std::isfinite(b) ||
        a <= 0.0f || b <= 0.0f)
        return false;

    const float scale =
        std::max({1.0f, std::fabs(a), std::fabs(b)});

    return std::fabs(a - b) <= 0.02f * scale;
}

inline float huber_cost(float residual, float scale)
{
    const float r = std::fabs(residual);
    scale = std::max(scale, 1.0e-3f);

    if (r <= scale)
        return 0.5f * r * r / scale;

    return r - 0.5f * scale;
}

// Standard 4/3-earth-radius beam geometry.
inline void beam_height_ground_range(
    float slant_range_m,
    float elevation_deg,
    float& height_m,
    float& ground_range_m)
{
    constexpr double PI_D =
        3.1415926535897932384626433832795;

    const double el =
        static_cast<double>(elevation_deg) * PI_D / 180.0;

    const double r =
        static_cast<double>(slant_range_m);

    const double Re =
        EARTH_RADIUS_EFFECTIVE_M;

    const double inside =
        r * r +
        Re * Re +
        2.0 * r * Re * std::sin(el);

    const double h =
        std::sqrt(std::max(0.0, inside)) - Re;

    const double asin_arg =
        (r * std::cos(el)) / (Re + h);

    const double clamped =
        std::max(-1.0, std::min(1.0, asin_arg));

    const double s =
        Re * std::asin(clamped);

    height_m = static_cast<float>(h);
    ground_range_m = static_cast<float>(s);
}

inline float same_tilt_scale(
    float nyquist,
    float spectrum_width)
{
    const float sw =
        std::isfinite(spectrum_width)
            ? std::max(0.0f, spectrum_width)
            : 0.0f;

    return std::max(
        1.5f,
        std::max(
            0.12f * nyquist,
            0.50f * sw));
}

inline float vertical_scale(
    float nyquist,
    float range_m,
    float spectrum_width)
{
    const float sw =
        std::isfinite(spectrum_width)
            ? std::max(0.0f, spectrum_width)
            : 0.0f;

    // Cross-tilt velocity differences naturally grow with range because
    // the beams sample different heights and slightly different volumes.
    const float range_term =
        0.0025f * std::max(0.0f, range_m);

    const float nyq_term =
        0.15f * nyquist;

    const float sw_term =
        0.35f * sw;

    return std::max(
        2.0f,
        std::max({
            range_term,
            nyq_term,
            sw_term
        }));
}

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------

struct Boundary
{
    int gate = -1;
    int neighbor = -1;
};

struct Region
{
    std::vector<int> gates;
    std::vector<Boundary> boundary;

    float nyquist = 0.0f;

    int fold = 0;
    int version = 0;

    bool resolved = false;

    size_t ref_support = 0;
    float ref_fraction = 0.0f;

    bool has_vertical = false;

    bool high_shear = false;
    bool tvs_like = false;

    int vertical_support = 0;
    int horizontal_support = 0;

    float confidence = 0.0f;
};

struct Candidate
{
    float priority = 0.0f;
    int region = -1;
    int fold = 0;
    int version = 0;

    bool operator<(const Candidate& other) const
    {
        return priority < other.priority;
    }
};

struct FoldChoice
{
    bool valid = false;

    int fold = 0;

    float confidence = 0.0f;

    float best_cost =
        std::numeric_limits<float>::infinity();

    float second_cost =
        std::numeric_limits<float>::infinity();

    int horizontal_support = 0;
    int vertical_support = 0;
};

struct GateCandidate
{
    bool valid = false;
    int fold = 0;
    float confidence = 0.0f;
    float best_cost = std::numeric_limits<float>::infinity();
    float second_cost = std::numeric_limits<float>::infinity();
    int support = 0;
    int axis_types = 0;
    bool has_vertical = false;
    int extrapolated_support = 0;
};

struct SimpleRay
{
    size_t start = 0;
    size_t count = 0;
    float azimuth = 0.0f;
};

struct KeyedSeries
{
    std::vector<SimpleRay> rays;
    std::vector<float> key;
    std::vector<float> value;
};

struct PriorLayer
{
    bool valid = false;

    // key = ground range
    // value = absolute dealiased velocity
    KeyedSeries series;
};

// ---------------------------------------------------------------------------
// Packed moment ray splitting
// ---------------------------------------------------------------------------

std::vector<SimpleRay> split_rays(
    const std::vector<float>& packed)
{
    std::vector<SimpleRay> rays;

    if (packed.size() < 3 ||
        packed.size() % 3 != 0)
        return rays;

    const size_t n =
        packed.size() / 3;

    for (size_t t = 0; t < n; ++t)
    {
        bool new_ray = rays.empty();

        if (!new_ray)
        {
            const float az =
                packed[3 * t];

            const float prev_az =
                packed[3 * (t - 1)];

            const float range =
                packed[3 * t + 1];

            const float prev_range =
                packed[3 * (t - 1) + 1];

            new_ray =
                !std::isfinite(az) ||
                !std::isfinite(range) ||
                std::fabs(az - prev_az) > AZ_EPS ||
                range <= prev_range;
        }

        if (new_ray)
        {
            SimpleRay ray;
            ray.start = t;

            ray.azimuth =
                std::isfinite(packed[3 * t])
                    ? wrap360(packed[3 * t])
                    : 0.0f;

            rays.push_back(ray);
        }

        ++rays.back().count;
    }

    return rays;
}

// ---------------------------------------------------------------------------
// Generic nearest-neighbor alignment.
//
// The source and destination are both organized as monotonic rays.
// ---------------------------------------------------------------------------

std::vector<float> align_series(
    const KeyedSeries& src,
    const std::vector<SimpleRay>& dst_rays,
    const std::vector<float>& dst_key,
    size_t dst_count,
    float az_tol,
    float key_tol)
{
    std::vector<float> out(
        dst_count,
        VELOCITY_NAN);

    if (src.rays.empty() ||
        dst_rays.empty())
        return out;

    size_t source_ray_index = 0;

    for (const SimpleRay& dst_ray : dst_rays)
    {
        const float dst_az =
            dst_ray.azimuth;

        while (
            source_ray_index + 1 < src.rays.size() &&
            az_distance(
                src.rays[source_ray_index + 1].azimuth,
                dst_az)
            <=
            az_distance(
                src.rays[source_ray_index].azimuth,
                dst_az))
        {
            ++source_ray_index;
        }

        if (az_distance(
                src.rays[source_ray_index].azimuth,
                dst_az) > az_tol)
            continue;

        size_t di = dst_ray.start;
        const size_t dend =
            dst_ray.start + dst_ray.count;

        size_t si =
            src.rays[source_ray_index].start;

        const size_t send =
            si + src.rays[source_ray_index].count;

        while (di < dend && si < send)
        {
            const float dk =
                dst_key[di];

            if (!std::isfinite(dk))
            {
                ++di;
                continue;
            }

            // Move to the nearest source key.
            while (
                si + 1 < send &&
                std::isfinite(src.key[si + 1]) &&
                std::fabs(
                    src.key[si + 1] - dk)
                <=
                std::fabs(
                    src.key[si] - dk))
            {
                ++si;
            }

            if (!std::isfinite(src.key[si]))
            {
                ++si;
                continue;
            }

            if (std::fabs(src.key[si] - dk) <= key_tol)
                out[di] = src.value[si];

            ++di;
        }
    }

    return out;
}

// ---------------------------------------------------------------------------
// Moment alignment
// ---------------------------------------------------------------------------

std::vector<float> align_moment(
    const std::vector<float>& aux_packed,
    const std::vector<SimpleRay>& vel_rays,
    const std::vector<float>& vel_dist,
    size_t vel_count)
{
    if (aux_packed.empty())
        return std::vector<float>(
            vel_count,
            VELOCITY_NAN);

    KeyedSeries src;

    src.rays =
        split_rays(aux_packed);

    const size_t n =
        aux_packed.size() / 3;

    src.key.resize(n);
    src.value.resize(n);

    for (size_t t = 0; t < n; ++t)
    {
        src.key[t] =
            aux_packed[3 * t + 1];

        src.value[t] =
            aux_packed[3 * t + 2];
    }

    return align_series(
        src,
        vel_rays,
        vel_dist,
        vel_count,
        1.0f,
        60.0f);
}

// ---------------------------------------------------------------------------
// Velocity rays -> simple rays
// ---------------------------------------------------------------------------

std::vector<SimpleRay> to_simple_rays(
    const std::vector<VelocityRay>& rays,
    const std::vector<float>& packed)
{
    std::vector<SimpleRay> out;

    out.reserve(rays.size());

    for (const auto& r : rays)
    {
        if (r.start >= packed.size() / 3)
            continue;

        SimpleRay s;

        s.start = r.start;
        s.count = r.count;

        s.azimuth =
            wrap360(
                packed[3 * r.start]);

        out.push_back(s);
    }

    return out;
}

// ---------------------------------------------------------------------------
// Split-cut RHOHV sibling lookup
// ---------------------------------------------------------------------------

const SingleTilt* find_split_cut_sibling(
    const AllTilt& volume,
    const SingleTilt& vel_tilt)
{
    const SingleTilt* best = nullptr;

    float best_angle_diff =
        std::numeric_limits<float>::infinity();

    const long long ms_a =
        static_cast<long long>(
            vel_tilt.msg_31.collect_date) *
        86400000LL +
        static_cast<long long>(
            vel_tilt.msg_31.collect_ms);

    for (const auto& other : volume.Tilts)
    {
        if (&other == &vel_tilt)
            continue;

        if (other.Radials_RHO.empty() &&
            other.Radials_ZDR.empty())
            continue;

        const float angle_diff =
            std::fabs(
                other.ElevationAngle -
                vel_tilt.ElevationAngle);

        if (angle_diff >=
            SPLIT_CUT_ANGLE_EPS)
            continue;

        const long long ms_b =
            static_cast<long long>(
                other.msg_31.collect_date) *
            86400000LL +
            static_cast<long long>(
                other.msg_31.collect_ms);

        if (std::llabs(ms_a - ms_b) >=
            SPLIT_CUT_TIME_EPS_MS)
            continue;

        if (angle_diff < best_angle_diff)
        {
            best_angle_diff = angle_diff;
            best = &other;
        }
    }

    return best;
}

// ---------------------------------------------------------------------------
// Confidence model
// ---------------------------------------------------------------------------

float fold_confidence(
    float best_cost,
    float second_cost,
    int horizontal_support,
    int vertical_support,
    int region_size)
{
    if (!std::isfinite(best_cost) ||
        !std::isfinite(second_cost))
        return 0.0f;

    const float margin =
        std::clamp(
            (second_cost - best_cost) /
            std::max(second_cost, 1.0e-3f),
            0.0f,
            1.0f);

    const int effective_support =
        horizontal_support +
        2 * vertical_support;

    const float support_quality =
        1.0f -
        std::exp(
            -0.35f *
            static_cast<float>(
                effective_support));

    const float size_quality =
        1.0f -
        std::exp(
            -0.15f *
            static_cast<float>(
                std::max(1, region_size)));

    const float residual_quality =
        std::exp(
            -best_cost /
            std::max(
                1.0f,
                4.0f *
                static_cast<float>(
                    std::max(
                        1,
                        effective_support))));

    const float confidence =
        0.40f * margin +
        0.25f * support_quality +
        0.20f * size_quality +
        0.15f * residual_quality;

    return std::clamp(
        confidence,
        0.0f,
        1.0f);
}

// ---------------------------------------------------------------------------
// Main per-tilt dealiaser
// ---------------------------------------------------------------------------

PriorLayer dealias_tilt(
    SingleTilt& tilt,
    const PriorLayer* lower_prior,
    const PriorLayer* upper_prior,
    const SingleTilt* sibling)
{
    PriorLayer next;

    std::vector<float>& packed =
        tilt.Radials_VEL;

    if (packed.size() < 24 ||
        packed.size() % 3 != 0)
        return next;

    const size_t count =
        packed.size() / 3;

    if (count >
        static_cast<size_t>(
            std::numeric_limits<int>::max()))
        return next;

    const float fallback_nyq =
        static_cast<float>(
            tilt.vol_el_rad.rad.nyquist_vel) /
        100.0f;

    // ---------------------------------------------------------------
    // Build/validate velocity rays.
    // ---------------------------------------------------------------

    std::vector<VelocityRay> rays =
        tilt.VelocityRays;

    if (rays.empty())
    {
        for (size_t t = 0; t < count; ++t)
        {
            bool new_ray = rays.empty();

            if (!new_ray)
            {
                new_ray =
                    std::fabs(
                        packed[3 * t] -
                        packed[3 * (t - 1)])
                    > AZ_EPS ||
                    packed[3 * t + 1] <=
                    packed[3 * (t - 1) + 1];
            }

            if (new_ray)
            {
                VelocityRay r{
                    t,
                    0,
                    fallback_nyq,
                    tilt.gateSpacing
                };

                rays.push_back(r);
            }

            ++rays.back().count;
        }

        if (!tilt.VelNyquist.empty() &&
            tilt.VelNyquist.size() != rays.size())
            return next;

        for (size_t r = 0;
             r < rays.size();
             ++r)
        {
            if (!tilt.VelNyquist.empty())
                rays[r].nyquist =
                    tilt.VelNyquist[r];
        }
    }

    if (rays.size() < 4)
        return next;

    // ---------------------------------------------------------------
    // Expand per-ray Nyquist to per-gate.
    // ---------------------------------------------------------------

    std::vector<float> nyq(
        count,
        VELOCITY_NAN);

    size_t expected_start = 0;

    for (const auto& ray : rays)
    {
        if (ray.start != expected_start ||
            ray.count == 0 ||
            ray.count >
                count - expected_start)
            return next;

        if (!std::isfinite(ray.gateSpacing) ||
            ray.gateSpacing <= 0.0f)
            return next;

        if (ray.start >= count)
            return next;

        const float az =
            packed[3 * ray.start];

        if (!std::isfinite(az) ||
            az < 0.0f ||
            az >= 360.0f)
            return next;

        expected_start += ray.count;

        const float use_nyq =
            (std::isfinite(ray.nyquist) &&
             ray.nyquist > 0.0f)
                ? ray.nyquist
                : fallback_nyq;

        if (!(use_nyq > 0.0f))
            return next;

        for (size_t t = ray.start;
             t < expected_start;
             ++t)
        {
            if (!std::isfinite(
                    packed[3 * t + 1]))
                return next;

            if (t > ray.start &&
                packed[3 * t + 1] <=
                packed[3 * (t - 1) + 1])
                return next;

            nyq[t] = use_nyq;
        }
    }

    if (expected_start != count)
        return next;

    // ---------------------------------------------------------------
    // Determine acceptable azimuth gap.
    // ---------------------------------------------------------------

    auto az_gap =
        [&](size_t a, size_t b)
    {
        const float aa =
            packed[3 * rays[a].start];

        const float bb =
            packed[3 * rays[b].start];

        float d =
            std::fmod(
                bb - aa + 360.0f,
                360.0f);

        if (d < 0.0f)
            d += 360.0f;

        return d;
    };

    std::vector<float> az_steps;

    for (size_t r = 1;
         r < rays.size();
         ++r)
    {
        const float gap =
            az_gap(r - 1, r);

        if (gap > AZ_EPS &&
            gap <= 1.5f)
            az_steps.push_back(gap);
    }

    float max_gap = 0.0f;

    if (!az_steps.empty())
    {
        std::sort(
            az_steps.begin(),
            az_steps.end());

        const float median =
            az_steps[
                az_steps.size() / 2];

        max_gap =
            std::min(
                1.5f,
                1.5f * median);
    }

    // ---------------------------------------------------------------
    // Polar graph.
    //
    // directions:
    //   0 = previous range
    //   1 = next range
    //   2 = previous azimuth
    //   3 = next azimuth
    // ---------------------------------------------------------------

    std::vector<float> grid(count);

    std::vector<std::array<int, 4>> neighbors(
        count,
        {-1, -1, -1, -1});

    for (size_t t = 0;
         t < count;
         ++t)
    {
        grid[t] =
            packed[3 * t + 2];
    }

    // Keep an immutable copy. Later the region solver changes grid[] to
    // corrected velocity; edge quality must still be computed from the raw
    // observed field.
    const std::vector<float> raw_grid = grid;

    auto connect =
        [&](int a, int b, int direction)
    {
        if (a < 0 || b < 0)
            return;

        if (!std::isfinite(grid[a]) ||
            !std::isfinite(grid[b]))
            return;

        if (!std::isfinite(nyq[a]) ||
            !std::isfinite(nyq[b]))
            return;

        neighbors[a][direction] = b;

        // Opposite directions:
        // 0 <-> 1, 2 <-> 3.
        neighbors[b][direction + 1] = a;
    };

    for (size_t r = 0;
         r < rays.size();
         ++r)
    {
        const auto& ray =
            rays[r];

        const size_t end =
            ray.start + ray.count;

        // Range neighbors.
        for (size_t t = ray.start + 1;
             t < end;
             ++t)
        {
            const float dr =
                packed[3 * t + 1] -
                packed[3 * (t - 1) + 1];

            if (std::fabs(
                    dr - ray.gateSpacing)
                <= 0.1f)
            {
                connect(
                    static_cast<int>(t - 1),
                    static_cast<int>(t),
                    0);
            }
        }

        // Adjacent azimuth ray.
        const size_t next_r =
            (r + 1) % rays.size();

        const float gap =
            az_gap(r, next_r);

        if (gap <= AZ_EPS ||
            gap > max_gap)
            continue;

        const auto& other =
            rays[next_r];

        size_t a = ray.start;
        size_t b = other.start;

        const size_t other_end =
            other.start + other.count;

        while (a < end &&
               b < other_end)
        {
            const float diff =
                packed[3 * a + 1] -
                packed[3 * b + 1];

            if (std::fabs(diff) <= 0.1f)
            {
                connect(
                    static_cast<int>(a),
                    static_cast<int>(b),
                    2);

                ++a;
                ++b;
            }
            else if (diff < 0.0f)
            {
                ++a;
            }
            else
            {
                ++b;
            }
        }
    }

    const std::vector<SimpleRay> simple_rays =
        to_simple_rays(
            rays,
            packed);

    if (simple_rays.size() != rays.size())
        return next;

    // ---------------------------------------------------------------
    // Assist fields.
    // ---------------------------------------------------------------

    std::vector<float> vel_dist(count);

    for (size_t t = 0;
         t < count;
         ++t)
    {
        vel_dist[t] =
            packed[3 * t + 1];
    }

    const std::vector<float> ref_aligned =
        align_moment(
            tilt.Radials_REF,
            simple_rays,
            vel_dist,
            count);

    const std::vector<float> sw_aligned =
        align_moment(
            tilt.Radials_SW,
            simple_rays,
            vel_dist,
            count);

    // Prefer RHO from the same cut.
    std::vector<float> rho_aligned;

    if (!tilt.Radials_RHO.empty())
    {
        rho_aligned =
            align_moment(
                tilt.Radials_RHO,
                simple_rays,
                vel_dist,
                count);
    }
    else if (sibling &&
             !sibling->Radials_RHO.empty())
    {
        rho_aligned =
            align_moment(
                sibling->Radials_RHO,
                simple_rays,
                vel_dist,
                count);
    }
    else
    {
        rho_aligned.assign(
            count,
            VELOCITY_NAN);
    }

    // ---------------------------------------------------------------
    // Ground range and vertical prior.
    // ---------------------------------------------------------------

    std::vector<float> ground_range(
        count,
        VELOCITY_NAN);

    for (size_t t = 0;
         t < count;
         ++t)
    {
        float h_unused = 0.0f;
        float s = 0.0f;

        beam_height_ground_range(
            vel_dist[t],
            tilt.ElevationAngle,
            h_unused,
            s);

        ground_range[t] = s;
    }

    const std::vector<float> vertical_lower =
        (lower_prior && lower_prior->valid)
            ? align_series(
                  lower_prior->series,
                  simple_rays,
                  ground_range,
                  count,
                  1.0f,
                  VERTICAL_KEY_TOL_M)
            : std::vector<float>(count, VELOCITY_NAN);

    const std::vector<float> vertical_upper =
        (upper_prior && upper_prior->valid)
            ? align_series(
                  upper_prior->series,
                  simple_rays,
                  ground_range,
                  count,
                  1.0f,
                  VERTICAL_KEY_TOL_M)
            : std::vector<float>(count, VELOCITY_NAN);

    // ---------------------------------------------------------------
    // 8-neighbor polar graph.
    //
    // The region fast path only needs the original 4-neighbor connectivity,
    // but the hard solver and shear detector benefit substantially from the
    // diagonals. Build the 8-neighbor graph once and keep the original graph
    // as its first four entries for compatibility with the region machinery.
    // ---------------------------------------------------------------

    std::vector<std::array<int, 8>> neighbors8(
        count,
        {-1,-1,-1,-1,-1,-1,-1,-1});

    std::vector<int> ray_of_gate(count, -1);
    std::vector<int> pos_in_ray(count, -1);

    for (size_t r = 0; r < rays.size(); ++r)
    {
        for (size_t p0 = 0; p0 < rays[r].count; ++p0)
        {
            const size_t g = rays[r].start + p0;
            ray_of_gate[g] = static_cast<int>(r);
            pos_in_ray[g] = static_cast<int>(p0);
        }
    }

    auto add_neighbor8 =
        [&](int a, int b)
    {
        if (a < 0 || b < 0 || a == b)
            return;

        auto add_one =
            [&](int x, int y)
        {
            for (int k = 0; k < 8; ++k)
                if (neighbors8[x][k] == y)
                    return;

            for (int k = 0; k < 8; ++k)
            {
                if (neighbors8[x][k] < 0)
                {
                    neighbors8[x][k] = y;
                    return;
                }
            }
        };

        add_one(a, b);
        add_one(b, a);
    };

    // Copy the reliable cardinal graph first.
    for (size_t t = 0; t < count; ++t)
    {
        for (int k = 0; k < 4; ++k)
        {
            if (neighbors[t][k] >= 0)
                add_neighbor8(
                    static_cast<int>(t),
                    neighbors[t][k]);
        }
    }

    // Add diagonals by pairing nearby gate positions on adjacent rays.
    // NEXRAD rays are normally aligned gate-for-gate, so this remains cheap.
    for (size_t r = 0; r < rays.size(); ++r)
    {
        const size_t nr = (r + 1) % rays.size();

        const float gap = az_gap(r, nr);
        if (gap <= AZ_EPS || gap > max_gap)
            continue;

        const auto& a = rays[r];
        const auto& b = rays[nr];

        const size_t common =
            std::min(a.count, b.count);

        if (common == 0)
            continue;

        const float range_tol =
            std::max(
                3.0f,
                0.10f *
                std::min(a.gateSpacing,
                         b.gateSpacing));

        for (size_t p0 = 0; p0 < common; ++p0)
        {
            const int ga =
                static_cast<int>(a.start + p0);

            // Same-range already exists; add +/- one gate diagonals.
            if (p0 + 1 < b.count)
            {
                const int gb =
                    static_cast<int>(b.start + p0 + 1);

                if (std::fabs(
                        packed[3 * ga + 1] -
                        packed[3 * gb + 1])
                    <=
                    2.0f * range_tol)
                {
                    add_neighbor8(ga, gb);
                }
            }

            if (p0 > 0)
            {
                const int gb =
                    static_cast<int>(b.start + p0 - 1);

                if (std::fabs(
                        packed[3 * ga + 1] -
                        packed[3 * gb + 1])
                    <=
                    2.0f * range_tol)
                {
                    add_neighbor8(ga, gb);
                }
            }
        }
    }

    // ---------------------------------------------------------------
    // Edge-quality model.
    // ---------------------------------------------------------------

    auto edge_weight =
        [&](int a, int b) -> float
    {
        const float common_nyq =
            0.5f * (nyq[a] + nyq[b]);

        if (!(common_nyq > 0.0f))
            return 0.0f;

        const float jump =
            std::fabs(raw_grid[a] - raw_grid[b]);

        float w = 1.0f;

        // High raw shear is exactly where continuity is least trustworthy.
        // This prevents the easy-side solution from bulldozing into a tornado.
        const float jf =
            jump / common_nyq;

        if (jf >= HIGH_SHEAR_FRAC)
            w *= HARD_CORE_EDGE_WEIGHT;
        else if (jf >= 0.55f)
            w *= HARD_BUFFER_EDGE_WEIGHT;

        const float sw_a =
            std::isfinite(sw_aligned[a])
                ? std::max(0.0f, sw_aligned[a])
                : 0.0f;

        const float sw_b =
            std::isfinite(sw_aligned[b])
                ? std::max(0.0f, sw_aligned[b])
                : 0.0f;

        const float sw =
            0.5f * (sw_a + sw_b);

        if (sw > 0.0f)
        {
            const float q =
                1.0f /
                (1.0f +
                 0.55f * sw /
                     std::max(1.0f, common_nyq));

            w *=
                std::max(0.25f,
                         std::min(1.0f, q));
        }

        // Low RHOHV/TDS makes velocity less trustworthy, but it never votes
        // for a fold by itself.
        if (std::isfinite(rho_aligned[a]) &&
            rho_aligned[a] < RHO_TDS_THRESHOLD)
        {
            w *= 0.65f;
        }

        if (std::isfinite(rho_aligned[b]) &&
            rho_aligned[b] < RHO_TDS_THRESHOLD)
        {
            w *= 0.65f;
        }

        // REF similarity is only a weak structural prior.
        if (std::isfinite(ref_aligned[a]) &&
            std::isfinite(ref_aligned[b]))
        {
            const float dr =
                std::fabs(ref_aligned[a] -
                          ref_aligned[b]);

            if (dr > 25.0f)
                w *= 0.75f;
            else if (dr < 8.0f)
                w *= 1.05f;
        }

        return std::max(0.05f,
                        std::min(1.0f, w));
    };

    // ---------------------------------------------------------------
    // R2D2-style high-shear mask + buffer.
    // ---------------------------------------------------------------

    std::vector<unsigned char> high_shear(
        count,
        0);

    for (size_t t = 0; t < count; ++t)
    {
        if (!std::isfinite(grid[t]))
            continue;

        float dmax = 0.0f;
        bool found = false;

        for (const int nb : neighbors8[t])
        {
            if (nb < 0 || !std::isfinite(grid[nb]))
                continue;

            const float common_nyq =
                0.5f * (nyq[t] + nyq[nb]);

            if (!(common_nyq > 0.0f))
                continue;

            dmax = std::max(
                dmax,
                std::fabs(grid[t] - grid[nb]));
            found = true;
        }

        if (found &&
            dmax >= HIGH_SHEAR_FRAC * nyq[t])
        {
            high_shear[t] = 1;
        }
    }

    std::vector<unsigned char> protected_gate =
        high_shear;

    // Two graph hops gives a cheap polar equivalent of the 5x5 buffer used
    // by R2D2 while respecting missing gates and irregular azimuth spacing.
    for (int hop = 0;
         hop < HIGH_SHEAR_BUFFER_HOPS;
         ++hop)
    {
        std::vector<unsigned char> next_mask =
            protected_gate;

        for (size_t t = 0; t < count; ++t)
        {
            if (!protected_gate[t])
                continue;

            for (const int nb : neighbors8[t])
            {
                if (nb >= 0)
                    next_mask[nb] = 1;
            }
        }

        protected_gate.swap(next_mask);
    }

    // ---------------------------------------------------------------
    // Phase 1: raw velocity segmentation.
    //
    // We deliberately do NOT use vertical/RHO/SW to connect regions.
    // Strong shear should remain a boundary.
    // ---------------------------------------------------------------

    std::vector<int> labels(
        count,
        -1);

    std::vector<Region> regions;

    regions.reserve(count / 8 + 1);

    for (int t = 0;
         t < static_cast<int>(count);
         ++t)
    {
        if (labels[t] >= 0)
            continue;

        if (!std::isfinite(grid[t]) ||
            !std::isfinite(nyq[t]) ||
            protected_gate[t])
            continue;

        const int label =
            static_cast<int>(
                regions.size());

        regions.emplace_back();

        Region& region =
            regions.back();

        region.nyquist =
            nyq[t];

        region.gates.push_back(t);

        labels[t] = label;

        if (std::isfinite(
                ref_aligned[t]))
            ++region.ref_support;

        if (std::isfinite(vertical_lower[t]) ||
            std::isfinite(vertical_upper[t]))
            region.has_vertical = true;

        for (size_t cursor = 0;
             cursor < region.gates.size();
             ++cursor)
        {
            const int gate =
                region.gates[cursor];

            for (const int neighbor :
                 neighbors[gate])
            {
                if (neighbor < 0)
                    continue;

                if (labels[neighbor] >= 0)
                    continue;

                if (!compatible_nyquist(
                        region.nyquist,
                        nyq[neighbor]))
                    continue;

                if (!std::isfinite(
                        grid[neighbor]) ||
                    protected_gate[neighbor])
                    continue;

                if (std::fabs(
                        grid[gate] -
                        grid[neighbor])
                    >
                    SEG_FRAC *
                    region.nyquist)
                    continue;

                labels[neighbor] =
                    label;

                region.gates.push_back(
                    neighbor);

                if (std::isfinite(
                        ref_aligned[neighbor]))
                    ++region.ref_support;

                if (std::isfinite(vertical_lower[neighbor]) ||
                    std::isfinite(vertical_upper[neighbor]))
                    region.has_vertical = true;
            }
        }

        region.ref_fraction =
            static_cast<float>(
                region.ref_support) /
            static_cast<float>(
                std::max<size_t>(
                    1,
                    region.gates.size()));
    }

    if (regions.empty())
        return next;

    // ---------------------------------------------------------------
    // Build region boundaries.
    // ---------------------------------------------------------------

    for (int t = 0;
         t < static_cast<int>(count);
         ++t)
    {
        if (labels[t] < 0)
            continue;

        const int this_region =
            labels[t];

        for (const int neighbor :
             neighbors[t])
        {
            if (neighbor < 0)
                continue;

            const int other_region =
                labels[neighbor];

            if (other_region < 0 ||
                other_region ==
                    this_region)
                continue;

            if (!compatible_nyquist(
                    regions[this_region].nyquist,
                    regions[other_region].nyquist))
                continue;

            regions[this_region]
                .boundary.push_back(
                    {t, neighbor});
        }
    }

    // ---------------------------------------------------------------
    // Phase 2: best-first region resolution.
    // ---------------------------------------------------------------

    std::priority_queue<Candidate> pq;

    size_t resolved_count = 0;

    // ---------------------------------------------------------------
    // Fold selection.
    // ---------------------------------------------------------------

    auto choose_fold =
        [&](int index) -> FoldChoice
    {
        Region& region =
            regions[index];

        int horizontal_support = 0;
        int vertical_support = 0;

        float sw_sum = 0.0f;
        int sw_count = 0;

        bool tds_confirmed = false;

        // Gather evidence statistics.
        for (const Boundary& edge :
             region.boundary)
        {
            const int nb_region =
                labels[edge.neighbor];

            if (nb_region < 0)
                continue;

            if (!regions[nb_region].resolved)
                continue;

            ++horizontal_support;

            if (std::isfinite(
                    sw_aligned[edge.gate]))
            {
                sw_sum +=
                    sw_aligned[edge.gate];

                ++sw_count;
            }

            if (std::isfinite(
                    rho_aligned[edge.gate]) &&
                rho_aligned[edge.gate] <
                    RHO_TDS_THRESHOLD)
            {
                tds_confirmed = true;
            }
        }

        for (const int gate :
             region.gates)
        {
            if (std::isfinite(
                    vertical_lower[gate]))
            {
                ++vertical_support;
            }

            if (std::isfinite(
                    vertical_upper[gate]))
            {
                ++vertical_support;
            }

            if (std::isfinite(
                    sw_aligned[gate]))
            {
                sw_sum +=
                    sw_aligned[gate];

                ++sw_count;
            }

            if (std::isfinite(
                    rho_aligned[gate]) &&
                rho_aligned[gate] <
                    RHO_TDS_THRESHOLD)
            {
                tds_confirmed = true;
            }
        }

        if (horizontal_support == 0 &&
            vertical_support == 0)
        {
            return {};
        }

        const float sw_hint =
            sw_count > 0
                ? sw_sum /
                    static_cast<float>(
                        sw_count)
                : 0.0f;

        float horizontal_scale =
            same_tilt_scale(
                region.nyquist,
                sw_hint);

        if (tds_confirmed)
        {
            horizontal_scale *=
                RHO_TDS_SCALE_MULTIPLIER;
        }

        // Evaluate all candidate fold integers.
        float best_cost =
            std::numeric_limits<float>::infinity();

        float second_cost =
            std::numeric_limits<float>::infinity();

        int best_k = 0;

        for (int k = -MAX_FOLD;
             k <= MAX_FOLD;
             ++k)
        {
            const float shift =
                static_cast<float>(k) *
                2.0f *
                region.nyquist;

            float cost = 0.0f;

            // Same-tilt evidence.
            for (const Boundary& edge :
                 region.boundary)
            {
                const int nb_region =
                    labels[edge.neighbor];

                if (nb_region < 0 ||
                    !regions[nb_region].resolved)
                    continue;

                const float residual =
                    grid[edge.gate] +
                    shift -
                    grid[edge.neighbor];

                const float sw =
                    std::isfinite(
                        sw_aligned[edge.gate])
                        ? sw_aligned[edge.gate]
                        : 0.0f;

                const float scale =
                    same_tilt_scale(
                        region.nyquist,
                        sw);

                const float neighbor_size =
                    static_cast<float>(
                        regions[nb_region]
                            .gates.size());

                const float weight =
                    std::sqrt(
                        std::max(
                            1.0f,
                            neighbor_size));

                cost +=
                    weight *
                    huber_cost(
                        residual,
                        scale);
            }

            // Vertical evidence. Use both adjacent elevations when
            // available; agreement between them is especially valuable for
            // compact tornado cores that are spatially ambiguous in one cut.
            for (const int gate :
                 region.gates)
            {
                const float sw =
                    std::isfinite(
                        sw_aligned[gate])
                        ? sw_aligned[gate]
                        : 0.0f;

                const float scale =
                    vertical_scale(
                        region.nyquist,
                        packed[3 * gate + 1],
                        sw);

                if (std::isfinite(
                        vertical_lower[gate]))
                {
                    const float residual =
                        grid[gate] +
                        shift -
                        vertical_lower[gate];

                    cost +=
                        VERTICAL_WEIGHT *
                        huber_cost(
                            residual,
                            scale);
                }

                if (std::isfinite(
                        vertical_upper[gate]))
                {
                    const float residual =
                        grid[gate] +
                        shift -
                        vertical_upper[gate];

                    cost +=
                        VERTICAL_WEIGHT *
                        huber_cost(
                            residual,
                            scale);
                }
            }

            // Very weak tie-break only.
            // This is NOT a global zero-mean prior.
            cost +=
                0.005f *
                std::fabs(
                    static_cast<float>(k)) *
                region.nyquist;

            if (cost < best_cost)
            {
                second_cost = best_cost;
                best_cost = cost;
                best_k = k;
            }
            else if (cost < second_cost)
            {
                second_cost = cost;
            }
        }

        if (!std::isfinite(best_cost) ||
            !std::isfinite(second_cost))
        {
            return {};
        }

        float confidence =
            fold_confidence(
                best_cost,
                second_cost,
                horizontal_support,
                vertical_support,
                static_cast<int>(
                    region.gates.size()));

        // Weak reflectivity + weak support:
        // don't manufacture a nonzero fold.
        const bool weak_region =
            region.ref_fraction <
                MIN_REF_FRACTION_FOR_FOLD &&
            region.gates.size() <
                MIN_REGION_SIZE_FOR_FOLD;

        if (best_k != 0 &&
            weak_region &&
            horizontal_support <
                MIN_HORIZONTAL_SUPPORT_FOR_FOLD &&
            vertical_support <
                MIN_VERTICAL_SUPPORT_FOR_FOLD)
        {
            best_k = 0;
            confidence *= 0.25f;
        }

        FoldChoice result;

        result.valid = true;
        result.fold = best_k;
        result.confidence = confidence;
        result.best_cost = best_cost;
        result.second_cost = second_cost;
        result.horizontal_support =
            horizontal_support;
        result.vertical_support =
            vertical_support;

        return result;
    };

    // ---------------------------------------------------------------
    // Candidate insertion.
    // ---------------------------------------------------------------

    auto push_candidate =
        [&](int index)
    {
        Region& region =
            regions[index];

        if (region.resolved)
            return;

        const FoldChoice choice =
            choose_fold(index);

        if (!choice.valid)
            return;

        // Do not put weak ambiguous choices into the propagation queue.
        // They can be reconsidered after neighboring regions resolve.
        if (choice.confidence <
            CONFIDENCE_ACCEPT)
            return;

        ++region.version;

        region.confidence =
            choice.confidence;

        region.horizontal_support =
            choice.horizontal_support;

        region.vertical_support =
            choice.vertical_support;

        const float priority =
            choice.confidence *
            std::sqrt(
                static_cast<float>(
                    std::max<size_t>(
                        1,
                        region.gates.size()))) *
            (0.75f +
             0.50f * region.ref_fraction) *
            (region.has_vertical ? 1.25f : 1.0f);

        pq.push({
            priority,
            index,
            choice.fold,
            region.version
        });
    };

    // ---------------------------------------------------------------
    // Resolve a region.
    // ---------------------------------------------------------------

    auto resolve =
        [&](int index, int fold)
    {
        Region& region =
            regions[index];

        if (region.resolved)
            return;

        region.fold = fold;
        region.resolved = true;

        ++resolved_count;

        const float shift =
            static_cast<float>(fold) *
            2.0f *
            region.nyquist;

        for (const int gate :
             region.gates)
        {
            grid[gate] += shift;
        }

        std::vector<int> touched;

        touched.reserve(
            region.boundary.size());

        for (const Boundary& edge :
             region.boundary)
        {
            const int nb_region =
                labels[edge.neighbor];

            if (nb_region >= 0 &&
                !regions[nb_region].resolved)
            {
                touched.push_back(
                    nb_region);
            }
        }

        std::sort(
            touched.begin(),
            touched.end());

        touched.erase(
            std::unique(
                touched.begin(),
                touched.end()),
            touched.end());

        for (const int region_index :
             touched)
        {
            push_candidate(
                region_index);
        }
    };

    // ---------------------------------------------------------------
    // Initial candidates.
    //
    // Vertical evidence can immediately resolve regions before horizontal
    // propagation, which is important for small tornado cores.
    // ---------------------------------------------------------------

    for (size_t i = 0;
         i < regions.size();
         ++i)
    {
        push_candidate(
            static_cast<int>(i));
    }

    // ---------------------------------------------------------------
    // Seed disconnected components conservatively.
    //
    // A completely unsupported component has no physically observable
    // absolute fold. We intentionally anchor it at fold 0 only when we
    // have no alternative, but mark the choice as low-confidence. This
    // seed is NEVER allowed to be promoted to a trusted vertical prior.
    //
    // The important difference from v6.1 is that there is no later global
    // min-|V| correction capable of shifting an entire tornado component.
    // ---------------------------------------------------------------

    std::vector<int> seed_order(
        regions.size());

    std::iota(
        seed_order.begin(),
        seed_order.end(),
        0);

    std::sort(
        seed_order.begin(),
        seed_order.end(),
        [&](int a, int b)
        {
            const float score_a =
                static_cast<float>(
                    regions[a].gates.size()) *
                (0.5f +
                 0.5f *
                 regions[a].ref_fraction);

            const float score_b =
                static_cast<float>(
                    regions[b].gates.size()) *
                (0.5f +
                 0.5f *
                 regions[b].ref_fraction);

            if (score_a != score_b)
                return score_a > score_b;

            return regions[a].nyquist >
                   regions[b].nyquist;
        });

    size_t seed_cursor = 0;

    auto seed_next_island =
        [&]() -> bool
    {
        while (
            seed_cursor <
                seed_order.size() &&
            regions[
                seed_order[seed_cursor]]
                .resolved)
        {
            ++seed_cursor;
        }

        if (seed_cursor >=
            seed_order.size())
            return false;

        const int index =
            seed_order[seed_cursor++];

        Region& region =
            regions[index];

        // Only use fold 0 as a disconnected-component baseline.
        // This is deliberately low-confidence and will not be published
        // as a trusted prior.
        region.confidence = 0.0f;
        region.horizontal_support = 0;
        region.vertical_support = 0;

        resolve(index, 0);

        return true;
    };

    // ---------------------------------------------------------------
    // Best-first propagation.
    // ---------------------------------------------------------------

    while (resolved_count <
           regions.size())
    {
        if (pq.empty())
        {
            if (!seed_next_island())
                break;

            continue;
        }

        const Candidate candidate =
            pq.top();

        pq.pop();

        Region& region =
            regions[candidate.region];

        if (region.resolved)
            continue;

        if (candidate.version !=
            region.version)
            continue;

        // Re-evaluate immediately before resolving. A newer neighboring
        // region may have changed the correct fold.
        const FoldChoice current =
            choose_fold(
                candidate.region);

        if (!current.valid ||
            current.confidence <
                CONFIDENCE_ACCEPT)
        {
            continue;
        }

        // Candidate may have become stale.
        if (current.fold !=
                candidate.fold ||
            current.confidence <
                candidate.priority /
                std::max(
                    1.0f,
                    std::sqrt(
                        static_cast<float>(
                            region.gates.size()))))
        {
            ++region.version;

            const float priority =
                current.confidence *
                std::sqrt(
                    static_cast<float>(
                        std::max<size_t>(
                            1,
                            region.gates.size())));

            pq.push({
                priority,
                candidate.region,
                current.fold,
                region.version
            });

            continue;
        }

        region.confidence =
            current.confidence;

        region.horizontal_support =
            current.horizontal_support;

        region.vertical_support =
            current.vertical_support;

        resolve(
            candidate.region,
            current.fold);
    }

    // ---------------------------------------------------------------
    // Phase 3: hard-region gate-level solver.
    //
    // The key tornado fix: high-shear regions are NOT assigned one fold as a
    // block. Each gate gets its own integer fold and is solved only when enough
    // independent evidence reaches it. This allows a fold boundary to pass
    // through a compact vortex without forcing the whole tornado core onto one
    // Nyquist interval.
    // ---------------------------------------------------------------

    std::vector<unsigned char> gate_solved(count, 0);
    std::vector<int> gate_fold(count, 0);
    std::vector<float> gate_confidence(count, 0.0f);

    for (size_t t = 0; t < count; ++t)
    {
        if (labels[t] < 0 || protected_gate[t])
            continue;

        const Region& region =
            regions[labels[t]];

        if (!region.resolved)
            continue;

        gate_solved[t] = 1;
        gate_fold[t] = region.fold;
        gate_confidence[t] =
            region.confidence;
    }

    auto axis_type =
        [](int slot) -> int
    {
        // We do not rely on the exact neighbor slot ordering. The first four
        // slots are populated from the cardinal graph; additional slots are
        // diagonals. This is therefore only a conservative independence hint.
        if (slot < 2) return 1; // range
        if (slot < 4) return 2; // azimuth
        return 4;               // diagonal
    };

    // Predict a hard gate from a solved neighbor. Direct continuity is the
    // baseline. When the neighbor sits on a valid radial or azimuthal line,
    // use one-sided extrapolation from the solved side. This is the important
    // "peel inward" mechanism for compact tornado couplets: the outer solved
    // flow can predict the next gate even when the immediate raw jump exceeds
    // half the Nyquist interval.
    auto predict_hard_boundary =
        [&](int gate, int nb, int slot, float& value, float& weight, bool& extrapolated)
        -> bool
    {
        if (nb < 0 || !gate_solved[nb] ||
            !std::isfinite(grid[nb]))
            return false;

        value = grid[nb];
        weight = edge_weight(gate, nb) * 0.80f;
        extrapolated = false;

        if (!(weight > 0.0f))
            return false;

        const int rg = ray_of_gate[gate];
        const int rn = ray_of_gate[nb];
        const int pg = pos_in_ray[gate];
        const int pn = pos_in_ray[nb];

        int behind = -1;

        // Radial edge: continue one gate farther through the solved side.
        if (rg >= 0 && rg == rn &&
            pg >= 0 && pn >= 0 &&
            std::abs(pn - pg) == 1)
        {
            const int step = pn - pg;
            const int p2 = pn + step;
            if (p2 >= 0 &&
                p2 < static_cast<int>(rays[rn].count))
            {
                behind = static_cast<int>(
                    rays[rn].start +
                    static_cast<size_t>(p2));
            }
        }
        // Azimuth edge: same gate position on adjacent rays. Continue in
        // the same azimuth direction through the solved side.
        else if (pg >= 0 && pn >= 0 && pg == pn && rn != rg)
        {
            int delta = rn - rg;
            const int nr = static_cast<int>(rays.size());
            if (delta > nr / 2) delta -= nr;
            if (delta < -nr / 2) delta += nr;

            if (std::abs(delta) == 1)
            {
                int r2 = rn + delta;
                if (r2 < 0) r2 += nr;
                if (r2 >= nr) r2 -= nr;

                if (pn < static_cast<int>(rays[r2].count))
                {
                    behind = static_cast<int>(
                        rays[r2].start +
                        static_cast<size_t>(pn));
                }
            }
        }

        if (behind >= 0 &&
            gate_solved[behind] &&
            std::isfinite(grid[behind]))
        {
            const float d1 =
                grid[nb] - grid[behind];

            const float max_grad =
                0.90f *
                std::min(nyq[gate], nyq[nb]);

            if (std::isfinite(d1) &&
                std::fabs(d1) <= max_grad)
            {
                value = grid[nb] + d1;
                weight *= 1.80f;
                extrapolated = true;

                // A second solved gate makes the extrapolation less sensitive
                // to a single noisy velocity estimate.
                int behind2 = -1;

                const int rb = ray_of_gate[behind];
                const int pb = pos_in_ray[behind];

                if (rg >= 0 && rb == rg &&
                    rb == rn && pb >= 0 &&
                    pn >= 0 && pb + 1 == pn)
                {
                    const int p3 = pn + (pn - pg) * 2;
                    if (p3 >= 0 &&
                        p3 < static_cast<int>(rays[rn].count))
                    {
                        behind2 = static_cast<int>(
                            rays[rn].start +
                            static_cast<size_t>(p3));
                    }
                }

                if (behind2 >= 0 &&
                    gate_solved[behind2] &&
                    std::isfinite(grid[behind2]))
                {
                    const float d2 =
                        grid[behind] - grid[behind2];

                    if (std::isfinite(d2) &&
                        std::fabs(d2) <= max_grad)
                    {
                        const float slope =
                            0.70f * d1 +
                            0.30f * d2;

                        value =
                            grid[nb] + slope;

                        weight *= 1.15f;
                    }
                }
            }
        }

        (void)slot;
        return true;
    };

    auto choose_hard_gate =
        [&](int gate) -> GateCandidate
    {
        GateCandidate result;

        if (!protected_gate[gate] ||
            !std::isfinite(grid[gate]))
            return result;

        float cost[2 * MAX_FOLD + 1];
        int support[2 * MAX_FOLD + 1];
        int extrap_support[2 * MAX_FOLD + 1];
        unsigned masks[2 * MAX_FOLD + 1];

        for (int i = 0;
             i < 2 * MAX_FOLD + 1;
             ++i)
        {
            cost[i] = 0.0f;
            support[i] = 0;
            extrap_support[i] = 0;
            masks[i] = 0u;
        }

        bool has_spatial = false;
        bool has_vertical = false;

        for (int slot = 0; slot < 8; ++slot)
        {
            const int nb = neighbors8[gate][slot];

            if (nb < 0 || !gate_solved[nb])
                continue;

            const float common_nyq =
                0.5f * (nyq[gate] + nyq[nb]);

            if (!(common_nyq > 0.0f))
                continue;

            float ref = grid[nb];
            float w = edge_weight(gate, nb);

            if (!(w > 0.0f))
                continue;

            // Only cardinal edges have a well-defined one-sided extrapolation
            // in the current polar indexing. Diagonals remain direct evidence.
            if (slot < 4)
            {
                float predicted = ref;
                float predicted_weight = w;
                bool extrapolated = false;
                predict_hard_boundary(
                    gate, nb, slot,
                    predicted,
                    predicted_weight,
                    extrapolated);

                ref = predicted;
                w = predicted_weight;

                if (extrapolated)
                {
                    for (int k = -MAX_FOLD; k <= MAX_FOLD; ++k)
                    {
                        const float candidate =
                            grid[gate] +
                            static_cast<float>(k) *
                            2.0f * nyq[gate];

                        const float residual =
                            candidate - ref;

                        if (std::fabs(residual) <=
                            0.50f * common_nyq)
                        {
                            ++extrap_support[k + MAX_FOLD];
                        }
                    }
                }
            }

            has_spatial = true;

            const float scale =
                std::max(
                    1.5f,
                    same_tilt_scale(
                        common_nyq,
                        std::isfinite(sw_aligned[gate])
                            ? sw_aligned[gate]
                            : 0.0f));

            for (int k = -MAX_FOLD;
                 k <= MAX_FOLD;
                 ++k)
            {
                const float candidate =
                    grid[gate] +
                    static_cast<float>(k) *
                    2.0f * nyq[gate];

                const float residual =
                    candidate - ref;

                const int idx = k + MAX_FOLD;

                cost[idx] +=
                    w *
                    huber_cost(
                        residual,
                        scale);

                if (std::fabs(residual) <=
                    0.50f * common_nyq)
                {
                    ++support[idx];
                    masks[idx] |=
                        static_cast<unsigned>(
                            axis_type(slot));
                }
            }
        }

        auto add_vertical =
            [&](const std::vector<float>& prior_values,
                float weight)
        {
            if (!std::isfinite(prior_values[gate]))
                return;

            has_vertical = true;

            const float sw =
                std::isfinite(sw_aligned[gate])
                    ? sw_aligned[gate]
                    : 0.0f;

            float scale =
                vertical_scale(
                    nyq[gate],
                    packed[3 * gate + 1],
                    sw);

            if (std::isfinite(rho_aligned[gate]) &&
                rho_aligned[gate] < RHO_TDS_THRESHOLD)
            {
                scale *=
                    RHO_TDS_SCALE_MULTIPLIER;
            }

            for (int k = -MAX_FOLD;
                 k <= MAX_FOLD;
                 ++k)
            {
                const float candidate =
                    grid[gate] +
                    static_cast<float>(k) *
                    2.0f * nyq[gate];

                const float residual =
                    candidate -
                    prior_values[gate];

                cost[k + MAX_FOLD] +=
                    weight *
                    huber_cost(
                        residual,
                        scale);
            }
        };

        add_vertical(vertical_lower, 1.15f);
        add_vertical(vertical_upper, 1.15f);

        if (!has_spatial && !has_vertical)
            return result;

        int best = -1;
        int second = -1;

        for (int i = 0;
             i < 2 * MAX_FOLD + 1;
             ++i)
        {
            if (best < 0 || cost[i] < cost[best])
            {
                second = best;
                best = i;
            }
            else if (second < 0 || cost[i] < cost[second])
            {
                second = i;
            }
        }

        if (best < 0 || second < 0)
            return result;

        const float support_score =
            std::min(
                1.0f,
                static_cast<float>(
                    support[best]) / 3.0f);

        const int axis_count =
            ((masks[best] & 1u) ? 1 : 0) +
            ((masks[best] & 2u) ? 1 : 0) +
            ((masks[best] & 4u) ? 1 : 0);

        const float margin =
            std::clamp(
                (cost[second] - cost[best]) /
                std::max(cost[second], 1.0e-3f),
                0.0f,
                1.0f);

        const float confidence =
            0.55f * margin +
            0.30f * support_score +
            0.15f * (has_vertical ? 1.0f : 0.0f);

        const bool core =
            high_shear[gate] != 0;

        bool accept =
            confidence >= HARD_MIN_CONFIDENCE;

        if (core)
        {
            const bool independent =
                support[best] >= HARD_MIN_SUPPORT_CORE &&
                axis_count >= HARD_MIN_AXIS_TYPES_CORE;

            const bool vertically_guided =
                has_vertical &&
                (margin >= HARD_RELAX_MARGIN ||
                 support[best] >= 1);

            const bool strong_extrapolation =
                extrap_support[best] >= 1 &&
                margin >= 0.08f &&
                support[best] >= 1;

            accept =
                accept &&
                (independent ||
                 vertically_guided ||
                 strong_extrapolation);
        }
        else
        {
            // Buffer gates can be peeled inward once one or more reliable
            // neighbors have been established.
            accept =
                accept &&
                (support[best] >= 1 || has_vertical);
        }

        if (!accept)
            return result;

        result.valid = true;
        result.fold = best - MAX_FOLD;
        result.confidence = confidence;
        result.best_cost = cost[best];
        result.second_cost = cost[second];
        result.support = support[best];
        result.axis_types = axis_count;
        result.has_vertical = has_vertical;
        result.extrapolated_support = extrap_support[best];
        return result;
    };

    struct GateQueueItem
    {
        float priority = 0.0f;
        int gate = -1;
        int version = 0;

        bool operator<(const GateQueueItem& other) const
        {
            if (priority != other.priority)
                return priority < other.priority;
            return gate > other.gate;
        }
    };

    std::priority_queue<GateQueueItem> hard_pq;
    std::vector<int> hard_version(count, 0);

    auto push_hard_gate =
        [&](int gate)
    {
        if (!protected_gate[gate] || gate_solved[gate])
            return;

        const GateCandidate c =
            choose_hard_gate(gate);

        if (!c.valid)
            return;

        ++hard_version[gate];

        const float priority =
            c.confidence *
            (1.0f +
             0.35f *
             static_cast<float>(c.support)) +
            (c.has_vertical ? 0.20f : 0.0f);

        hard_pq.push({
            priority,
            gate,
            hard_version[gate]
        });
    };

    // First allow vertical evidence to seed compact hard regions directly.
    for (size_t t = 0; t < count; ++t)
    {
        if (protected_gate[t] &&
            (std::isfinite(vertical_lower[t]) ||
             std::isfinite(vertical_upper[t])))
        {
            push_hard_gate(
                static_cast<int>(t));
        }
    }

    // Then grow inward from resolved easy regions.
    for (size_t t = 0; t < count; ++t)
    {
        if (!gate_solved[t])
            continue;

        for (const int nb : neighbors8[t])
        {
            if (nb >= 0 && protected_gate[nb])
                push_hard_gate(nb);
        }
    }

    while (!hard_pq.empty())
    {
        const GateQueueItem item =
            hard_pq.top();

        hard_pq.pop();

        const int gate = item.gate;

        if (gate < 0 || gate_solved[gate])
            continue;

        if (item.version != hard_version[gate])
            continue;

        const GateCandidate current =
            choose_hard_gate(gate);

        if (!current.valid)
            continue;

        gate_fold[gate] = current.fold;
        gate_confidence[gate] = current.confidence;
        gate_solved[gate] = 1;

        grid[gate] +=
            static_cast<float>(current.fold) *
            2.0f * nyq[gate];

        for (const int nb : neighbors8[gate])
        {
            if (nb >= 0 &&
                protected_gate[nb] &&
                !gate_solved[nb])
            {
                push_hard_gate(nb);
            }
        }
    }

    // One cheap relaxation pass. It uses already corrected neighbors and can
    // fill gates that became decidable only after the first growth wave.
    for (size_t t = 0; t < count; ++t)
    {
        if (!protected_gate[t] ||
            gate_solved[t])
            continue;

        const GateCandidate c =
            choose_hard_gate(
                static_cast<int>(t));

        if (!c.valid ||
            c.confidence < 0.30f)
            continue;

        gate_fold[t] = c.fold;
        gate_confidence[t] = c.confidence;
        gate_solved[t] = 1;

        grid[t] +=
            static_cast<float>(c.fold) *
            2.0f * nyq[t];
    }

    // ---------------------------------------------------------------
    // Write corrected velocity.
    // ---------------------------------------------------------------
    // ---------------------------------------------------------------
    // Conservative local repair.
    //
    // Never repair:
    //   * regions with vertical evidence
    //   * high-shear regions
    //   * TVS-like regions
    //
    // Repair can only change +/- one Nyquist fold.
    // ---------------------------------------------------------------

    for (int pass = 0;
         pass < REPAIR_PASSES;
         ++pass)
    {
        bool changed = false;

        for (size_t i = 0;
             i < regions.size();
             ++i)
        {
            Region& region =
                regions[i];

            if (!region.resolved)
                continue;

            if (region.boundary.empty())
                continue;

            if (region.has_vertical)
                continue;

            if (region.high_shear ||
                region.tvs_like)
                continue;

            double mismatch[3] =
                {0.0, 0.0, 0.0};

            double considered_weight =
                0.0;

            for (const Boundary& edge :
                 region.boundary)
            {
                const int nb_region =
                    labels[edge.neighbor];

                if (nb_region < 0 ||
                    !regions[nb_region].resolved)
                    continue;

                const double weight =
                    std::sqrt(
                        static_cast<double>(
                            std::max<size_t>(
                                1,
                                regions[nb_region]
                                    .gates.size())));

                considered_weight +=
                    weight;

                for (int shift = -1;
                     shift <= 1;
                     ++shift)
                {
                    const float candidate =
                        grid[edge.gate] +
                        static_cast<float>(
                            shift) *
                        2.0f *
                        region.nyquist;

                    mismatch[shift + 1] +=
                        weight *
                        std::fabs(
                            candidate -
                            grid[edge.neighbor]);
                }
            }

            if (considered_weight <=
                0.0)
                continue;

            const int best_shift =
                static_cast<int>(
                    std::min_element(
                        mismatch,
                        mismatch + 3) -
                    mismatch) - 1;

            if (best_shift == 0)
                continue;

            if (mismatch[
                    best_shift + 1]
                >=
                REPAIR_HYSTERESIS *
                mismatch[1])
                continue;

            // Never let repair push a low-confidence region through a
            // large fold based on weak evidence.
            if (region.confidence <
                CONFIDENCE_ACCEPT)
                continue;

            const float shift_value =
                static_cast<float>(
                    best_shift) *
                2.0f *
                region.nyquist;

            for (const int gate :
                 region.gates)
            {
                grid[gate] +=
                    shift_value;
            }

            region.fold +=
                best_shift;

            changed = true;
        }

        if (!changed)
            break;
    }

    // ---------------------------------------------------------------
    // Write corrected velocity.
    // ---------------------------------------------------------------

    for (size_t t = 0;
         t < count;
         ++t)
    {
        packed[3 * t + 2] =
            grid[t];
    }

    // ---------------------------------------------------------------
    // Publish this tilt as the vertical prior.
    //
    // IMPORTANT:
    // Only publish regions with actual evidence. Fold-0 disconnected
    // seeds are not allowed to contaminate the next elevation.
    // ---------------------------------------------------------------

    std::vector<float> prior_velocity(
        count,
        VELOCITY_NAN);

    for (size_t t = 0;
         t < count;
         ++t)
    {
        if (protected_gate[t])
        {
            if (gate_solved[t] &&
                gate_confidence[t] >= 0.30f)
            {
                prior_velocity[t] = grid[t];
            }
            continue;
        }

        const int label = labels[t];
        if (label < 0)
            continue;

        const Region& region =
            regions[label];

        if (!region.resolved)
            continue;

        const bool trusted =
            region.confidence >= CONFIDENCE_STRONG ||
            region.vertical_support >= MIN_VERTICAL_SUPPORT_FOR_FOLD ||
            region.horizontal_support >= MIN_HORIZONTAL_SUPPORT_FOR_FOLD;

        if (trusted)
            prior_velocity[t] = grid[t];
    }

    next.valid = true;

    next.series.rays =
        simple_rays;

    next.series.key =
        ground_range;

    next.series.value =
        std::move(prior_velocity);

    return next;
}

} // namespace

// ---------------------------------------------------------------------------
// Public volume entry point
// ---------------------------------------------------------------------------

void dealias_velocity_volume_v7(AllTilt& volume)
{
    std::vector<size_t> order(volume.Tilts.size());
    std::iota(order.begin(), order.end(), 0);

    std::sort(
        order.begin(),
        order.end(),
        [&](size_t a, size_t b)
        {
            return volume.Tilts[a].ElevationAngle <
                   volume.Tilts[b].ElevationAngle;
        });

    // Preserve the original folded field so the second pass can re-solve from
    // raw data rather than treating the first-pass answer as raw input.
    std::vector<std::vector<float>> raw_velocity(
        volume.Tilts.size());

    for (size_t i = 0; i < volume.Tilts.size(); ++i)
    {
        raw_velocity[i] =
            volume.Tilts[i].Radials_VEL;
    }

    // Pass 1: bottom -> top. Each tilt is guided by the already corrected cut
    // immediately below it.
    std::vector<PriorLayer> lower_priors(
        volume.Tilts.size());

    PriorLayer prior;

    for (const size_t index : order)
    {
        SingleTilt& tilt =
            volume.Tilts[index];

        if (raw_velocity[index].empty())
            continue;

        tilt.Radials_VEL =
            raw_velocity[index];

        const SingleTilt* sibling =
            find_split_cut_sibling(volume, tilt);

        PriorLayer result =
            dealias_tilt(
                tilt,
                prior.valid ? &prior : nullptr,
                nullptr,
                sibling);

        if (result.valid)
        {
            lower_priors[index] = result;
            prior = std::move(result);
        }
    }

    // Pass 2: top -> bottom. The solve uses the corrected cut above and the
    // first-pass corrected cut below. This gives the difficult middle/low
    // elevations two independent vertical references without a third full pass.
    std::vector<PriorLayer> final_priors(
        volume.Tilts.size());

    PriorLayer upper_prior;

    for (auto it = order.rbegin();
         it != order.rend();
         ++it)
    {
        const size_t index = *it;
        SingleTilt& tilt = volume.Tilts[index];

        if (raw_velocity[index].empty())
            continue;

        tilt.Radials_VEL =
            raw_velocity[index];

        const PriorLayer* lower = nullptr;

        // Find the nearest lower non-empty prior. In typical NEXRAD data this
        // is simply index-1 in elevation order, but explicit searching makes
        // split/empty cuts safe.
        for (auto jt = it; jt != order.rend(); ++jt)
        {
            if (jt == it)
                continue;

            const size_t lower_index = *jt;

            if (lower_priors[lower_index].valid)
            {
                lower = &lower_priors[lower_index];
                break;
            }
        }

        const SingleTilt* sibling =
            find_split_cut_sibling(volume, tilt);

        PriorLayer result =
            dealias_tilt(
                tilt,
                lower,
                upper_prior.valid ? &upper_prior : nullptr,
                sibling);

        if (result.valid)
        {
            final_priors[index] = result;
            upper_prior = std::move(result);
        }
    }
}

