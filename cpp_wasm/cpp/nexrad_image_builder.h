#pragma once

#include <vector>
#include <cstdint>
#include <string>
#include "structs_and_constants.h"

extern char selected_radar_moment[4];
extern float * moment_data_vertices_then_val;
extern int moment_data_vertices_then_val_size;

std::vector<uint8_t> saveTiltAsPNGInterpolate2(const SingleTilt& tilt, const std::string& filename, const int SIZE = 1000, float M_PER_PIXEL = 800.0f);
float* buildMomentDataVerticesThenValue(const SingleTilt& tilt, int* output_size);
std::vector<float> new_shader_vals(const SingleTilt& tilt);