export function readMomentData(module) {
  if (
    !module._get_moment_data ||
    !module._get_moment_data_vertices_then_val_size
  ) {
    throw new Error("Moment-data WASM exports are unavailable");
  }

  const ptr = module._get_moment_data();
  const size = module._get_moment_data_vertices_then_val_size();

  if (!ptr || size === 0) {
    return new Float32Array();
  }

  // Copy immediately. The pointer belongs to WASM memory and is regenerated
  // the next time get_moment_data() is called.
  return new Float32Array(module.HEAPF32.buffer, ptr, size).slice();
}

export function readPackedRadarData(module) {
  if (
    !module._get_packed_radar_data ||
    !module._get_packed_radar_data_size
  ) {
    throw new Error("Packed-radar-data WASM exports are unavailable");
  }

  const ptr = module._get_packed_radar_data();
  const size = module._get_packed_radar_data_size();

  if (!ptr || size === 0) {
    return new Float32Array();
  }

  // Copy immediately. The pointer belongs to WASM memory and is regenerated
  // the next time get_moment_data() is called.
  return new Float32Array(module.HEAPF32.buffer, ptr, size).slice();
}

export function readTiltAngles(module) {
  if (
    !module._get_tilt_angles ||
    !module._get_tilt_angles_size
  ) {
    throw new Error("Tilt angles WASM exports are unavailable");
  }

  const ptr = module._get_tilt_angles();
  const size = module._get_tilt_angles_size();

  if (!ptr || size === 0) {
    return new Float32Array();
  }

  // Copy immediately. The pointer belongs to WASM memory and is regenerated
  // the next time get_moment_data() is called.
  // Unused slots in the fixed-size C array are -1; drop them.
  return new Float32Array(module.HEAPF32.buffer, ptr, size)
    .slice()
    .filter((angle) => angle !== -1);
}


export function readRadarStationLatitude(module) {
  if (!module._get_current_radar_station_latitude) {
    throw new Error("Radar station latitude WASM export is unavailable");
  }
  return module._get_current_radar_station_latitude();
}

export function readRadarStationLongitude(module) {
  if (!module._get_current_radar_station_longitude) {
    throw new Error("Radar station longitude WASM export is unavailable");
  }
  return module._get_current_radar_station_longitude();
}

export function readCurrentDataTiltAngle(module) {
  if (!module._get_current_tilt_angle) {
    throw new Error("Current tilt angle WASM export is unavailable");
  }
  return module._get_current_tilt_angle();
}