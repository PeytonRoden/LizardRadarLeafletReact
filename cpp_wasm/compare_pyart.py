#!/usr/bin/env python3
"""
compare_pyart.py  --  compare C++ v10_1 / v10_2 against PyART dealiasers

Workflow
--------
1. Build:      cd cpp_wasm && make compare_dealias
2. Diagnose:   ./build/compare_dealias LEVEL2 all out.csv
3. Compare:    python compare_pyart.py LEVEL2 out.csv results/myrun

Inputs
------
LEVEL2         NEXRAD Level-2 archive file
COMPARE_CSV    CSV from compare_dealias:
               tilt, elevation, collect_ms, ray, azimuth, range,
               nyquist, raw, v10_1, v10_2
OUTPUT_PREFIX  Path prefix for all outputs

Outputs
-------
<prefix>_summary.csv     Per-tilt metrics table
<prefix>_tN.png          5-panel plot per tilt (up to --max-plots)

Metrics explained
-----------------
fold_jumps   Gates adjacent to a neighbour that differs by >= 0.9 * 2 * Nyquist.
             Lower is better.  PyART's region-based is the typical reference;
             our goal is to match or beat it.

vs_region    Gates where the dealiaser disagrees with PyART region-based
             (|diff| > 0.05 m/s).  Not a ground-truth -- region-based can
             be wrong -- but a useful sanity signal.

consensus    PyART region and unwrap agree; consensus_changed = consensus
             gates where region corrected the raw value.  A high
             "v10_2 wrong" fraction here signals genuine errors.
"""

import argparse
import sys
import time
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
import pyart
from netCDF4 import num2date

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
parser = argparse.ArgumentParser(
    description="Compare v10_1, v10_2 and PyART dealiasers gate-by-gate.")
parser.add_argument("volume",        help="NEXRAD Level-2 file")
parser.add_argument("csv",           help="CSV produced by compare_dealias (all tilts)")
parser.add_argument("output_prefix", help="Path prefix for all output files")
parser.add_argument("--max-plots",   type=int, default=6,
                    help="Number of tilts to plot (default: 6)")
parser.add_argument("--tol",         type=float, default=0.05,
                    help="Disagreement threshold m/s (default: 0.05)")
parser.add_argument("--jump-frac",   type=float, default=0.9,
                    help="Fraction of 2*Nyq for a fold jump (default: 0.9)")
args = parser.parse_args()

TOL  = args.tol
JFRC = args.jump_frac

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def count_fold_jumps(values, nyquist_per_gate, ray_idx, gate_idx, shape):
    """Count gates adjacent to a fold-level discontinuity."""
    grid = np.full(shape, np.nan)
    nq   = np.full(shape, np.nan)
    grid[ray_idx, gate_idx] = values
    nq[ray_idx, gate_idx]   = nyquist_per_gate
    limit = JFRC * 2.0 * nq

    jumps = np.zeros(shape, dtype=bool)
    along = np.abs(np.diff(grid, axis=1)) > limit[:, :-1]
    jumps[:, :-1] |= along
    jumps[:, 1:]  |= along
    across = np.abs(grid - np.roll(grid, -1, axis=0)) > limit
    jumps |= across | np.roll(across, 1, axis=0)
    return int((jumps & np.isfinite(grid)).sum())


def sweep_end_ms(radar, sweep):
    end = int(radar.sweep_end_ray_index["data"][sweep])
    t   = num2date(radar.time["data"][end], radar.time["units"])
    return (t.hour * 3600 + t.minute * 60 + t.second) * 1000 + t.microsecond // 1000


def best_sweep(radar, collect_ms, max_delta_ms=10):
    best_delta, best_idx = 9999, -1
    for s in range(radar.nsweeps):
        start = int(radar.sweep_start_ray_index["data"][s])
        end   = int(radar.sweep_end_ray_index["data"][s])
        if radar.fields["velocity"]["data"][start:end+1].count() == 0:
            continue
        d = abs(sweep_end_ms(radar, s) - collect_ms)
        if d < best_delta:
            best_delta, best_idx = d, s
    if best_delta > max_delta_ms:
        return -1, best_delta
    return best_idx, best_delta


# ---------------------------------------------------------------------------
# Load
# ---------------------------------------------------------------------------
print("Loading radar file...")
radar = pyart.io.read_nexrad_archive(args.volume)
rows  = pd.read_csv(args.csv)
print(f"  {radar.nsweeps} sweeps  |  CSV: {len(rows)} gates across "
      f"{rows['tilt'].nunique()} tilts\n")

# ---------------------------------------------------------------------------
# Per-tilt loop
# ---------------------------------------------------------------------------
results = []
plot_data = []   # store (frame, sweep_index) for the first few tilts

for tilt_index, frame in rows.groupby("tilt", sort=True):
    collect_ms = int(frame["collect_ms"].iloc[0])
    sweep_idx, delta_ms = best_sweep(radar, collect_ms)
    if sweep_idx < 0:
        print(f"tilt {tilt_index}: no matching sweep (best delta {delta_ms} ms), skip")
        continue

    sweep_obj = radar.extract_sweeps([sweep_idx])
    raw_full  = sweep_obj.fields["velocity"]["data"].filled(np.nan)

    t0 = time.perf_counter()
    region_full = pyart.correct.dealias_region_based(
        sweep_obj, gatefilter=False)["data"].filled(np.nan)
    region_ms = (time.perf_counter() - t0) * 1e3

    t0 = time.perf_counter()
    unwrap_full = pyart.correct.dealias_unwrap_phase(
        sweep_obj, unwrap_unit="sweep", gatefilter=False)["data"].filled(np.nan)
    unwrap_ms = (time.perf_counter() - t0) * 1e3

    # Gate indices into the PyART array.
    ray_idx  = frame["ray"].to_numpy(dtype=int)
    rng      = frame["range"].to_numpy()
    dr       = np.diff(sweep_obj.range["data"])[0]
    gate_idx = np.rint((rng - sweep_obj.range["data"][0]) / dr).astype(int)

    # Azimuth sanity check.
    az_err = np.abs((frame["azimuth"].to_numpy()
                     - sweep_obj.azimuth["data"][ray_idx] + 180) % 360 - 180)
    if az_err.max() > 0.5:
        print(f"tilt {tilt_index}: azimuth mismatch {az_err.max():.2f}°, skip")
        continue

    raw_s    = raw_full[ray_idx, gate_idx]
    region_s = region_full[ray_idx, gate_idx]
    unwrap_s = unwrap_full[ray_idx, gate_idx]
    v10_1    = frame["v10_1"].to_numpy()
    v10_2    = frame["v10_2"].to_numpy()
    v10_3      = frame["v10_3"].to_numpy() if "v10_3" in frame.columns else v10_2.copy()
    pyart_cpp  = frame["pyart_region"].to_numpy() if "pyart_region" in frame.columns else v10_2.copy()
    nyquist    = frame["nyquist"].to_numpy()

    nrays  = int(ray_idx.max()) + 1
    ngates = int(gate_idx.max()) + 1
    shape  = (nrays, ngates)

    raw_jumps    = count_fold_jumps(raw_s,    nyquist, ray_idx, gate_idx, shape)
    v10_1_jumps  = count_fold_jumps(v10_1,   nyquist, ray_idx, gate_idx, shape)
    v10_2_jumps  = count_fold_jumps(v10_2,   nyquist, ray_idx, gate_idx, shape)
    v10_3_jumps  = count_fold_jumps(v10_3,   nyquist, ray_idx, gate_idx, shape)
    pyart_cpp_jumps = count_fold_jumps(pyart_cpp, nyquist, ray_idx, gate_idx, shape)
    region_jumps = count_fold_jumps(region_s, nyquist, ray_idx, gate_idx, shape)
    unwrap_jumps = count_fold_jumps(unwrap_s, nyquist, ray_idx, gate_idx, shape)

    # PyART consensus: both algorithms agree.
    pyart_ok  = np.isfinite(region_s) & np.isfinite(unwrap_s)
    consensus = pyart_ok & (np.abs(region_s - unwrap_s) < TOL)
    cons_chgd = consensus & (np.abs(region_s - raw_s) > TOL)

    def vs(a, b):
        ok = np.isfinite(a) & np.isfinite(b)
        return int(np.count_nonzero(ok & (np.abs(a - b) > TOL)))

    # Where both PyART algorithms agree AND corrected, does v10_2/v10_3 match?
    v10_2_wrong_at_consensus = int(np.count_nonzero(
        cons_chgd & (np.abs(v10_2 - region_s) > TOL)))
    v10_3_wrong_at_consensus = int(np.count_nonzero(
        cons_chgd & (np.abs(v10_3 - region_s) > TOL)))
    pyart_cpp_wrong_at_consensus = int(np.count_nonzero(
        cons_chgd & (np.abs(pyart_cpp - region_s) > TOL)))

    result = dict(
        tilt             = int(tilt_index),
        elevation        = float(frame["elevation"].iloc[0]),
        nyquist_med      = float(np.median(nyquist)),
        gates            = len(frame),
        region_ms        = round(region_ms, 1),
        unwrap_ms        = round(unwrap_ms, 1),
        # fold-jump counts (lower = better)
        raw_jumps        = raw_jumps,
        v10_1_jumps      = v10_1_jumps,
        v10_2_jumps      = v10_2_jumps,
        v10_3_jumps      = v10_3_jumps,
        region_jumps     = region_jumps,
        unwrap_jumps     = unwrap_jumps,
        # pairwise differences
        pyart_cpp_jumps       = pyart_cpp_jumps,
        pyart_cpp_vs_region   = vs(pyart_cpp, region_s),
        v10_1_vs_region  = vs(v10_1, region_s),
        v10_2_vs_region  = vs(v10_2, region_s),
        v10_3_vs_region  = vs(v10_3, region_s),
        v10_1_vs_unwrap  = vs(v10_1, unwrap_s),
        v10_2_vs_unwrap  = vs(v10_2, unwrap_s),
        v10_3_vs_unwrap  = vs(v10_3, unwrap_s),
        v10_1_vs_v10_2   = vs(v10_1, v10_2),
        v10_1_vs_v10_3   = vs(v10_1, v10_3),
        v10_2_vs_v10_3   = vs(v10_2, v10_3),
        pyart_disagrees  = vs(region_s, unwrap_s),
        consensus_gates  = int(consensus.sum()),
        consensus_changed= int(cons_chgd.sum()),
        v10_2_wrong_cons      = v10_2_wrong_at_consensus,
        v10_3_wrong_cons      = v10_3_wrong_at_consensus,
        pyart_cpp_wrong_cons  = pyart_cpp_wrong_at_consensus,
    )
    results.append(result)

    # Print concise one-liner.
    print(f"t{tilt_index:02d} el={result['elevation']:5.2f}° "
          f"  jumps raw={raw_jumps:4d} v10_1={v10_1_jumps:4d} v10_2={v10_2_jumps:4d} "
          f"v10_3={v10_3_jumps:4d} region={region_jumps:4d} unwrap={unwrap_jumps:4d}"
          f"  cpp_region={pyart_cpp_jumps:4d} cpp_vs_pyart={result['pyart_cpp_vs_region']:6d}")

    if len(plot_data) < args.max_plots:
        plot_data.append(dict(
            tilt_index  = int(tilt_index),
            frame       = frame,
            sweep_obj   = sweep_obj,
            ray_idx     = ray_idx,
            gate_idx    = gate_idx,
            raw_s       = raw_s,
            v10_1       = v10_1,
            v10_2       = v10_2,
            v10_3       = v10_3,
            region_s    = region_s,
            unwrap_s    = unwrap_s,
            nyquist     = nyquist,
            elev        = result["elevation"],
            shape       = shape,
        ))

if not results:
    print("No tilts processed -- check the CSV path and NEXRAD file.")
    sys.exit(1)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
summary = pd.DataFrame(results)
Path(args.output_prefix).parent.mkdir(parents=True, exist_ok=True)
csv_out = args.output_prefix + "_summary.csv"
summary.to_csv(csv_out, index=False)

print("\n" + "=" * 90)
print("PER-TILT JUMP COUNTS  (lower = better)")
print("=" * 90)
jump_cols = ["tilt", "elevation", "gates",
             "raw_jumps", "v10_1_jumps", "v10_2_jumps"]
if "v10_3_jumps" in summary.columns:
    jump_cols.append("v10_3_jumps")
jump_cols += ["region_jumps", "unwrap_jumps"]
print(summary[jump_cols].to_string(index=False))

print("\n" + "=" * 90)
print("PAIRWISE DIFFERENCES  (gates where |a - b| > {:.2f} m/s)".format(TOL))
print("=" * 90)
cols2 = ["tilt", "elevation", "gates",
         "v10_1_vs_region", "v10_2_vs_region"]
if "v10_3_vs_region" in summary.columns:
    cols2.append("v10_3_vs_region")
cols2 += ["v10_1_vs_unwrap", "v10_2_vs_unwrap", "v10_1_vs_v10_2", "pyart_disagrees"]
print(summary[cols2].to_string(index=False))

totals_cols = [
    "gates", "raw_jumps", "v10_1_jumps", "v10_2_jumps",
    "region_jumps", "unwrap_jumps",
    "v10_1_vs_region", "v10_2_vs_region",
    "v10_1_vs_unwrap", "v10_2_vs_unwrap",
    "v10_1_vs_v10_2", "pyart_disagrees",
    "consensus_changed", "v10_2_wrong_cons",
]
print("\nTOTALS:")
totals_cols_v3 = [c for c in totals_cols if c in summary.columns] + \
                 [c for c in ["v10_3_jumps","v10_3_vs_region","v10_3_vs_unwrap",
                               "v10_1_vs_v10_3","v10_2_vs_v10_3","v10_3_wrong_cons"]
                  if c in summary.columns]
for c in totals_cols_v3:
    print(f"  {c:<30s}  {int(summary[c].sum()):>8d}")
print(f"\n  region_ms total: {summary['region_ms'].sum():.0f} ms")
print(f"  unwrap_ms total: {summary['unwrap_ms'].sum():.0f} ms")
print(f"\n  Summary CSV -> {csv_out}")

# ---------------------------------------------------------------------------
# Plots: 5-panel per tilt
# ---------------------------------------------------------------------------
for pd_ in plot_data:
    ti       = pd_["tilt_index"]
    elev     = pd_["elev"]
    nyq      = float(np.median(pd_["nyquist"]))
    ray_idx  = pd_["ray_idx"]
    gate_idx = pd_["gate_idx"]
    shape    = pd_["shape"]

    panels = [
        ("Raw",          pd_["raw_s"],    nyq),
        ("v10_1",        pd_["v10_1"],    3 * nyq),
        ("v10_2",        pd_["v10_2"],    3 * nyq),
        ("v10_3",        pd_["v10_3"],    3 * nyq),
        ("PyART region", pd_["region_s"], 3 * nyq),
        ("PyART unwrap", pd_["unwrap_s"], 3 * nyq),
    ]

    fig, axes = plt.subplots(1, 6, figsize=(32, 5), constrained_layout=True)
    for ax, (name, values, span) in zip(axes, panels):
        grid = np.full(shape, np.nan)
        grid[ray_idx, gate_idx] = values
        im = ax.imshow(grid.T, origin="lower", aspect="auto",
                       interpolation="nearest", cmap="RdBu_r",
                       vmin=-span, vmax=span)
        ax.set(title=f"{name}\n±{span:.0f} m/s",
               xlabel="Ray", ylabel="Gate")
        fig.colorbar(im, ax=ax, shrink=0.75)

    fig.suptitle(f"Tilt {ti} — el={elev:.3f}° — Nyquist={nyq:.1f} m/s",
                 fontsize=13)
    out_path = f"{args.output_prefix}_t{ti:02d}.png"
    fig.savefig(out_path, dpi=130)
    plt.close(fig)
    print(f"Saved {out_path}")
