import argparse
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pyart

parser = argparse.ArgumentParser()
parser.add_argument("volume")
parser.add_argument("csv")
parser.add_argument("output")
parser.add_argument("--elevation", type=float, default=3.99)
args = parser.parse_args()
radar = pyart.io.read_nexrad_archive(args.volume)
sweep_index = int(np.argmin(np.abs(radar.fixed_angle["data"] - args.elevation)))
radar = radar.extract_sweeps([sweep_index])
print("sweep", sweep_index, "elevation", radar.fixed_angle["data"], "nyquist", np.unique(radar.instrument_parameters["nyquist_velocity"]["data"]))
print("range", radar.range["data"][:5], "shape", radar.fields["velocity"]["data"].shape)
raw = radar.fields["velocity"]["data"].filled(np.nan)
region = pyart.correct.dealias_region_based(radar, gatefilter=False)["data"].filled(np.nan)
unwrap = pyart.correct.dealias_unwrap_phase(radar, unwrap_unit="sweep", gatefilter=False)["data"].filled(np.nan)
rows = np.genfromtxt(args.csv, delimiter=",", names=True)
ray_angles = radar.azimuth["data"]
az_diff = np.abs((rows["azimuth"][:, None] - ray_angles[None, :] + 180) % 360 - 180)
r = az_diff.argmin(axis=1)
g = np.rint((rows["range"] - radar.range["data"][0]) / np.diff(radar.range["data"])[0]).astype(int)
if np.max(az_diff[np.arange(len(r)), r]) > 0.01:
    raise RuntimeError("C++ and Py-ART sweep azimuths differ")
print("decode mismatches", int(np.count_nonzero(np.abs(raw[r, g] - rows["raw"]) > 0.01)))
cpp = np.full_like(raw, np.nan)
cpp[r, g] = rows["corrected"]
x, y, _ = radar.get_gate_x_y_z(0)
x, y = x / 1000, y / 1000
core = (x > 0) & (x < 18) & (y > -48) & (y < -27) & np.isfinite(raw)
consensus = core & np.isfinite(region) & np.isfinite(unwrap) & (np.abs(region - unwrap) < 0.01)
disagreement = consensus & (np.abs(cpp - region) > 0.01)
print("reference-consensus gates", consensus.sum(), "C++ disagreements", disagreement.sum())
for ray, gate in np.argwhere(disagreement)[:20]:
    print("disagreement", "az", ray_angles[ray], "range", radar.range["data"][gate], "raw", raw[ray, gate], "cpp", cpp[ray, gate], "reference", region[ray, gate])
for name, values in [("raw", raw), ("cpp", cpp), ("pyart_region", region), ("pyart_unwrap", unwrap)]:
    valid = core & np.isfinite(values)
    print(name, "core gates", valid.sum(), "range", values[valid].min(), values[valid].max(), "corrected", np.count_nonzero(np.abs(values[valid] - raw[valid]) > 0.1), "vs region", np.count_nonzero(np.abs(values[valid] - region[valid]) > 0.1))
fig, axes = plt.subplots(1, 4, figsize=(18, 6), constrained_layout=True)
for ax, (name, values) in zip(axes, [("Raw", raw), ("C++", cpp), ("Py-ART region", region), ("Py-ART unwrap", unwrap)]):
    plot = ax.scatter(x[core], y[core], c=values[core], s=12, marker="s", cmap="RdBu_r", vmin=-80, vmax=80)
    ax.set(title=name, xlabel="East of KPAH (km)", ylabel="North of KPAH (km)", xlim=(0, 18), ylim=(-48, -27), aspect="equal")
fig.colorbar(plot, ax=axes, label="Radial velocity (m/s)")
fig.savefig(args.output, dpi=150)
np.savez(Path(args.output).with_suffix(".npz"), raw=raw, cpp=cpp, region=region, unwrap=unwrap, x=x, y=y, azimuth=ray_angles, ranges=radar.range["data"])
