#pragma once

#include "structs_and_constants.h"

// Region-growing velocity dealiasing for all tilts in the volume.
// Runs in place on SingleTilt::Radials_VEL; values that cannot be trusted
// are left at their observed (folded) value.
void dealias_velocity_volume_v2(AllTilt& volume);
