// velocity_dealias_pyart_region.cpp
//
// Faithful C++ port of PyART's dealias_region_based algorithm.
// Reference: pyart/correct/region_dealias.py + _fast_edge_finder.pyx
//
// Algorithm per tilt:
//   1. Build 2D velocity grid (nrays × nbins)
//   2. Find regions: split [-Vn, Vn] into interval_splits=3 equal bands;
//      4-connectivity connected components within each band (NO wrap in this step).
//   3. Find edges: for each adjacent gate pair in different regions, accumulate
//      sum(vel1 - vel2) / nyquist_interval and count.
//      Bridges up to skip=100 masked gates; rays wrap around.
//   4. Merge regions: repeatedly pop highest-weight edge, merge smaller into
//      larger with fold = round(mean_vel_diff / nyquist_interval).
//      sum_diff values are kept live (updated immediately after each merge).
//   5. Center: subtract weighted mean fold so average fold ≈ 0.
//   6. Apply: velocity += fold * nyquist_interval.
//
// Performance optimisations (accuracy-preserving):
//   - build_edges: hash-map dedup replaces sort-then-deduplicate on all raw hits.
//     Only the much smaller unique-edge list is sorted for canonical ordering.
//   - merge_all_regions: max-heap with lazy deletion replaces O(E) linear scan;
//     O(E log E + K log E) instead of O(K*E).
//   - common_finder rebuild uses a dirty list (O(degree)) not std::fill (O(N)).
//   - edges_in_node removal uses swap-and-pop instead of erase.
//
#include "velocity_dealias_pyart_region.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <queue>
#include <unordered_map>
#include <vector>

namespace {

constexpr int  INTERVAL_SPLITS   = 3;
constexpr int  SKIP_BETWEEN_RAYS = 100;
constexpr int  SKIP_ALONG_RAY    = 100;
constexpr bool CENTERED          = true;

// ---------------------------------------------------------------------------
// 2D grid
// ---------------------------------------------------------------------------
struct Grid2D {
    int nrows = 0, ncols = 0;
    std::vector<float>   vel;
    std::vector<int32_t> label;  // 0 = masked

    float   get_vel  (int r, int c) const { return vel  [r * ncols + c]; }
    int32_t get_label(int r, int c) const { return label[r * ncols + c]; }
    void    set_label(int r, int c, int32_t v) { label[r * ncols + c] = v; }
};

Grid2D build_grid(const SingleTilt& tilt, float& out_nyquist) {
    if (tilt.VelocityRays.empty()) return {};
    out_nyquist = tilt.VelocityRays[0].nyquist;

    const float gs = tilt.VelocityRays[0].gateSpacing;
    float min_range = std::numeric_limits<float>::infinity();
    float max_range = -std::numeric_limits<float>::infinity();
    for (const auto& ray : tilt.VelocityRays) {
        for (size_t g = ray.start; g < ray.start + ray.count; ++g) {
            float rng = tilt.Radials_VEL[3 * g + 1];
            if (rng < min_range) min_range = rng;
            if (rng > max_range) max_range = rng;
        }
    }
    if (!std::isfinite(min_range)) return {};

    const int nrays = (int)tilt.VelocityRays.size();
    const int nbins = (int)std::round((max_range - min_range) / gs) + 1;

    Grid2D g2;
    g2.nrows = nrays;
    g2.ncols = nbins;
    g2.vel  .assign(nrays * nbins, std::numeric_limits<float>::quiet_NaN());
    g2.label.assign(nrays * nbins, 0);

    for (int r = 0; r < nrays; ++r) {
        const auto& ray = tilt.VelocityRays[r];
        for (size_t g = ray.start; g < ray.start + ray.count; ++g) {
            float rng = tilt.Radials_VEL[3 * g + 1];
            float v   = tilt.Radials_VEL[3 * g + 2];
            if (!std::isfinite(v)) continue;
            int bin = (int)std::round((rng - min_range) / gs);
            if (bin >= 0 && bin < nbins)
                g2.vel[r * nbins + bin] = v;
        }
    }
    return g2;
}

// ---------------------------------------------------------------------------
// Matches PyART's _find_sweep_interval_splits: extends bands when velocities
// fall outside [-nyquist, nyquist].
static std::vector<float> compute_interval_limits(
    const Grid2D& g2, float nyquist)
{
    float vmin =  nyquist;
    float vmax = -nyquist;
    for (float v : g2.vel) {
        if (!std::isfinite(v)) continue;
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }

    const float interval = (2.0f * nyquist) / INTERVAL_SPLITS;
    int add_start = 0, add_end = 0;
    if (vmax > nyquist)
        add_start = (int)std::ceil((vmax - nyquist) / interval);
    if (vmin < -nyquist)
        add_end = (int)std::ceil(-(vmin + nyquist) / interval);

    const float start = -nyquist - add_start * interval;
    const float end   =  nyquist + add_end   * interval;
    const int   num   = INTERVAL_SPLITS + 1 + add_start + add_end;

    std::vector<float> limits(num);
    for (int i = 0; i < num; ++i)
        limits[i] = start + i * (end - start) / (num - 1);
    return limits;
}

// Step 2: region finding — NO wrap-around, matches scipy.ndimage.label
// ---------------------------------------------------------------------------
int find_regions(Grid2D& g2, float nyquist) {
    const int nrows = g2.nrows, ncols = g2.ncols;

    const std::vector<float> limits = compute_interval_limits(g2, nyquist);
    const int n_bands = (int)limits.size() - 1;

    int next_label = 1;
    std::vector<int> queue;
    queue.reserve(16384);

    for (int b = 0; b < n_bands; ++b) {
        const float lmin = limits[b];
        const float lmax = limits[b + 1];

        for (int r = 0; r < nrows; ++r) {
            for (int c = 0; c < ncols; ++c) {
                int idx = r * ncols + c;
                if (g2.label[idx] != 0) continue;
                float v = g2.vel[idx];
                if (!std::isfinite(v) || v < lmin || v >= lmax) continue;

                int32_t lbl = next_label++;
                g2.label[idx] = lbl;
                queue.clear();
                queue.push_back(idx);

                for (size_t qi = 0; qi < queue.size(); ++qi) {
                    int cur = queue[qi];
                    int cr  = cur / ncols, cc = cur % ncols;
                    // 4 neighbors — NO wrap
                    int neighbors[4] = {
                        cr > 0         ? (cr-1)*ncols+cc : -1,
                        cr < nrows-1   ? (cr+1)*ncols+cc : -1,
                        cc > 0         ? cr*ncols+(cc-1) : -1,
                        cc < ncols-1   ? cr*ncols+(cc+1) : -1,
                    };
                    for (int nidx : neighbors) {
                        if (nidx < 0 || g2.label[nidx] != 0) continue;
                        float nv = g2.vel[nidx];
                        if (!std::isfinite(nv) || nv < lmin || nv >= lmax) continue;
                        g2.label[nidx] = lbl;
                        queue.push_back(nidx);
                    }
                }
            }
        }
    }
    return next_label - 1;
}

// ---------------------------------------------------------------------------
// Step 3: build edges — hash-map dedup then sort unique edges.
//
// Replaces: collect all raw hits → O(N log N) sort → deduplicate.
// With:     accumulate directly into hash map → O(E log E) sort of unique edges.
// E (unique edges) << N (raw hits), so sort is much cheaper.
// Final edge order (node_b asc, node_a asc) is identical to the original,
// preserving tie-breaking in the merge loop.
// ---------------------------------------------------------------------------
struct Edge {
    int32_t node_a = 0, node_b = 0; // node_a > node_b (canonical)
    double  sum_diff = 0.0;          // sum(vel_a - vel_b) / nyq_interval, live
    int32_t weight   = 0;            // gate-pair count; -999 = deleted
};

std::vector<Edge> build_edges(const Grid2D& g2, float nyquist, int /*nfeatures*/) {
    const int nrows = g2.nrows, ncols = g2.ncols;
    const double nyq_interval = 2.0 * nyquist;

    // Key: (node_a as uint32) in upper 32 bits, (node_b as uint32) in lower 32.
    // node_a > node_b always (canonical).
    struct EdgeAcc { double sum_diff = 0.0; int32_t weight = 0; };
    std::unordered_map<uint64_t, EdgeAcc> edge_map;
    edge_map.reserve(static_cast<size_t>(nrows) * ncols / 2);

    auto add_hit = [&](int32_t a, int32_t b, float va, float vb) {
        if (a < b) { std::swap(a, b); std::swap(va, vb); }
        uint64_t key = ((uint64_t)(uint32_t)a << 32) | (uint32_t)b;
        auto& acc = edge_map[key];
        acc.sum_diff += (double)(va - vb) / nyq_interval;
        ++acc.weight;
    };

    for (int r = 0; r < nrows; ++r) {
        for (int c = 0; c < ncols; ++c) {
            int32_t lbl = g2.get_label(r, c);
            if (lbl == 0) continue;
            float v = g2.get_vel(r, c);

            // left-ray neighbor (with wrap)
            {
                int x = r - 1;
                if (x < 0) x = nrows - 1;
                for (int gap = 0; gap <= SKIP_BETWEEN_RAYS; ++gap) {
                    int32_t nb = g2.get_label(x, c);
                    if (nb != 0) {
                        if (nb != lbl) add_hit(lbl, nb, v, g2.get_vel(x, c));
                        break;
                    }
                    --x;
                    if (x < 0) x = nrows - 1;
                }
            }
            // right-ray neighbor (with wrap)
            {
                int x = r + 1;
                if (x >= nrows) x = 0;
                for (int gap = 0; gap <= SKIP_BETWEEN_RAYS; ++gap) {
                    int32_t nb = g2.get_label(x, c);
                    if (nb != 0) {
                        if (nb != lbl) add_hit(lbl, nb, v, g2.get_vel(x, c));
                        break;
                    }
                    ++x;
                    if (x >= nrows) x = 0;
                }
            }
            // closer-range neighbor (no wrap)
            {
                int y = c - 1;
                for (int gap = 0; gap <= SKIP_ALONG_RAY && y >= 0; ++gap) {
                    int32_t nb = g2.get_label(r, y);
                    if (nb != 0) {
                        if (nb != lbl) add_hit(lbl, nb, v, g2.get_vel(r, y));
                        break;
                    }
                    --y;
                }
            }
            // farther-range neighbor (no wrap)
            {
                int y = c + 1;
                for (int gap = 0; gap <= SKIP_ALONG_RAY && y < ncols; ++gap) {
                    int32_t nb = g2.get_label(r, y);
                    if (nb != 0) {
                        if (nb != lbl) add_hit(lbl, nb, v, g2.get_vel(r, y));
                        break;
                    }
                    ++y;
                }
            }
        }
    }

    // Collect unique edges and sort by (node_b asc, node_a asc) —
    // matches PyART's np.lexsort((index1, index2)) tie-breaking convention.
    std::vector<Edge> edges;
    edges.reserve(edge_map.size());
    for (auto& [key, acc] : edge_map) {
        Edge e;
        e.node_a   = (int32_t)(key >> 32);
        e.node_b   = (int32_t)(key & 0xFFFFFFFFu);
        e.sum_diff = acc.sum_diff;
        e.weight   = acc.weight;
        edges.push_back(e);
    }
    std::sort(edges.begin(), edges.end(), [](const Edge& x, const Edge& y) {
        return x.node_b != y.node_b ? x.node_b < y.node_b : x.node_a < y.node_a;
    });
    return edges;
}

// ---------------------------------------------------------------------------
// Step 4: merge regions, keeping sum_diff live (exact PyART port)
//
// Performance changes vs. original:
//   - Max-heap with lazy deletion replaces O(E) linear scan per merge.
//     Heap entry: (weight, -edge_index); equal weights pop smallest index first,
//     matching the original linear scan's "first occurrence" tie-breaking.
//   - common_finder cleared via dirty list (O(degree)) not std::fill (O(N)).
//   - edges_in_node removal uses swap-and-pop instead of erase+shift.
// ---------------------------------------------------------------------------

void merge_all_regions(
    std::vector<int32_t>& unwrap_num,
    std::vector<int32_t>& node_size,
    std::vector<std::vector<int32_t>>& regions_in_node,
    std::vector<Edge>& edges,
    int nfeatures)
{
    // Per-node edge list
    std::vector<std::vector<int32_t>> edges_in_node(nfeatures + 1);
    for (int i = 0; i < (int)edges.size(); ++i) {
        edges_in_node[edges[i].node_a].push_back(i);
        edges_in_node[edges[i].node_b].push_back(i);
    }

    std::vector<bool>    common_finder(nfeatures + 1, false);
    std::vector<int32_t> common_index (nfeatures + 1, -1);
    // Dirty list: indices where common_finder[i] == true, for O(degree) clearing.
    std::vector<int32_t> common_dirty;
    common_dirty.reserve(256);
    int last_base = -1;

    // Swap-and-pop: O(n) find, O(1) removal (avoids erase's O(n) shift).
    auto remove_from_list = [](std::vector<int32_t>& v, int32_t val) {
        for (size_t i = 0; i < v.size(); ++i) {
            if (v[i] == val) {
                v[i] = v.back();
                v.pop_back();
                return;
            }
        }
    };

    // Max-heap: (weight, neg_edge_index).
    // For equal weights, largest (-index) = smallest index pops first,
    // matching the original linear scan's first-occurrence tie-breaking.
    using HeapEntry = std::pair<int32_t, int32_t>;
    std::priority_queue<HeapEntry> pq;
    for (int i = 0; i < (int)edges.size(); ++i)
        if (edges[i].weight > 0)
            pq.push({edges[i].weight, -i});

    while (!pq.empty()) {
        auto [w, neg_idx] = pq.top(); pq.pop();
        if (w <= 0) break;
        int best = -neg_idx;
        // Stale check: edge was deleted or weight changed (increased by a merge).
        if (edges[best].weight != w) continue;

        Edge& be = edges[best];
        int na = be.node_a, nb = be.node_b;

        // Determine fold: mean(sum_diff) / weight → round
        double mean_diff = be.sum_diff / be.weight;
        int rdiff = (int)std::round(mean_diff);

        // Merge smaller into larger
        int base_node  = (node_size[na] >= node_size[nb]) ? na : nb;
        int merge_node = (base_node == na) ? nb : na;
        if (base_node == nb) rdiff = -rdiff;

        // --- unwrap merge_node's regions ---
        if (rdiff != 0) {
            for (int r : regions_in_node[merge_node])
                unwrap_num[r] += rdiff;
            // Update sum_diff on all edges touching merge_node
            for (int eid : edges_in_node[merge_node]) {
                if (edges[eid].weight < 0) continue;
                if (edges[eid].node_a == merge_node)
                    edges[eid].sum_diff += edges[eid].weight * rdiff;
                else
                    edges[eid].sum_diff -= edges[eid].weight * rdiff;
            }
        }

        // --- delete the best edge ---
        be.weight = -999;
        remove_from_list(edges_in_node[merge_node], best);
        remove_from_list(edges_in_node[base_node],  best);
        common_finder[merge_node] = false;

        // --- rebuild common_finder for base_node if needed ---
        if (last_base != base_node) {
            // Clear only the entries we set (O(degree) not O(nfeatures))
            for (int idx : common_dirty) common_finder[idx] = false;
            common_dirty.clear();

            for (int eid : edges_in_node[base_node]) {
                if (edges[eid].weight < 0) continue;
                // Normalise direction: node_a = base_node
                if (edges[eid].node_b == base_node) {
                    std::swap(edges[eid].node_a, edges[eid].node_b);
                    edges[eid].sum_diff = -edges[eid].sum_diff;
                }
                int neighbor = edges[eid].node_b;
                common_finder[neighbor] = true;
                common_index [neighbor] = eid;
                common_dirty.push_back(neighbor);
            }
            last_base = base_node;
        }

        // --- re-assign merge_node's remaining edges to base_node ---
        std::vector<int32_t> merge_edges = edges_in_node[merge_node];  // copy
        for (int eid : merge_edges) {
            if (edges[eid].weight < 0) continue;
            // Normalise: node_a = merge_node
            if (edges[eid].node_b == merge_node) {
                std::swap(edges[eid].node_a, edges[eid].node_b);
                edges[eid].sum_diff = -edges[eid].sum_diff;
            }
            int neighbor = edges[eid].node_b;
            edges[eid].node_a = base_node;

            if (common_finder[neighbor]) {
                // Combine with existing base_node ↔ neighbor edge
                int base_eid = common_index[neighbor];
                edges[base_eid].weight   += edges[eid].weight;
                edges[base_eid].sum_diff += edges[eid].sum_diff;
                // Push updated weight into heap (lazy update; old entry is stale)
                pq.push({edges[base_eid].weight, -base_eid});
                edges[eid].weight = -999;
                remove_from_list(edges_in_node[merge_node], eid);
                remove_from_list(edges_in_node[neighbor],   eid);
            } else {
                common_finder[neighbor] = true;
                common_index [neighbor] = eid;
                common_dirty.push_back(neighbor);
            }
        }
        // Move remaining (non-deleted) merge_node edges to base_node
        for (int eid : merge_edges) {
            if (edges[eid].weight >= 0)
                edges_in_node[base_node].push_back(eid);
        }
        edges_in_node[merge_node].clear();

        // --- merge node metadata ---
        for (int r : regions_in_node[merge_node])
            regions_in_node[base_node].push_back(r);
        regions_in_node[merge_node].clear();
        node_size[base_node] += node_size[merge_node];
        node_size[merge_node] = 0;

        last_base = base_node;
    }
}

// ---------------------------------------------------------------------------
// Per-tilt entry
// ---------------------------------------------------------------------------
void dealias_tilt_pyart_region(SingleTilt& tilt) {
    if (tilt.VelocityRays.empty() || tilt.Radials_VEL.empty()) return;

    float nyquist = 0.0f;
    Grid2D g2 = build_grid(tilt, nyquist);
    if (g2.nrows == 0 || !std::isfinite(nyquist) || nyquist <= 0.0f) return;

    int nfeatures = find_regions(g2, nyquist);
    if (nfeatures < 2) return;

    // Region sizes (labels are 1-based)
    std::vector<int32_t> region_sizes(nfeatures, 0);
    for (int32_t lbl : g2.label)
        if (lbl > 0) ++region_sizes[lbl - 1];

    std::vector<Edge> edges = build_edges(g2, nyquist, nfeatures);
    if (edges.empty()) return;

    // Initialise per-node state (node index = original region label, 1-based)
    std::vector<int32_t> unwrap_num (nfeatures + 1, 0);
    std::vector<int32_t> node_size  (nfeatures + 1, 0);
    std::vector<std::vector<int32_t>> regions_in_node(nfeatures + 1);
    for (int i = 0; i <= nfeatures; ++i) regions_in_node[i] = {i};
    for (int i = 1; i <= nfeatures; ++i) node_size[i] = region_sizes[i - 1];

    merge_all_regions(unwrap_num, node_size, regions_in_node, edges, nfeatures);

    // Center sweep
    if (CENTERED) {
        int64_t total_gates = 0, total_folds = 0;
        for (int i = 1; i <= nfeatures; ++i) {
            total_gates += region_sizes[i - 1];
            total_folds += (int64_t)unwrap_num[i] * region_sizes[i - 1];
        }
        if (total_gates > 0) {
            int offset = (int)std::round((double)total_folds / total_gates);
            if (offset != 0)
                for (int i = 0; i <= nfeatures; ++i)
                    unwrap_num[i] -= offset;
        }
    }

    // Apply
    const float nqi = 2.0f * nyquist;
    float min_range = std::numeric_limits<float>::infinity();
    for (const auto& ray : tilt.VelocityRays)
        for (size_t g = ray.start; g < ray.start + ray.count; ++g)
            if (tilt.Radials_VEL[3*g+1] < min_range) min_range = tilt.Radials_VEL[3*g+1];

    const float gs    = tilt.VelocityRays[0].gateSpacing;
    const int   ncols = g2.ncols;

    for (int r = 0; r < (int)tilt.VelocityRays.size(); ++r) {
        const auto& ray = tilt.VelocityRays[r];
        for (size_t g = ray.start; g < ray.start + ray.count; ++g) {
            float v = tilt.Radials_VEL[3*g+2];
            if (!std::isfinite(v)) continue;
            int bin = (int)std::round((tilt.Radials_VEL[3*g+1] - min_range) / gs);
            if (bin < 0 || bin >= ncols) continue;
            int32_t lbl = g2.get_label(r, bin);
            if (lbl <= 0) continue;
            int nwrap = unwrap_num[lbl];
            if (nwrap != 0) tilt.Radials_VEL[3*g+2] = v + nwrap * nqi;
        }
    }
}

}  // namespace

void dealias_velocity_volume_pyart_region(AllTilt& volume) {
    for (auto& tilt : volume.Tilts)
        dealias_tilt_pyart_region(tilt);
}
