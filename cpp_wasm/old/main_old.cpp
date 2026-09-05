#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include <bzlib.h>
#include <cstdint>
#include <algorithm>  // for std::copy
#include <iterator>   // for std::ostream_iterator
#include <cstdlib>
#include <queue>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <emscripten.h>
#include "stb_image_write.h"
#include <cmath>
#include <cstdint>
#include <vector>
#include <string>

// --- Constants ---
constexpr size_t RECORD_SIZE = 2432;
constexpr size_t COMPRESSION_RECORD_SIZE = 12;
constexpr size_t CONTROL_WORD_SIZE = 4;


//global variables that control stuff

//selected radar moment, default to relfectivity
char selected_radar_moment[4] = "REF";

//store tilt angles here
float tilt_angles[50];
//store selected tilt index here
int tilt_number_for_data = 0;


bool cStringsEqual(const char* str1, const char* str2) {
    return std::strcmp(str1, str2) == 0;
}

std::vector<uint8_t> png_buffer;
float latitude_topleft;
float longitude_topleft;

float latitude_bottomright;
float longitude_bottomright;

//hi

// --- Structs based on Py-ART format descriptions ---

#pragma pack(push, 1)  // Disable padding
struct VolumeHeader {
    char tape[9];        // 9s
    char extension[3];   // 3s
    uint32_t date;       // I (uint32_t)
    uint32_t time;       // I (uint32_t)
    char icao[4];        // 4s
} __attribute__((packed));

struct MessageHeader {
    uint16_t size;       // INT2
    uint8_t channels;    // INT1
    uint8_t type;        // INT1
    uint16_t seq_id;     // INT2
    uint16_t date;       // INT2
    uint32_t ms;         // INT4
    uint16_t segments;   // INT2
    uint16_t seg_num;    // INT2
} __attribute__((packed));


struct ArchiveIIMessageHeader {
    int16_t size;         // size of the message in halfwords (excluding header)
    uint8_t channels;     // channel
    uint8_t type;         // message type
    int16_t seq_id;       // sequence number
    int16_t julian_date;  // modified Julian date
    int32_t ms_since_mid; // milliseconds since midnight
    int16_t segments;     // number of segments (for segmented messages)
    int16_t seg_num;      // segment number (for segmented messages)
};



struct MSG_31 {
    char id[4];                    // 0-3: 4-character string
    uint32_t collect_ms;           // 4-7: 4-byte unsigned integer
    uint16_t collect_date;         // 8-9: 2-byte unsigned integer
    uint16_t azimuth_number;       // 10-11: 2-byte unsigned integer
    float azimuth_angle;           // 12-15: 4-byte real number
    uint8_t compress_flag;         // 16: 1-byte code
    uint8_t spare_0;               // 17: 1-byte integer
    uint16_t radial_length;        // 18-19: 2-byte unsigned integer
    uint8_t azimuth_resolution;    // 20: 1-byte code
    uint8_t radial_spacing;        // 21: 1-byte code
    uint8_t elevation_number;      // 22: 1-byte unsigned integer
    uint8_t cut_sector;            // 23: 1-byte unsigned integer
    float elevation_angle;         // 24-27: 4-byte real number
    uint8_t radial_blanking;       // 28: 1-byte code
    int8_t azimuth_mode;           // 29: 1-byte signed integer
    uint16_t block_count;          // 30-31: 2-byte unsigned integer
    uint32_t block_pointer_1;      // 32-35: Volume Data Constant XVII-E
    uint32_t block_pointer_2;      // 36-39: Elevation Data Constant XVII-F
    uint32_t block_pointer_3;      // 40-43: Radial Data Constant XVII-H
    uint32_t block_pointer_4;      // 44-47: Moment "REF" XVII-{B/I}
    uint32_t block_pointer_5;      // 48-51: Moment "VEL"
    uint32_t block_pointer_6;      // 52-55: Moment "SW"
    uint32_t block_pointer_7;      // 56-59: Moment "ZDR"
    uint32_t block_pointer_8;      // 60-63: Moment "PHI"
    uint32_t block_pointer_9;      // 64-67: Moment "RHO"
    uint32_t block_pointer_10;     // Moment "CFP"
};


// Table XVII-B Data Block (Descriptor of Generic Data Moment Type)
// pages 3-90 and 3-91
struct GENERIC_DATA_BLOCK {
    char block_type;              // 1 byte, e.g. 'D'
    char data_name[3];            // 3 bytes, e.g. "REF", no null terminator
    uint32_t reserved;            // 4 bytes
    uint16_t gate_count;          // ngates
    int16_t first_gate;           // signed 2 bytes, meters
    int16_t gate_spacing;         // signed 2 bytes, meters
    int16_t thresh;               // signed 2 bytes
    int16_t snr_threshold;        // signed 2 bytes
    uint8_t flags;                // 1 byte
    uint8_t word_size;            // 1 byte (bits per data word)
    float scale;                  // 4 bytes float
    float offset;                 // 4 bytes float
    // Data bytes follow here, size = gate_count * word_size/8
};


// Table XVII-E Data Block (Volume Data Constant Type)
// page 3-92
struct VOLUME_DATA_BLOCK {
    char block_type;               // 1-byte block type
    char data_name[3];             // 3-byte data name
    uint16_t lrtup;                // 2-byte LRTUP
    uint8_t version_major;         // 1-byte major version
    uint8_t version_minor;         // 1-byte minor version
    float lat;                     // 4-byte latitude
    float lon;                     // 4-byte longitude
    int16_t height;                // 2-byte signed height
    uint16_t feedhorn_height;      // 2-byte feedhorn height
    float refl_calib;              // 4-byte reflectivity calibration
    float power_h;                 // 4-byte horizontal power
    float power_v;                 // 4-byte vertical power
    float diff_refl_calib;         // 4-byte differential reflectivity calibration
    float init_phase;              // 4-byte initial phase
    uint16_t vcp;                  // 2-byte VCP
    char spare[2];                 // 2-byte spare
};

// Table XVII-F Data Block (Elevation Data Constant Type)
// page 3-93
struct ELEVATION_DATA_BLOCK {
    char block_type;               // 1-byte block type
    char data_name[3];             // 3-byte data name
    uint16_t lrtup;                // 2-byte LRTUP
    int16_t atmos;                 // 2-byte signed atmospheric
    float refl_calib;              // 4-byte reflectivity calibration
};

// Table XVII-H Data Block (Radial Data Constant Type)
// page 3-93
struct RADIAL_DATA_BLOCK {
    char block_type;               // 1-byte block type
    char data_name[3];             // 3-byte data name
    uint16_t lrtup;                // 2-byte LRTUP
    int16_t unambig_range;         // 2-byte signed unambiguous range
    float noise_h;                 // 4-byte horizontal noise
    float noise_v;                 // 4-byte vertical noise
    int16_t nyquist_vel;           // 2-byte signed Nyquist velocity
    char spare[2];                 // 2-byte spare
};


struct VOL_EL_RAD{
    VOLUME_DATA_BLOCK vol;
    ELEVATION_DATA_BLOCK el;
    RADIAL_DATA_BLOCK rad;
};

struct RadialData {
    float azimuth_deg;
    float dist;     // gate_index * gate_spacing
    float value;    // e.g., dBZ, velocity, spectrum width
};

struct SingleTilt {
    float ElevationAngle;               // Nominal tilt angle
    int count = 0;
    float maxDist = 0;  //maximum distance in m of this tilt.
    float gateSpacing = 250.0f;
    VOL_EL_RAD vol_el_rad;   //save the first one from each tilt!!!
    std::vector<RadialData> Radials;    // All radials for this tilt
};

struct AllTilt {
    std::string type;                   // e.g., "REF", "VEL", "SW"
    std::vector<SingleTilt> Tilts;      // One entry per tilt/elevation
};

#pragma pack(pop)

constexpr double PI = 3.14159265358979323846;

inline double deg2rad(double degrees) {
    return degrees * (PI / 180.0);
}
inline double rad2deg(double radians) {
    return radians * (180.0 / PI);
}

inline float fold_velocity(float v, float VNYQ) {
    float range = 2.0f * VNYQ;
    v = std::fmod(v + VNYQ, range);
    if (v < 0) v += range;       // wrap negatives
    return v - VNYQ;
}


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


#include <array>
#include <stdexcept>

class AtmosphericRayTracer {
public:
    AtmosphericRayTracer(double earth_radius = 6371000.0,
                         double N0 = 315.0,
                         double h0 = 8500.0)
        : earth_radius(earth_radius), N0(N0), h0(h0) {}

    // Calculate refractive index at height h
    double refractive_index(double h) const {
        double N = N0 * std::exp(-h / h0);
        return 1.0 + N / 1e6;
    }

    // Calculate dn/dh at height h
    double refractive_index_gradient(double h) const {
        double N = N0 * std::exp(-h / h0);
        double dN_dh = -N / h0;
        return dN_dh / 1e6;
    }

    // Ray equations for RK integration
    std::array<double, 4> ray_equations(double t,
                                        const std::array<double, 4>& state) const {
        double x = state[0];
        double y = state[1];
        double px = state[2];
        double py = state[3];

        // Current height above Earth surface
        double r = std::sqrt(x * x + y * y);
        double h = r - earth_radius;
        if (h < 0) h = 0;

        // Current refractive index and gradient
        double n = refractive_index(h);
        double dn_dh = refractive_index_gradient(h);

        // Unit vector pointing radially outward
        double grad_n_x = 0.0;
        double grad_n_y = 0.0;
        if (r > 0) {
            grad_n_x = dn_dh * x / r;
            grad_n_y = dn_dh * y / r;
        } else {
            grad_n_y = dn_dh;
        }

        // Ray equations
        double dx_ds = px / n;
        double dy_ds = py / n;
        double dpx_ds = grad_n_x;
        double dpy_ds = grad_n_y;

        return {dx_ds, dy_ds, dpx_ds, dpy_ds};
    }

    // Arc angle between two vectors from circle center
    double arc_angle(double center_x, double center_y,
                     double start_x, double start_y,
                     double last_x, double last_y) const {
        double ux = start_x - center_x;
        double uy = start_y - center_y;
        double vx = last_x - center_x;
        double vy = last_y - center_y;

        double u_norm = std::sqrt(ux * ux + uy * uy);
        double v_norm = std::sqrt(vx * vx + vy * vy);

        if (u_norm == 0.0 || v_norm == 0.0) {
            throw std::runtime_error("One point coincides with the circle center.");
        }

        double dot = ux * vx + uy * vy;
        double cos_theta = dot / (u_norm * v_norm);

        // Clamp to [-1, 1]
        if (cos_theta > 1.0) cos_theta = 1.0;
        if (cos_theta < -1.0) cos_theta = -1.0;

        return std::acos(cos_theta); // radians
    }

    // Main ray tracing function
    float get_earth_arc_angle_station_to_beam_end(
        double start_x, double start_y,
        double start_angle_rad,
        double max_distance = 400000.0,
        double step_size = 100.0) const
    {
        // Initial position
        double x0 = start_x;
        double y0 = start_y;
        double h0 = std::sqrt(x0 * x0 + y0 * y0) - earth_radius;
        double n0 = refractive_index(h0);

        // Initial ray parameter
        double px0 = n0 * std::cos(start_angle_rad);
        double py0 = n0 * std::sin(start_angle_rad);

        std::array<double, 4> state = {x0, y0, px0, py0};

        double s = 0.0;
        double last_x = x0;
        double last_y = y0;

        while (s < max_distance) {
            auto k1 = scale(step_size, ray_equations(s, state));
            auto k2 = scale(step_size, ray_equations(s + step_size / 2.0, add(state, scale(0.5, k1))));
            auto k3 = scale(step_size, ray_equations(s + step_size / 2.0, add(state, scale(0.5, k2))));
            auto k4 = scale(step_size, ray_equations(s + step_size, add(state, k3)));

            state = add(state, scale(1.0 / 6.0, add(add(k1, scale(2.0, k2)), add(scale(2.0, k3), k4))));
            s += step_size;

            last_x = state[0];
            last_y = state[1];

            // Check if ray hits Earth surface
            double r = std::sqrt(last_x * last_x + last_y * last_y);
            if (r <= earth_radius) {
                break;
            }
        }

        double theta = arc_angle(0, 0, start_x, start_y, last_x, last_y);
        std::cout << "Arc angle (rad): " << theta << "\n";

        return theta;
    }

    double earth_radius;
    double N0;
    double h0;

    // Vector helpers
    std::array<double, 4> add(const std::array<double, 4>& a,
                              const std::array<double, 4>& b) const {
        return {a[0] + b[0], a[1] + b[1], a[2] + b[2], a[3] + b[3]};
    }

    std::array<double, 4> scale(double s,
                                const std::array<double, 4>& a) const {
        return {s * a[0], s * a[1], s * a[2], s * a[3]};
    }
};


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


// Decompress one bzip2 block (input: compressed data and size)
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
        std::cout << "Detected BZip2 compression\n";
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
               (comp_rec[CONTROL_WORD_SIZE] == 0x09 && comp_rec[CONTROL_WORD_SIZE+1] == (char)0x80)) {
        // Uncompressed data follows
        std::cout << "Data is uncompressed\n";
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
    // std::cout << "lat: " << vol.lat << std::endl;
    // std::cout << "lon: " << vol.lon << std::endl;

    
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

    // std::cout << "block type: " << rad.block_type << std::endl;
    // std::cout << "name: " << rad.data_name << std::endl;
    //std::cout << "nyquist vel: " << rad.nyquist_vel << std::endl;



    return vol_el_rad;
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
    hdr.ms_since_mid = read_be32(p);       p += 4;
    hdr.segments          = read_be16(p);       p += 2;
    hdr.seg_num       = read_be16(p);       p += 2;


    // std::cout << "in the archive message header II parsing function \n \n \n" << std::endl;



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
        const uint8_t* ref_ptr;// Start of the Message 31 (after ArchiveIIMessageHeader)


        if (cStringsEqual(selected_radar_moment, "REF")) {
            ref_ptr =  msg31_ptr + msg31.block_pointer_4; 
        }
        else if (cStringsEqual(selected_radar_moment, "VEL")) {
            ref_ptr = msg31_ptr + msg31.block_pointer_5; 
        }
        else if (cStringsEqual(selected_radar_moment, "SW ")) {
            ref_ptr =  msg31_ptr + msg31.block_pointer_6; 
        }
        else if (cStringsEqual(selected_radar_moment, "ZDR")) {
            ref_ptr =  msg31_ptr + msg31.block_pointer_7; 
        }
        else if (cStringsEqual(selected_radar_moment, "PHI")) {
            ref_ptr =  msg31_ptr + msg31.block_pointer_8; 
        }
        else if (cStringsEqual(selected_radar_moment, "RHO")) {
            ref_ptr =  msg31_ptr + msg31.block_pointer_9; 
        }
        else {
            std::cout << "this shouldnt run " << std::endl;
            ref_ptr =  msg31_ptr + msg31.block_pointer_4; 
        }



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

        // std::cout << "moment name: " << moment_buf << std::endl;
        // std::cout << "selected radar moment: "<< selected_radar_moment <<std::endl;
        if (!cStringsEqual(selected_radar_moment, moment_buf)){
            // some sort of error in parsing going on, just gonnna skip those data
            return hdr;
        }
        // std::cout << "moment name: " << MOMENT.data_name << std::endl;
        // std::cout << "gate count:  " << MOMENT.gate_count << std::endl;
        // std::cout << "gate spacing:  " << MOMENT.gate_spacing << std::endl;
        // std::cout << "SCALE:  " << MOMENT.scale << std::endl;
        // std::cout << "offset:  " << MOMENT.offset << std::endl;
        //std::cout << "word size: " << static_cast<int>(MOMENT.word_size) << std::endl;



        const uint8_t* data_ptr = ref_ptr;

        if (alltilts.type.empty()) {
            // If alltilts isn't populated yet, start populating
            alltilts.type = std::string(selected_radar_moment);

            SingleTilt tilt_0;
            tilt_0.ElevationAngle = msg31.elevation_angle;

            VOL_EL_RAD vol_el_rad = parse_vol_el_rad_blocks(msg31_ptr + msg31.block_pointer_1,msg31_ptr + msg31.block_pointer_2,msg31_ptr + msg31.block_pointer_3 );
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
        current_tilt.Radials.reserve(500000);

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
                std::cout << "Unsupported word size: " << int(MOMENT.word_size) << std::endl;
                continue;
            }
            if (raw_val == 0) continue;

            moment_val = (raw_val - MOMENT.offset) / MOMENT.scale;

            if (cStringsEqual(selected_radar_moment, "VEL")) {
                moment_val = fold_velocity(moment_val, current_tilt.vol_el_rad.rad.nyquist_vel);
                //std::cout << moment_val << ", ";
            }

            float distance_m = i * MOMENT.gate_spacing; // gate_spacing in meters

            RadialData point;
            point.azimuth_deg = msg31.azimuth_angle;
            point.dist = distance_m;
            point.value = moment_val;

            if (distance_m > current_tilt.maxDist) current_tilt.maxDist = distance_m;

            current_tilt.Radials.push_back(point);
        }
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
            std::cout << "errorroroorr" << std::endl;
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
    std::cout << "=== Reflectivity Data Summary ===\n";
    std::cout << "Type: " << reflectivity_data.type << "\n";
    std::cout << "Number of tilts: " << reflectivity_data.Tilts.size() << "\n\n";

    for (size_t t = 0; t < reflectivity_data.Tilts.size(); ++t) {
        const SingleTilt& tilt = reflectivity_data.Tilts[t];
        std::cout << "Tilt " << t << " | Elevation: " << std::fixed << std::setprecision(2)
                  << tilt.ElevationAngle << " deg\n";
        std::cout << "  Number of radials: " << tilt.Radials.size() << "\n";

        if (tilt.Radials.empty()) {
            std::cout << "  (No data)\n\n";
            continue;
        }

        // Print a few example radar points, e.g., 5 samples
        size_t step = std::max((size_t)1, tilt.Radials.size() / 25);
        for (size_t i = 0; i < tilt.Radials.size(); i += step) {
            const RadialData& pt = tilt.Radials[i];
            std::cout << "    Azimuth: " << std::fixed << std::setprecision(2) << pt.azimuth_deg
                      << "°, Dist: " << pt.dist << " m, Value: " << pt.value << " dBZ\n";
        }
        std::cout << "\n";
    }

    std::cout << "=== End of Summary ===\n";
}

#include <algorithm>
#include <cmath>
#include <cstdint>

void colormap_velocity(float vel, uint8_t& r, uint8_t& g, uint8_t& b, float VNYQ = 30.0f) {
    if (std::isnan(vel) || std::isinf(vel)) {
        r = g = b = 0;
        return;
    }

    // Clamp velocity into [-VNYQ, VNYQ]
    //should already be folded
    //float v = std::max(-VNYQ, std::min(VNYQ, vel));
    float v = vel;
    VNYQ = VNYQ/100;

    //std::cout << v << ", ";

    // Normalize to [-1, 1]
    float norm = v / VNYQ;

    //std::cout << ": norm: "<< norm << ", ";

    // Near zero → gray (shrink this threshold!)
    if (std::fabs(norm) < 0.00000001f) { // ±1% of Nyquist
        r = g = b = 128;
        return;
    }

    if (norm < 0) {
        // Toward radar (negative) → green
        float t = (norm + 1.0f); // maps [-1,0] → [0,1]
        r = static_cast<uint8_t>(30 * t);
        g = static_cast<uint8_t>(180 + 75 * t);
        b = static_cast<uint8_t>(30 * t);
    } else {
        // Away from radar (positive) → red
        float t = (1.0f - norm); // maps [0,1] → [1,0]
        r = static_cast<uint8_t>(180 + 75 * t);
        g = static_cast<uint8_t>(30 * t);
        b = static_cast<uint8_t>(30 * t);
    }
}



constexpr float kt2ms = 0.51444f;

void colormap_velocity_evans(float vel, uint8_t& r, uint8_t& g, uint8_t& b,  float VNYQ = 30.0f) {
    //https://www.wxtools.org/velocity/awips-evans
    if (std::isnan(vel) || std::isinf(vel)) {
        r = g = b = 0;
        return;
    }

    VNYQ = VNYQ/100;

    // Convert from knots to m/s
    vel *= 1.0f; // if vel already in m/s, leave as-is

    if (vel <= -120*kt2ms)      { r = 255; g = 0;   b = 128; return; }
    else if (vel <= -90.5*kt2ms){ r =  static_cast<uint8_t>(255 + (0-255)*(vel+120*kt2ms)/(29.5*kt2ms));
                                   g = static_cast<uint8_t>(0 + (0-0)*(vel+120*kt2ms)/(29.5*kt2ms));
                                   b = static_cast<uint8_t>(128 + (160-128)*(vel+120*kt2ms)/(29.5*kt2ms)); return; }
    else if (vel <= -70*kt2ms)  { r = 0; g = static_cast<uint8_t>(0 + (224-0)*(vel+90.5*kt2ms)/(20.5*kt2ms)); b = static_cast<uint8_t>(160 + (255-160)*(vel+90.5*kt2ms)/(20.5*kt2ms)); return; }
    else if (vel <= -69.99*kt2ms){ r = 0; g = 255; b = 224; return; }
    else if (vel <= -60*kt2ms)   { r = 0; g = 255; b = 225; return; }
    else if (vel <= -59.99*kt2ms){ r = 160; g = 255; b = 208; return; }
    else if (vel <= -50*kt2ms)   { r = 160; g = 255; b = 208; return; }
    else if (vel <= -49.99*kt2ms){ r = 160; g = 255; b = 208; return; }
    else if (vel <= -40*kt2ms)   { r = 0; g = 255; b = 0; return; }
    else if (vel <= -10*kt2ms)   { r = 16; g = 96; b = 16; return; }
    else if (vel <= -0.01*kt2ms) { r = 112; g = 128; b = 112; return; }
    else if (vel <= 0)            { r = 144; g = 128; b = 144; return; }
    else if (vel <= 10*kt2ms)     { r = 112; g = 0; b = 0; return; }
    else if (vel <= 40*kt2ms)     { r = 255; g = 0; b = 0; return; }
    else if (vel <= 48.6*kt2ms)   { r = 255; g = 0; b = 128; return; }
    else if (vel <= 49.5*kt2ms)   { r = 255; g = 0; b = 144; return; }
    else if (vel <= 69.99*kt2ms)  { r = 255; g = 196; b = 255; return; }
    else if (vel <= 70*kt2ms)     { r = 255; g = 96; b = 0; return; }
    else if (vel <= 120*kt2ms)    { r = 255; g = 255; b = 0; return; }

    // Fallback RF
    r = 128; g = 0; b = 208;
}

void colormap_dbz(float dbz, uint8_t& r, uint8_t& g, uint8_t& b) {
    if (std::isnan(dbz) || std::isinf(dbz)) {
        r = g = b = 0;
        return;
    }

    struct ColorStep {
        float threshold;
        uint8_t r, g, b;
    };

    static const ColorStep scale[] = {
        { 5.0f,   4, 233, 231 },  // light blue
        { 10.0f,  1, 159, 244 },
        { 15.0f,  3, 0, 244 },
        { 20.0f,  2, 253, 2 },
        { 25.0f,  1, 197, 1 },
        { 30.0f,  0, 142, 0 },
        { 35.0f,  253, 248, 2 },
        { 40.0f,  229, 188, 0 },
        { 45.0f,  253, 149, 0 },
        { 50.0f,  253, 0, 0 },
        { 55.0f,  212, 0, 0 },
        { 60.0f,  188, 0, 0 },
        { 65.0f,  248, 0, 253 }, // magenta
        { 70.0f,  152, 84, 198 },
        { INFINITY, 255, 255, 255 }  // fallback (white)
    };

    for (const auto& step : scale) {
        if (dbz < step.threshold) {
            r = step.r;
            g = step.g;
            b = step.b;
            return;
        }
    }
}

void colormap_reflectivity_lacrosse(float dbz, uint8_t& r, uint8_t& g, uint8_t& b) {
    //https://www.wxtools.org/reflectivity/2004-lacrosse-br
    if (std::isnan(dbz) || std::isinf(dbz)) {
        r = g = b = 0;
        return;
    }

    if (dbz <= -30) { r = 0; g = 0; b = 0; return; }
    else if (dbz <= 0)   { r = 105; g = 126; b = 108; return; }
    else if (dbz <= 5)   { r = 94; g = 94; b = 94; return; }
    else if (dbz <= 10)  { r = 131; g = 131; b = 131; return; }
    else if (dbz <= 15)  { r = 171; g = 171; b = 171; return; }
    else if (dbz <= 20)  { r = 0; g = 225; b = 255; return; }
    else if (dbz <= 25)  { r = 0; g = 150; b = 255; return; }
    else if (dbz <= 30)  { r = 0; g = 255; b = 0; return; }
    else if (dbz <= 35)  { r = 0; g = 180; b = 0; return; }
    else if (dbz <= 40)  { r = 255; g = 250; b = 0; return; }
    else if (dbz <= 45)  { r = 255; g = 170; b = 0; return; }
    else if (dbz <= 50)  { r = 255; g = 0; b = 0; return; }
    else if (dbz <= 60)  { r = 254; g = 222; b = 255; return; }
    else if (dbz <= 65)  { r = 255; g = 99; b = 255; return; }
    else if (dbz <= 70)  { r = 173; g = 0; b = 176; return; }
    else if (dbz <= 75)  { r = 255; g = 255; b = 255; return; }

    // Anything above max
    r = 255; g = 255; b = 255;
}

void colormap_correlation_coefficient(float rho, uint8_t& r, uint8_t& g, uint8_t& b) {
    //https://www.wxtools.org/correlation-coefficient/awips-rho-cc
    if (std::isnan(rho) || std::isinf(rho)) {
        r = g = b = 0;
        return;
    }

    if (rho >= 1.05f) { r = 164; g = 54;  b = 150; return; }
    else if (rho >= 1.00f) { r = 255; g = 180; b = 215; return; }
    else if (rho >= 0.99f) { r = 139; g = 30;  b = 77; return; }
    else if (rho >= 0.97f) { r = 225; g = 3;   b = 0; return; }
    else if (rho >= 0.95f) { r = 255; g = 140; b = 0; return; }
    else if (rho >= 0.90f) { r = 255; g = 255; b = 0; return; }
    else if (rho >= 0.85f) { r = 135; g = 215; b = 10; return; }
    else if (rho >= 0.80f) { r = 95;  g = 245; b = 100; return; }
    else if (rho >= 0.75f) { r = 120; g = 120; b = 255; return; }
    else if (rho >= 0.60f) { r = 10;  g = 10;  b = 190; return; }
    else if (rho >= 0.45f) { r = 15;  g = 15;  b = 140; return; }
    else                   { r = 15;  g = 15;  b = 140; return; }
}

void colormap_spectrum_width(float vel_ms, uint8_t& r, uint8_t& g, uint8_t& b) {
    //https://www.wxtools.org/spectrum-width/bens-sw
    if (std::isnan(vel_ms) || std::isinf(vel_ms)) {
        r = g = b = 0;
        return;
    }

    // Convert m/s → knots
    float vel = vel_ms * 1.9426f;

    if (vel <= 0)      { r = 20;  g = 5;   b = 72;  return; }
    else if (vel <= 2) { r = 50;  g = 20;  b = 140; return; }
    else if (vel <= 4) { r = 124; g = 38;  b = 190; return; }
    else if (vel <= 7) { r = 218; g = 55;  b = 120; return; }
    else if (vel <= 15){ r = 251; g = 126; b = 33;  return; }
    else if (vel <= 18){ r = 255; g = 255; b = 0;   return; }
    else if (vel <= 22){ r = 153; g = 255; b = 51;  return; }
    else if (vel <= 30){ r = 0;   g = 153; b = 230; return; }
    else if (vel <= 35){ r = 0;   g = 17;  b = 26;  return; }
    else if (vel <= 40){ r = 255; g = 255; b = 255; return; }

    // Fallback RF color
    r = 117; g = 0; b = 117;
}

void saveTiltAsPNGInterpolate(const SingleTilt& tilt, const std::string& filename, const int SIZE = 1000, float M_PER_PIXEL = 800.0f) {

    const int CENTER = SIZE / 2;

    int nyquist_vel = tilt.vol_el_rad.rad.nyquist_vel;
    std::cout << "nyquist: " << nyquist_vel << std::endl;

    // Define a struct to hold moment value, count, and distance
    struct GridPoint {
        float value = 0.0f;  // Sum of radar moment values
        int count = 0;       // Number of contributing radials
        float distance = 0.0f; // Distance from radar (radial.dist)
    };

    // Grid declaration
    std::vector<std::vector<GridPoint>> grid(SIZE, std::vector<GridPoint>(SIZE, {0.0f, 0, 0.0f}));
    std::vector<std::vector<float>> interpolated(SIZE, std::vector<float>(SIZE, std::numeric_limits<float>::quiet_NaN()));

    // Step 1: Populate known grid values
    for (const auto& radial : tilt.Radials) {
        //if (std::isnan(radial.value) || radial.value < -30 || radial.value > 80) continue;

        float az_rad = radial.azimuth_deg * M_PI / 180.0f;
        float x = radial.dist * std::sin(az_rad);
        float y = radial.dist * std::cos(az_rad);

        int px = static_cast<int>(x / M_PER_PIXEL + CENTER);
        int py = static_cast<int>(CENTER - y / M_PER_PIXEL);

        if (px >= 0 && px < SIZE && py >= 0 && py < SIZE) {
            grid[py][px].value += radial.value;
            grid[py][px].count++;
            grid[py][px].distance = radial.dist; // Store the distance (last radial wins)
            // Alternatively, average distances: grid[py][px].distance += radial.dist;
        } else {
            //std::cout << "Clipped: dist = " << radial.dist << ", az = " << radial.azimuth_deg << ", px = " << px << ", py = " << py << std::endl;
        }
    }
    // Step 2: Assign known averages
    for (int y = 0; y < SIZE; ++y) {
        for (int x = 0; x < SIZE; ++x) {
            if (grid[y][x].count > 0 && grid[y][x].distance > 1000 ) {
                interpolated[y][x] = grid[y][x].value / grid[y][x].count;
            }
        }
    }

    // Step 3: Interpolate enclosed NaNs
    for (int step = 0; step < 4; ++step) {
        std::vector<std::vector<float>> snapshot = interpolated;

        for (int y = 2; y < SIZE - 2; ++y) {
            for (int x = 2; x < SIZE - 2; ++x) {
                if (!std::isnan(interpolated[y][x])) continue;

                float sum = 0.0f;
                int count = 0;

                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0) continue;
                        float neighbor = snapshot[y + dy][x + dx];
                        if (!std::isnan(neighbor)) {
                            sum += neighbor;
                            count++;
                        }
                    }
                }

                if (count >= 5) { // allow some flexibility
                    if (grid[y][x].distance < 1000) continue;
                    interpolated[y][x] = sum / count;
                }
            }
        }
    }

    // Step 4: Render RGBA image
    std::vector<uint8_t> image(SIZE * SIZE * 4, 0);

    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            float value = interpolated[y][x];
            int idx = (y * SIZE + x) * 4;

            if (!std::isnan(value) && value != 0.0f && (value >= 0.01 || value <= 0.01) ) {
                uint8_t r, g, b;

                colormap_dbz(value, r, g, b);
                // Or: colormap_velocity(value, r, g, b, float(nyquist_vel));

                image[idx + 0] = r;
                image[idx + 1] = g;
                image[idx + 2] = b;
                image[idx + 3] = 255; // opaque
            } else {
                // Transparent pixel
                image[idx + 0] = 0;
                image[idx + 1] = 0;
                image[idx + 2] = 0;
                image[idx + 3] = 0; // fully transparent
            }
        }
    }

    // Step 5: Save as PNG with alpha
    png_buffer.clear();
    stbi_write_png_to_func(
        [](void* context, void* data, int size) {
            auto* out = static_cast<std::vector<uint8_t>*>(context);
            out->insert(out->end(), (uint8_t*)data, (uint8_t*)data + size);
        },
        &png_buffer,
        SIZE, SIZE, 4,         // 4 = RGBA
        image.data(), SIZE * 4 // row stride
    );

    std::cout << "Saved RGBA image with transparency to buffer." << std::endl;
}



void saveTiltAsPNGInterpolate2(const SingleTilt& tilt, const std::string& filename, const int SIZE = 1000, float M_PER_PIXEL = 800.0f) {

    const int CENTER = SIZE / 2;

    float center_x = CENTER * M_PER_PIXEL;
    float center_y = CENTER * M_PER_PIXEL;


    const float beam_half_width_rad = 0.5f * M_PI / 180.0f; // 0.5 degree, not 0.5 radians

    int nyquist_vel = tilt.vol_el_rad.rad.nyquist_vel;
    std::cout << "nyquist: " << nyquist_vel << std::endl;

    struct GridPoint {
        float value = 0.0f;
        int   count = 0;
        float distance_sq = 0.0f;
    };

    float gate_width_m = tilt.gateSpacing;
    float half_gate = gate_width_m * 0.5f;
    const float inv_m_per_pixel = 1.0f / M_PER_PIXEL;

    struct RadialCache {
        float sin_az;
        float cos_az;
        float inner_r;
        float outer_r;
        float value;
        float inner_r_sq;
        float outer_r_sq;
        float min_x;
        float max_x;
        float min_y;
        float max_y;
    };
    std::vector<RadialCache> radial_cache;
    radial_cache.reserve(tilt.Radials.size());

    for (const auto& radial : tilt.Radials) {
        if (std::isnan(radial.value)) continue;

        //float az_rad = radial.azimuth_deg * M_PI / 180.0f;
        float az_rad = (90.0f - radial.azimuth_deg) * M_PI / 180.0f;
        float cos_az_rad = std::cos(az_rad);
        float sin_az_rad = std::sin(az_rad);

        float inner_r = std::max(0.0f, radial.dist - half_gate);
        float outer_r = radial.dist + half_gate;

        // bounding box corners (in meters from origin)
        float x_plusrad_plusdist = (radial.dist+ half_gate) * std::cos(az_rad + beam_half_width_rad)  + center_x;
        float x_plusrad_minusdist = (radial.dist- half_gate) * std::cos(az_rad + beam_half_width_rad)+ center_x;
        
        float x_minusrad_plusdist = (radial.dist+ half_gate)  * std::cos(az_rad - beam_half_width_rad)+ center_x;
        float x_minusrad_minusdist = (radial.dist-  half_gate)  * std::cos(az_rad - beam_half_width_rad)+ center_x;

        float y_plusrad_plusdist = (radial.dist+ half_gate) *std::sin(az_rad + beam_half_width_rad)+ center_y;
        float y_plusrad_minusdist = (radial.dist- half_gate) *std::sin(az_rad + beam_half_width_rad)+ center_y;

        float y_minusrad_plusdist = (radial.dist+ half_gate) * std::sin(az_rad - beam_half_width_rad)+ center_y;
        float y_minusrad_minusdist = (radial.dist- half_gate) * std::sin(az_rad - beam_half_width_rad)+ center_y;

        float min_x = std::min({x_plusrad_minusdist, x_plusrad_plusdist, x_minusrad_minusdist, x_minusrad_plusdist});
        float max_x = std::max({x_plusrad_minusdist, x_plusrad_plusdist, x_minusrad_minusdist, x_minusrad_plusdist});
        float min_y = std::min({y_plusrad_minusdist, y_plusrad_plusdist, y_minusrad_minusdist, y_minusrad_plusdist});
        float max_y = std::max({y_plusrad_minusdist, y_plusrad_plusdist, y_minusrad_minusdist, y_minusrad_plusdist});

        radial_cache.push_back({
            sin_az_rad,
            cos_az_rad,
            inner_r,
            outer_r,
            radial.value,
            inner_r * inner_r,
            outer_r * outer_r,
            min_x,
            max_x,
            min_y,
            max_y
        });
    }

    std::vector<GridPoint> grid(SIZE * SIZE);
    std::vector<float> interpolated(SIZE * SIZE, std::numeric_limits<float>::quiet_NaN());

    std::vector<float> xm_for_px(SIZE);
    std::vector<float> ym_for_py(SIZE);
    for (int px = 0; px < SIZE; ++px) xm_for_px[px] = (px - CENTER) * M_PER_PIXEL;
    for (int py = 0; py < SIZE; ++py) ym_for_py[py] = (CENTER - py) * M_PER_PIXEL;

    const float beam_half_angle = 0.5f * M_PI / 180.0f;
    const float cos_tol = std::cos(beam_half_angle);
    const float cos_tol_sq = cos_tol * cos_tol;
    const float min_r2_eps = 1e-6f;

    for (const auto& rad : radial_cache) {
        // Convert bounding box meters → pixel coords
        int px_min = std::max(0, int(std::floor((rad.min_x - center_x) / M_PER_PIXEL + CENTER)));
        int px_max = std::min(SIZE - 1, int(std::floor((rad.max_x - center_x) / M_PER_PIXEL + CENTER)));
        int py_min = std::max(0, int(std::floor((center_y - rad.max_y) / M_PER_PIXEL + CENTER)));
        int py_max = std::min(SIZE - 1, int(std::floor((center_y - rad.min_y) / M_PER_PIXEL + CENTER)));


        for (int py = py_min; py <= py_max; ++py) {
            const float ym = ym_for_py[py];
            const float ym_sq = ym * ym;
            for (int px = px_min; px <= px_max; ++px) {

                const float xm = xm_for_px[px];
                const float r2 = xm * xm + ym_sq;

                if (r2 < rad.inner_r_sq || r2 > rad.outer_r_sq) continue;
                if (r2 <= min_r2_eps) continue;

                const float dot = xm * rad.cos_az + ym * rad.sin_az; // fixed alignment
                if (dot * dot < cos_tol_sq * r2) continue;

                int idx = py * SIZE + px;
                GridPoint &g = grid[idx];
                g.value += rad.value;
                g.count += 1;
                g.distance_sq = r2;
            }
        }
    }

    const float thresh1000_sq = 1000.0f * 1000.0f;
    for (int y = 0; y < SIZE; ++y) {
        for (int x = 0; x < SIZE; ++x) {
            int idx = y * SIZE + x;
            GridPoint &g = grid[idx];
            if (g.count > 0 && g.distance_sq > thresh1000_sq) {
                interpolated[idx] = g.value / static_cast<float>(g.count);
            }
        }
    }

    std::vector<uint8_t> image(SIZE * SIZE * 4, 0);
    for (int y = 0; y < SIZE; ++y) {
        for (int x = 0; x < SIZE; ++x) {
            int idx_interp = y * SIZE + x;
            float value = interpolated[idx_interp];
            int out_i = idx_interp * 4;

            if (!std::isnan(value) && std::fabs(value) >= 0.01f) {
                uint8_t r, g, b;

                if (cStringsEqual(selected_radar_moment, "REF")) {
                    colormap_reflectivity_lacrosse(value, r, g, b);
                }
                else if (cStringsEqual(selected_radar_moment, "VEL")) {
                    //std::cout << "nyquist velocity : "<< nyquist_vel << std::endl;
                    colormap_velocity_evans(value, r, g, b, float(nyquist_vel));
                }
                else if (cStringsEqual(selected_radar_moment, "SW ")) {
                    colormap_spectrum_width(value, r, g, b); 
                }
                else if (cStringsEqual(selected_radar_moment, "ZDR")) {
                    colormap_dbz(value, r, g, b);
                }
                else if (cStringsEqual(selected_radar_moment, "PHI")) {
                    colormap_dbz(value, r, g, b);
                }
                else if (cStringsEqual(selected_radar_moment, "RHO")) {
                    colormap_correlation_coefficient(value, r, g, b);
                }
                else {
                    colormap_dbz(value, r, g, b);
                }

                //colormap_dbz(value, r, g, b);
                image[out_i + 0] = r;
                image[out_i + 1] = g;
                image[out_i + 2] = b;
                image[out_i + 3] = 255;
            } else {
                image[out_i + 0] = 0;
                image[out_i + 1] = 0;
                image[out_i + 2] = 0;
                image[out_i + 3] = 0;
            }
        }
    }

    png_buffer.clear();
    stbi_write_png_to_func(
        [](void* context, void* data, int size) {
            auto* out = static_cast<std::vector<uint8_t>*>(context);
            out->insert(out->end(), (uint8_t*)data, (uint8_t*)data + size);
        },
        &png_buffer,
        SIZE, SIZE, 4,
        image.data(), SIZE * 4
    );

    std::cout << "Saved RGBA image with transparency to buffer." << std::endl;
}

std::pair<float, float> project_from_bearing(
    float latitude, float longitude, float arc_len_m, float bearing_deg)
{
    const float earth_radius_m = 6371000.0;
    float delta = arc_len_m / earth_radius_m;

    float latitude_rad = deg2rad(latitude);
    float longitude_rad = deg2rad(longitude);
    float bearing_rad = deg2rad(bearing_deg);

    float latitude2_rad = std::asin(
        std::sin(latitude_rad) * std::cos(delta) +
        std::cos(latitude_rad) * std::sin(delta) * std::cos(bearing_rad));

    float longitude2_rad = longitude_rad + std::atan2(
        std::sin(bearing_rad) * std::sin(delta) * std::cos(latitude_rad),
        std::cos(delta) - std::sin(latitude_rad) * std::sin(latitude2_rad));

    return std::make_pair(rad2deg(latitude2_rad), rad2deg(longitude2_rad));
}


std::pair<std::pair<float, float>, std::pair<float, float>>
get_latlon_corners_or_midpoints(float latitude, float longitude, float elevation_angle,
                                 float m_per_pixel, float png_size, float max_dist,
                                 bool use_midpoints = false)
{
    float beam_len = use_midpoints ? max_dist
                                          : std::sqrt(2 * std::pow((png_size * m_per_pixel) / 2, 2));
    
    float earth_radius_m = 6371000;

    std::cout << "earth radius M: " << earth_radius_m << std:: endl;


    // float center_to_beam = std::sqrt(
    //     beam_len * beam_len +
    //     earth_radius_m * earth_radius_m -
    //     2 * earth_radius_m * beam_len * std::cos(deg2rad(90 + elevation_angle)));

    // float arc_angle = std::acos(
    //     (beam_len * beam_len - earth_radius_m * earth_radius_m - center_to_beam * center_to_beam) /
    //     (-2 * earth_radius_m * center_to_beam));

    AtmosphericRayTracer tracer;

    float arc_angle = tracer.get_earth_arc_angle_station_to_beam_end(
        0.0,
        tracer.earth_radius,
        deg2rad(elevation_angle),
        max_dist,
        20.0
    );



    std::cout << "arc angle: "<< arc_angle <<std::endl;

    float arc_len_m = arc_angle * earth_radius_m;

    // For corners or side midpoints
    std::vector<float> bearings = use_midpoints ? std::vector<float>{0, 90, 180, 270}
                                                : std::vector<float>{315, 135};

    std::vector<std::pair<float, float>> projected_points;
    for (float bearing : bearings) {
        projected_points.push_back(project_from_bearing(latitude, longitude, arc_len_m, bearing));
    }

    if (use_midpoints) {
        // Return (top, left) and (bottom, right)
        return std::make_pair(
            std::make_pair(projected_points[0].first, projected_points[3].second),  // top, left
            std::make_pair(projected_points[2].first, projected_points[1].second)   // bottom, right
        );
    } else {
        // Return (top-left, bottom-right)
        return std::make_pair(projected_points[0], projected_points[1]);
    }
}



extern "C" {
    // Get pointer to PNG data
    EMSCRIPTEN_KEEPALIVE
    const uint8_t* get_png_data() {return png_buffer.data();}
    
    // Get size of PNG data
    EMSCRIPTEN_KEEPALIVE
    int get_png_size() {return static_cast<int>(png_buffer.size());}

    EMSCRIPTEN_KEEPALIVE
    float get_latitude_topleft() {return latitude_topleft;}
    EMSCRIPTEN_KEEPALIVE
    float get_longitude_topleft() {return longitude_topleft;}
    EMSCRIPTEN_KEEPALIVE
    float get_latitude_bottomright() {return latitude_bottomright;}
    EMSCRIPTEN_KEEPALIVE
    float get_longitude_bottomright() {return longitude_bottomright;}

    EMSCRIPTEN_KEEPALIVE
    float* get_tilt_angles(){
        return tilt_angles;
    }

    EMSCRIPTEN_KEEPALIVE
    int get_tilt_angles_size(){
        return sizeof(tilt_angles) / sizeof(tilt_angles[0]);
    }

    EMSCRIPTEN_KEEPALIVE
    void set_tilt_angles_index(const int tilt_num){
        tilt_number_for_data = tilt_num;
    }



    EMSCRIPTEN_KEEPALIVE
    void set_selected_radar_moment(const char* moment) {
        // Copy at most 3 characters to leave room for '\0'
        strncpy(selected_radar_moment, moment, 3);
        selected_radar_moment[3] = '\0';  // Ensure null termination
    }


    EMSCRIPTEN_KEEPALIVE
    int parse_nexrad(uint8_t* data, int length) {
        std::cout << "=== NEXRAD Level II Parser (AR2V Format) ===" << std::endl;
        std::cout << "File length: " << length << std::endl;
        
        if (length < 24) {
            std::cout << "File too small" << std::endl;
            return -1;
        }

        //initialize all tilt angles as -1, these get reupdated each time a new icao gets run
        for(int i = 0; i< 50; i++){
            tilt_angles[i] = -1;
        }

        
        //uint8_t* data_ptr = buffer.data();
        std::vector<uint8_t> buffer(data, data + length);

        uint8_t* data_ptr = data;
        VolumeHeader vol_header;
        std::memcpy(vol_header.tape, data_ptr, 9); data_ptr += 9;
        std::memcpy(vol_header.extension, data_ptr, 3); data_ptr += 3;
        vol_header.date = read_be32(data_ptr); data_ptr += 4;
        vol_header.time = read_be32(data_ptr); data_ptr += 4;
        std::memcpy(vol_header.icao, data_ptr, 4); data_ptr += 4;


        std::cout << "Tape: '" << vol_header.tape << "'\n";
        std::cout << "Extension: '" << vol_header.extension << "'\n";
        std::cout << "Date: " << vol_header.date << "\n";
        std::cout << "Time: " << vol_header.time << "\n";
        std::cout << "ICAO: '" << vol_header.icao << "'\n";

        //std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(file)), {});


        std::vector<uint8_t> full_decompressed_data;

        AllTilt reflectivity_data;

        //std::vector<size_t> bz2_offsets,  = find_bzip2_block_offsets(buffer);  // already implemented

        auto [bz2_offsets, bz2_block_sizes] = find_bzip2_block_offsets(buffer); 

        // std::copy(bz2_offsets.begin(), bz2_offsets.end(), std::ostream_iterator<int>(std::cout, " "));
        // std::cout << "erm" << std::endl;
        // std::copy(bz2_block_sizes.begin(), bz2_block_sizes.end(), std::ostream_iterator<int>(std::cout, " "));

        for (size_t i = 0; i < bz2_offsets.size(); ++i) {
            size_t start = bz2_offsets[i];

            // Use a max chunk window; 1MB should be more than enough
            //size_t max_block_len = 1024 * 1024;  // 1MB
            size_t max_block_len = bz2_block_sizes[i];  // 1MB
            size_t remaining = buffer.size() - start;
            size_t chunk_len = std::min(max_block_len, remaining);

            const uint8_t* chunk_ptr = buffer.data() + start;

            // std::cout << "Decompressing block " << i + 1
            //         << " at offset " << start << ", max chunk: " << chunk_len << std::endl;

            std::vector<uint8_t> decompressed = decompress_bzip2_stream(chunk_ptr, chunk_len);


            process_ldm_block(decompressed, reflectivity_data);


            if (!decompressed.empty()) {
                full_decompressed_data.insert(
                    full_decompressed_data.end(),
                    decompressed.begin(),
                    decompressed.end()
                );
            } else {
                std::cerr << "Decompression failed on block " << i + 1 << std::endl;
            }

        }

        int tilt_counter = 0;
        for (auto& tilt : reflectivity_data.Tilts) {
            // do something with each tilt
            //std::cout << "Processing tilt at elevation " << tilt.ElevationAngle << "\n";
            tilt_angles[tilt_counter]= tilt.ElevationAngle;
            //std::cout << "from array: "<< tilt_angles[tilt_counter] << std::endl;
            tilt_counter++;
        }

        // int tilt_number_for_data = 0;
        //tilt 5 for velocity
        //printReflectivitySummary(reflectivity_data);
        //saveTiltAsPNG(reflectivity_data.Tilts[9], "tilt0_reflectivity.png");

        //const int SIZE = 1600;
        const int SIZE = 3000;
        float max_dist = reflectivity_data.Tilts[tilt_number_for_data ].maxDist;
        std::cout << "max dist " << max_dist << std::endl;
        float M_PER_PIXEL = max_dist*2 / SIZE;
        std::cout << "new m per pixel: " << M_PER_PIXEL << std::endl;
        
        saveTiltAsPNGInterpolate2(reflectivity_data.Tilts[tilt_number_for_data ], "tilt1_reflectivity.png", SIZE, M_PER_PIXEL);


        std::cout << "latidude: " <<reflectivity_data.Tilts[tilt_number_for_data ].vol_el_rad.vol.lat << std::endl;
        std::cout << "longitude: " <<reflectivity_data.Tilts[tilt_number_for_data ].vol_el_rad.vol.lon << std::endl;


        std::pair<std::pair<float, float>, std::pair<float, float>> lat_long_pairs;

        //lat_long_pairs = get_latitude_longitude_topright_bottomleft(reflectivity_data.Tilts[tilt_number_for_data ].vol_el_rad.vol.lat, reflectivity_data.Tilts[tilt_number_for_data ].vol_el_rad.vol.lon, reflectivity_data.Tilts[tilt_number_for_data ].ElevationAngle, M_PER_PIXEL, SIZE);
        //lat_long_pairs = get_latitude_longitude_topright_bottomleft(reflectivity_data.Tilts[tilt_number_for_data ].vol_el_rad.vol.lat, reflectivity_data.Tilts[tilt_number_for_data ].vol_el_rad.vol.lon, reflectivity_data.Tilts[tilt_number_for_data ].ElevationAngle, M_PER_PIXEL, SIZE);

        //lat_long_pairs = get_latitude_longitude_from_midpoints(reflectivity_data.Tilts[tilt_number_for_data ].vol_el_rad.vol.lat, reflectivity_data.Tilts[tilt_number_for_data ].vol_el_rad.vol.lon, reflectivity_data.Tilts[tilt_number_for_data ].ElevationAngle, M_PER_PIXEL, SIZE, max_dist);
        lat_long_pairs = get_latlon_corners_or_midpoints(reflectivity_data.Tilts[tilt_number_for_data ].vol_el_rad.vol.lat, reflectivity_data.Tilts[tilt_number_for_data ].vol_el_rad.vol.lon, reflectivity_data.Tilts[tilt_number_for_data ].ElevationAngle, M_PER_PIXEL, SIZE, max_dist, true);
        latitude_topleft = lat_long_pairs.first.first;
        longitude_topleft = lat_long_pairs.first.second;
        latitude_bottomright = lat_long_pairs.second.first;
        longitude_bottomright = lat_long_pairs.second.second;


        return 0;
    }
}

