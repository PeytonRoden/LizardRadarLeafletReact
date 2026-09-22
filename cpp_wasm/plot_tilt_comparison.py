import argparse

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
import pyart
from netCDF4 import num2date

parser = argparse.ArgumentParser()
parser.add_argument("volume")
parser.add_argument("csv")
parser.add_argument("tilt", type=int)
parser.add_argument("output")
args = parser.parse_args()
radar = pyart.io.read_nexrad_archive(args.volume)
frame = pd.read_csv(args.csv)
frame = frame[frame["tilt"] == args.tilt]
collect_ms = int(frame["collect_ms"].iloc[0])
candidates = []
for index in range(radar.nsweeps):
    start = int(radar.sweep_start_ray_index["data"][index])
    end = int(radar.sweep_end_ray_index["data"][index])
    if radar.fields["velocity"]["data"][start : end + 1].count() == 0:
        continue
    time = num2date(radar.time["data"][end], radar.time["units"])
    end_ms = ((time.hour * 60 + time.minute) * 60 + time.second) * 1000 + time.microsecond // 1000
    candidates.append((abs(end_ms - collect_ms), index))
_, sweep_index = min(candidates)
sweep = radar.extract_sweeps([sweep_index])
raw = sweep.fields["velocity"]["data"].filled(np.nan)
region = pyart.correct.dealias_region_based(sweep, gatefilter=False)["data"].filled(np.nan)
unwrap = pyart.correct.dealias_unwrap_phase(sweep, unwrap_unit="sweep", gatefilter=False)["data"].filled(np.nan)
ray = frame["ray"].to_numpy(dtype=int)
gate = np.rint((frame["range"].to_numpy() - sweep.range["data"][0]) / np.diff(sweep.range["data"])[0]).astype(int)
cpp = np.full_like(raw, np.nan)
cpp[ray, gate] = frame["corrected"].to_numpy()
valid_columns = np.where(np.isfinite(raw).any(axis=0))[0]
last_gate = valid_columns[-1] + 1
fig, axes = plt.subplots(4, 1, figsize=(18, 12), constrained_layout=True, sharex=True, sharey=True)
for ax, (name, values) in zip(axes, [("Raw", raw), ("C++", cpp), ("Py-ART region", region), ("Py-ART unwrap", unwrap)]):
    image = ax.imshow(values[:, :last_gate], origin="lower", aspect="auto", interpolation="nearest", cmap="RdBu_r", vmin=-80, vmax=80)
    ax.set(title=name, ylabel="Ray index")
axes[-1].set_xlabel("Range gate")
fig.colorbar(image, ax=axes, label="Radial velocity (m/s)")
fig.suptitle(f"C++ tilt {args.tilt}, Py-ART sweep {sweep_index}, elevation {frame['elevation'].iloc[0]:.3f}°")
fig.savefig(args.output, dpi=130)
