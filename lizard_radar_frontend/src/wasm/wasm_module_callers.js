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

export function setVoxelBounds(module, bounds) {
  const setters = [
    module._set_latitude_topleft,
    module._set_longitude_topleft,
    module._set_latitude_bottomright,
    module._set_longitude_bottomright,
  ];

  if (setters.some((setter) => !setter)) {
    throw new Error("Voxel-bound setter WASM exports are unavailable");
  }

  module._set_latitude_topleft(bounds.north);
  module._set_longitude_topleft(bounds.east);
  module._set_latitude_bottomright(bounds.south);
  module._set_longitude_bottomright(bounds.west);
}

export function readVoxelBounds(module) {
  const getters = [
    module._get_latitude_topleft,
    module._get_longitude_topleft,
    module._get_latitude_bottomright,
    module._get_longitude_bottomright,
  ];

  if (getters.some((getter) => !getter)) {
    throw new Error("Voxel-bound getter WASM exports are unavailable");
  }

  return {
    north: module._get_latitude_topleft(),
    east: module._get_longitude_topleft(),
    south: module._get_latitude_bottomright(),
    west: module._get_longitude_bottomright(),
  };
}

// Interpolation kernel tunables. These mirror the C++ defaults in main.cpp;
// pass overrides to setInterpolationParams to tune at runtime without a rebuild.
export const DEFAULT_INTERPOLATION_PARAMS = {
  Lh0: 0.7,        // horizontal kernel floor, km (near-range)
  Lv0: 0.7,        // vertical kernel floor, km (near-range)
  kLh: 0.065,      // horizontal km of influence added per km of range (~1deg beam)
  kLv: 0.065,      // vertical km of influence added per km of range
  weightSharpness: 50.0, // exponent on splat weights; 1 = plain weighted mean, high = near-center bins dominate
};

export function setInterpolationParams(module, params) {
  const merged = { ...DEFAULT_INTERPOLATION_PARAMS, ...params };
  const setters = [
    module._set_Lh_0,
    module._set_Lv_0,
    module._set_k_Lh,
    module._set_k_Lv,
    module._set_weight_sharpness,
  ];

  if (setters.some((setter) => !setter)) {
    throw new Error("Interpolation-param setter WASM exports are unavailable");
  }

  module._set_Lh_0(merged.Lh0);
  module._set_Lv_0(merged.Lv0);
  module._set_k_Lh(merged.kLh);
  module._set_k_Lv(merged.kLv);
  module._set_weight_sharpness(merged.weightSharpness);
}

export function readInterpolationParams(module) {
  const getters = [
    module._get_Lh_0,
    module._get_Lv_0,
    module._get_k_Lh,
    module._get_k_Lv,
    module._get_weight_sharpness,
  ];

  if (getters.some((getter) => !getter)) {
    throw new Error("Interpolation-param getter WASM exports are unavailable");
  }

  return {
    Lh0: module._get_Lh_0(),
    Lv0: module._get_Lv_0(),
    kLh: module._get_k_Lh(),
    kLv: module._get_k_Lv(),
    weightSharpness: module._get_weight_sharpness(),
  };
}

export function setMaxNumVoxels(module, maxNumVoxels) {
  if (!module._set_max_num_voxels) {
    throw new Error("Max-num-voxels setter WASM export is unavailable");
  }
  module._set_max_num_voxels(maxNumVoxels);
}

export function populateVoxelGrid(module) {
  if (!module._populate_voxel_grid) {
    throw new Error("Populate voxel grid WASM export is unavailable");
  }
  module._populate_voxel_grid();
}

export function releaseVoxelGrid(module) {
  if (!module._release_voxel_grid) {
    throw new Error("Release voxel grid WASM export is unavailable");
  }
  module._release_voxel_grid();
}


export function readVoxelGrid(module) {
  const dimensionGetters = [
    module._get_interpolated_voxels_size_latitude,
    module._get_interpolated_voxels_size_longitude,
    module._get_interpolated_voxels_height,
  ];

  if (!module._get_voxel_grid || dimensionGetters.some((getter) => !getter)) {
    throw new Error("Voxel-grid WASM exports are unavailable");
  }

  const dimensions = {
    latitude: module._get_interpolated_voxels_size_latitude(),
    longitude: module._get_interpolated_voxels_size_longitude(),
    height: module._get_interpolated_voxels_height(),
  };
  const voxelCount = dimensions.latitude * dimensions.longitude * dimensions.height;
  const dimensionsAreValid = Object.values(dimensions).every(
    (dimension) => Number.isInteger(dimension) && dimension > 0,
  );

  if (!dimensionsAreValid) {
    throw new Error("WASM returned invalid voxel-grid dimensions");
  }

  const ptr = module._get_voxel_grid();
  if (!ptr) {
    return { dimensions, voxels: new Float32Array() };
  }

  // Copy immediately. The pointer belongs to WASM memory and is regenerated
  // the next time get_moment_data() is called.
  return {
    dimensions,
    voxels: new Float32Array(module.HEAPF32.buffer, ptr, voxelCount).slice(),
  };
}




export function setDealiasVelocity(module, enabled) {
  if (!module._set_dealias_velocity) {
    throw new Error("Dealias velocity setter WASM export is unavailable");
  }
  module._set_dealias_velocity(enabled);
}

export function toggleDealiasVelocity(module) {
  const next = !getDealiasVelocity(module);
  setDealiasVelocity(module, next);
  return next;
}

export function getDealiasVelocity(module) {
  if (!module._get_dealias_velocity) {
    throw new Error("Get dealias velocity WASM export is unavailable");
  }
  return module._get_dealias_velocity();
}
