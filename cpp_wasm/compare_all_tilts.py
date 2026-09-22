import argparse
from datetime import datetime

import numpy as np
import pandas as pd
import pyart
from netCDF4 import num2date
from scipy import ndimage

parser = argparse.ArgumentParser()
parser.add_argument("volume")
parser.add_argument("csv")
parser.add_argument("output")
args = parser.parse_args()
radar = pyart.io.read_nexrad_archive(args.volume)
rows = pd.read_csv(args.csv)
results = []
for tilt_index, frame in rows.groupby("tilt", sort=True):
    collect_ms = int(frame["collect_ms"].iloc[0])
    candidates = []
    for sweep in range(radar.nsweeps):
        start = int(radar.sweep_start_ray_index["data"][sweep])
        end = int(radar.sweep_end_ray_index["data"][sweep])
        if radar.fields["velocity"]["data"][start : end + 1].count() == 0:
            continue
        end_time = num2date(radar.time["data"][end], radar.time["units"])
        end_ms = ((end_time.hour * 60 + end_time.minute) * 60 + end_time.second) * 1000 + end_time.microsecond // 1000
        candidates.append((abs(end_ms - collect_ms), sweep, end_ms))
    delta_ms, sweep_index, end_ms = min(candidates)
    if delta_ms > 2:
        raise RuntimeError(f"tilt {tilt_index} has no matching sweep: nearest delta {delta_ms} ms")
    sweep = radar.extract_sweeps([sweep_index])
    raw = sweep.fields["velocity"]["data"].filled(np.nan)
    region = pyart.correct.dealias_region_based(sweep, gatefilter=False)["data"].filled(np.nan)
    unwrap = pyart.correct.dealias_unwrap_phase(sweep, unwrap_unit="sweep", gatefilter=False)["data"].filled(np.nan)
    ray = frame["ray"].to_numpy(dtype=int)
    gate = np.rint((frame["range"].to_numpy() - sweep.range["data"][0]) / np.diff(sweep.range["data"])[0]).astype(int)
    azimuth = frame["azimuth"].to_numpy()
    az_error = np.abs((azimuth - sweep.azimuth["data"][ray] + 180) % 360 - 180)
    if az_error.max() > 0.01:
        raise RuntimeError(f"tilt {tilt_index} azimuth mismatch {az_error.max()}")
    decoded = frame["raw"].to_numpy()
    cpp = frame["corrected"].to_numpy()
    source = raw[ray, gate]
    decode_mismatches = int(np.count_nonzero(np.abs(decoded - source) > 0.01))
    if decode_mismatches:
        raise RuntimeError(f"tilt {tilt_index} has {decode_mismatches} decode mismatches")
    region_values = region[ray, gate]
    unwrap_values = unwrap[ray, gate]
    region_changed = np.abs(region_values - decoded) > 0.01
    unwrap_changed = np.abs(unwrap_values - decoded) > 0.01
    cpp_changed = np.abs(cpp - decoded) > 0.01
    consensus = np.isfinite(region_values) & np.isfinite(unwrap_values) & (np.abs(region_values - unwrap_values) < 0.01)
    consensus_changed = consensus & region_changed
    consensus_raw = consensus & ~region_changed
    cpp_disagrees = consensus & (np.abs(cpp - region_values) > 0.01)
    false_positive = consensus_raw & cpp_changed
    missed = consensus_changed & ~cpp_changed
    wrong_fold = consensus_changed & cpp_changed & (np.abs(cpp - region_values) > 0.01)
    changed_grid = np.zeros(raw.shape, dtype=bool)
    changed_grid[ray, gate] = cpp_changed
    labels, components = ndimage.label(changed_grid, np.ones((3, 3), dtype=int))
    spoke_components = 0
    isolated_components = 0
    largest_component = 0
    for label in range(1, components + 1):
        rr, gg = np.where(labels == label)
        size = len(rr)
        largest_component = max(largest_component, size)
        isolated_components += size <= 2
        spoke_components += size >= 10 and rr.max() - rr.min() <= 2 and gg.max() - gg.min() >= 20
    result = {
        "tilt": int(tilt_index),
        "sweep": sweep_index,
        "elevation": float(frame["elevation"].iloc[0]),
        "end_ms": end_ms,
        "nyquist_min": float(frame["nyquist"].min()),
        "nyquist_max": float(frame["nyquist"].max()),
        "gates": len(frame),
        "cpp_changed": int(cpp_changed.sum()),
        "region_changed": int(region_changed.sum()),
        "unwrap_changed": int(unwrap_changed.sum()),
        "reference_consensus": int(consensus.sum()),
        "consensus_changed": int(consensus_changed.sum()),
        "cpp_disagrees": int(cpp_disagrees.sum()),
        "false_positive": int(false_positive.sum()),
        "missed": int(missed.sum()),
        "wrong_fold": int(wrong_fold.sum()),
        "cpp_region_diff": int(np.count_nonzero(np.isfinite(region_values) & (np.abs(cpp - region_values) > 0.01))),
        "cpp_unwrap_diff": int(np.count_nonzero(np.isfinite(unwrap_values) & (np.abs(cpp - unwrap_values) > 0.01))),
        "pyart_disagrees": int(np.count_nonzero(np.isfinite(region_values) & np.isfinite(unwrap_values) & (np.abs(region_values - unwrap_values) > 0.01))),
        "isolated_components": isolated_components,
        "spoke_components": spoke_components,
        "largest_component": largest_component,
        "cpp_min": float(np.nanmin(cpp)),
        "cpp_max": float(np.nanmax(cpp)),
    }
    results.append(result)
    print(result)
    if false_positive.any():
        print("false-positive extent", "azimuth", float(azimuth[false_positive].min()), float(azimuth[false_positive].max()),
              "range", float(frame["range"].to_numpy()[false_positive].min()), float(frame["range"].to_numpy()[false_positive].max()))
summary = pd.DataFrame(results)
summary.to_csv(args.output, index=False)
print("\nTOTALS")
for column in ["gates", "cpp_changed", "region_changed", "unwrap_changed", "reference_consensus", "consensus_changed", "cpp_disagrees", "false_positive", "missed", "wrong_fold", "cpp_region_diff", "cpp_unwrap_diff", "pyart_disagrees", "isolated_components", "spoke_components"]:
    print(column, int(summary[column].sum()))
