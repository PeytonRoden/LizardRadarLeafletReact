#pragma once

#include <vector>
#include <cstdint>
#include <string>
#include "structs_and_constants.h"
#include <iostream>

extern char selected_radar_moment[4];
extern AllTilt combined;

float* interpolate_radar_data_to_voxels(float latitude_topleft, float longitude_topleft, float latitude_bottomright, float longitude_bottomright, int num_voxels_per_side);
