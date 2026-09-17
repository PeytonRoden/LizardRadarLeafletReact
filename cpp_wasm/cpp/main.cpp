
#include "nexrad_image_builder.h"
#include "voxel_interpolation.h"
#include "structs_and_constants.h"
#include <iostream>
#include <ctime>
#include <iomanip>
#include <fstream>
#include <vector>
#include <cstring>
#include <bzlib.h>
#include <cstdint>
#include <algorithm>  // for std::copy
#include <iterator>   // for std::ostream_iterator
#include <cstdlib>
#include <queue>
#include <thread>
#include <chrono>


#include <emscripten.h>
#include <emscripten/bind.h>
#include <emscripten/val.h>
#include <emscripten/wire.h>
#include <emscripten/emscripten.h>
#include <cmath>
#include <cstdint>
#include <vector>
#include <string>



//global variables that control stuff

//selected radar moment, default to relfectivity
char selected_radar_moment[4] = "REF";

int number_of_radar_moments = 6;
std::vector<std::string> radar_moment_names = {"REF", "VEL", "SW", "ZDR", "PHI", "RHO"};


//store tilt angles here
std::vector<float> tilt_angles;

std::vector<TiltInfo> tilt_info; //angle, timestamp
//store selected tilt index here
int tilt_number_for_data = 0;


float * moment_data_vertices_then_val = nullptr; //will point to [x1,y1,val, x2,y2,val, x3,y3,val, ...]
int moment_data_vertices_then_val_size = 0;


std::vector<uint8_t> png_buffer;
int MAX_NUM_VOXELS_PER_SIDE = 96;
int num_voxels_latitude = 96;
int num_voxels_longitude = 96;
int num_voxels_height = 96;

float* RadarVoxelVolume = nullptr;

float latitude_topleft;
float longitude_topleft;

float latitude_bottomright;
float longitude_bottomright;


//L_v(r) = Lv_0 + k_Lv * r
//L_h(r) = Lh_0 + k_Lh * r
// w_i = exp( - ( ((x-x_i)^2 + (y-y_i)^2 )/ (L_h^2)) + ((z-z_i)^2 / (L_v^2))  )
// Z(x) = sum_i( w_i * Z_i ) / sum_i( w_i )
// Lv_0/Lh_0 are in km, k_Lv/k_Lh are km of influence added per km of range.
float Lv_0 = 1.0f;
float Lh_0 = 1.5f;


float k_Lv = 0.00005f;
float k_Lh = 0.0001f;


float latlong_radius_0 = 5.0f;
float height_radius_0 = 10.0f;
float latlong_radius_scale = 1.0f;
float height_radius_scale = 1.0f;


AllTilt combined;

//temporary storage for packed radar data (dist, az, value), will migrate allTilt to use packed later
std::vector<float> packed_radar_data;


// --- Helpers to read big endian values from raw buffer ---

uint16_t read_be16(const uint8_t* p) {
    return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
}

uint32_t read_be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

int16_t read_be16s(const uint8_t* p) {
    uint16_t u = (uint16_t(p[0]) << 8) | p[1];
    return static_cast<int16_t>(u);
}

int32_t read_be32s(const uint8_t* p) {
    uint32_t u = (uint32_t(p[0]) << 24) |
                 (uint32_t(p[1]) << 16) |
                 (uint32_t(p[2]) << 8) |
                 p[3];
    return static_cast<int32_t>(u);
}


float read_be_float(const uint8_t* p) {
    uint32_t val = read_be32(p);
    float f;
    std::memcpy(&f, &val, sizeof(float));
    return f;
}


// Returns decompressed data or empty vector if failed
std::vector<uint8_t> decompress_bzip2_block(const uint8_t* input, size_t input_size) {
    const size_t output_max_size = 50000; // adjust as needed
    std::vector<uint8_t> output(output_max_size);
    unsigned int dest_len = static_cast<unsigned int>(output_max_size);

    int ret = BZ2_bzBuffToBuffDecompress(
        reinterpret_cast<char*>(output.data()), &dest_len,
        const_cast<char*>(reinterpret_cast<const char*>(input)), static_cast<unsigned int>(input_size),
        0, 0);

    if (ret != BZ_OK) {
        std::cerr << "BZip2 decompression failed with code: " << ret << std::endl;
        return {};
    }

    output.resize(dest_len);
    return output;
}



#include <vector>
#include <cstdint>

// Returns the file offsets of all BZip2 compressed blocks (indicated by 'BZh' + digit)
std::pair<std::vector<size_t>, std::vector<int>> find_bzip2_block_offsets(const std::vector<uint8_t>& data) {
    std::vector<size_t> offsets;
    std::vector<int> block_sizes;

    const size_t start_scan = 28;  // Skip volume header

    for (size_t i = start_scan; i + 4 < data.size(); ++i) {
        if (data[i] == 'B' && data[i + 1] == 'Z' && data[i + 2] == 'h') {
            if (data[i + 3] >= '1' && data[i + 3] <= '9') {
                offsets.push_back(i);
                int j = i-4;

                uint32_t control_word = (data[j] << 24) | (data[j+1] << 16) | (data[j+2] << 8) | data[j+3];
                int32_t signed_size = static_cast<int32_t>(control_word);
                size_t block_size = std::abs(signed_size);  // Absolute value (per ICD)

                block_sizes.push_back(block_size);

                i += block_size-5;  // optional: skip ahead to avoid overlapping matches
                //i += (block_size-5)-i;
            }
        }
    }

    return {offsets,block_sizes};
}

// --- Decompress a single bzip2 block ---
std::vector<uint8_t> decompress_bzip_block(const uint8_t* input, size_t size) {
    std::vector<uint8_t> output(50000); // Estimate max decompressed size
    unsigned int out_len = output.size();
    int ret = BZ2_bzBuffToBuffDecompress(reinterpret_cast<char*>(output.data()), &out_len,
                                         const_cast<char*>(reinterpret_cast<const char*>(input)),
                                         size, 0, 0);
    if (ret != BZ_OK) {
        std::cerr << "Bzip2 decompression failed with code " << ret << std::endl;
        return {};
    }
    output.resize(out_len);
    return output;
}



std::vector<uint8_t> decompress_bzip2_stream(const uint8_t* input, size_t input_size) {
    std::vector<uint8_t> output;
    const size_t CHUNK_SIZE = 4096;

    bz_stream strm{};
    strm.next_in = reinterpret_cast<char*>(const_cast<uint8_t*>(input));
    strm.avail_in = input_size;

    int ret = BZ2_bzDecompressInit(&strm, 0, 0);
    if (ret != BZ_OK) {
        std::cerr << "BZ2_bzDecompressInit failed: " << ret << std::endl;
        return {};
    }

    std::vector<uint8_t> out_chunk(CHUNK_SIZE);
    do {
        strm.next_out = reinterpret_cast<char*>(out_chunk.data());
        strm.avail_out = CHUNK_SIZE;

        ret = BZ2_bzDecompress(&strm);
        if (ret != BZ_OK && ret != BZ_STREAM_END) {
            std::cerr << "BZ2_bzDecompress failed: " << ret << std::endl;
            BZ2_bzDecompressEnd(&strm);
            return {};
        }

        size_t bytes_decompressed = CHUNK_SIZE - strm.avail_out;
        output.insert(output.end(), out_chunk.begin(), out_chunk.begin() + bytes_decompressed);
    } while (ret != BZ_STREAM_END && strm.avail_in > 0);

    BZ2_bzDecompressEnd(&strm);
    return output;
}


//Find all bzip2 blocks by scanning for 'B' 'Z' 'h' signature
std::vector<std::pair<size_t, size_t>> find_bzip2_blocks(const std::vector<uint8_t>& data) {
    std::vector<size_t> starts;
    size_t pos = 0;
    const size_t data_size = data.size();

    while (pos + 3 <= data_size) {
        if (data[pos] == 'B' && data[pos + 1] == 'Z' && data[pos + 2] == 'h') {
            starts.push_back(pos);
            pos += 3;  // Skip ahead after finding signature
        } else {
            ++pos;
        }
    }

    std::vector<std::pair<size_t, size_t>> blocks;
    for (size_t i = 0; i < starts.size(); ++i) {
        size_t block_start = starts[i];
        size_t block_end = (i + 1 < starts.size()) ? starts[i + 1] : data_size;
        size_t block_size = block_end - block_start;
        blocks.emplace_back(block_start, block_size);
    }
    return blocks;
}


// --- Read compression record and decompress if needed ---
std::vector<uint8_t> read_and_decompress(std::ifstream& file) {
    // Compression record is 12 bytes after volume header
    std::vector<uint8_t> comp_rec(COMPRESSION_RECORD_SIZE);
    file.read(reinterpret_cast<char*>(comp_rec.data()), COMPRESSION_RECORD_SIZE);

    // Check if BZ2 compressed
    if (comp_rec[CONTROL_WORD_SIZE] == 'B' && comp_rec[CONTROL_WORD_SIZE+1] == 'Z') {
        //std::cout << "Detected BZip2 compression\n";
        std::vector<uint8_t> decompressed;

        // Read each block and decompress
        std::vector<uint8_t> block(RECORD_SIZE);
        while (file.read(reinterpret_cast<char*>(block.data()), RECORD_SIZE)) {
            // The first 12 bytes in the block are a header (skip)
            const uint8_t* compressed_data = block.data() + COMPRESSION_RECORD_SIZE;
            size_t compressed_size = RECORD_SIZE - COMPRESSION_RECORD_SIZE;

            std::vector<uint8_t> dec = decompress_bzip_block(compressed_data, compressed_size);
            if (dec.empty()) {
                std::cerr << "Decompression failed on a block\n";
                break;
            }
            decompressed.insert(decompressed.end(), dec.begin(), dec.end());
        }
        return decompressed;

    } else if ((comp_rec[CONTROL_WORD_SIZE] == 0x00 && comp_rec[CONTROL_WORD_SIZE+1] == 0x00) ||
               (comp_rec[CONTROL_WORD_SIZE] == 0x09 && comp_rec[CONTROL_WORD_SIZE+1] == 0x80)) {
        // Uncompressed data follows
        //std::cout << "Data is uncompressed\n";
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), {});
    }

    throw std::runtime_error("Unknown compression format");
}



VOL_EL_RAD parse_vol_el_rad_blocks(const uint8_t* p_vol, const uint8_t* p_el, const uint8_t* p_rad){
    VOL_EL_RAD vol_el_rad;

    VOLUME_DATA_BLOCK vol;

    vol.block_type = *p_vol++; 
    std::memcpy(vol.data_name, p_vol, 3); p_vol += 3;
    vol.lrtup = read_be16(p_vol); p_vol+=2;
    vol.version_major = *p_vol++;               // 1 byte
    vol.version_minor = *p_vol++;               // 1 byte
    vol.lat = read_be_float(p_vol); p_vol+=4;
    vol.lon = read_be_float(p_vol); p_vol+=4;
    vol.height = read_be16s(p_vol); p_vol+=2;
    vol.feedhorn_height = read_be16(p_vol); p_vol+=2;
    vol.refl_calib = read_be_float(p_vol); p_vol+=4;
    vol.power_h = read_be_float(p_vol); p_vol+=4;
    vol.power_v = read_be_float(p_vol); p_vol+=4;
    vol.diff_refl_calib = read_be_float(p_vol); p_vol+=4;
    vol.init_phase = read_be_float(p_vol); p_vol+=4;
    vol.vcp = read_be16(p_vol); p_vol+=2;
    std::memcpy(vol.spare, p_vol, 2); p_vol += 2;

    vol_el_rad.vol = vol;
    // //std::cout << "lat: " << vol.lat << std::endl;
    // //std::cout << "lon: " << vol.lon << std::endl;

    
    ELEVATION_DATA_BLOCK el;

    el.block_type = *p_el++; 
    std::memcpy(el.data_name, p_el, 3); p_el += 3;
    el.lrtup = read_be16(p_el); p_el+=2;
    el.atmos = read_be16s(p_el); p_el+=2;
    el.refl_calib = read_be_float(p_el); p_el+=4;

    vol_el_rad.el = el;


    RADIAL_DATA_BLOCK rad;

    rad.block_type = *p_rad++; 
    std::memcpy(rad.data_name, p_rad, 3); p_rad += 3;
    rad.lrtup = read_be16(p_rad); p_rad+=2;
    rad.unambig_range = read_be16s(p_rad); p_rad+=2;
    rad.noise_h = read_be_float(p_rad); p_rad+=4;
    rad.noise_v = read_be_float(p_rad); p_rad+=4;
    rad.nyquist_vel = read_be16s(p_rad); p_rad+=2;
    std::memcpy(rad.spare, p_rad, 2); p_rad += 2;

    vol_el_rad.rad = rad;

    //rad.nyquist_vel = rad.nyquist_vel/100;

    // //std::cout << "block type: " << rad.block_type << std::endl;
    // //std::cout << "name: " << rad.data_name << std::endl;
    //std::cout << "nyquist vel: " << rad.nyquist_vel << std::endl;

    return vol_el_rad;
}


void parse_one_moment(AllTilt& alltilts, const uint8_t* ref_ptr , MSG_31& msg31, const uint8_t* msg31_ptr){


        GENERIC_DATA_BLOCK MOMENT;
        MOMENT.block_type = *ref_ptr++;
        std::memcpy(MOMENT.data_name, ref_ptr, 3); ref_ptr += 3;
        // Read the remaining fields using big-endian readers:
        MOMENT.reserved = read_be32(ref_ptr); ref_ptr += 4;
        MOMENT.gate_count = read_be16(ref_ptr); ref_ptr += 2;
        MOMENT.first_gate = read_be16s(ref_ptr); ref_ptr += 2;
        MOMENT.gate_spacing= read_be16s(ref_ptr); ref_ptr += 2;
        MOMENT.thresh = read_be16s(ref_ptr); ref_ptr += 2;
        MOMENT.snr_threshold= read_be16s(ref_ptr); ref_ptr += 2;
        MOMENT.flags = *ref_ptr++;
        MOMENT.word_size = *ref_ptr++;
        MOMENT.scale = read_be_float(ref_ptr); ref_ptr += 4;
        MOMENT.offset = read_be_float(ref_ptr); ref_ptr += 4;


        char moment_buf[4];                    // one extra for '\0'
        std::memcpy(moment_buf, MOMENT.data_name, 3);
        moment_buf[3] = '\0';    


        const uint8_t* data_ptr = ref_ptr;
        std::string moment_str; //store moment str 'REF', 'VEL' ...

        if (alltilts.Tilts.empty()) {
            // If alltilts isn't populated yet, start populating
            moment_str = std::string(moment_buf);

            SingleTilt tilt_0;
            tilt_0.ElevationAngle = msg31.elevation_angle;

            VOL_EL_RAD vol_el_rad = parse_vol_el_rad_blocks(msg31_ptr + msg31.block_pointer_1, msg31_ptr + msg31.block_pointer_2,msg31_ptr + msg31.block_pointer_3 );
            tilt_0.vol_el_rad = vol_el_rad    ;
            tilt_0.gateSpacing = MOMENT.gate_spacing;

            alltilts.Tilts.push_back(tilt_0);
        } else {
            // Check if we are at the same tilt or a different tilt
            SingleTilt& last = alltilts.Tilts.back();

            if (std::abs(last.ElevationAngle - msg31.elevation_angle) >= 0.3f) {
                // New tilt
                SingleTilt tilt_next;
                tilt_next.ElevationAngle = msg31.elevation_angle;
                VOL_EL_RAD vol_el_rad = parse_vol_el_rad_blocks(msg31_ptr + msg31.block_pointer_1,msg31_ptr + msg31.block_pointer_2,msg31_ptr + msg31.block_pointer_3 );
                tilt_next.vol_el_rad = vol_el_rad    ;
                tilt_next.gateSpacing = MOMENT.gate_spacing;

                alltilts.Tilts.push_back(tilt_next);
            }else{
                //update elevation angle to be avg instead:
                last.count +=1;
                last.ElevationAngle = ((last.count-1) * last.ElevationAngle + msg31.elevation_angle) / ((last.count));
            }
        }
        SingleTilt& current_tilt = alltilts.Tilts.back();

        // Store MSG_31 data, we only wanna store last one for the tilt
        current_tilt.msg_31 = msg31;

        //increment count
        current_tilt.count++;

        std::vector<float>* radials = get_moment_radials(current_tilt, moment_buf);
 

        if (radials->capacity() == 0) {
            radials->reserve(500000);
        }

        //store tilt information
        // i is gate number
        for (int i = 0; i < MOMENT.gate_count; ++i) {

            float moment_val = 0;
            //uint8_t raw_val = data_ptr[i];
            int raw_val = 0;
            
            if (MOMENT.word_size == 8) {
                raw_val = data_ptr[i];
            }
            else if (MOMENT.word_size == 16) {
                raw_val = read_be16((uint8_t*)(data_ptr + i*2));
            }
            else {
                //std::cout << "Unsupported word size: " << int(MOMENT.word_size) << std::endl;
                continue;
            }
            if (raw_val == 0) continue;

            moment_val = (raw_val - MOMENT.offset) / MOMENT.scale;

            float distance_m = i * MOMENT.gate_spacing; // gate_spacing in meters

            // RadialData point;
            // point.azimuth_deg = msg31.azimuth_angle;
            // point.dist = distance_m;
            // point.value = moment_val;

            // //std::cout << "point.azimuth_deg: "<< point.azimuth_deg << std::endl;
            // //std::cout << "point.dist: "<< point.dist << std::endl;
            // //std::cout << "point.value:  "<< point.value << std::endl;

            if (distance_m > current_tilt.maxDist) current_tilt.maxDist = distance_m;

            radials->push_back(msg31.azimuth_angle);
            radials->push_back(distance_m);
            radials->push_back(moment_val);
        }
}




// --- Parse header from decompressed LDM block ---
ArchiveIIMessageHeader parse_archive_ii_header(const uint8_t* p, bool first_message, AllTilt& alltilts) {

    if (first_message){
        p = p + 12;  // Skip 12-byte CTM padding
    }else{
        p = p + 12;  // Skip 12-byte CTM padding
    }

    ArchiveIIMessageHeader hdr;
    hdr.size              = read_be16(p);       p += 2;
    hdr.channels          = *p++;               // 1 byte
    hdr.type              = *p++;               // 1 byte
    hdr.seq_id            = read_be16(p);       p += 2;
    hdr.julian_date       = read_be16(p);       p += 2;
    hdr.ms_since_mid      = read_be32(p);       p += 4;
    hdr.segments          = read_be16(p);       p += 2;
    hdr.seg_num           = read_be16(p);       p += 2;


    // //std::cout << "in the archive message header II parsing function \n \n \n" << std::endl;



    if (int(hdr.type) == 31 ){
        ////parse out message 31s
        const uint8_t* msg31_ptr = p; // Start of the Message 31 (after ArchiveIIMessageHeader)
        
        MSG_31 msg31;
        std::memcpy(msg31.id, p, 4);       p += 4;
        //msg31.id[4] = '\0'; // manually null-terminate
        msg31.collect_ms = read_be32(p);   p+=4;           // 4-7: 4-byte unsigned integer
        msg31.collect_date = read_be16(p); p+=2;         // 8-9: 2-byte unsigned integer
        msg31.azimuth_number = read_be16(p); p+=2;       // 10-11: 2-byte unsigned integer
        msg31.azimuth_angle = read_be_float(p); p+=4;           // 12-15: 4-byte real number
        msg31.compress_flag = *p++;         // 16: 1-byte code
        msg31.spare_0 = *p++;               // 17: 1-byte integer
        msg31.radial_length = read_be16(p); p+=2;       // 18-19: 2-byte unsigned integer
        msg31.azimuth_resolution= *p++;;    // 20: 1-byte code
        msg31.radial_spacing= *p++;;        // 21: 1-byte code
        msg31.elevation_number= *p++;;      // 22: 1-byte unsigned integer
        msg31.cut_sector= *p++;;            // 23: 1-byte unsigned integer
        msg31.elevation_angle = read_be_float(p); p+=4;         // 24-27: 4-byte real number
        msg31.radial_blanking= *p++;;       // 28: 1-byte code
        msg31.azimuth_mode= *p++;;           // 29: 1-byte signed integer
        msg31.block_count = read_be16(p); p+=2;          // 30-31: 2-byte unsigned integer
        msg31.block_pointer_1 = read_be32(p); p += 4; // 32-35: Volume Data Constant XVII-E
        msg31.block_pointer_2 = read_be32(p); p += 4;// 36-39: Elevation Data Constant XVII-F
        msg31.block_pointer_3 = read_be32(p); p += 4;
        msg31.block_pointer_4 = read_be32(p); p += 4;
        msg31.block_pointer_5 = read_be32(p); p += 4;
        msg31.block_pointer_6 = read_be32(p); p += 4;
        msg31.block_pointer_7 = read_be32(p); p += 4;
        msg31.block_pointer_8 = read_be32(p); p += 4;
        msg31.block_pointer_9 = read_be32(p); p += 4;
        msg31.block_pointer_10 = read_be32(p); p += 4;


        //short for refrence pointer
        const uint8_t* ref_ptr_REF;// ref pointer to REF
        const uint8_t* ref_ptr_VEL;// ref pointer to VEL
        const uint8_t* ref_ptr_SW; // ref pointer to SW
        const uint8_t* ref_ptr_ZDR;// ref pointer to ZDR
        const uint8_t* ref_ptr_PHI;// ref pointer to PHI
        const uint8_t* ref_ptr_RHO;// ref pointer to RHO

        ref_ptr_REF =  msg31_ptr + msg31.block_pointer_4; 
        ref_ptr_VEL = msg31_ptr + msg31.block_pointer_5; 
        ref_ptr_SW =   msg31_ptr + msg31.block_pointer_6; 
        ref_ptr_ZDR =  msg31_ptr + msg31.block_pointer_7; 
        ref_ptr_PHI =  msg31_ptr + msg31.block_pointer_8; 
        ref_ptr_RHO =  msg31_ptr + msg31.block_pointer_9; 

        // //std::cout <<" header size: " << hdr.size << std::endl;
        // //std::cout << "ref ptr ref: " << ref_ptr_REF << std::endl; 

        if (msg31.block_pointer_4 != 0 && msg31.block_pointer_4 > 0 && msg31.block_pointer_4 < hdr.size *1.4) parse_one_moment(alltilts, ref_ptr_REF, msg31, msg31_ptr);
        if (msg31.block_pointer_5 != 0 && msg31.block_pointer_5 > 0 && msg31.block_pointer_5 < hdr.size *1.4) parse_one_moment(alltilts, ref_ptr_VEL, msg31, msg31_ptr);
        if (msg31.block_pointer_6 != 0 && msg31.block_pointer_6 > 0 && msg31.block_pointer_6 < hdr.size *1.4) parse_one_moment(alltilts, ref_ptr_SW, msg31, msg31_ptr);
        if (msg31.block_pointer_7 != 0 && msg31.block_pointer_7 > 0 && msg31.block_pointer_7 < hdr.size *1.4) parse_one_moment(alltilts, ref_ptr_ZDR, msg31, msg31_ptr);
        if (msg31.block_pointer_8 != 0 && msg31.block_pointer_8 > 0 && msg31.block_pointer_8 < hdr.size *1.4) parse_one_moment(alltilts, ref_ptr_PHI, msg31, msg31_ptr);
        if (msg31.block_pointer_9 != 0 && msg31.block_pointer_9 > 0 && msg31.block_pointer_9 < hdr.size *1.4) parse_one_moment(alltilts, ref_ptr_RHO, msg31, msg31_ptr);
        // if (msg31.block_pointer_4 != 0 && msg31.block_pointer_4 > 0) parse_one_moment(alltilts, ref_ptr_REF, msg31, msg31_ptr);
        // if (msg31.block_pointer_5 != 0 && msg31.block_pointer_5 > 0) parse_one_moment(alltilts, ref_ptr_VEL, msg31, msg31_ptr);
        // if (msg31.block_pointer_6 != 0 && msg31.block_pointer_6 > 0) parse_one_moment(alltilts, ref_ptr_SW, msg31, msg31_ptr);
        // if (msg31.block_pointer_7 != 0 && msg31.block_pointer_7 > 0) parse_one_moment(alltilts, ref_ptr_ZDR, msg31, msg31_ptr);
        // if (msg31.block_pointer_8 != 0 && msg31.block_pointer_8 > 0) parse_one_moment(alltilts, ref_ptr_PHI, msg31, msg31_ptr);
        // if (msg31.block_pointer_9 != 0 && msg31.block_pointer_9 > 0) parse_one_moment(alltilts, ref_ptr_RHO, msg31, msg31_ptr);



    }

    return hdr;
}


// --- Example usage ---
void process_ldm_block(const std::vector<uint8_t>& decompressed, AllTilt& alltilts) {
    size_t pos = 0;
    const size_t size = decompressed.size();
    ArchiveIIMessageHeader hdr;

    int counter = 0;

    int message31_counter = 0;
    (void)message31_counter;

    while (pos + 16 <= size) {  // 16 bytes = header size approx
        //std::vector<uint8_t> subvec(decompressed.begin() + pos, decompressed.end());
        if (counter == 0){
            hdr = parse_archive_ii_header(decompressed.data() + pos, true, alltilts);
        }else{
            hdr = parse_archive_ii_header(decompressed.data() + pos, false, alltilts);
        }
        //ArchiveIIMessageHeader hdr = parse_archive_ii_header(decompressed.data() + pos);

        //std::cout << "Type: " << int(hdr.type);
                //   << " | Size (halfwords): " << hdr.size
                //   << " | Seq: " << hdr.seq_id
                //   << " | Segment " << hdr.seg_num << " of " << hdr.segments << "\n";


        if (int(hdr.type) == 31 ){
            message31_counter ++;
        }
        if (int(hdr.type) == 0 ){
            //std::cout << "header type is 0: " << int(hdr.type) << std::endl;
            break;
        }
        //size_t bytes_to_advance = hdr.size * 2+ 12;  // halfwords to bytes

        size_t message_bytes = hdr.size * 2 + 12;  // include CTM padding bytes

        if (message_bytes == 12 || pos + message_bytes > size) {
            // size zero + padding only, or overrun
            break;
        }

        pos += message_bytes;
        counter++;
    }
    ///std::cout << "message 31 count: " << message31_counter << std::endl;
}



#include <iostream>
#include <iomanip>  // for std::setprecision

void printReflectivitySummary(const AllTilt& reflectivity_data) {
    //std::cout << "=== Reflectivity Data Summary ===\n";
    //std::cout << "Number of tilts: " << reflectivity_data.Tilts.size() << "\n\n";

    for (size_t t = 0; t < reflectivity_data.Tilts.size(); ++t) {
        const SingleTilt& tilt = reflectivity_data.Tilts[t];
        //std::cout << "Tilt " << t << " | Elevation: " << std::fixed << std::setprecision(2)
                 // << tilt.ElevationAngle << " deg\n";
        //std::cout << "  Number of radials: " << tilt.Radials_REF.size() << "\n";

        if (tilt.Radials_REF.empty()) {
            //std::cout << "  (No data)\n\n";
            continue;
        }

        // Print a few example radar points, e.g., 5 samples
        size_t step = std::max((size_t)1, tilt.Radials_REF.size() / 25);
        // for (size_t i = 0; i < tilt.Radials_REF.size(); i += step) {
        //     const float& pt = tilt.Radials_REF[i];
        //     //std::cout << "    Azimuth: " << std::fixed << std::setprecision(2) << pt.azimuth_deg
        //               << "°, Dist: " << pt.dist << " m, Value: " << pt.value << " dBZ\n";
        // }
        // //std::cout << "\n";
    }

    //std::cout << "=== End of Summary ===\n";
}





void unzip_process_ldm_worker(size_t i, const size_t* bz2_offsets_array, const int* bz2_block_sizes_array, const std::vector<uint8_t>& buffer, AllTilt& process_ldm_block_result){

    AllTilt all_tilt_data;

    size_t start = bz2_offsets_array[i];

    // Use a max chunk window; 1MB should be more than enough
    //size_t max_block_len = 1024 * 1024;  // 1MB
    size_t max_block_len = bz2_block_sizes_array[i];  // 1MB
    size_t remaining = buffer.size() - start;
    size_t chunk_len = std::min(max_block_len, remaining);

    const uint8_t* chunk_ptr = buffer.data() + start;

    // //std::cout << "Decompressing block " << i + 1
    //         << " at offset " << start << ", max chunk: " << chunk_len << std::endl;

    std::vector<uint8_t> decompressed = decompress_bzip2_stream(chunk_ptr, chunk_len);
    process_ldm_block(decompressed, all_tilt_data);


    if (decompressed.empty()) {
        std::cerr << "Decompression failed on block " << i + 1 << std::endl;
    }

    //store tilt data
    process_ldm_block_result = all_tilt_data;
}
AllTilt combine_all_tilts_from_thread_results(std::vector<AllTilt>& thread_results) {
    AllTilt combined;

    AllTilt pre_combined;
    for (auto& result : thread_results) {
        pre_combined.Tilts.insert(pre_combined.Tilts.end(), result.Tilts.begin(), result.Tilts.end());
        if (result.julian_date != 0 && pre_combined.julian_date == 0) {
            pre_combined.julian_date = result.julian_date;
        }
    }

    //std::cout << "Processing result with " << pre_combined.Tilts.size() << " tilts" << std::endl;

    auto append_radials = [](std::vector<float>& dst, const std::vector<float>& src) {
        dst.insert(dst.end(), src.begin(), src.end());
    };

    auto merge_tilt = [&](SingleTilt& dst, const SingleTilt& src) {
        int dst_count = std::max(dst.count, 1);
        int src_count = std::max(src.count, 1);
        dst.ElevationAngle = ((dst.ElevationAngle * dst_count) + (src.ElevationAngle * src_count)) / (dst_count + src_count);
        dst.count = dst_count + src_count;
        dst.maxDist = std::max(dst.maxDist, src.maxDist);
        if (dst.gateSpacing <= 0) dst.gateSpacing = src.gateSpacing;
        dst.msg_31 = src.msg_31;
        append_radials(dst.Radials_REF, src.Radials_REF);
        append_radials(dst.Radials_VEL, src.Radials_VEL);
        append_radials(dst.Radials_SW, src.Radials_SW);
        append_radials(dst.Radials_ZDR, src.Radials_ZDR);
        append_radials(dst.Radials_PHI, src.Radials_PHI);
        append_radials(dst.Radials_RHO, src.Radials_RHO);
    };

    combined.julian_date = pre_combined.julian_date;

    for (auto& tilt : pre_combined.Tilts) {
        bool merged = false;

        for (auto& combined_tilt : combined.Tilts) {
            if (std::fabs(tilt.ElevationAngle - combined_tilt.ElevationAngle) < 0.1f &&
                std::llabs(static_cast<long long>(tilt.msg_31.collect_ms) - static_cast<long long>(combined_tilt.msg_31.collect_ms)) < 30000) {
                //std::cout << "Combining tilts" << std::endl;
                //std::cout << "tilt 1 elevation angle: " << combined_tilt.ElevationAngle << std::endl;
                //std::cout << "tilt 2 elevation angle: " << tilt.ElevationAngle << std::endl;
                //std::cout << "tilt 1 time: " << combined_tilt.msg_31.collect_ms << std::endl;
                //std::cout << "tilt 2 time: " << tilt.msg_31.collect_ms << std::endl;
                merge_tilt(combined_tilt, tilt);
                merged = true;
                break;
            }
        }

        if (!merged) {
            combined.Tilts.push_back(tilt);
        }
    }

    std::sort(combined.Tilts.begin(), combined.Tilts.end(), [](const SingleTilt& a, const SingleTilt& b) {
        return a.ElevationAngle < b.ElevationAngle;
    });



    //clear old tilt info
    tilt_info.clear();

    for (auto& tilt : combined.Tilts) {
        //std::cout << "Combined tilt elevation angle: " << tilt.ElevationAngle << std::endl;

        time_t timestamp = tilt.msg_31.collect_date * 86400 + tilt.msg_31.collect_ms / 1000;

        struct tm *tm = gmtime(&timestamp);

        char buffer[32];
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H-%M-%S", tm);

        //std::cout << buffer
                // << "." << std::setfill('0') << std::setw(3)
                // << (tilt.msg_31.collect_ms % 1000)
                // << std::endl;

        tilt_info.push_back({tilt.ElevationAngle, std::string(buffer)});


    }

    // sort tilt_info by tilt angle
    std::sort(tilt_info.begin(), tilt_info.end(), [](const TiltInfo& a, const TiltInfo& b) {
        return a.tilt < b.tilt;
    });

    return combined;
}


void set_voxel_dimensions(float latitude_topleft, float longitude_topleft, float latitude_bottomright, float longitude_bottomright) {
    // calculate aspect ratio
    bool latitude_range_larger = std::abs(latitude_bottomright - latitude_topleft) > std::abs(longitude_bottomright - longitude_topleft);
    
    if (latitude_range_larger) {
        num_voxels_latitude = static_cast<int>(MAX_NUM_VOXELS_PER_SIDE);
        num_voxels_longitude = static_cast<int>(MAX_NUM_VOXELS_PER_SIDE * (std::abs(longitude_bottomright - longitude_topleft)) / (std::abs(latitude_bottomright - latitude_topleft)));
    } else {
        num_voxels_latitude = static_cast<int>(MAX_NUM_VOXELS_PER_SIDE * (std::abs(latitude_bottomright - latitude_topleft)) / (std::abs(longitude_bottomright - longitude_topleft)));
        num_voxels_longitude = static_cast<int>(MAX_NUM_VOXELS_PER_SIDE);
    }

    //make sure both are at least 1 or more
    if (num_voxels_latitude < 1) {
        num_voxels_latitude = 1;
    }
    if (num_voxels_longitude < 1) {
        num_voxels_longitude = 1;
    }

    // always use max for height
    num_voxels_height = MAX_NUM_VOXELS_PER_SIDE;

}



extern "C" {

    EMSCRIPTEN_KEEPALIVE
    int get_tilt_count() {
        return tilt_info.size();
    }


    EMSCRIPTEN_KEEPALIVE
    float get_tilt_angle(int index) {
        if (index < 0 || index >= static_cast<int>(tilt_info.size())) {
            return 0.0f;
        }
        return tilt_info[index].tilt;
    }

    EMSCRIPTEN_KEEPALIVE
    const char* get_tilt_name(int index) {
        if (index < 0 || index >= static_cast<int>(tilt_info.size())) {
            return "";
        }
        return tilt_info[index].name.c_str();
    }

    EMSCRIPTEN_KEEPALIVE
    void set_tilt_index(int index) {
        if (index < 0 || index >= static_cast<int>(tilt_info.size())) {
            return;
        }
        tilt_number_for_data = index;
    }

    EMSCRIPTEN_KEEPALIVE
    const float * get_tilt_angles() {
        return tilt_angles.data();
    }

    EMSCRIPTEN_KEEPALIVE
    int get_tilt_angles_size() {
        return tilt_angles.size();
        //return sizeof(tilt_angles) / sizeof(tilt_angles[0]);
    }

    EMSCRIPTEN_KEEPALIVE
    int get_number_of_radar_moments() {
        return number_of_radar_moments;
    }

    EMSCRIPTEN_KEEPALIVE
    const char* get_radar_moment_name(int index) {
        if (index < 0 || index >= number_of_radar_moments) {
            return "";
        }
        return radar_moment_names[index].c_str();
    }


    EMSCRIPTEN_KEEPALIVE
    void set_selected_radar_moment(const char* moment) {
        std::memset(selected_radar_moment, ' ', 3);
        selected_radar_moment[3] = '\0';

        if (moment == nullptr) {
            return;
        }

        std::strncpy(selected_radar_moment, moment, std::min<size_t>(std::strlen(moment), 3));
    }

    EMSCRIPTEN_KEEPALIVE
    const uint8_t* get_png_data() {
        if (combined.Tilts.empty() || tilt_number_for_data < 0 || tilt_number_for_data >= static_cast<int>(combined.Tilts.size())) {
            png_buffer.clear();
            return png_buffer.data();
        }
        png_buffer = saveTiltAsPNGInterpolate2(combined.Tilts[tilt_number_for_data], "test.png");
        return png_buffer.data();
    }
    
    EMSCRIPTEN_KEEPALIVE
    int get_png_data_size() {
        return png_buffer.size();
    }

    EMSCRIPTEN_KEEPALIVE
    float* get_moment_data() {

        if (combined.Tilts.empty() || tilt_number_for_data < 0 || tilt_number_for_data >= static_cast<int>(combined.Tilts.size())) {
            moment_data_vertices_then_val_size = 0;
            return nullptr;
        }

        delete[] moment_data_vertices_then_val;
        moment_data_vertices_then_val = buildMomentDataVerticesThenValue(
            combined.Tilts[tilt_number_for_data], &moment_data_vertices_then_val_size);

        return moment_data_vertices_then_val;
    }

    EMSCRIPTEN_KEEPALIVE
    int get_moment_data_vertices_then_val_size() {
        return moment_data_vertices_then_val_size;
    }



    EMSCRIPTEN_KEEPALIVE
    const float * get_packed_radar_data() {
        if (combined.Tilts.empty() || tilt_number_for_data < 0 ||
            tilt_number_for_data >= static_cast<int>(combined.Tilts.size())) {
            packed_radar_data.clear();
            return packed_radar_data.data();
        }

        packed_radar_data = new_shader_vals(combined.Tilts[tilt_number_for_data]);

        return packed_radar_data.data();
    }

    EMSCRIPTEN_KEEPALIVE
    int get_packed_radar_data_size() {
        return packed_radar_data.size();
    }

    EMSCRIPTEN_KEEPALIVE
    float get_current_tilt_angle() {
        if (combined.Tilts.empty() || tilt_number_for_data < 0 ||
            tilt_number_for_data >= static_cast<int>(combined.Tilts.size())) {
            return 0.0f;
        }
        return combined.Tilts[tilt_number_for_data].ElevationAngle;
    }

    EMSCRIPTEN_KEEPALIVE
    float get_current_radar_station_latitude() {
        if (combined.Tilts.empty() || tilt_number_for_data < 0 ||
            tilt_number_for_data >= static_cast<int>(combined.Tilts.size())) {
            return 0.0f;
        }
        return combined.Tilts[tilt_number_for_data].vol_el_rad.vol.lat;
    }

    EMSCRIPTEN_KEEPALIVE
    float get_current_radar_station_longitude() {
        if (combined.Tilts.empty() || tilt_number_for_data < 0 ||
            tilt_number_for_data >= static_cast<int>(combined.Tilts.size())) {
            return 0.0f;
        }
        return combined.Tilts[tilt_number_for_data].vol_el_rad.vol.lon;
    }

    EMSCRIPTEN_KEEPALIVE
    void set_latitude_topleft(float lat) {
        latitude_topleft = lat;
    }

    EMSCRIPTEN_KEEPALIVE
    void set_longitude_topleft(float lon) {
        longitude_topleft = lon;
    }

    EMSCRIPTEN_KEEPALIVE
    void set_latitude_bottomright(float lat) {
        latitude_bottomright = lat;
    }

    EMSCRIPTEN_KEEPALIVE
    void set_longitude_bottomright(float lon) {
        longitude_bottomright = lon;
    }

    EMSCRIPTEN_KEEPALIVE
    float get_latitude_topleft() {
        return latitude_topleft;
    }

    EMSCRIPTEN_KEEPALIVE
    float get_longitude_topleft() {
        return longitude_topleft;
    }

    EMSCRIPTEN_KEEPALIVE
    float get_latitude_bottomright() {
        return latitude_bottomright;
    }

    EMSCRIPTEN_KEEPALIVE
    float get_longitude_bottomright() {
        return longitude_bottomright;
    }

    EMSCRIPTEN_KEEPALIVE
    void set_max_num_voxels(int max_voxels) {

        //make sure max_voxels is at least 1
        if (max_voxels < 1) {
            max_voxels = 1;
        } else if (max_voxels > 500) {
            max_voxels = 500;
        }
        MAX_NUM_VOXELS_PER_SIDE = max_voxels;
    }

    EMSCRIPTEN_KEEPALIVE
    int get_interpolated_voxels_size_latitude() {
        return num_voxels_latitude;
    }

    EMSCRIPTEN_KEEPALIVE
    int get_interpolated_voxels_size_longitude() {
        return num_voxels_longitude;
    }

    EMSCRIPTEN_KEEPALIVE
    int get_interpolated_voxels_height() {
        return num_voxels_height;
    }
    
    EMSCRIPTEN_KEEPALIVE
    void populate_voxel_grid() {
        if (combined.Tilts.empty() || tilt_number_for_data < 0 ||
            tilt_number_for_data >= static_cast<int>(combined.Tilts.size())) {
            return;
        }

        // set number voxels for lat, lng, height
        set_voxel_dimensions(latitude_topleft, longitude_topleft, latitude_bottomright, longitude_bottomright);


        interpolate_radar_data_to_voxels_new(
            latitude_topleft,
            longitude_topleft,
            latitude_bottomright,
            longitude_bottomright,
            num_voxels_latitude,
            num_voxels_longitude,
            num_voxels_height
        );

        return;

        // return RadarVoxelVolume;
    }

    EMSCRIPTEN_KEEPALIVE
    float * get_voxel_grid() {
        if (combined.Tilts.empty() || tilt_number_for_data < 0 ||
            tilt_number_for_data >= static_cast<int>(combined.Tilts.size())) {
            return nullptr;
        }
        return RadarVoxelVolume;
    }

    EMSCRIPTEN_KEEPALIVE
    void release_voxel_grid() {
        delete[] RadarVoxelVolume;
        RadarVoxelVolume = nullptr;
    }
  
    EMSCRIPTEN_KEEPALIVE
    int parse_nexrad(uint8_t* data, int length) {

        //std::cout << "=== NEXRAD Level II Parser (AR2V Format) ===" << std::endl;
        //std::cout << "File length: " << length << std::endl;
        
        if (length < 24) {
            //std::cout << "File too small" << std::endl;
            return -1;
        }

        //initialize all tilt angles as -1, these get reupdated each time a new icao gets run
        // for(int i = 0; i< 50; i++){
        //     tilt_angles[i] = -1;
        // }

        //clear tilt angles
        tilt_angles.clear();

        
        //uint8_t* data_ptr = buffer.data();
        std::vector<uint8_t> buffer(data, data + length);

        uint8_t* data_ptr = data;
        VolumeHeader vol_header;
        std::memcpy(vol_header.tape, data_ptr, 9); data_ptr += 9;
        std::memcpy(vol_header.extension, data_ptr, 3); data_ptr += 3;
        vol_header.date = read_be32(data_ptr); data_ptr += 4;
        vol_header.time = read_be32(data_ptr); data_ptr += 4;
        std::memcpy(vol_header.icao, data_ptr, 4); data_ptr += 4;


        //std::cout << "Tape: '" << vol_header.tape << "'\n";
        //std::cout << "Extension: '" << vol_header.extension << "'\n";
        //std::cout << "Date: " << vol_header.date << "\n";
        //std::cout << "Time: " << vol_header.time << "\n";
        //std::cout << "ICAO: '" << vol_header.icao << "'\n";

        //std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(file)), {});


        // AllTilt reflectivity_data;

        //std::vector<size_t> bz2_offsets,  = find_bzip2_block_offsets(buffer);  // already implemented

        auto [bz2_offsets, bz2_block_sizes] = find_bzip2_block_offsets(buffer); 
        
        std::vector<std::thread> threads_to_process_blocks;
        std::vector<AllTilt> process_ldm_blocks_results(bz2_offsets.size());
        constexpr size_t MAX_THREADS = 8;

        

        // std::copy(bz2_offsets.begin(), bz2_offsets.end(), std::ostream_iterator<int>(std::cout, " "));
        // //std::cout << "erm" << std::endl;
        // std::copy(bz2_block_sizes.begin(), bz2_block_sizes.end(), std::ostream_iterator<int>(std::cout, " "));

        for (size_t i = 0; i < bz2_offsets.size(); ++i) {
            threads_to_process_blocks.emplace_back(
                unzip_process_ldm_worker,
                i,
                bz2_offsets.data(),
                bz2_block_sizes.data(),
                std::cref(buffer),
                std::ref(process_ldm_blocks_results[i])
            );

            // Wait for current batch to finish when limit is reached
            if (threads_to_process_blocks.size() == MAX_THREADS || i == bz2_offsets.size() - 1) {
                for (auto& t : threads_to_process_blocks) {
                    if (t.joinable()) t.join();
                }
                threads_to_process_blocks.clear(); // Empty the vector for the next batch

            }

        }

        // //std::cout << "Processed " << process_ldm_blocks_results.size() << " blocks" << std::endl;
        // for (auto& result : process_ldm_blocks_results) {
        //     // loop through tilts and print elevation angles
        //     for (auto& tilt: result.Tilts) {
        //         //std::cout << "Elevation angle: " << tilt.ElevationAngle << std::endl;
        //     }
        // }

        combined = combine_all_tilts_from_thread_results(process_ldm_blocks_results);
        tilt_number_for_data = 0;

        for (auto& tilt : combined.Tilts) {
            // do something with each tilt
            //std::cout << "Processing tilt at elevation " << tilt.ElevationAngle << "\n";
            tilt_angles.push_back(tilt.ElevationAngle);

            //std::cout << "Tilt count: " << tilt.count << std::endl;
            //std::cout << "\n\n\n\n\n\n\n\n\n" << std::endl;
        }



        return 0;
    }
}

