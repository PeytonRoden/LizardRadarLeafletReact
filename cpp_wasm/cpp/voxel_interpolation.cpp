#include "voxel_interpolation.h"
#include <cmath>

extern float * RadarVoxelVolume;
// Inputs:
//   radarLatDeg, radarLonDeg : radar location
//   rangeKm                  : bin-center range from radar
//   azimuthDeg               : clockwise from north
//   elevationDeg             : beam elevation
//
// Returns:
//   latitude/longitude in degrees
//
// Uses:
//   4/3 effective Earth radius for beam curvature
//   local tangent-plane approximation for lat/lon
inline LatLonHeight nexradBinLatLonHeightPrepared(
    double radarLatRad,
    double radarLonRad,
    double cosRadarLat,
    double sinElevation,
    double cosElevation,
    double rangem,
    double sinAzimuth,
    double cosAzimuth)
{
    float rangeKm = rangem / 1000.0f;
    constexpr double Re = 6371.0;          // km
    constexpr double R  = Re * (4.0 / 3.0); // effective Earth radius

    // Beam height above the effective Earth surface.
    const double h =
        std::sqrt(
            rangeKm * rangeKm +
            R * R +
            2.0 * rangeKm * R * sinElevation
        ) - R;

    // Ground-range arc on the effective Earth.
    const double groundRange =
        R * std::asin(
            (rangeKm * cosElevation) / (R + h)
        ); 

    // Local east/north displacement.
    const double east  = groundRange * sinAzimuth;
    const double north = groundRange * cosAzimuth;
 
    // Cheap tangent-plane conversion to lat/lon.
    return {
        (radarLatRad + north / Re) * (180.0 / 3.14159265358979323846),
        (radarLonRad + east / (Re * cosRadarLat)) * (180.0 / 3.14159265358979323846),
        h
    };
}

inline LatLonHeight nexradBinLatLonHeight(
    double radarLatDeg,
    double radarLonDeg,
    double rangem,
    double azimuth,
    double elevationDeg)
{
    constexpr double PI = 3.14159265358979323846;
    const double radarLatRad = radarLatDeg * PI / 180.0;
    return nexradBinLatLonHeightPrepared(
        radarLatRad,
        radarLonDeg * PI / 180.0,
        std::cos(radarLatRad),
        std::sin(elevationDeg * PI / 180.0),
        std::cos(elevationDeg * PI / 180.0),
        rangem,
        std::sin(azimuth),
        std::cos(azimuth));
}





float * interpolate_radar_data_to_voxels(const AllTilt& all_tilt, float latitude_topleft, float longitude_topleft, float latitude_bottomright, float longitude_bottomright, int num_voxels_per_side) {
    
    RadarVoxelVolume = new float[num_voxels_per_side * num_voxels_per_side * num_voxels_per_side]; 

    float latitude_range = latitude_bottomright - latitude_topleft;
    float longitude_range = longitude_bottomright - longitude_topleft;




    return RadarVoxelVolume;
}

