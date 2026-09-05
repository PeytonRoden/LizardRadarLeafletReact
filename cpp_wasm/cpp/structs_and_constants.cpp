#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

///Constants

// --- Constants ---
constexpr size_t RECORD_SIZE = 2432;
constexpr size_t COMPRESSION_RECORD_SIZE = 12;
constexpr size_t CONTROL_WORD_SIZE = 4;

constexpr double PI = 3.14159265358979323846;




/// Structs


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

struct RadialBuffer {
    std::vector<float> azimuths;
    std::vector<float> distances;
    std::vector<float> values;
};

struct SingleTilt {
    float ElevationAngle;               // Nominal tilt angle
    int count = 0;
    float maxDist = 0;  //maximum distance in m of this tilt.
    float gateSpacing = 250.0f;
    MSG_31 msg_31;
    VOL_EL_RAD vol_el_rad;   //save the first one from each tilt!!!
    std::vector<RadialData> Radials_REF;    // All radials for this tilt
    std::vector<RadialData> Radials_VEL;    // All radials for this tilt
    std::vector<RadialData> Radials_SW;     // All radials for this tilt
    std::vector<RadialData> Radials_ZDR;    // All radials for this tilt
    std::vector<RadialData> Radials_PHI;    // All radials for this tilt
    std::vector<RadialData> Radials_RHO;    // All radials for this tilt
};

struct AllTilt {
    int16_t julian_date = 0;
    std::vector<SingleTilt> Tilts;      // One entry per tilt/elevation
};

struct TiltInfo {
    float tilt;
    std::string name;
};

struct LatLon {
    double lat;
    double lon;
};

struct LatLonHeight {
    double lat;
    double lon;
    double height;
};

#pragma pack(pop)


///Functions:


inline double deg2rad(double degrees) {
    return degrees * (PI / 180.0);
}
inline double rad2deg(double radians) {
    return radians * (180.0 / PI);
}

inline float fold_velocity(float v, float VNYQ) {
    float range = 2.0f * VNYQ;
    v = std::fmod(v + VNYQ, range);
    if (v < 0) v += range;
    return v - VNYQ;
}

inline bool cStringsEqual(const char* str1, const char* str2) {
    return std::strcmp(str1, str2) == 0;
}

inline std::vector<RadialData>* get_moment_radials(SingleTilt& tilt, const char* moment_buf) {
    if (cStringsEqual(moment_buf, "REF")) return &tilt.Radials_REF;
    if (cStringsEqual(moment_buf, "VEL")) return &tilt.Radials_VEL;
    if (cStringsEqual(moment_buf, "SW ")) return &tilt.Radials_SW;
    if (cStringsEqual(moment_buf, "ZDR")) return &tilt.Radials_ZDR;
    if (cStringsEqual(moment_buf, "PHI")) return &tilt.Radials_PHI;
    if (cStringsEqual(moment_buf, "RHO")) return &tilt.Radials_RHO;
    return nullptr;
}

inline const std::vector<RadialData>* get_moment_radials(const SingleTilt& tilt, const char* moment_buf) {
    if (cStringsEqual(moment_buf, "REF")) return &tilt.Radials_REF;
    if (cStringsEqual(moment_buf, "VEL")) return &tilt.Radials_VEL;
    if (cStringsEqual(moment_buf, "SW ")) return &tilt.Radials_SW;
    if (cStringsEqual(moment_buf, "ZDR")) return &tilt.Radials_ZDR;
    if (cStringsEqual(moment_buf, "PHI")) return &tilt.Radials_PHI;
    if (cStringsEqual(moment_buf, "RHO")) return &tilt.Radials_RHO;
    return nullptr;
}
