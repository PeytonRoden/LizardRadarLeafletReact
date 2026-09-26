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

// ============================================================================
// Velocity Dealiasing v6
//
// Design goals:
//
//   1. Fast polar-grid operation.
//   2. Region-based unfolding.
//   3. Per-ray Nyquist support.
//   4. Lower-tilt vertical continuity.
//   5. Optional temporal prior through an externally supplied previous field.
//   6. Explicit preservation of coherent high-shear / TVS structures.
//   7. RHOHV / SW / REF / NCP are confidence modifiers, NOT fold votes.
//   8. Best-first region propagation.
//   9. Confidence from best-vs-second-best fold margin.
//  10. Conservative repair.
//  11. Ambiguous regions are allowed to remain ambiguous.
//
// The most important rule:
//
//     NEVER "repair" a coherent, well-supported velocity couplet simply
//     because it disagrees with a neighboring low-shear region.
//
// ============================================================================


// -----------------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------------

constexpr float VELOCITY_NAN =
    std::numeric_limits<float>::quiet_NaN();

constexpr int MAX_FOLD = 4;

constexpr float AZ_EPS = 0.01f;

// Region connectivity threshold.
// This is intentionally conservative around strong shear.
constexpr float SEG_FRAC = 0.18f;

// Maximum number of local repair iterations.
constexpr int REPAIR_PASSES = 2;

// Lower-tilt prior.
constexpr float VERTICAL_WEIGHT = 1.25f;

// Ground-range matching tolerance.
constexpr float VERTICAL_KEY_TOL_M = 300.0f;

// Moment matching.
constexpr float MOMENT_RANGE_TOL_M = 60.0f;

// Split-cut matching.
constexpr float SPLIT_CUT_ANGLE_EPS = 0.10f;
constexpr long long SPLIT_CUT_TIME_EPS_MS = 30000;

// TDS threshold.
constexpr float RHO_TDS_THRESHOLD = 0.80f;

// Minimum reflectivity for a fold correction to be trusted.
constexpr float MIN_REF_DBZ = 5.0f;

// Minimum fraction of gates having useful REF.
constexpr float MIN_REF_FRACTION_FOR_FOLD = 0.30f;

// Low-ref regions require more independent evidence.
constexpr float MIN_EVIDENCE_FOR_LOW_REF_FOLD = 3.0f;
constexpr size_t MIN_REGION_SIZE_FOR_LOW_REF_FOLD = 4;

// Strong velocity shear.
constexpr float HIGH_SHEAR_FRAC = 0.55f;

// Opposite-sign couplet threshold.
constexpr float COUPLET_MIN_ABS_VELOCITY_FRAC = 0.35f;

// Minimum fraction of a region that needs to be supported by quality data.
constexpr float MIN_QUALITY_FRACTION = 0.35f;

// Confidence thresholds.
constexpr float CONFIDENCE_ACCEPT = 0.20f;
constexpr float CONFIDENCE_STRONG = 0.55f;

// Repair hysteresis.
constexpr float REPAIR_HYSTERESIS = 0.82f;

// Effective Earth radius.
constexpr double EARTH_RADIUS_EFFECTIVE_M =
    6371000.0 * 4.0 / 3.0;


// -----------------------------------------------------------------------------
// Utility
// -----------------------------------------------------------------------------

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

inline float sqr(float x)
{
    return x * x;
}

inline bool finite_positive(float x)
{
    return std::isfinite(x) && x > 0.0f;
}

inline float huber_cost(float residual, float scale)
{
    const float r = std::fabs(residual);

    if (r <= scale) {
        return 0.5f * r * r / scale;
    }

    return r - 0.5f * scale;
}

inline float safe_mean(float sum, int n)
{
    return n > 0 ? sum / static_cast<float>(n) : 0.0f;
}


// -----------------------------------------------------------------------------
// Beam geometry
// -----------------------------------------------------------------------------

inline void beam_height_ground_range(
    float slant_range_m,
    float elevation_deg,
    float& height_m,
    float& ground_range_m)
{
    constexpr double PI = 3.1415926535897932384626433832795;

    const double el =
        static_cast<double>(elevation_deg) * PI / 180.0;

    const double r =
        static_cast<double>(slant_range_m);

    const double Re =
        EARTH_RADIUS_EFFECTIVE_M;

    const double h =
        std::sqrt(
            r * r +
            Re * Re +
            2.0 * r * Re * std::sin(el))
        - Re;

    const double s =
        Re *
        std::asin(
            (r * std::cos(el)) /
            (Re + h));

    height_m = static_cast<float>(h);
    ground_range_m = static_cast<float>(s);
}


// -----------------------------------------------------------------------------
// Ray representation
// -----------------------------------------------------------------------------

struct SimpleRay {
    size_t start = 0;
    size_t count = 0;
    float azimuth = 0.0f;
};


// -----------------------------------------------------------------------------
// Generic keyed series
// -----------------------------------------------------------------------------

struct KeyedSeries {
    std::vector<SimpleRay> rays;
    std::vector<float> key;
    std::vector<float> value;
};


// -----------------------------------------------------------------------------
// Region boundary
// -----------------------------------------------------------------------------

struct Boundary {
    int gate = -1;
    int neighbor = -1;
};


// -----------------------------------------------------------------------------
// Region flags
// -----------------------------------------------------------------------------

enum RegionFlags : uint32_t {
    REGION_NONE          = 0,
    REGION_HIGH_SHEAR    = 1u << 0,
    REGION_TVS_LIKE      = 1u << 1,
    REGION_TDS           = 1u << 2,
    REGION_WEAK_ECHO     = 1u << 3,
    REGION_VERTICAL      = 1u << 4,
    REGION_AMBIGUOUS     = 1u << 5,
    REGION_TEMPORAL      = 1u << 6,
};


// -----------------------------------------------------------------------------
// Region
// -----------------------------------------------------------------------------

struct Region {

    std::vector<int> gates;
    std::vector<Boundary> boundary;

    float nyquist = 0.0f;

    int fold = 0;

    bool resolved = false;

    int version = 0;

    // -------------------------------------------------------------------------
    // Quality statistics
    // -------------------------------------------------------------------------

    size_t ref_support = 0;
    float ref_fraction = 0.0f;

    float mean_ref = VELOCITY_NAN;

    float mean_sw = VELOCITY_NAN;

    float mean_rho = VELOCITY_NAN;

    // -------------------------------------------------------------------------
    // Velocity statistics
    // -------------------------------------------------------------------------

    float mean_velocity = VELOCITY_NAN;

    float velocity_std = 0.0f;

    float max_gradient = 0.0f;

    // -------------------------------------------------------------------------
    // Shear / TVS
    // -------------------------------------------------------------------------

    bool high_shear = false;
    bool tvs_like = false;
    bool tds = false;

    // -------------------------------------------------------------------------
    // Cross-tilt
    // -------------------------------------------------------------------------

    bool has_vertical = false;

    size_t vertical_support = 0;

    // -------------------------------------------------------------------------
    // Confidence
    // -------------------------------------------------------------------------

    float confidence = 0.0f;

    float best_cost = std::numeric_limits<float>::infinity();

    float second_cost = std::numeric_limits<float>::infinity();

    uint32_t flags = REGION_NONE;
};


// -----------------------------------------------------------------------------
// Candidate
// -----------------------------------------------------------------------------

struct Candidate {

    float priority = 0.0f;

    int region = -1;

    int fold = 0;

    int version = 0;

    float confidence = 0.0f;

    bool operator<(const Candidate& other) const
    {
        return priority < other.priority;
    }
};


// -----------------------------------------------------------------------------
// Fold choice
// -----------------------------------------------------------------------------

struct FoldChoice {

    bool has_evidence = false;

    int fold = 0;

    float confidence = 0.0f;

    float best_cost = std::numeric_limits<float>::infinity();

    float second_cost = std::numeric_limits<float>::infinity();
};


// -----------------------------------------------------------------------------
// Ray splitting
// -----------------------------------------------------------------------------

std::vector<SimpleRay> split_rays(
    const std::vector<float>& packed)
{
    std::vector<SimpleRay> rays;

    if (packed.size() < 3 || packed.size() % 3 != 0) {
        return rays;
    }

    const size_t n = packed.size() / 3;

    for (size_t t = 0; t < n; ++t) {

        const bool new_ray =
            rays.empty() ||

            !std::isfinite(packed[3 * t]) ||

            !std::isfinite(packed[3 * t + 1]) ||

            std::fabs(
                packed[3 * t] -
                packed[3 * (t - 1)])
                > AZ_EPS ||

            packed[3 * t + 1] <=
                packed[3 * (t - 1) + 1];

        if (new_ray) {

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


// -----------------------------------------------------------------------------
// Convert VelocityRay -> SimpleRay
// -----------------------------------------------------------------------------

std::vector<SimpleRay> to_simple_rays(
    const std::vector<VelocityRay>& rays,
    const std::vector<float>& packed)
{
    std::vector<SimpleRay> out;

    out.reserve(rays.size());

    for (const auto& r : rays) {

        if (r.start >= packed.size() / 3) {
            continue;
        }

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


// -----------------------------------------------------------------------------
// Generic O(n) alignment
// -----------------------------------------------------------------------------

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

    if (src.rays.empty() || dst_rays.empty()) {
        return out;
    }

    size_t a = 0;

    for (size_t r = 0; r < dst_rays.size(); ++r) {

        const float dst_az =
            dst_rays[r].azimuth;

        while (
            a + 1 < src.rays.size() &&
            az_distance(
                src.rays[a + 1].azimuth,
                dst_az)
            <=
            az_distance(
                src.rays[a].azimuth,
                dst_az))
        {
            ++a;
        }

        if (
            az_distance(
                src.rays[a].azimuth,
                dst_az)
            > az_tol)
        {
            continue;
        }

        size_t vi =
            dst_rays[r].start;

        const size_t vend =
            vi + dst_rays[r].count;

        size_t si =
            src.rays[a].start;

        const size_t send =
            si + src.rays[a].count;

        while (vi < vend && si < send) {

            const float vk =
                dst_key[vi];

            if (!std::isfinite(vk)) {
                ++vi;
                continue;
            }

            while (
                si + 1 < send &&
                std::isfinite(src.key[si + 1]) &&
                std::fabs(
                    src.key[si + 1] - vk)
                <=
                std::fabs(
                    src.key[si] - vk))
            {
                ++si;
            }

            if (!std::isfinite(src.key[si])) {
                ++si;
                continue;
            }

            if (
                std::fabs(
                    src.key[si] - vk)
                <= key_tol)
            {
                out[vi] =
                    src.value[si];
            }

            ++vi;
        }
    }

    return out;
}


// -----------------------------------------------------------------------------
// Align auxiliary moment
// -----------------------------------------------------------------------------

std::vector<float> align_moment(
    const std::vector<float>& aux_packed,
    const std::vector<SimpleRay>& vel_rays,
    const std::vector<float>& vel_dist,
    size_t vel_count)
{
    KeyedSeries src;

    src.rays =
        split_rays(aux_packed);

    const size_t n =
        aux_packed.size() / 3;

    src.key.resize(n);

    src.value.resize(n);

    for (size_t i = 0; i < n; ++i) {

        src.key[i] =
            aux_packed[3 * i + 1];

        src.value[i] =
            aux_packed[3 * i + 2];
    }

    return align_series(
        src,
        vel_rays,
        vel_dist,
        vel_count,
        1.0f,
        MOMENT_RANGE_TOL_M);
}


// -----------------------------------------------------------------------------
// Previous-tilt prior
// -----------------------------------------------------------------------------

struct PriorLayer {

    bool valid = false;

    KeyedSeries series;
};


// -----------------------------------------------------------------------------
// Split-cut sibling
// -----------------------------------------------------------------------------

const SingleTilt* find_split_cut_sibling(
    const AllTilt& volume,
    const SingleTilt& vel_tilt)
{
    const SingleTilt* best = nullptr;

    float best_angle_diff =
        std::numeric_limits<float>::infinity();

    const long long ms_a =
        static_cast<long long>(
            vel_tilt.msg_31.collect_date)
        * 86400000LL
        +
        static_cast<long long>(
            vel_tilt.msg_31.collect_ms);

    for (const auto& other : volume.Tilts) {

        if (&other == &vel_tilt) {
            continue;
        }

        if (
            other.Radials_RHO.empty() &&
            other.Radials_ZDR.empty())
        {
            continue;
        }

        const float angle_diff =
            std::fabs(
                other.ElevationAngle -
                vel_tilt.ElevationAngle);

        if (
            angle_diff >=
            SPLIT_CUT_ANGLE_EPS)
        {
            continue;
        }

        const long long ms_b =
            static_cast<long long>(
                other.msg_31.collect_date)
            * 86400000LL
            +
            static_cast<long long>(
                other.msg_31.collect_ms);

        if (
            std::llabs(ms_a - ms_b) >=
            SPLIT_CUT_TIME_EPS_MS)
        {
            continue;
        }

        if (
            angle_diff <
            best_angle_diff)
        {
            best_angle_diff =
                angle_diff;

            best = &other;
        }
    }

    return best;
}


// ============================================================================
// Main tilt dealiaser
// ============================================================================

PriorLayer dealias_tilt(
    SingleTilt& tilt,
    const PriorLayer& prior,
    const SingleTilt* sibling)
{
    PriorLayer next;

    std::vector<float>& packed =
        tilt.Radials_VEL;

    if (
        packed.size() < 24 ||
        packed.size() % 3 != 0)
    {
        return next;
    }

    const size_t count =
        packed.size() / 3;

    if (
        count >
        static_cast<size_t>(
            std::numeric_limits<int>::max() / 3))
    {
        return next;
    }


    // -------------------------------------------------------------------------
    // Nyquist
    // -------------------------------------------------------------------------

    const float fallback_nyq =
        static_cast<float>(
            tilt.vol_el_rad.rad.nyquist_vel)
        / 100.0f;


    // -------------------------------------------------------------------------
    // Rays
    // -------------------------------------------------------------------------

    const int num_triplets =
        static_cast<int>(count);

    std::vector<VelocityRay> rays =
        tilt.VelocityRays;

    if (rays.empty()) {

        for (int t = 0;
             t < num_triplets;
             ++t)
        {
            if (
                t == 0 ||

                std::fabs(
                    packed[3 * t] -
                    packed[3 * (t - 1)])
                > AZ_EPS ||

                packed[3 * t + 1] <=
                    packed[3 * (t - 1) + 1])
            {
                rays.push_back({
                    static_cast<size_t>(t),
                    0,
                    fallback_nyq,
                    tilt.gateSpacing
                });
            }

            ++rays.back().count;
        }

        if (
            !tilt.VelNyquist.empty() &&
            tilt.VelNyquist.size() != rays.size())
        {
            return next;
        }

        for (size_t r = 0;
             r < rays.size();
             ++r)
        {
            if (!tilt.VelNyquist.empty()) {
                rays[r].nyquist =
                    tilt.VelNyquist[r];
            }
        }
    }

    if (rays.size() < 4) {
        return next;
    }


    // -------------------------------------------------------------------------
    // Validate rays and expand Nyquist
    // -------------------------------------------------------------------------

    size_t expected_start = 0;

    std::vector<float> nyq(
        count,
        VELOCITY_NAN);

    for (const auto& ray : rays) {

        if (
            ray.start != expected_start ||
            ray.count == 0 ||
            ray.count >
                count - expected_start)
        {
            return next;
        }

        expected_start += ray.count;

        if (
            !finite_positive(
                ray.gateSpacing))
        {
            return next;
        }

        if (
            !std::isfinite(
                packed[3 * ray.start]) ||
            packed[3 * ray.start] < 0.0f ||
            packed[3 * ray.start] >= 360.0f)
        {
            return next;
        }

        const float use_nyq =
            (
                std::isfinite(ray.nyquist) &&
                ray.nyquist > 0.0f
            )
            ? ray.nyquist
            : fallback_nyq;

        if (!(use_nyq > 0.0f)) {
            return next;
        }

        for (
            size_t t = ray.start;
            t < expected_start;
            ++t)
        {
            if (
                !std::isfinite(
                    packed[3 * t + 1]) ||

                (
                    t > ray.start &&
                    packed[3 * t + 1] <=
                        packed[3 * (t - 1) + 1]
                ))
            {
                return next;
            }

            nyq[t] = use_nyq;
        }
    }

    if (expected_start != count) {
        return next;
    }


    // -------------------------------------------------------------------------
    // Polar grid
    // -------------------------------------------------------------------------

    std::vector<float> grid(count);

    for (size_t i = 0; i < count; ++i) {
        grid[i] =
            packed[3 * i + 2];
    }


    // -------------------------------------------------------------------------
    // Neighbor graph
    //
    // [0] radial previous
    // [1] radial next
    // [2] azimuth previous
    // [3] azimuth next
    // -------------------------------------------------------------------------

    std::vector<std::array<int, 4>> neighbors(
        count,
        {-1, -1, -1, -1});

    auto connect =
        [&](int a, int b, int direction)
    {
        if (
            a < 0 ||
            b < 0 ||
            !std::isfinite(grid[a]) ||
            !std::isfinite(grid[b]) ||
            !std::isfinite(nyq[a]) ||
            !std::isfinite(nyq[b]))
        {
            return;
        }

        neighbors[a][direction] =
            b;

        neighbors[b][direction + 1] =
            a;
    };


    // -------------------------------------------------------------------------
    // Ray connectivity
    // -------------------------------------------------------------------------

    for (size_t r = 0;
         r < rays.size();
         ++r)
    {
        const auto& ray =
            rays[r];

        const size_t end =
            ray.start + ray.count;

        for (
            size_t t = ray.start + 1;
            t < end;
            ++t)
        {
            const float spacing =
                packed[3 * t + 1] -
                packed[3 * (t - 1) + 1];

            if (
                std::fabs(
                    spacing -
                    ray.gateSpacing)
                <= 0.1f)
            {
                connect(
                    static_cast<int>(t - 1),
                    static_cast<int>(t),
                    0);
            }
        }
    }


    // -------------------------------------------------------------------------
    // Azimuth connectivity
    // -------------------------------------------------------------------------

    auto az_gap =
        [&](size_t a, size_t b)
    {
        return std::fmod(
            packed[3 * rays[b].start] -
            packed[3 * rays[a].start] +
            360.0f,
            360.0f);
    };


    std::vector<float> az_steps;

    for (size_t r = 1;
         r < rays.size();
         ++r)
    {
        const float gap =
            az_gap(r - 1, r);

        if (
            gap > AZ_EPS &&
            gap <= 1.5f)
        {
            az_steps.push_back(gap);
        }
    }

    float max_gap = 0.0f;

    if (!az_steps.empty()) {

        std::sort(
            az_steps.begin(),
            az_steps.end());

        max_gap =
            std::min(
                1.5f,
                1.5f *
                az_steps[
                    az_steps.size() / 2]);
    }


    for (size_t r = 0;
         r < rays.size();
         ++r)
    {
        const size_t next_r =
            (r + 1) % rays.size();

        const float gap =
            az_gap(r, next_r);

        if (
            gap <= AZ_EPS ||
            gap > max_gap)
        {
            continue;
        }

        const auto& a_ray =
            rays[r];

        const auto& b_ray =
            rays[next_r];

        size_t a =
            a_ray.start;

        size_t b =
            b_ray.start;

        const size_t a_end =
            a_ray.start +
            a_ray.count;

        const size_t b_end =
            b_ray.start +
            b_ray.count;

        while (
            a < a_end &&
            b < b_end)
        {
            const float diff =
                packed[3 * a + 1] -
                packed[3 * b + 1];

            if (
                std::fabs(diff)
                <= 0.1f)
            {
                connect(
                    static_cast<int>(a++),
                    static_cast<int>(b++),
                    2);
            }
            else if (diff < 0.0f) {
                ++a;
            }
            else {
                ++b;
            }
        }
    }


    // -------------------------------------------------------------------------
    // Auxiliary fields
    // -------------------------------------------------------------------------

    const std::vector<SimpleRay>
        simple_rays =
            to_simple_rays(
                rays,
                packed);

    std::vector<float> vel_dist(count);

    for (size_t i = 0;
         i < count;
         ++i)
    {
        vel_dist[i] =
            packed[3 * i + 1];
    }

    const std::vector<float>
        ref_aligned =
            align_moment(
                tilt.Radials_REF,
                simple_rays,
                vel_dist,
                count);

    const std::vector<float>
        sw_aligned =
            align_moment(
                tilt.Radials_SW,
                simple_rays,
                vel_dist,
                count);

    const std::vector<float>
        rho_aligned =
            sibling
                ? align_moment(
                    sibling->Radials_RHO,
                    simple_rays,
                    vel_dist,
                    count)
                : std::vector<float>(
                    count,
                    VELOCITY_NAN);


    // -------------------------------------------------------------------------
    // Ground range
    // -------------------------------------------------------------------------

    std::vector<float> ground_range(
        count,
        VELOCITY_NAN);

    for (size_t i = 0;
         i < count;
         ++i)
    {
        float height_unused;
        float ground;

        beam_height_ground_range(
            vel_dist[i],
            tilt.ElevationAngle,
            height_unused,
            ground);

        ground_range[i] =
            ground;
    }


    // -------------------------------------------------------------------------
    // Vertical prior
    // -------------------------------------------------------------------------

    const std::vector<float>
        vertical_aligned =
            prior.valid
                ? align_series(
                    prior.series,
                    simple_rays,
                    ground_range,
                    count,
                    1.0f,
                    VERTICAL_KEY_TOL_M)
                : std::vector<float>(
                    count,
                    VELOCITY_NAN);


    // =========================================================================
    // PHASE 1: REGION SEGMENTATION
    // =========================================================================

    std::vector<int> labels(
        count,
        -1);

    std::vector<Region> regions;

    regions.reserve(count / 8);


    for (int t = 0;
         t < num_triplets;
         ++t)
    {
        if (
            labels[t] >= 0 ||
            !std::isfinite(grid[t]) ||
            !std::isfinite(nyq[t]))
        {
            continue;
        }

        const int label =
            static_cast<int>(
                regions.size());

        regions.emplace_back();

        Region& region =
            regions.back();

        region.nyquist =
            nyq[t];

        region.gates.push_back(t);

        labels[t] =
            label;


        // ---------------------------------------------------------------------
        // Flood fill
        // ---------------------------------------------------------------------

        for (
            size_t cursor = 0;
            cursor < region.gates.size();
            ++cursor)
        {
            const int gate =
                region.gates[cursor];

            for (const int nb :
                 neighbors[gate])
            {
                if (
                    nb < 0 ||
                    labels[nb] >= 0 ||
                    nyq[nb] != region.nyquist)
                {
                    continue;
                }

                // Do not bridge a large raw velocity discontinuity.
                if (
                    std::fabs(
                        grid[gate] -
                        grid[nb])
                    >
                    SEG_FRAC *
                    region.nyquist)
                {
                    continue;
                }

                labels[nb] =
                    label;

                region.gates.push_back(
                    nb);
            }
        }
    }


    if (regions.empty()) {
        return next;
    }


    // =========================================================================
    // PHASE 2: REGION STATISTICS
    // =========================================================================

    for (Region& region :
         regions)
    {
        float ref_sum = 0.0f;
        int ref_n = 0;

        float sw_sum = 0.0f;
        int sw_n = 0;

        float rho_sum = 0.0f;
        int rho_n = 0;

        float vel_sum = 0.0f;
        int vel_n = 0;

        for (const int gate :
             region.gates)
        {
            const float ref =
                ref_aligned[gate];

            if (std::isfinite(ref)) {

                ++region.ref_support;

                if (ref > MIN_REF_DBZ) {
                    ref_sum += ref;
                    ++ref_n;
                }
            }

            const float sw =
                sw_aligned[gate];

            if (std::isfinite(sw)) {
                sw_sum += sw;
                ++sw_n;
            }

            const float rho =
                rho_aligned[gate];

            if (std::isfinite(rho)) {
                rho_sum += rho;
                ++rho_n;
            }

            const float v =
                grid[gate];

            if (std::isfinite(v)) {
                vel_sum += v;
                ++vel_n;
            }

            if (
                std::isfinite(
                    vertical_aligned[gate]))
            {
                region.has_vertical =
                    true;

                ++region.vertical_support;
            }
        }

        region.ref_fraction =
            static_cast<float>(
                region.ref_support)
            /
            static_cast<float>(
                region.gates.size());

        region.mean_ref =
            safe_mean(
                ref_sum,
                ref_n);

        region.mean_sw =
            safe_mean(
                sw_sum,
                sw_n);

        region.mean_rho =
            safe_mean(
                rho_sum,
                rho_n);

        region.mean_velocity =
            safe_mean(
                vel_sum,
                vel_n);

        if (
            region.ref_fraction <
            MIN_REF_FRACTION_FOR_FOLD)
        {
            region.flags |=
                REGION_WEAK_ECHO;
        }

        if (region.has_vertical) {
            region.flags |=
                REGION_VERTICAL;
        }

        if (
            rho_n > 0 &&
            region.mean_rho <
                RHO_TDS_THRESHOLD)
        {
            region.tds = true;

            region.flags |=
                REGION_TDS;
        }
    }


    // =========================================================================
    // PHASE 3: BOUNDARY CONSTRUCTION
    // =========================================================================

    for (int t = 0;
         t < num_triplets;
         ++t)
    {
        if (labels[t] < 0) {
            continue;
        }

        for (const int nb :
             neighbors[t])
        {
            if (
                nb >= 0 &&
                labels[nb] >= 0 &&
                labels[nb] != labels[t])
            {
                regions[
                    labels[t]]
                    .boundary.push_back({
                        t,
                        nb
                    });
            }
        }
    }


    // =========================================================================
    // PHASE 4: SHEAR / TVS ANALYSIS
    //
    // This is the major v6 addition.
    //
    // A large raw velocity jump is NOT automatically something to repair.
    // If the jump is spatially coherent and embedded in quality echo, preserve
    // it.
    // =========================================================================

    for (size_t ri = 0;
         ri < regions.size();
         ++ri)
    {
        Region& region =
            regions[ri];

        if (
            region.boundary.empty())
        {
            continue;
        }

        float max_jump = 0.0f;

        int strong_edges = 0;

        int opposite_sign_edges = 0;

        int valid_edges = 0;

        for (const Boundary& edge :
             region.boundary)
        {
            const float a =
                grid[edge.gate];

            const float b =
                grid[edge.neighbor];

            if (
                !std::isfinite(a) ||
                !std::isfinite(b))
            {
                continue;
            }

            ++valid_edges;

            const float jump =
                std::fabs(a - b);

            max_jump =
                std::max(
                    max_jump,
                    jump);

            if (
                jump >
                HIGH_SHEAR_FRAC *
                    region.nyquist)
            {
                ++strong_edges;
            }

            const bool opposite =
                (a > 0.0f && b < 0.0f) ||
                (a < 0.0f && b > 0.0f);

            if (
                opposite &&
                std::min(
                    std::fabs(a),
                    std::fabs(b))
                >
                COUPLET_MIN_ABS_VELOCITY_FRAC *
                    region.nyquist)
            {
                ++opposite_sign_edges;
            }
        }

        region.max_gradient =
            max_jump;

        if (valid_edges == 0) {
            continue;
        }

        const float strong_fraction =
            static_cast<float>(
                strong_edges)
            /
            static_cast<float>(
                valid_edges);

        const float couplet_fraction =
            static_cast<float>(
                opposite_sign_edges)
            /
            static_cast<float>(
                valid_edges);

        region.high_shear =
            strong_fraction >= 0.20f;

        region.tvs_like =
            region.high_shear &&
            (
                couplet_fraction >= 0.10f ||
                region.tds
            );

        if (region.high_shear) {
            region.flags |=
                REGION_HIGH_SHEAR;
        }

        if (region.tvs_like) {
            region.flags |=
                REGION_TVS_LIKE;
        }
    }


    // =========================================================================
    // PHASE 5: FOLD RESOLUTION
    // =========================================================================

    std::priority_queue<Candidate> pq;

    size_t resolved_count = 0;


    auto choose_fold =
        [&](int index) -> FoldChoice
    {
        Region& region =
            regions[index];

        float same_weight = 0.0f;

        float vertical_weight = 0.0f;

        float same_scale_sum = 0.0f;

        int same_scale_n = 0;

        float vertical_scale_sum = 0.0f;

        int vertical_scale_n = 0;

        bool has_same =
            false;

        bool has_vertical =
            false;


        // ---------------------------------------------------------------------
        // Determine available evidence.
        // ---------------------------------------------------------------------

        for (const Boundary& edge :
             region.boundary)
        {
            const int nb =
                labels[
                    edge.neighbor];

            if (
                nb < 0 ||
                !regions[nb].resolved)
            {
                continue;
            }

            has_same = true;

            same_weight +=
                std::sqrt(
                    static_cast<float>(
                        regions[nb]
                            .gates.size()));

            if (
                std::isfinite(
                    sw_aligned[
                        edge.gate]))
            {
                same_scale_sum +=
                    sw_aligned[
                        edge.gate];

                ++same_scale_n;
            }
        }


        for (const int gate :
             region.gates)
        {
            if (
                std::isfinite(
                    vertical_aligned[
                        gate]))
            {
                has_vertical = true;

                vertical_weight +=
                    VERTICAL_WEIGHT;

                if (
                    std::isfinite(
                        sw_aligned[
                            gate]))
                {
                    vertical_scale_sum +=
                        sw_aligned[
                            gate];

                    ++vertical_scale_n;
                }
            }
        }


        const float evidence =
            same_weight +
            vertical_weight;

        if (evidence <= 0.0f) {
            return {};
        }


        // ---------------------------------------------------------------------
        // Local uncertainty scale.
        // ---------------------------------------------------------------------

        const float same_sw =
            safe_mean(
                same_scale_sum,
                same_scale_n);

        const float vertical_sw =
            safe_mean(
                vertical_scale_sum,
                vertical_scale_n);

        float sw_hint = 0.0f;

        if (
            same_scale_n > 0 &&
            vertical_scale_n > 0)
        {
            sw_hint =
                0.5f *
                (same_sw + vertical_sw);
        }
        else if (
            same_scale_n > 0)
        {
            sw_hint =
                same_sw;
        }
        else
        {
            sw_hint =
                vertical_sw;
        }

        float scale =
            std::max(
                1.5f,
                std::max(
                    0.15f *
                        region.nyquist,
                    0.5f *
                        sw_hint));


        // TDS/high-shear regions are intentionally more tolerant.
        if (region.tds) {
            scale *= 1.8f;
        }

        if (region.high_shear) {
            scale *= 1.35f;
        }


        // ---------------------------------------------------------------------
        // Candidate folds.
        // ---------------------------------------------------------------------

        int best_k = 0;

        float best_cost =
            std::numeric_limits<float>
                ::infinity();

        float second_cost =
            std::numeric_limits<float>
                ::infinity();


        for (
            int k = -MAX_FOLD;
            k <= MAX_FOLD;
            ++k)
        {
            const float shift =
                static_cast<float>(k)
                * 2.0f
                * region.nyquist;

            float cost = 0.0f;


            // -----------------------------------------------------------------
            // Same-tilt evidence
            // -----------------------------------------------------------------

            if (has_same) {

                for (const Boundary& edge :
                     region.boundary)
                {
                    const int nb =
                        labels[
                            edge.neighbor];

                    if (
                        nb < 0 ||
                        !regions[nb]
                            .resolved)
                    {
                        continue;
                    }

                    const float candidate =
                        grid[edge.gate]
                        + shift;

                    const float reference =
                        grid[edge.neighbor];

                    float weight =
                        std::sqrt(
                            static_cast<float>(
                                regions[nb]
                                    .gates.size()));

                    // Strong shear is precisely where we should NOT let
                    // same-tilt raw continuity dominate.
                    if (region.high_shear) {
                        weight *= 0.35f;
                    }

                    cost +=
                        weight *
                        huber_cost(
                            candidate -
                                reference,
                            scale);
                }
            }


            // -----------------------------------------------------------------
            // Vertical evidence
            // -----------------------------------------------------------------

            if (has_vertical) {

                for (const int gate :
                     region.gates)
                {
                    const float prior_v =
                        vertical_aligned[
                            gate];

                    if (
                        !std::isfinite(
                            prior_v))
                    {
                        continue;
                    }

                    const float candidate =
                        grid[gate]
                        + shift;

                    float weight =
                        VERTICAL_WEIGHT;

                    // Vertical continuity is strong, but don't allow it to
                    // erase a coherent current-sweep TVS.
                    if (region.tvs_like) {
                        weight *= 0.65f;
                    }

                    cost +=
                        weight *
                        huber_cost(
                            candidate -
                                prior_v,
                            scale);
                }
            }


            // -----------------------------------------------------------------
            // Mild zero-fold regularization.
            //
            // This is deliberately tiny. The data should determine the fold.
            // -----------------------------------------------------------------

            cost +=
                0.01f *
                std::fabs(
                    static_cast<float>(k));


            if (cost < best_cost) {

                second_cost =
                    best_cost;

                best_cost =
                    cost;

                best_k =
                    k;
            }
            else if (
                cost < second_cost)
            {
                second_cost =
                    cost;
            }
        }


        // ---------------------------------------------------------------------
        // Confidence from cost separation.
        // ---------------------------------------------------------------------

        float confidence = 0.0f;

        if (
            std::isfinite(best_cost) &&
            std::isfinite(second_cost))
        {
            const float margin =
                second_cost -
                best_cost;

            const float denom =
                std::max(
                    1.0f,
                    second_cost);

            confidence =
                std::clamp(
                    margin / denom,
                    0.0f,
                    1.0f);
        }


        // ---------------------------------------------------------------------
        // Quality scaling.
        // ---------------------------------------------------------------------

        float quality = 1.0f;

        if (
            region.ref_fraction <
            MIN_REF_FRACTION_FOR_FOLD)
        {
            quality *= 0.65f;
        }

        if (region.tds) {
            quality *= 1.10f;
        }

        if (region.has_vertical) {
            quality *= 1.10f;
        }

        confidence =
            std::clamp(
                confidence *
                quality,
                0.0f,
                1.0f);


        // ---------------------------------------------------------------------
        // Weak isolated region protection.
        // ---------------------------------------------------------------------

        if (
            best_k != 0 &&
            region.ref_fraction <
                MIN_REF_FRACTION_FOR_FOLD &&
            evidence <
                MIN_EVIDENCE_FOR_LOW_REF_FOLD &&
            region.gates.size() <
                MIN_REGION_SIZE_FOR_LOW_REF_FOLD)
        {
            best_k = 0;

            confidence *= 0.5f;
        }


        // ---------------------------------------------------------------------
        // HIGH-SHEAR PROTECTION
        //
        // If the region is a coherent high-shear/TVS structure, don't accept
        // a fold correction from weak same-tilt evidence alone.
        // ---------------------------------------------------------------------

        if (
            best_k != 0 &&
            region.tvs_like &&
            !has_vertical)
        {
            // Require strong evidence for changing a TVS region when there
            // is no independent vertical reference.
            if (
                confidence <
                CONFIDENCE_STRONG)
            {
                best_k = 0;

                confidence *= 0.5f;
            }
        }


        FoldChoice result;

        result.has_evidence =
            true;

        result.fold =
            best_k;

        result.confidence =
            confidence;

        result.best_cost =
            best_cost;

        result.second_cost =
            second_cost;

        return result;
    };


    // =========================================================================
    // Candidate insertion
    // =========================================================================

    auto push_candidate =
        [&](int index)
    {
        Region& region =
            regions[index];

        if (region.resolved) {
            return;
        }

        const FoldChoice choice =
            choose_fold(index);

        if (!choice.has_evidence) {
            return;
        }

        ++region.version;

        region.confidence =
            choice.confidence;

        region.best_cost =
            choice.best_cost;

        region.second_cost =
            choice.second_cost;

        if (
            choice.confidence <
            CONFIDENCE_ACCEPT)
        {
            region.flags |=
                REGION_AMBIGUOUS;
        }

        float priority =
            choice.confidence;

        // Large regions are more useful seeds, but with diminishing returns.
        priority *=
            std::sqrt(
                static_cast<float>(
                    region.gates.size()));

        // Independent reflectivity support.
        priority *=
            0.75f +
            0.50f *
                region.ref_fraction;

        // Vertical support is extremely useful.
        if (region.has_vertical) {
            priority *= 1.25f;
        }

        // TDS/high-shear gets protected rather than promoted blindly.
        if (region.tvs_like) {
            priority *= 1.10f;
        }

        pq.push({
            priority,
            index,
            choice.fold,
            region.version,
            choice.confidence
        });
    };


    // =========================================================================
    // Resolve
    // =========================================================================

    auto resolve =
        [&](int index, int fold)
    {
        Region& region =
            regions[index];

        if (region.resolved) {
            return;
        }

        region.fold =
            fold;

        region.resolved =
            true;

        ++resolved_count;


        const float shift =
            static_cast<float>(fold)
            * 2.0f
            * region.nyquist;

        for (const int gate :
             region.gates)
        {
            grid[gate] +=
                shift;
        }


        // Push neighboring regions.
        std::vector<int> touched;

        touched.reserve(
            region.boundary.size());

        for (const Boundary& edge :
             region.boundary)
        {
            const int nb =
                labels[
                    edge.neighbor];

            if (
                nb >= 0 &&
                !regions[nb].resolved)
            {
                touched.push_back(nb);
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

        for (const int nb :
             touched)
        {
            push_candidate(nb);
        }
    };


    // =========================================================================
    // Seed ordering
    //
    // Prefer physically trustworthy regions, NOT simply largest regions.
    // =========================================================================

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
            const Region& A =
                regions[a];

            const Region& B =
                regions[b];

            auto score =
                [](const Region& r)
            {
                float s =
                    std::sqrt(
                        static_cast<float>(
                            r.gates.size()));

                s *=
                    0.75f +
                    0.75f *
                        r.ref_fraction;

                if (r.has_vertical) {
                    s *= 1.30f;
                }

                if (r.tds) {
                    s *= 1.10f;
                }

                return s;
            };

            return score(A) >
                   score(B);
        });


    size_t seed_cursor = 0;


    auto seed_next_island =
        [&]()
    {
        while (
            seed_cursor <
                seed_order.size() &&
            regions[
                seed_order[
                    seed_cursor]]
                .resolved)
        {
            ++seed_cursor;
        }

        if (
            seed_cursor >=
            seed_order.size())
        {
            return;
        }

        Region& region =
            regions[
                seed_order[
                    seed_cursor]];

        // Global fold origin is arbitrary. Fold zero is the least-assumptive
        // choice for a disconnected component.
        resolve(
            static_cast<int>(
                seed_order[
                    seed_cursor]),
            0);
    };


    // =========================================================================
    // Initial candidates
    // =========================================================================

    for (size_t i = 0;
         i < regions.size();
         ++i)
    {
        push_candidate(
            static_cast<int>(i));
    }


    // =========================================================================
    // Best-first propagation
    // =========================================================================

    while (
        resolved_count <
        regions.size())
    {
        if (pq.empty()) {

            seed_next_island();

            continue;
        }

        const Candidate top =
            pq.top();

        pq.pop();

        Region& region =
            regions[
                top.region];

        if (
            region.resolved ||
            top.version !=
                region.version)
        {
            continue;
        }

        // A very low-confidence candidate should not become a propagation
        // anchor. Instead let it remain raw/ambiguous until the component
        // becomes connected to something stronger.
        if (
            top.confidence <
            CONFIDENCE_ACCEPT &&
            !region.has_vertical)
        {
            continue;
        }

        resolve(
            top.region,
            top.fold);
    }


    // =========================================================================
    // CONSERVATIVE REPAIR
    //
    // Only repair regions without independent vertical evidence and without
    // a coherent high-shear/TVS signature.
    // =========================================================================

    for (
        int pass = 0;
        pass < REPAIR_PASSES;
        ++pass)
    {
        bool changed = false;

        for (
            size_t i = 0;
            i < regions.size();
            ++i)
        {
            Region& region =
                regions[i];

            if (
                region.boundary.empty())
            {
                continue;
            }

            // NEVER casually repair a vertically supported TVS.
            if (
                region.has_vertical ||
                region.tvs_like)
            {
                continue;
            }

            double mismatch[3] = {
                0.0,
                0.0,
                0.0
            };

            double total_weight =
                0.0;

            for (const Boundary& edge :
                 region.boundary)
            {
                const int nb =
                    labels[
                        edge.neighbor];

                if (
                    nb < 0 ||
                    !regions[nb].resolved)
                {
                    continue;
                }

                const double weight =
                    std::sqrt(
                        static_cast<double>(
                            regions[nb]
                                .gates.size()));

                total_weight +=
                    weight;

                for (
                    int shift = -1;
                    shift <= 1;
                    ++shift)
                {
                    const float candidate =
                        grid[edge.gate]
                        +
                        static_cast<float>(
                            shift)
                        *
                        2.0f
                        *
                        region.nyquist;

                    mismatch[
                        shift + 1] +=
                        weight *
                        std::fabs(
                            candidate -
                            grid[
                                edge.neighbor]);
                }
            }

            if (
                total_weight <=
                static_cast<double>(
                    region.gates.size()))
            {
                continue;
            }

            const int best_shift =
                static_cast<int>(
                    std::min_element(
                        mismatch,
                        mismatch + 3)
                    - mismatch)
                - 1;

            if (best_shift == 0) {
                continue;
            }

            if (
                mismatch[
                    best_shift + 1]
                >=
                REPAIR_HYSTERESIS *
                    mismatch[1])
            {
                continue;
            }

            // If this is a strong-shear region, require overwhelming evidence.
            if (
                region.high_shear &&
                mismatch[
                    best_shift + 1]
                >=
                0.60 *
                    mismatch[1])
            {
                continue;
            }

            const float correction =
                static_cast<float>(
                    best_shift)
                *
                2.0f
                *
                region.nyquist;

            for (const int gate :
                 region.gates)
            {
                grid[gate] +=
                    correction;
            }

            region.fold +=
                best_shift;

            region.flags |=
                REGION_AMBIGUOUS;

            changed = true;
        }

        if (!changed) {
            break;
        }
    }


    // =========================================================================
    // Write back
    // =========================================================================

    for (size_t i = 0;
         i < count;
         ++i)
    {
        packed[3 * i + 2] =
            grid[i];
    }


    // =========================================================================
    // Publish current tilt as vertical prior for next tilt
    // =========================================================================

    next.valid = true;

    next.series.rays =
        simple_rays;

    next.series.key =
        ground_range;

    next.series.value =
        grid;

    return next;
}


// ============================================================================
// Public API
// ============================================================================

} // namespace


void dealias_velocity_volume_internal(
    AllTilt& volume)
{
    PriorLayer prior;

    for (auto& tilt :
         volume.Tilts)
    {
        if (
            tilt.Radials_VEL.empty())
        {
            continue;
        }

        const SingleTilt* sibling =
            find_split_cut_sibling(
                volume,
                tilt);

        PriorLayer resolved =
            dealias_tilt(
                tilt,
                prior,
                sibling);

        if (resolved.valid) {
            prior =
                std::move(resolved);
        }
    }
}


// Keep the existing public API.
void dealias_velocity_volume_v4(
    AllTilt& volume)
{
    dealias_velocity_volume_internal(
        volume);
}
 
// void dealias_velocity_volume_v4(AllTilt& volume)
// {
//     // AllTilt::Tilts is sorted ascending by ElevationAngle at ingest, so
//     // this loop naturally proceeds low-to-high elevation.
//     PriorLayer prior;
//     for (auto& tilt : volume.Tilts) {
//         if (tilt.Radials_VEL.empty()) continue; // surveillance-only split-cut pass, nothing to dealias
//         const SingleTilt* sibling = find_split_cut_sibling(volume, tilt);
//         PriorLayer resolved = dealias_tilt(tilt, prior, sibling);
//         if (resolved.valid) prior = std::move(resolved);
//     }
// }
 

