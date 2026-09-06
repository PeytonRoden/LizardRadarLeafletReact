#include "voxel_interpolation.h"
#include <cmath>
#include <cstdint>
#include <iostream>

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


inline RadarBin nexradLatLonToBinPrepared(
    double radarLatRad,
    double radarLonRad,
    double cosRadarLat,
    double targetLatRad,
    double targetLonRad,
    double sinElevation,
    double cosElevation)
{
    constexpr double PI = 3.14159265358979323846;
    constexpr double Re = 6371.0;              // km
    constexpr double R  = Re * (4.0 / 3.0);    // km

    // ------------------------------------------------------------
    // 1. Convert target lat/lon into local east/north displacement
    // ------------------------------------------------------------

    const double dLat = targetLatRad - radarLatRad;
    const double dLon = targetLonRad - radarLonRad;

    const double north = Re * dLat;
    const double east  = Re * cosRadarLat * dLon;

    // ------------------------------------------------------------
    // 2. Ground range
    // ------------------------------------------------------------

    const double groundRange =
        std::sqrt(east * east + north * north);

    // ------------------------------------------------------------
    // 3. Azimuth
    //
    // 0   = north
    // 90  = east
    // 180 = south
    // 270 = west
    // ------------------------------------------------------------

    double azimuth =
        std::atan2(east, north);

    if (azimuth < 0.0)
        azimuth += 2.0 * PI;

    // ------------------------------------------------------------
    // 4. Solve for slant range
    //
    // Forward:
    //
    // groundRange =
    //     R * asin(
    //         range * cosElevation / (R + h)
    //     )
    //
    // Therefore:
    //
    // sin(groundRange / R)
    //     = range * cosElevation / (R + h)
    //
    // ------------------------------------------------------------

    const double theta = groundRange / R;
    const double sinTheta = std::sin(theta);

    //
    // Forward height equation:
    //
    // h = sqrt(
    //      range² +
    //      R² +
    //      2*range*R*sinElevation
    //     ) - R
    //
    // Therefore:
    //
    // R + h =
    // sqrt(
    //      range² +
    //      R² +
    //      2*range*R*sinElevation
    // )
    //
    // Substitute:
    //
    // range*cosElevation =
    //     (R+h)*sinTheta
    //
    // Square both sides and solve for range.
    //

    const double A =
        cosElevation * cosElevation -
        sinTheta * sinTheta;

    const double B =
        -2.0 * R *
        sinElevation *
        sinTheta * sinTheta;

    const double C =
        -R * R *
        sinTheta * sinTheta;

    const double discriminant =
        B * B - 4.0 * A * C;

    const double rangeKm =
        (-B + std::sqrt(discriminant)) /
        (2.0 * A);

    // ------------------------------------------------------------
    // 5. Recover beam height using the exact same equation
    //    as the forward transform.
    // ------------------------------------------------------------

    const double heightKm =
        std::sqrt(
            rangeKm * rangeKm +
            R * R +
            2.0 * rangeKm * R * sinElevation
        ) - R;

    return {
        rangeKm * 1000.0,
        azimuth * 180.0 / PI,
        heightKm * 1000.0
    };
}

inline RadarBin nexradLatLonToBin(
    double radarLatDeg,
    double radarLonDeg,
    double targetLatDeg,
    double targetLonDeg,
    double elevationDeg)
{
    constexpr double PI = 3.14159265358979323846;

    const double radarLatRad =
        radarLatDeg * PI / 180.0;

    const double radarLonRad =
        radarLonDeg * PI / 180.0;

    const double targetLatRad =
        targetLatDeg * PI / 180.0;

    const double targetLonRad =
        targetLonDeg * PI / 180.0;

    const double elevationRad =
        elevationDeg * PI / 180.0;

    return nexradLatLonToBinPrepared(
        radarLatRad,
        radarLonRad,
        std::cos(radarLatRad),
        targetLatRad,
        targetLonRad,
        std::sin(elevationRad),
        std::cos(elevationRad)
    );
}


int get_voxel_index(int x, int y, int z, int num_voxels_per_side) {
    return x + y * num_voxels_per_side + z * num_voxels_per_side * num_voxels_per_side;
}

int get_nearest_voxel_index(
    float lat,
    float lon,
    float height,
    float latitude_topleft,
    float longitude_topleft,
    float latitude_bottomright,
    float longitude_bottomright,
    float latitude_range,
    float longitude_range,
    float latitude_step,
    float longitude_step,
    float height_step,
    int num_voxels_per_side
){

    int x = (int)((lat - latitude_topleft) / latitude_step);
    int y = (int)((lon - longitude_topleft) / longitude_step);
    int z = (int)(height / height_step);
    
    int voxel_index = get_voxel_index(x, y, z, num_voxels_per_side);

    if (voxel_index < 0 ){
        std::cout << "oops... negative voxel index" << std::endl;
    }

    return voxel_index;
}


float * interpolate_radar_data_to_voxels(float latitude_topleft, float longitude_topleft, float latitude_bottomright, float longitude_bottomright, int num_voxels_per_side) {
    //However we want to be as efficent as possible, 
    // we dont want to calculate lat, long, height for every bin in the alltilts
    // we will start from both heighest and lowest tilt angles.
    // determine max, min of both dist, azimuth use those to create a bounding box
    // then we will iterate through the bounding box and calculate the lat, long, height for each bin and try to put them in the voxel grid
    const float MAX_HEIGHT_M = 10668.0f;
    const float MIN_HEIGHT_M = 0.0f;

    RadarVoxelVolume = new float[num_voxels_per_side * num_voxels_per_side * num_voxels_per_side]; 
    
    //init to zeroes
    for (int i = 0; i < num_voxels_per_side * num_voxels_per_side * num_voxels_per_side; i++) {
        RadarVoxelVolume[i] = 0.0f;
    }
    
    int* visited_count = new int[num_voxels_per_side * num_voxels_per_side * num_voxels_per_side];

    //init to zeroes
    for (int i = 0; i < num_voxels_per_side * num_voxels_per_side * num_voxels_per_side; i++) {
        visited_count[i] = 0;
    }



    float latitude_range = latitude_bottomright - latitude_topleft;
    float longitude_range = longitude_bottomright - longitude_topleft;

    float latitude_step = latitude_range / num_voxels_per_side;
    float longitude_step = longitude_range / num_voxels_per_side;

    //height step trickier to calculate, lets just go from 0 to really high up in atmosphere, say 35000 feet which is roughly 10668 meters
    float height_step = MAX_HEIGHT_M / num_voxels_per_side;

    // //now we will iterate through the bounding box and calculate the lat, long, height for each bin and try to put them in the voxel grid
    // for (int i = 0; i < num_voxels_per_side; i++) {
    //     for (int j = 0; j < num_voxels_per_side; j++) {
    //         for (int k = 0; k < num_voxels_per_side; k++) {
    //             float latitude = latitude_topleft + i * latitude_step;
    //             float longitude = longitude_topleft + j * longitude_step;
    //             float height = k * height_step;
                
    //         }
    //     }
    // }

    float bound_azimuth_curr_1;
    float bound_azimuth_curr_2;
    float bound_azimuth_curr_3;
    float bound_azimuth_curr_4;

    float bound_dist_curr_1;
    float bound_dist_curr_2;
    float bound_dist_curr_3;
    float bound_dist_curr_4;

    float lower_bound_azimuth_curr;
    float upper_bound_azimuth_curr;
    float lower_bound_dist_curr;
    float upper_bound_dist_curr;

    float radar_latitude =  combined.Tilts[0].vol_el_rad.vol.lat;
    float radar_longitude = combined.Tilts[0].vol_el_rad.vol.lon;

    float azimuth_deg;
    float dist;
    float value;

    // tillts are already sorted by elevation angle, so we can iterate through, start at 0 then going to highest tilt, then to tilt 1 then highest tilt -1
    bool lower_tilt = true;
    for (int i = 0; i < combined.Tilts.size(); i++) {

        int tilt_index;
        if (lower_tilt) {
            lower_tilt = false;
            tilt_index = static_cast<int>((i+1) / 2);
        } else {
            lower_tilt = true;
            tilt_index = combined.Tilts.size() - 1 - static_cast<int>((i) / 2);
        }

        //get the tilt angle
        SingleTilt tilt = combined.Tilts[tilt_index];
        float tilt_angle = tilt.ElevationAngle;

        
        std::cout << "tilt angle is: " << tilt_angle << std::endl;
        std::cout << "tilt index is: " << tilt_index << std::endl;


        auto selected_radial = get_moment_radials(tilt, selected_radar_moment);
        if (selected_radial == nullptr) {
            std::cerr << "Invalid moment: " << selected_radar_moment << std::endl;
            continue;
        }

        // Calculate upper and lower bounds for the azimuth and deg

        auto bound_topleft = nexradLatLonToBin(radar_latitude, radar_longitude, latitude_topleft, longitude_topleft, tilt_angle);
        auto bound_topright = nexradLatLonToBin(radar_latitude, radar_longitude, latitude_bottomright, longitude_topleft, tilt_angle);
        auto bound_bottomleft = nexradLatLonToBin(radar_latitude, radar_longitude, latitude_topleft, longitude_bottomright, tilt_angle);
        auto bound_bottomright = nexradLatLonToBin(radar_latitude, radar_longitude, latitude_bottomright, longitude_bottomright, tilt_angle);

        bound_azimuth_curr_1 = bound_topleft.azimuth_deg;
        bound_azimuth_curr_2 = bound_topright.azimuth_deg;
        bound_azimuth_curr_3 = bound_bottomleft.azimuth_deg;
        bound_azimuth_curr_4 = bound_bottomright.azimuth_deg;

        bound_dist_curr_1 = bound_topleft.range_m;
        bound_dist_curr_2 = bound_topright.range_m;
        bound_dist_curr_3 = bound_bottomleft.range_m;
        bound_dist_curr_4 = bound_bottomright.range_m;

        // determine bounds
        lower_bound_azimuth_curr = std::min({bound_azimuth_curr_1, bound_azimuth_curr_2, bound_azimuth_curr_3, bound_azimuth_curr_4});
        upper_bound_azimuth_curr = std::max({bound_azimuth_curr_1, bound_azimuth_curr_2, bound_azimuth_curr_3, bound_azimuth_curr_4});
        lower_bound_dist_curr = std::min({bound_dist_curr_1, bound_dist_curr_2, bound_dist_curr_3, bound_dist_curr_4});
        upper_bound_dist_curr = std::max({bound_dist_curr_1, bound_dist_curr_2, bound_dist_curr_3, bound_dist_curr_4});


        std::cout << "radar latitude: " << radar_latitude << std::endl;
        std::cout << "radar longitude: " << radar_longitude << std::endl;

        std::cout << "latitude topleft: " << latitude_topleft << std::endl;
        std::cout << "longitude topleft: " << longitude_topleft << std::endl;
        std::cout << "latitude bottomright: " << latitude_bottomright << std::endl;
        std::cout << "longitude bottomright: " << longitude_bottomright << std::endl;
        
        //if radar station falls within bounding box, then lwoer bound dist is gonna be 0
        if (radar_latitude <= latitude_topleft && radar_latitude >= latitude_bottomright &&
            radar_longitude <= longitude_topleft && radar_longitude >= longitude_bottomright) {
            
            lower_bound_dist_curr = 0.0f;
            lower_bound_azimuth_curr = 0.0f;
            upper_bound_azimuth_curr = 360.0f;
        }

        // each singletilt object has its own max_dist, make sure upper_bound_dist_curr is not larger
        if (upper_bound_dist_curr > tilt.maxDist){
            upper_bound_dist_curr = tilt.maxDist;
        }

        // also check for edge case where lower_bound_dist_curr is > maxdist then maybe just return a nullptr
        if (lower_bound_dist_curr > tilt.maxDist){
            return nullptr;
        }



        ///print all the bounds:
        std::cout << "lower_bound_azimuth_curr: " << lower_bound_azimuth_curr << std::endl;
        std::cout << "upper_bound_azimuth_curr: " << upper_bound_azimuth_curr << std::endl;
        std::cout << "lower_bound_dist_curr: " << lower_bound_dist_curr << std::endl;
        std::cout << "upper_bound_dist_curr: " << upper_bound_dist_curr << std::endl;




        for (size_t i = 0; i + 2 < selected_radial->size(); i += 3) {
            azimuth_deg = (*selected_radial)[i];
            dist = (*selected_radial)[i + 1];
            value = (*selected_radial)[i + 2];

            //check if azimuth and distance are within bounds
            if (azimuth_deg < lower_bound_azimuth_curr || azimuth_deg > upper_bound_azimuth_curr || dist < lower_bound_dist_curr || dist > upper_bound_dist_curr) {
                continue;
            }

            //add to voxel grid
            //TODO: implement voxel grid addition

            if (!std::isnan(value)) {

                LatLonHeight latLonHeight = nexradBinLatLonHeight(
                    radar_latitude,
                    radar_longitude,
                    dist,
                    azimuth_deg,
                    tilt.ElevationAngle
                );

                int voxel_index = get_nearest_voxel_index(
                    latLonHeight.lat,
                    latLonHeight.lon,
                    latLonHeight.height,
                    latitude_topleft,
                    longitude_topleft,
                    latitude_bottomright,
                    longitude_bottomright,
                    latitude_range,
                    longitude_range,
                    latitude_step,
                    longitude_step,
                    height_step,
                    num_voxels_per_side
                );

                // run an averaging 
                int current_count = visited_count[voxel_index]++;
                if (current_count > 0) {
                    RadarVoxelVolume[voxel_index] = (RadarVoxelVolume[voxel_index] * current_count + value) / (current_count + 1);
                } else {
                    RadarVoxelVolume[voxel_index] = value;
                }

                //run a lil debug print every 1000 vals or so
                if (i % 1000 == 0) {
                    std::cout << "voxel_index: " << voxel_index << " value: " << value << std::endl;
                }
            }







        }





    }












    return RadarVoxelVolume;
}

