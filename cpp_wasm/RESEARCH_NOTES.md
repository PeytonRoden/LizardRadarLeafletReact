# Velocity Dealiaser Research Notes
Last updated: 2026-09-26

---

## The Problem

Doppler radar measures the radial component of wind velocity. The measurement is ambiguous: any true velocity v_true that satisfies

    v_measured = v_true + 2k·Vn    (k = any integer)

produces the same raw sample. Vn is the Nyquist velocity — the maximum unambiguous speed for a given PRF. For VCP 21 at KPAH, Vn ≈ 27.8 m/s at low tilts and rises to ~35 m/s at high tilts. A 55 m/s wind (plausible in a tornado environment) would be measured as 55 - 2×27.8 = -0.6 m/s, completely indistinguishable from calm air.

**Dealiasing** recovers k for each gate. It is the first step in any quantitative velocity analysis.

**Fold jump**: a gate adjacent (range or azimuth) to a neighbor whose velocity differs by ≥ 0.9 × 2Vn. This is the reference-free quality metric we use throughout. Lower is better.

---

## Test Data

Five NEXRAD Level-2 archives from KPAH (Paducah KY) during the December 11, 2021 tornado outbreak. VCP 21 (surveillance + Doppler), split-cut scanning at low elevations.

| File | Time (UTC) | Notes |
|------|-----------|-------|
| KPAH20211211_025905_V06 | 02:59 | Tornado active |
| KPAH20211211_030505_V06 | 03:05 | Tornado active |
| KPAH20211211_031104_V06 | 03:11 | Tornado active |
| KPAH20211211_032349_V06 | 03:23 | Tornado active |
| KPAH20211211_032939_V06 | 03:29 | Dissipating |

Each file has 16–17 velocity tilts (tilt = one 360° scan at a fixed elevation). Low tilts (t03–t09, 0.5°–1.4°) are the hardest because wind structure is most complex near the surface and Vn is lowest.

---

## Fold-Jump Scores (lower = better)

### Per-file totals

| Version | 025905 | 030505 | 031104 | 032349 | 032939 | **TOTAL** |
|---------|--------|--------|--------|--------|--------|-----------|
| v10_1   | 1073   | 725    | 770    | 1053   | 1128   | **4749**  |
| v10_2   | 1173   | 985    | 1087   | 1536   | 1149   | **5930**  |
| v10_3   | 1127   | 880    | 800    | 1033   | 1129   | **4969**  |
| PyART region | 269 | 305 | 72 | 163 | 246 | **1055** |
| PyART unwrap | 431 | 387 | 327 | 532 | 603 | **2280** |

v10_3 is the latest work-in-progress. It beats v10_2 by ~961 jumps but is still 220 behind v10_1. PyART region-based remains ~4.5x better than our best.

### Detailed per-tilt (file 025905, worst-first for our algorithm)

| Tilt | Elev | Gates | v10_1 | v10_2 | v10_3 | Region | Unwrap |
|------|------|-------|-------|-------|-------|--------|--------|
| t09  | 1.35° | 289K | 184 | 226 | 219 | 12 | 73 |
| t07  | 0.88° | 306K | 303 | 305 | 303 | 53 | 40 |
| t03  | 0.53° | 302K | 139 | 122 | 142 | 34 | 90 |
| t05  | 0.53° | 300K | 119 | 124 | 119 | 22 | 50 |
| t04  | 0.53° | 307K | 99  | 99  | 99  | 22 | 66 |
| t11  | 2.44° | 129K | 47  | 54  | 54  | 27 | 31 |
| t10  | 1.81° | 120K | 36  | 36  | 36  | 62 | 27 |
| t15  | 6.38° | 115K | 3   | 49  | 16  | 11 | 0  |

Note t10 and t15: v10_1 actually beats PyART region at t10 (36 vs 62). PyART is not ground truth — it also makes mistakes.

---

## Algorithm Pipeline (v10_1 / v10_3 shared structure)

```
For each tilt in order of increasing elevation:
  Stage A:  per-tilt region solve
For the volume:
  Stage V:  absolute fold anchoring via VAD
  Stage B:  vertical reconciliation
  Stage X:  vortex (tornado couplet) check
  Stage C:  hard gate rescue
```

### Stage A: Per-tilt region solve

**Entry**: `dealias_tilt_stage_a(SingleTilt&)` → returns `TiltCache`

1. **Build adjacency graph** — for each gate, find neighbors within `RANGE_BRIDGE_GATES=3` range gates and `AZ_BRIDGE_MULT=2.3` × typical_spacing azimuth. Bridging handles dropped radials without creating false isolation.

2. **Classify hard gates** — gates are flagged `hard=1` if:
   - They sit next to a circular-velocity discontinuity ≥ `HARD_CIRC_FRAC=0.5` × Vn with ≥25 dBZ reflectivity, OR
   - They are in a cluster that covers ≤ `HARD_DILATE_MAX_FRAC=10%` of the neighboring data.
   Hard gates are NOT assigned to any region — they are the "walls" between components.

3. **Region growing** (best-first priority queue, SW-weighted votes) — soft gates form connected regions through adjacency. When a region resolves its fold, it casts weighted votes onto all unresolved neighbors. SW (spectrum width) confidence tapers votes from high-SW gates (`SW_CONF_FLOOR=0.15` at SW ≥ 8 m/s).

4. **Seed selection** — regions seed largest-first. The first seed in a connected group defines `comp=0` (main component). Within each component, relative folds are resolved by boundary votes.

5. **Bounded ±1 repair** — after all regions resolve, each region checks if shifting by ±1 reduces its boundary cost by >20%. Fixes isolated single-fold errors from seed noise.

6. **Pair-balanced sector anchor** — for `comp=0` only: divides the tilt into 36 × 10° sectors and computes `mean(v(az)) + mean(v(az+180°))` ≈ 0 for pure horizontal wind. If the mean offset is > 0.35 Vn and shifting by k folds reduces it by 65%, applies the shift. Only applied when ≥3 opposing-sector pairs each have ≥30 gates.

**Output stored in `TiltCache`**:
- `regions[]` — list of Region objects, each with gate list, boundary list, fold, comp index
- `region_of_gate[]` — map from gate index → region index (-1 for hard gates)
- `hard[]` — per-gate hard flag
- `graph[]` — per-gate adjacency list (4-connected: range±1, az±1 with bridging)
- `ncomp` — number of connected components in this tilt
- `nyq[]` — per-gate Nyquist velocity
- `vel_rays[]` — ray index for range lookups

### Stage V: Absolute anchoring

After Stage A, each component's RELATIVE fold structure is correct but its absolute fold is unknown. Stage V pins the absolute fold using a wind profile.

**Substage V-1: AR-VAD** (`fit_ar_vad`) — alias-robust VAD from Xu et al.
- Uses the **folded** (raw, not dealiased) velocities — immune to Stage A errors
- Grid-searches (U,V) wind vector per 500m height bin by maximizing `mean(cos(π(v_i - m_i)/Vn))` where m_i is the VAD model prediction at gate i
- Up to 8 local maxima per bin refined with subgrid search
- **Bottom-up dynamic programming** picks one candidate per height bin: start from weakest wind at lowest occupied bin, reject candidates that jump > 15 m/s per bin from the previous choice. This breaks the 2Vn reversal ambiguity — a completely reversed wind profile needs an implausibly large jump somewhere.
- Score threshold: candidates with score < 0.45 are discarded

**Substage V-2: LS-VAD** (`fit_vad`) — ordinary least-squares VAD
- Fit on already-dealiased data from Stage A
- Iterative reweighted LS (3 iterations), rejecting outliers > 0.6 Vn from the current model
- More accurate than AR-VAD when Stage A gave mostly correct folds

**`anchor_components`** — for each tilt, for each component:
1. Sample up to `ANCHOR_MAX_SAMPLES=600` gates in the component
2. Score each fold candidate k ∈ [-4..4]:
   ```
   score[k] += max(0, 1 - |d - 2k·Vn| / (1.0·Vn))
   ```
   where d = VAD_ref(height, azimuth) - measured_velocity
3. Require: `frac = score[best]/N ≥ 0.55` (big comp) or `0.45` (small), `margin ≥ 0.25` or `0.15`
4. Apply shift k × 2Vn to all gates in the component

**v10_3 change from v10_2**: removed the hard-gate union-find grouping and the median residual check (`ANCHOR_MAX_MEDIAN_RES`). These caused regressions (see below).

### Stage B: Vertical reconciliation (`reconcile_vertical`)

For each region in each tilt, looks up the same horizontal position in the tilt below and above. Votes for the fold shift k that minimizes the total velocity difference. Requirements:
- ≥6 supporting lookups total (`VERT_MIN_SUPPORT=6`)
- ≥72% of votes agree on the same k (`VERT_MIN_FRAC=0.72`)
- If both neighbors exist, both must independently agree on the same k at ≥65% (`VERT_SIDE_FRAC=0.65`)
- Shifted cost must be < 45% of unshifted cost (`VERT_MAX_RESIDUAL_RATIO=0.45`)

This is particularly effective at fixing whole-tilt fold errors where Stage V got the absolute anchor wrong.

### Stage X: Rankine vortex check (`vortex_reference_check`)

Fits an alias-robust Rankine vortex model to clusters of hard gates (detected shear). The model:
```
v_model = cos_e · f(rho) · [Vt · (t̂ · b̂) + Vr · (r̂ · b̂)] + c
```
where f(rho) = rho/R inside core radius R, R/rho outside (Rankine profile), and c is a constant background solved in closed form. Vt dimension is scanned via complex rotation (one sincos per sample per (center, R, Vr) combination).

Gates where |current - model| ≥ 1.2 Vn are re-folded to match the model if the result is within 0.4 Vn. Confident vortex gates become anchors for Stage C. Skipped if the window coherence is already ≥ 0.85.

### Stage C: Hard gate rescue (`hard_gate_rescue`)

Bounded continuity rescue for hard gates that were left at their raw (folded) values. For each hard gate, tests all fold candidates and picks the one that minimizes the Huber-weighted velocity difference with surrounding soft gates. Applied only when improvement ≥ 30% and ≥2 supporting neighbors.

---

## Why v10_2 Failed

Three changes to `anchor_components` introduced regressions.

### 1. Hard-gate union-find grouping

**What it does**: Before VAD scoring, runs a BFS over all hard gates. Any two soft components that touch through the hard-gate cluster get merged into one super-component for anchoring.

**Intent**: Prevent the VAD from independently shifting two components that are "really" connected (just separated by detected-shear gates). The idea was that the shear is real wind, so both sides should move together.

**Why it fails**: Stage A's region growing already correctly resolves the RELATIVE fold between components separated by hard gates (via boundary votes through the graph). After Stage A, if component A is at fold 0 and component B (adjacent through hard gates) is at fold +1, that relative fold is CORRECT. Merging them and then applying a single VAD shift destroys the correct relative structure. The merged super-component now has gates at two fold levels, the VAD picks the majority fold, and the minority side gets incorrectly shifted.

**Concrete example** (file 4, tilt 9, 1.32°):
```
v10_1: 337 fold jumps
v10_2: 751 fold jumps   (union-find grouping)
v10_3: 337 fold jumps   (grouping removed → fully fixed)
PyART: 36 fold jumps
```

**Fix**: Remove the union-find entirely. Each Stage A component gets anchored independently.

### 2. Narrower 0.5 Vn kernel

**What it does**: Changes the vote kernel from `1 - |d - 2k·Vn| / (1.0·Vn)` to `1 - |d - 2k·Vn| / (0.5·Vn)`.

**Intent**: Narrower kernel should be more discriminating — a true fold lands close to the environmental wind; a real shear line with a 2Vn wind difference would score poorly.

**Why it fails**: The narrower kernel concentrates votes more tightly around each candidate, making the score more sensitive to noise. In tilts where the VAD wind doesn't perfectly predict the true velocity (due to turbulence, tornado vortex, or poor azimuthal coverage), the score peaks at the wrong fold candidate.

**Concrete example** (file 1, tilt 15, 6.38°):
```
v10_1 (1.0 Vn): 3 fold jumps
v10_2 (0.5 Vn): 49 fold jumps
v10_3 (0.75 Vn): 49 fold jumps  (intermediate width still fails)
v10_3 (1.0 Vn): 16 fold jumps   (back to wider, much better)
```

**Fix**: Use 1.0 Vn kernel (as in v10_1).

### 3. Median residual check (ANCHOR_MAX_MEDIAN_RES = 0.25)

**What it does**: After selecting the best fold shift k, computes the median of `|d/Vn - 2k|` across sampled gates. If the median exceeds 0.25, rejects the shift.

**Intent**: On a real shear line, after applying the fold shift, residuals would cluster near `(2Vn - shear_jump) / Vn` ≈ large value, not near 0. The check should distinguish "genuine fold error" from "real wind transition."

**Why it fails**: In low-tilt, complex wind environments, even correct corrections can have median residuals > 0.25 Vn. The turbulence and horizontal wind gradient within a component produce scatter that exceeds the threshold. Valid corrections get blocked.

**Tested thresholds**:
- 0.25 Vn: too strict, causes many regressions
- 0.30 Vn: still too strict (same regressions, just slightly smaller)
- Removed: fully restores v10_1 behavior at this step

**Fix**: Remove the check. Stage W provides an alternative mechanism to catch bad corrections post-hoc.

---

## Stage W: Fold-Jump Targeted Repair (v10_3 new addition)

### Motivation

After Stage V, some small isolated components may still be at wrong fold levels because:
- The VAD had insufficient azimuthal coverage at their location
- The component is too small for the VAD score to be decisive (N < 12)
- The wind at that location genuinely differs from the VAD profile

Stage W catches these by looking directly at fold-jump evidence at region boundaries — the same signal that PyART region-based uses throughout.

### Algorithm

```
For each tilt (bottom-up order):
  Sort regions by size (ascending)
  For each region R with size < FJUMP_MAX_REGION_FRAC × tilt_total:
    Count fold-jump pairs: boundary pairs where |vel[g] - vel[nb]| ≥ 0.85 × 2Vn
    If fold-jump count < FJUMP_MIN_JUMPS: skip
    For k in [-2, -1, +1, +2]:
      Compute new fold-jump count and total cost with R shifted by k
      If new_cost < (1 - FJUMP_NET_IMPROVE) × old_cost
         AND jumps_eliminated ≥ FJUMP_ELIM_FRAC × old_jumps:
        Record as candidate
    Apply the best k (lowest new cost) if any candidate found
  Repeat until convergence (max FJUMP_MAX_PASSES passes)
```

### Current thresholds

| Parameter | Value | Meaning |
|-----------|-------|---------|
| `FJUMP_MAX_REGION_FRAC` | 0.04 | Only repair regions < 4% of tilt total |
| `FJUMP_FRAC` | 0.85 | Fold-jump threshold: |Δv| ≥ 0.85 × 2Vn |
| `FJUMP_MIN_JUMPS` | 5 | Min fold-jump pairs to attempt correction |
| `FJUMP_ELIM_FRAC` | 0.80 | Must eliminate ≥80% of fold-jump pairs |
| `FJUMP_NET_IMPROVE` | 0.30 | Net cost must drop ≥30% |
| `FJUMP_MAX_FOLDS` | 2 | Max shift ±2 |
| `FJUMP_MAX_PASSES` | 5 | Iterations until convergence |

### What it helps

Stage W specifically improves higher-elevation tilts where isolated regions are small and the fold-jump signal is clean:
- File 1 t15: 49 → 16 (vs v10_1's 3)
- File 4 t16: 20 → 2 (actually beats v10_1!)
- File 3 t15: 19 → 4

### What it doesn't help (remaining issues)

`030505` tilts t10–t13 (1.8°–4°) remain worse than v10_1. These have medium-to-large regions with genuine wind shear at boundaries that Stage W might misidentify as fold errors (Stage W size filter set to 4% might not be strict enough for these tilts).

**The last change before interruption**: Moved Stage W to run AFTER both VAD anchor passes instead of between them. Rationale: running Stage W between AR-VAD and LS-VAD contaminated the LS-VAD fit (shifted gates were included in the LS-VAD, biasing subsequent `anchor_components` calls). This version compiled but was **not yet run** — it is the next thing to test.

---

## Lessons on What Doesn't Work

### "Boundary coherence" voting (abandoned approach)

Tried first: for each component, vote based on cross-component boundary pairs (fold_from_pair) and apply the majority-vote fold shift. Variations tried:
- All sizes, 65% threshold
- Small only (<20% of tilt), 75% threshold, large neighbor (3× bigger) requirement

**Why it fails**: Cannot distinguish dealiasing errors from genuine ~2Vn wind shear across a component boundary. A large component at fold=0 with real 2Vn shear at its edge will force its small correct neighbor to shift — introducing errors. Net result: always made totals worse than v10_1.

### 0.5 Vn kernel (abandoned)

Worse than 1.0 Vn in every test. The intuition (tighter discrimination of fold candidates) is theoretically sound but fails in practice due to noise sensitivity at higher tilts. The median check was added to compensate, but also failed.

### ANCHOR_MAX_MEDIAN_RES (abandoned)

Blocks too many valid corrections in high-turbulence, tornado-outbreak environments. Not worth having.

---

## PyART Region-Based C++ Port (`velocity_dealias_pyart_region.cpp`)

### Goal

Establish a faithful C++ port of PyART's `dealias_region_based` as a ground-truth baseline. Target: **≤ 10 gates with different dealiased values** vs. PyART Python output, across all velocity tilts in all 5 KPAH test files.

### Status: COMPLETE + OPTIMIZED (3×)

All tilts pass with ≤ 4 gates different. Most tilts are 0.

| File | Max gate diff (any tilt) | Notes |
|------|--------------------------|-------|
| KPAH20211211_025905_V06 | 0 | perfect match |
| KPAH20211211_030505_V06 | 0 | perfect match |
| KPAH20211211_031104_V06 | 3 | t04=2, t07=3 |
| KPAH20211211_032349_V06 | 4 | t03=1, t11=4, t16=4 |
| KPAH20211211_032939_V06 | 2 | t03=1, t07=2 |

Remaining differences (~1–4 gates) are due to floating-point sort tie-breaking instability within identical-weight edges — they don't affect fold-jump scores.

### Performance (per-volume, 21 tilts, Apple M-series, -O3)

| Version | KPAH_025905 | KPAH_031104 | Speedup |
|---------|-------------|-------------|---------|
| Original | ~712 ms | ~586 ms | 1× |
| Optimised | ~218 ms | ~194 ms | **~3×** |

Accuracy is unchanged — `cpp_vs_pyart` gate diffs are identical before and after optimisation.

### Algorithm Summary

1. **`build_grid()`** — maps packed (az, range, vel) triples into a 2D `(nrays × range_bins)` grid. Range bin = `round((range − min_range) / gateSpacing)`.

2. **`compute_interval_limits()`** — exact port of `_find_sweep_interval_splits`. Splits `[−Vn, Vn]` into `INTERVAL_SPLITS=3` equal bands. Critically: if actual min or max velocity exceeds ±Vn, adds extra bands using `ceil((vmax−Vn)/interval)` and `ceil(−(vmin+Vn)/interval)`. This handles NEXRAD split-cut tilts where staggered PRF produces raw velocities slightly outside ±Vn (e.g. Vn=27.83, raw=±28.0).

3. **`find_regions()`** — BFS within each velocity band to find connected components (scipy.ndimage.label semantics). **Does NOT wrap rays** — this is critical; wrapping causes catastrophically wrong label assignments.

4. **`build_edges()`** — scans all 4 directions with skip bridging up to SKIP_BETWEEN_RAYS=100 and SKIP_ALONG_RAY=100. Rays DO wrap (PPI geometry). Canonicalizes edges as (max_label, min_label). **Sort order: `(b ascending, a ascending)`** — matches PyART's `np.lexsort((index1, index2))` where index1=neighbor, index2=label. Deduplication uses an `unordered_map` (accumulate hits directly) followed by a sort of only the unique edges — avoids O(N log N) sort of all raw hits (N ≈ 4 × nrays × nbins).

5. **`merge_all_regions()`** — exact port of `_RegionTracker` + `_EdgeTracker`. Greedy merge by highest weight edge. `sum_diff` and `weight` are kept live per edge; after merging node B into node A, immediately updates all edges touching B by removing B's old contribution and adding A's corrected contribution. After all merges, applies centering (subtracts weighted mean fold number so average fold ≈ 0). Best-edge selection uses a **max-heap with lazy deletion** (O(E log E) total instead of O(K×E) linear scan). Equal-weight tie-break: smallest edge index wins, matching the original linear scan's first-occurrence behaviour. `common_finder` is cleared via a dirty list (O(degree)) instead of `std::fill` (O(nfeatures)).

6. **`dealias_tilt_pyart_region()`** — per-tilt driver: build_grid → compute_interval_limits → find_regions → build_edges → merge_all_regions → write dealiased velocities back into Radials_VEL.

### Key Bugs Fixed During Implementation

**Bug 1 — Region finding wrapped rays**: First implementation set `RAYS_WRAP_AROUND=true` in the BFS. PyART uses `scipy.ndimage.label` which is 4-connected, non-wrapping. With wrapping, gates at az=0 and az=359 would share labels, merging regions that should be separate and breaking all downstream edge logic. Result: ~9000–11000 fold jumps instead of 34. Fix: remove wrap entirely from BFS.

**Bug 2 — Velocities outside ±Vn left unlabeled**: With a fixed 3-band split, gates with |v| slightly > Vn (common at low tilts due to staggered PRF) fell between bands and got label=0, treated as masked. This broke connectivity for entire tilts (files 1–2 t04/t05, all files t17). Fix: `compute_interval_limits` adds extra bands to cover the actual observed velocity range.

**Bug 3 — Edge sort order**: PyART's `np.lexsort((index1, index2))` sorts **neighbor-first** (index1), then label. First C++ port sorted `(node_a, node_b)` = `(max_label, min_label)`, i.e. label-first. This caused different tie-breaking in the greedy merge queue, producing hundreds to >1000 gate differences on some tilts. Fix: sort by `(b ascending, a ascending)`.

### fold_offset vs. live sum_diff (architectural lesson)

First attempt tracked a single `fold_offset[node]` scalar per region and applied it lazily. After `unite(base, merge)`, future lookups would try to find merge's entry — but merge's root now points to base, so the offset got double-applied or lost. The correct approach (matching PyART exactly) is to maintain `sum_diff` and `weight` live on every edge, and after merging B into A, iterate all of B's edges and re-express them in terms of A's new fold. This is what `unwrap_node()` does: it loops `edges_in_node[b]`, adjusts `sum_diff` by the fold difference, then remaps the edge key from `(b, neighbor)` to `(a, neighbor)`.

### Fold-Jump Scores (same 5 test files)

The port achieves the same fold-jump counts as PyART Python on every tilt:

```
f1: region=34/22/22/53/12/62/27/6/0/13/11/7/0/0/0/0   cpp_region=same
f2: region=46/14/0/79/74/52/15/0/6/2/4/13/0/0/0/0     cpp_region=same
... etc.
```

This is used as ground truth when tuning v10_3 toward PyART quality.

### Files

- `cpp/velocity_dealias_pyart_region.h` — declares `dealias_velocity_volume_pyart_region(AllTilt&)`
- `cpp/velocity_dealias_pyart_region.cpp` — full implementation (~370 lines, includes optimisations)
- `compare_dealias.cpp` — includes pyart_region column in CSV output
- `compare_pyart.py` — reports `cpp_region` jumps and `cpp_vs_pyart` gate diff per tilt

### run command

```bash
# Build includes pyart_region automatically
make compare_dealias

# Check cpp_vs_pyart gate diffs across all files
for f in radar_files/KPAH*; do
  base=$(basename $f)
  ./build/compare_dealias "$f" all "/tmp/radar_compare/${base}.csv" 2>/dev/null
done
for f in radar_files/KPAH*; do
  base=$(basename $f)
  /Users/peytonroden/.pyenv/versions/3.11.1/envs/weather/bin/python compare_pyart.py \
    "$f" "/tmp/radar_compare/${base}.csv" "/tmp/radar_compare/${base}" --max-plots 0 2>&1 | \
    grep -E "^t[0-9].*cpp_region"
done
```

---

## The Remaining 4.5x Gap vs PyART

### Why PyART region-based is so much better

PyART `dealias_region_based` is fundamentally a **spatially constrained flood fill**. It starts from a reference gate (smallest absolute velocity, or first gate processed) and expands outward, at each step choosing the fold that minimizes the jump with already-processed neighbors. Spatial continuity is maintained BY CONSTRUCTION — a fold discontinuity simply cannot exist unless there is missing data between the two regions.

Our algorithm is **globally incoherent by design**. We solve relative folds within each tilt's connected regions, then use a global VAD profile to anchor absolute folds. The gap comes from the absolute anchoring failing for some components.

### Where the ~3694 remaining jumps come from

Examining per-tilt data, the large majority come from low-elevation tilts (0.5°–1.4°) in each file:

| Tilt range | Our jumps | PyART region | Ratio |
|-----------|-----------|-------------|-------|
| 0.5°–0.9° | ~1500     | ~120        | 12.5× |
| 0.9°–1.4° | ~1200     | ~55         | 22×   |
| 1.4°–3.0° | ~600      | ~100        | 6×    |
| > 3.0°    | ~450      | ~200        | 2.25× |

At 0.5° elevation in a tornado environment, the beam is probing the lowest 500m of the atmosphere. Wind shear and rotation are severe. The VAD profile (which assumes a horizontally homogeneous wind at each height bin) is a poor model here. Components that happen to be in the couplet region cannot be reliably VAD-anchored.

### Paths to closing the gap

**Path 1: Post-VAD BFS from anchored large components**

After VAD anchoring, the large well-anchored component (usually `comp=0`, the main echo) is probably correct. Do a BFS from it outward through component boundaries, similar to PyART. For each adjacent component:
- Vote using fold_from_pair across all boundary gates
- If ≥85% of votes agree AND the component is smaller than the current BFS frontier component: apply the shift
- Enqueue the corrected component as a new frontier

Key guard vs. the failed "boundary coherence" approach: only propagate from larger to smaller, never the reverse. And require a higher threshold (85% vs 65%).

**Path 2: Better VAD anchoring for small components**

For components with N < 40 VAD samples (the current min threshold), the vote is unreliable. Instead of skipping them entirely (leaving them at Stage A's pair-balanced anchor), use a 3-level fallback:
1. If VAD coverage is good (both sectors for the component's azimuth range): use VAD
2. If VAD coverage is poor: use fold_from_pair votes with all boundary neighbors (strict threshold)
3. If no boundary neighbors exist (truly isolated): leave at 0, flag for Stage C

**Path 3: Iterative VAD + anchor**

Currently: AR-VAD → anchor → LS-VAD → anchor. Add a 3rd pass: after Stage B (vertical reconciliation), refit LS-VAD and run anchor once more. Stage B may have corrected some components, improving the VAD quality.

**Path 4: Pair-balanced anchor for ALL components ≥10% of tilt**

Currently the pair-balanced sector mean check only applies to `comp=0`. At tilts where the tornado vortex splits the echo into two large components (neither dominant), `comp=1` may be at a wrong fold that neither VAD nor vertical reconciliation catches. Applying the pair-balanced anchor to any component ≥10% of the tilt total would help.

---

## Code Locations

All source in `cpp_wasm/cpp/`. Key functions and line numbers may shift as the file is edited — use `grep -n` to find current positions.

| Function | File | What it does |
|----------|------|-------------|
| `dealias_tilt_stage_a` | v10_3.cpp | Per-tilt region solve, returns TiltCache |
| `fit_ar_vad` | v10_3.cpp | Alias-robust VAD from folded phases |
| `fit_vad` | v10_3.cpp | Ordinary LS-VAD on dealiased data |
| `anchor_components` | v10_3.cpp | VAD-based absolute fold anchoring |
| `boundary_coherence_pass` | v10_3.cpp | Stage W: fold-jump targeted repair |
| `reconcile_vertical` | v10_3.cpp | Stage B: cross-tilt vertical voting |
| `vortex_reference_check` | v10_3.cpp | Stage X: Rankine vortex fit |
| `hard_gate_rescue` | v10_3.cpp | Stage C: bounded continuity rescue |
| `dealias_velocity_volume_v10_3` | v10_3.cpp | Top-level entry point |

Relevant constants to tune:

```cpp
// In anchor_components:
constexpr int   ANCHOR_MAX_FOLD    = 4;      // max fold shift ±4
constexpr size_t ANCHOR_MAX_SAMPLES = 600;   // gates sampled per component
// kernel = 1.0 * vn (hardcoded in the scoring loop)
// need_frac: 0.55 (large comp) / 0.45 (small)
// need_margin: 0.25 (large comp) / 0.15 (small)

// In boundary_coherence_pass (Stage W):
constexpr int   FJUMP_MAX_PASSES        = 5;
constexpr float FJUMP_FRAC              = 0.85f;
constexpr int   FJUMP_MIN_JUMPS         = 5;
constexpr float FJUMP_ELIM_FRAC         = 0.80f;
constexpr float FJUMP_NET_IMPROVE       = 0.30f;
constexpr int   FJUMP_MAX_FOLDS         = 2;
constexpr float FJUMP_MAX_REGION_FRAC   = 0.04f;

// In reconcile_vertical (Stage B):
constexpr float VERT_MIN_FRAC           = 0.72f;
constexpr float VERT_SIDE_FRAC          = 0.65f;
constexpr float VERT_MAX_RESIDUAL_RATIO = 0.45f;
constexpr int   VERT_MIN_SUPPORT        = 6;
```

---

## Commands

### Build

```bash
cd cpp_wasm

# Build all native diagnostic tools
make tools

# Build just compare_dealias (fastest for iteration, recompiles only changed .cpp)
make compare_dealias

# Force rebuild of a specific version after editing
touch cpp/velocity_dealias_v10_3.cpp && make compare_dealias

# Build WASM module and copy to frontend
make copy-wasm

# Clean everything
make clean
```

### Run compare_dealias

compare_dealias outputs one CSV row per velocity gate:
`tilt, elevation, collect_ms, ray, azimuth, range, nyquist, raw, v10_1, v10_2, v10_3`

```bash
# Single file, single tilt (closest elevation to 0.5°)
./build/compare_dealias radar_files/KPAH20211211_025905_V06 0.5 /tmp/out.csv

# All tilts, single file
./build/compare_dealias radar_files/KPAH20211211_025905_V06 all \
  /tmp/radar_compare/KPAH20211211_025905_V06.csv

# All 5 files — show timing
for f in radar_files/KPAH*; do
  base=$(basename $f)
  echo "=== $base ===" && \
  ./build/compare_dealias "$f" all "/tmp/radar_compare/${base}.csv" 2>&1 | \
    grep "^v10"
done
```

### Run PyART comparison

Requires: `pip install pyart netCDF4 matplotlib pandas` in the weather env.

```bash
PYTHON=/Users/peytonroden/.pyenv/versions/3.11.1/envs/weather/bin/python

# Single file, save plots for first 6 tilts
$PYTHON compare_pyart.py \
  radar_files/KPAH20211211_025905_V06 \
  /tmp/radar_compare/KPAH20211211_025905_V06.csv \
  /tmp/radar_compare/KPAH20211211_025905_V06 \
  --max-plots 6

# All 5 files, no plots — show per-tilt fold jump counts
for f in radar_files/KPAH*; do
  base=$(basename $f)
  $PYTHON compare_pyart.py \
    "$f" "/tmp/radar_compare/${base}.csv" \
    "/tmp/radar_compare/${base}" --max-plots 0 2>&1 | \
    grep -E "^t[0-9]|^TOTALS|v10_[123]_jumps|region_jumps"
  echo "---"
done

# Compact summary: just totals per file
for f in radar_files/KPAH*; do
  base=$(basename $f)
  echo -n "$base: " && \
  $PYTHON compare_pyart.py \
    "$f" "/tmp/radar_compare/${base}.csv" \
    "/tmp/radar_compare/${base}" --max-plots 0 2>&1 | \
    grep -E "v10_[123]_jumps|region_jumps" | \
    awk '{printf "%s=%s  ", $1, $2}' && echo
done
```

### Aggregate analysis across all 5 files

```bash
/Users/peytonroden/.pyenv/versions/3.11.1/envs/weather/bin/python << 'EOF'
import pandas as pd, numpy as np, glob

summaries = sorted(glob.glob("/tmp/radar_compare/*_summary.csv"))
dfs = [pd.read_csv(f).assign(file=f.split("/")[-1].replace("_summary.csv","")) for f in summaries]
combined = pd.concat(dfs, ignore_index=True)

# Totals
print("=== FOLD-JUMP TOTALS ===")
for col in ["v10_1_jumps","v10_2_jumps","v10_3_jumps","region_jumps","unwrap_jumps"]:
    if col in combined.columns:
        print(f"  {col:<25s} {int(combined[col].sum()):>6d}")

# Per-file
print("\n=== PER-FILE ===")
print(f"{'file':<44} {'v10_1':>6} {'v10_2':>6} {'v10_3':>6} {'region':>7}")
for fname, g in combined.groupby("file"):
    print(f"  {fname:<42} {int(g['v10_1_jumps'].sum()):>6} "
          f"{int(g['v10_2_jumps'].sum()):>6} "
          f"{int(g.get('v10_3_jumps', pd.Series([0])).sum()):>6} "
          f"{int(g['region_jumps'].sum()):>7}")

# Regressions vs v10_1
if "v10_3_jumps" in combined.columns:
    combined["diff_3_1"] = combined["v10_3_jumps"] - combined["v10_1_jumps"]
    regressions = combined[combined["diff_3_1"] > 5].sort_values("diff_3_1", ascending=False)
    print(f"\n=== v10_3 REGRESSIONS vs v10_1 (>{5} jumps worse) ===")
    for _, r in regressions.iterrows():
        print(f"  {r['file']} t{int(r['tilt']):02d} el={r['elevation']:.2f}  "
              f"v10_1={int(r['v10_1_jumps'])} v10_2={int(r['v10_2_jumps'])} "
              f"v10_3={int(r['v10_3_jumps'])} region={int(r['region_jumps'])} "
              f"diff={int(r['diff_3_1'])}")
    improvements = combined[combined["diff_3_1"] < -5].sort_values("diff_3_1")
    print(f"\n=== v10_3 IMPROVEMENTS vs v10_1 (>{5} jumps better) ===")
    for _, r in improvements.iterrows():
        print(f"  {r['file']} t{int(r['tilt']):02d} el={r['elevation']:.2f}  "
              f"v10_1={int(r['v10_1_jumps'])} v10_3={int(r['v10_3_jumps'])} "
              f"region={int(r['region_jumps'])} diff={int(r['diff_3_1'])}")
EOF
```

### Full pipeline: build + run all files + compare (one shot)

```bash
cd cpp_wasm
make compare_dealias && \
for f in radar_files/KPAH*; do
  base=$(basename $f)
  ./build/compare_dealias "$f" all "/tmp/radar_compare/${base}.csv"
done && \
for f in radar_files/KPAH*; do
  base=$(basename $f)
  /Users/peytonroden/.pyenv/versions/3.11.1/envs/weather/bin/python compare_pyart.py \
    "$f" "/tmp/radar_compare/${base}.csv" "/tmp/radar_compare/${base}" --max-plots 0 \
    2>&1 | grep -E "^TOTALS|v10_[123]_jumps|region_jumps"
  echo "---"
done
```

### Inspect a specific tilt's velocity data

```bash
/Users/peytonroden/.pyenv/versions/3.11.1/envs/weather/bin/python << 'EOF'
import pandas as pd, numpy as np

# Pick file and tilt index
FILE  = "/tmp/radar_compare/KPAH20211211_030505_V06.csv"
TILT  = 10

df = pd.read_csv(FILE)
g = df[df["tilt"] == TILT]
print(f"tilt={TILT} el={g['elevation'].iloc[0]:.3f} gates={len(g)}")
print(f"nyquist: {sorted(g['nyquist'].unique())}")
print(f"raw range:   {g['raw'].min():.1f} .. {g['raw'].max():.1f}")
print(f"v10_1 range: {g['v10_1'].min():.1f} .. {g['v10_1'].max():.1f}")
print(f"v10_3 range: {g['v10_3'].min():.1f} .. {g['v10_3'].max():.1f}")
diff = (np.abs(g['v10_3'] - g['v10_1']) > 0.05).sum()
print(f"gates where v10_1 != v10_3: {diff}")
EOF
```

---

## File Map

```
cpp_wasm/
├── cpp/
│   ├── velocity_dealias.h              # shared structs: AllTilt, SingleTilt, VelocityRay
│   ├── structs_and_constants.h         # radar data types, packed VEL format
│   ├── velocity_dealias_v10_1.cpp/.h   # STABLE BEST (4749 total fold jumps)
│   ├── velocity_dealias_v10_2.cpp/.h   # BROKEN (5930 jumps) — union-find broke it
│   ├── velocity_dealias_v10_3.cpp/.h   # WORK-IN-PROGRESS (4969 jumps)
│   ├── velocity_dealias_pyart_region.cpp/.h  # COMPLETE PyART port (≤4 gate diff)
├── compare_dealias.cpp                  # standalone tool: runs v10_1 + v10_2 + v10_3
├── compare_pyart.py                     # gate-by-gate vs PyART region + unwrap
├── main_as_lib.cpp                      # wrapper: #define main _unused_ ; #include cpp/main.cpp
│                                        # lets standalone tools link against main.cpp globals
├── radar_files/
│   ├── KPAH20211211_025905_V06          # ~13M gates total across 16 tilts
│   ├── KPAH20211211_030505_V06
│   ├── KPAH20211211_031104_V06
│   ├── KPAH20211211_032349_V06
│   └── KPAH20211211_032939_V06
├── Makefile                             # make tools / make compare_dealias / make copy-wasm
└── RESEARCH_NOTES.md                    # this file
```

**Output CSV schema** (written to `/tmp/radar_compare/`):
- `KPAH*_V06.csv` — one row per gate: `tilt, elevation, collect_ms, ray, azimuth, range, nyquist, raw, v10_1, v10_2, v10_3, pyart_region`
- `KPAH*_V06_summary.csv` — one row per tilt: fold-jump counts, pairwise differences, consensus stats
- `KPAH*_V06_tNN.png` — 6-panel plots: Raw / v10_1 / v10_2 / v10_3 / PyART region / PyART unwrap

**Velocity data format** (packed float triples): `[azimuth_deg, range_m, velocity_ms]` stored in `SingleTilt::Radials_VEL`. Gate index g maps to `[3g], [3g+1], [3g+2]`.
