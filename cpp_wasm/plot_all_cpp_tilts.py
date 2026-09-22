import argparse

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

parser = argparse.ArgumentParser()
parser.add_argument("csv")
parser.add_argument("output")
args = parser.parse_args()
rows = pd.read_csv(args.csv)
fig, axes = plt.subplots(4, 4, figsize=(20, 14), constrained_layout=True)
for ax, (tilt, frame) in zip(axes.flat, rows.groupby("tilt", sort=True)):
    ray = frame["ray"].to_numpy(dtype=int)
    ranges = frame["range"].to_numpy()
    spacing = float(np.min(np.diff(np.unique(ranges))))
    first = float(ranges.min())
    gate = np.rint((ranges - first) / spacing).astype(int)
    values = np.full((ray.max() + 1, gate.max() + 1), np.nan)
    values[ray, gate] = frame["corrected"].to_numpy()
    image = ax.imshow(values, origin="lower", aspect="auto", interpolation="nearest", cmap="RdBu_r", vmin=-80, vmax=80)
    ax.set_title(f"Tilt {int(tilt)}: {frame['elevation'].iloc[0]:.3f}°")
    ax.set_xlabel("Range gate")
    ax.set_ylabel("Ray")
fig.colorbar(image, ax=axes, label="Dealiased radial velocity (m/s)", shrink=0.8)
fig.savefig(args.output, dpi=130)
