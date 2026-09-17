#include "voxel_interpolation.h"
#include <cmath>
#include <cstdint>
#include <iostream>

extern float * RadarVoxelVolume;
extern float Lv_0;
extern float Lh_0;
extern float k_Lv;
extern float k_Lh;
extern float latlong_radius_0;
extern float height_radius_0;
extern float latlong_radius_scale;
extern float height_radius_scale;
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
        h * 1000.0
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
    const double azimuthRad = azimuth * PI / 180.0;
    return nexradBinLatLonHeightPrepared(
        radarLatRad,
        radarLonDeg * PI / 180.0,
        std::cos(radarLatRad),
        std::sin(elevationDeg * PI / 180.0),
        std::cos(elevationDeg * PI / 180.0),
        rangem,
        std::sin(azimuthRad),
        std::cos(azimuthRad));
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



float calculate_Lv(float r) {
    // for z's
    return Lv_0 + k_Lv * r;
}

float calculate_Lh(float r) {
    // for x's and y's
    return Lh_0 + k_Lh * r;
}

int calculate_latlong_radius(float dist_m) {
    //take in dist_m as r
    float dist_km = dist_m / 1000.0;
    return static_cast<int>(latlong_radius_0 + latlong_radius_scale * dist_km);
}

int calculate_height_radius(float height_m) {
    //take in height_m as height
    float height_km = height_m / 1000.0;
    return static_cast<int>(height_radius_0 + height_radius_scale * height_km);
}























int get_voxel_index_new(int x, int y, int z, int num_voxels_latitude, int num_voxels_longitude, int num_voxels_height) {
    return x + y * num_voxels_latitude + z * num_voxels_latitude * num_voxels_longitude;
}

int get_nearest_voxel_index_new(
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
    int num_voxels_latitude,
    int num_voxels_longitude,
    int num_voxels_height
){

    float x_position = (lat - latitude_topleft) / latitude_step;
    float y_position = (lon - longitude_topleft) / longitude_step;
    float z_position = height / height_step;

    if (!std::isfinite(x_position) || !std::isfinite(y_position) || !std::isfinite(z_position) ||
        x_position < 0.0f || x_position >= num_voxels_latitude ||
        y_position < 0.0f || y_position >= num_voxels_longitude ||
        z_position < 0.0f || z_position >= num_voxels_height) {
        //std::cout << "Invalid voxel position: " << x_position << ", " << y_position << ", " << z_position << std::endl;
        return -1;
    }

    int x = static_cast<int>(x_position);
    int y = static_cast<int>(y_position);
    int z = static_cast<int>(z_position);

    return get_voxel_index_new(x, y, z, num_voxels_latitude, num_voxels_longitude, num_voxels_height);
}

std::pair<std::vector<float>, std::vector<int>> get_voxel_weights_and_indices_new(
    float lat,
    float lon,
    float height,
    float dist,
    float latitude_topleft,
    float longitude_topleft,
    float latitude_bottomright,
    float longitude_bottomright,
    float latitude_range,
    float longitude_range,
    float latitude_step,
    float longitude_step,
    float height_step,
    int num_voxels_latitude,
    int num_voxels_longitude,
    int num_voxels_height
) {

    std::vector<float> weights;
    std::vector<int> indices;

    // get radius of interest
    // radius will be just number of voxel points other than absolute nearest
    // say we get 3 we will -3, -2, -1, 0 1, 2, 3 , center being the absolute nearest
    constexpr float KM_PER_DEGREE_LATITUDE = 111.32f;
    constexpr float PI = 3.14159265358979323846f;
    const float range_km = dist / 1000.0f;
    const float Lh = std::max(calculate_Lh(range_km), 0.000001f);
    const float Lv = std::max(calculate_Lv(range_km), 0.000001f);
    const float latitude_voxel_size_km = std::max(std::abs(latitude_step) * KM_PER_DEGREE_LATITUDE, 0.000001f);
    const float longitude_voxel_size_km = std::max(std::abs(longitude_step) * KM_PER_DEGREE_LATITUDE * std::abs(std::cos(lat * PI / 180.0f)), 0.000001f);
    const float height_voxel_size_km = std::max(std::abs(height_step) / 1000.0f, 0.000001f);
    // cap the neighborhood so a single bin can never fan out over the whole grid
    constexpr int MAX_VOXEL_RADIUS = 64;
    const int radius_latitude = std::min({num_voxels_latitude - 1, MAX_VOXEL_RADIUS, static_cast<int>(std::ceil(3.0f * Lh / latitude_voxel_size_km))});
    const int radius_longitude = std::min({num_voxels_longitude - 1, MAX_VOXEL_RADIUS, static_cast<int>(std::ceil(3.0f * Lh / longitude_voxel_size_km))});
    const int radius_height = std::min({num_voxels_height - 1, MAX_VOXEL_RADIUS, static_cast<int>(std::ceil(3.0f * Lv / height_voxel_size_km))});


    float x_position_center = (lat - latitude_topleft) / latitude_step;
    float y_position_center = (lon - longitude_topleft) / longitude_step;
    float z_position_center = height / height_step;

    if (!std::isfinite(x_position_center) || !std::isfinite(y_position_center) || !std::isfinite(z_position_center) ||
        x_position_center < 0.0f || x_position_center >= num_voxels_latitude ||
        y_position_center < 0.0f || y_position_center >= num_voxels_longitude ||
        z_position_center < 0.0f || z_position_center >= num_voxels_height) {
        //std::cout << "Invalid voxel position: " << x_position << ", " << y_position << ", " << z_position << std::endl;
        return std::make_pair(weights, indices);
    }

    int x_center = static_cast<int>(x_position_center);
    int y_center = static_cast<int>(y_position_center);
    int z_center = static_cast<int>(z_position_center);

    int voxel_index;
    float weight;

    for (int i = x_center - radius_latitude; i <= x_center + radius_latitude; i++) {
        for (int j = y_center - radius_longitude; j <= y_center + radius_longitude; j++) {
            for (int k = z_center - radius_height; k <= z_center + radius_height; k++) {
                // TODO: calculate weight and add to weights and indices

                if (i < 0 || i >= num_voxels_latitude ||
                    j < 0 || j >= num_voxels_longitude ||
                    k < 0 || k >= num_voxels_height) {
                    continue;
                }

                voxel_index = get_voxel_index_new(i, j, k, num_voxels_latitude, num_voxels_longitude, num_voxels_height);

                // add a guard so voxel_index is not less than 0 or greater than num_voxels_per_side^3
                if (voxel_index < 0 || voxel_index >= num_voxels_latitude * num_voxels_longitude * num_voxels_height) {
                    continue;
                }

                // TODO: add to weights and indices
                const float latitude_distance_km = (i - x_center) * latitude_voxel_size_km;
                const float longitude_distance_km = (j - y_center) * longitude_voxel_size_km;
                const float height_distance_km = (k - z_center) * height_voxel_size_km;
                weight = std::exp(-((latitude_distance_km * latitude_distance_km + longitude_distance_km * longitude_distance_km) / (Lh * Lh) +
                    (height_distance_km * height_distance_km) / (Lv * Lv)));
                weights.push_back(weight);
                indices.push_back(voxel_index);
            }
        }
    }
    
    return std::make_pair(weights, indices);
}


float * interpolate_radar_data_to_voxels_new(float latitude_topleft, float longitude_topleft, float latitude_bottomright, float longitude_bottomright, int num_voxels_latitude, int num_voxels_longitude, int num_voxels_height) {
    //However we want to be as efficent as possible, 
    // we dont want to calculate lat, long, height for every bin in the alltilts
    // we will start from both heighest and lowest tilt angles.
    // determine max, min of both dist, azimuth use those to create a bounding box
    // then we will iterate through the bounding box and calculate the lat, long, height for each bin and try to put them in the voxel grid
    const float MAX_HEIGHT_M = 20668.0f;
    const float MIN_HEIGHT_M = 0.0f;

    delete[] RadarVoxelVolume;
    RadarVoxelVolume = new float[num_voxels_latitude * num_voxels_longitude * num_voxels_height]; 

    float * sum_weight_n_minus_1 = new float[num_voxels_latitude * num_voxels_longitude * num_voxels_height];
    
    //init to no-data values
    for (int i = 0; i < num_voxels_latitude * num_voxels_longitude * num_voxels_height; i++) {
        RadarVoxelVolume[i] = NAN;
        sum_weight_n_minus_1[i] = 0.0f;
    }

    // int* visited_count = new int[num_voxels_latitude * num_voxels_longitude * num_voxels_height];

    // //init to zeroes
    // for (int i = 0; i < num_voxels_latitude * num_voxels_longitude * num_voxels_height; i++) {
    //     visited_count[i] = 0;
    // }



    float latitude_range = latitude_bottomright - latitude_topleft;
    float longitude_range = longitude_bottomright - longitude_topleft;
    float minimum_latitude = std::min(latitude_topleft, latitude_bottomright);
    float maximum_latitude = std::max(latitude_topleft, latitude_bottomright);
    float minimum_longitude = std::min(longitude_topleft, longitude_bottomright);
    float maximum_longitude = std::max(longitude_topleft, longitude_bottomright);

    float latitude_step = latitude_range / num_voxels_latitude;
    float longitude_step = longitude_range / num_voxels_longitude;

    //height step trickier to calculate, lets just go from 0 to really high up in atmosphere, say 35000 feet which is roughly 10668 meters
    float height_step = MAX_HEIGHT_M / num_voxels_height;

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
    bool azimuth_bounds_wrap;
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
        const SingleTilt& tilt = combined.Tilts[tilt_index];
        float tilt_angle = tilt.ElevationAngle;

        
        // std::cout << "tilt angle is: " << tilt_angle << std::endl;
        // std::cout << "tilt index is: " << tilt_index << std::endl;


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
        azimuth_bounds_wrap = upper_bound_azimuth_curr - lower_bound_azimuth_curr > 180.0f;
        lower_bound_dist_curr = std::min({bound_dist_curr_1, bound_dist_curr_2, bound_dist_curr_3, bound_dist_curr_4});
        upper_bound_dist_curr = std::max({bound_dist_curr_1, bound_dist_curr_2, bound_dist_curr_3, bound_dist_curr_4});


        // std::cout << "radar latitude: " << radar_latitude << std::endl;
        // std::cout << "radar longitude: " << radar_longitude << std::endl;

        // std::cout << "latitude topleft: " << latitude_topleft << std::endl;
        // std::cout << "longitude topleft: " << longitude_topleft << std::endl;
        // std::cout << "latitude bottomright: " << latitude_bottomright << std::endl;
        // std::cout << "longitude bottomright: " << longitude_bottomright << std::endl;
        
        //if radar station falls within bounding box, then lwoer bound dist is gonna be 0
        if (radar_latitude <= latitude_topleft && radar_latitude >= latitude_bottomright &&
            radar_longitude <= longitude_topleft && radar_longitude >= longitude_bottomright) {
            
            lower_bound_dist_curr = 0.0f;
            lower_bound_azimuth_curr = 0.0f;
            upper_bound_azimuth_curr = 360.0f;
            azimuth_bounds_wrap = false;
        }

        // each singletilt object has its own max_dist, make sure upper_bound_dist_curr is not larger
        if (upper_bound_dist_curr > tilt.maxDist){
            upper_bound_dist_curr = tilt.maxDist;
        }

        // skip tilts that do not reach the selected bounds
        if (lower_bound_dist_curr > tilt.maxDist){
            continue;
        }



        ///print all the bounds:
        // std::cout << "lower_bound_azimuth_curr: " << lower_bound_azimuth_curr << std::endl;
        // std::cout << "upper_bound_azimuth_curr: " << upper_bound_azimuth_curr << std::endl;
        // std::cout << "lower_bound_dist_curr: " << lower_bound_dist_curr << std::endl;
        // std::cout << "upper_bound_dist_curr: " << upper_bound_dist_curr << std::endl;




        for (size_t i = 0; i + 2 < selected_radial->size(); i += 3) {
            azimuth_deg = (*selected_radial)[i];
            dist = (*selected_radial)[i + 1];
            value = (*selected_radial)[i + 2];

            //check if azimuth and distance are within bounds
            bool azimuth_outside_bounds = azimuth_bounds_wrap
                ? azimuth_deg > lower_bound_azimuth_curr && azimuth_deg < upper_bound_azimuth_curr
                : azimuth_deg < lower_bound_azimuth_curr || azimuth_deg > upper_bound_azimuth_curr;
            if (azimuth_outside_bounds || dist < lower_bound_dist_curr || dist > upper_bound_dist_curr) {
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

                if (!std::isfinite(latLonHeight.lat) || !std::isfinite(latLonHeight.lon) || !std::isfinite(latLonHeight.height) ||
                    latLonHeight.lat < minimum_latitude || latLonHeight.lat >= maximum_latitude ||
                    latLonHeight.lon < minimum_longitude || latLonHeight.lon >= maximum_longitude ||
                    latLonHeight.height < MIN_HEIGHT_M || latLonHeight.height >= MAX_HEIGHT_M) {
                    continue;
                }

                auto weight_and_indices = get_voxel_weights_and_indices_new(
                    latLonHeight.lat,
                    latLonHeight.lon,
                    latLonHeight.height,
                    dist,
                    latitude_topleft,
                    longitude_topleft,
                    latitude_bottomright,
                    longitude_bottomright,
                    latitude_range,
                    longitude_range,
                    latitude_step,
                    longitude_step,
                    height_step,
                    num_voxels_latitude,
                    num_voxels_longitude,
                    num_voxels_height
                );

                std::vector<float> weights = weight_and_indices.first;
                std::vector<int> indices = weight_and_indices.second;

                for (size_t j = 0; j < indices.size(); j++) {
                    int voxel_index = indices[j];
                    double weight = weights[j];
                    double old_value = RadarVoxelVolume[voxel_index];

                    if (std::isnan(old_value)) {
                        RadarVoxelVolume[voxel_index] = value;
                        sum_weight_n_minus_1[voxel_index] = weight;
                        continue;
                    }

                    double old_weight = sum_weight_n_minus_1[voxel_index];
                    
                    // TODO: Implement weighted average update here
                    // Similar to the simple average but weighted by 'weight'
                    RadarVoxelVolume[voxel_index] = (old_weight * old_value + weight * value) / (old_weight + weight);

                    //update the weight
                    sum_weight_n_minus_1[voxel_index] += weight;
                    
                    // if (i % 1000 == 0) {
                    //     std::cout << "voxel_index: " << voxel_index << " value: " << value << std::endl;
                    // }
                }
                //run a lil debug print every 1000 vals or so
            }
        }
    }
    delete[] sum_weight_n_minus_1;
    return RadarVoxelVolume;
}

