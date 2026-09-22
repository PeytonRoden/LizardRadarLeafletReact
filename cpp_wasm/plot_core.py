import argparse

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

parser = argparse.ArgumentParser(description="Zoom on an azimuth/range window of one tilt.")
parser.add_argument("csv")
parser.add_argument("tilt", type=int)
parser.add_argument("output")
parser.add_argument("--azimuth", type=float, nargs=2, default=(155.0, 180.0))
parser.add_argument("--range", type=float, nargs=2, default=(30000.0, 55000.0))
args = parser.parse_args()
frame = pd.read_csv(args.csv)
frame = frame[frame["tilt"] == args.tilt]
window = frame[frame["azimuth"].between(*args.azimuth) & frame["range"].between(*args.range)]
nyquist = float(window["nyquist"].iloc[0])
# Rays are stored in scan order, which can wrap through 360; order the window by azimuth.
order = {ray: index for index, ray in enumerate(window.groupby("ray")["azimuth"].first().sort_values().index)}
ray = window["ray"].map(order).to_numpy(dtype=int)
ranges = window["range"].to_numpy()
spacing = float(np.min(np.diff(np.unique(frame["range"].to_numpy()))))
gate = np.rint((ranges - ranges.min()) / spacing).astype(int)
shape = (ray.max() + 1, gate.max() + 1)
fig, axes = plt.subplots(1, 3, figsize=(19, 7), constrained_layout=True)
panels = [("Raw", window["raw"].to_numpy(), nyquist), ("Dealiased", window["corrected"].to_numpy(), 3 * nyquist)]
for ax, (name, values, span) in zip(axes, panels):
    grid = np.full(shape, np.nan)
    grid[ray, gate] = values
    image = ax.imshow(grid.T, origin="lower", aspect="auto", interpolation="nearest", cmap="RdBu_r", vmin=-span, vmax=span)
    ax.set(title=f"{name} (+/- {span:.0f} m/s)", xlabel="Ray in window", ylabel="Gate in window")
    fig.colorbar(image, ax=ax, label="m/s")
corrected = np.full(shape, np.nan)
corrected[ray, gate] = window["corrected"].to_numpy()
jump = np.zeros(shape, dtype=bool)
along = np.abs(np.diff(corrected, axis=1)) > 0.9 * 2 * nyquist
jump[:, :-1] |= along
jump[:, 1:] |= along
across = np.abs(np.diff(corrected, axis=0)) > 0.9 * 2 * nyquist
jump[:-1, :] |= across
jump[1:, :] |= across
axes[2].imshow(np.isfinite(corrected).T, origin="lower", aspect="auto", interpolation="nearest", cmap="Greys", vmin=0, vmax=4)
rows, columns = np.where(jump & np.isfinite(corrected))
axes[2].scatter(rows, columns, s=9, color="red")
axes[2].set(title=f"Residual fold discontinuities: {len(rows)}", xlabel="Ray in window", ylabel="Gate in window")
fig.suptitle(f"Tilt {args.tilt}, azimuth {args.azimuth[0]}-{args.azimuth[1]}, Nyquist {nyquist:.2f} m/s")
fig.savefig(args.output, dpi=130)
