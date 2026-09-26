#pragma once
#include "structs_and_constants.h"

// C++ port of PyART's dealias_region_based algorithm.
// Reference: pyart/correct/region_dealias.py
// Parameters matched to PyART defaults:
//   interval_splits=3, rays_wrap_around=True, centered=True,
//   skip_between_rays=100, skip_along_ray=100
void dealias_velocity_volume_pyart_region(AllTilt& volume);
