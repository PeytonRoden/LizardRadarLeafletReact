import argparse

import numpy as np
import pandas as pd
from scipy import ndimage

parser = argparse.ArgumentParser(description="Reference-free audit: count residual fold jumps.")
parser.add_argument("csv")
parser.add_argument("--fraction", type=float, default=0.9,
                    help="Flag neighbour jumps larger than fraction * 2 * Nyquist.")
args = parser.parse_args()
rows = pd.read_csv(args.csv)
print(f"{'tilt':>4} {'elev':>7} {'gates':>8} {'raw_jumps':>10} {'out_jumps':>10} {'worst_run':>10} {'clusters':>9}")
totals = np.zeros(3, dtype=int)
for tilt, frame in rows.groupby("tilt", sort=True):
    ray = frame["ray"].to_numpy(dtype=int)
    ranges = frame["range"].to_numpy()
    spacing = float(np.min(np.diff(np.unique(ranges))))
    gate = np.rint((ranges - ranges.min()) / spacing).astype(int)
    shape = (ray.max() + 1, gate.max() + 1)
    nyquist = np.full(shape, np.nan)
    nyquist[ray, gate] = frame["nyquist"].to_numpy()
    grids = {}
    for name in ("raw", "corrected"):
        values = np.full(shape, np.nan)
        values[ray, gate] = frame[name].to_numpy()
        grids[name] = values
    counts = {}
    for name, values in grids.items():
        limit = args.fraction * 2.0 * nyquist
        jumps = np.zeros(shape, dtype=bool)
        along = np.abs(np.diff(values, axis=1)) > limit[:, :-1]
        jumps[:, :-1] |= along
        jumps[:, 1:] |= along
        across = np.abs(values - np.roll(values, -1, axis=0)) > limit
        jumps |= across | np.roll(across, 1, axis=0)
        counts[name] = jumps & np.isfinite(values)
    labels, clusters = ndimage.label(counts["corrected"], np.ones((3, 3), dtype=int))
    worst = int(np.max(np.bincount(labels.ravel())[1:])) if clusters else 0
    print(f"{int(tilt):>4} {frame['elevation'].iloc[0]:>7.3f} {len(frame):>8} "
          f"{int(counts['raw'].sum()):>10} {int(counts['corrected'].sum()):>10} {worst:>10} {clusters:>9}")
    totals += [len(frame), int(counts["raw"].sum()), int(counts["corrected"].sum())]
print(f"\ntotal gates {totals[0]}, raw fold jumps {totals[1]}, remaining fold jumps {totals[2]}")
