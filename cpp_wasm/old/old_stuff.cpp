AllTilt parse_nexrad_new(uint8_t* data, int length) {

    std::cout << "=== NEXRAD Level II Parser (AR2V Format) ===" << std::endl;
    std::cout << "File length: " << length << std::endl;
    
    if (length < 24) {
        std::cout << "File too small" << std::endl;
        return AllTilt{};
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

    // AllTilt reflectivity_data;

    //std::vector<size_t> bz2_offsets,  = find_bzip2_block_offsets(buffer);  // already implemented

    auto [bz2_offsets, bz2_block_sizes] = find_bzip2_block_offsets(buffer); 
    
    std::vector<std::thread> threads_to_process_blocks;
    // 1. Create a vector to store the return values (e.g., int, bool, or a custom struct)
    std::vector<AllTilt> process_ldm_blocks_results(bz2_offsets.size()); 
    int MAX_THREADS = 8;

    

    // std::copy(bz2_offsets.begin(), bz2_offsets.end(), std::ostream_iterator<int>(std::cout, " "));
    // std::cout << "erm" << std::endl;
    // std::copy(bz2_block_sizes.begin(), bz2_block_sizes.end(), std::ostream_iterator<int>(std::cout, " "));

    for (size_t i = 0; i < bz2_offsets.size(); ++i) {

        size_t* bz2_offsets_array = bz2_offsets.data();
        int* bz2_block_sizes_array = bz2_block_sizes.data();

        threads_to_process_blocks.emplace_back(unzip_process_ldm_worker, 
                    i, 
                    bz2_offsets_array, 
                    bz2_block_sizes_array, 
                    std::ref(buffer), 
                    // std::ref(reflectivity_data), 
                    std::ref(full_decompressed_data),
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
    std::cout << "Processed " << process_ldm_blocks_results.size() << " blocks" << std::endl;


    for (auto& result : process_ldm_blocks_results) {
        // loop through tilts and print elevation angles
        for (auto& tilt: result.Tilts) {
            std::cout << "Elevation angle: " << tilt.ElevationAngle << std::endl;
        }

    }

    AllTilt combined_tilts = combine_all_tilts_from_thread_results(process_ldm_blocks_results);

    // int tilt_counter = 0;
    // for (auto& tilt : reflectivity_data.Tilts) {
    //     // do something with each tilt
    //     //std::cout << "Processing tilt at elevation " << tilt.ElevationAngle << "\n";
    //     tilt_angles[tilt_counter]= tilt.ElevationAngle;
    //     //std::cout << "from array: "<< tilt_angles[tilt_counter] << std::endl;
    //     tilt_counter++;
    // }

    return combined_tilts;
}


using namespace emscripten;

AllTilt parse_nexrad_bound(uintptr_t data_ptr, int length) {
    return parse_nexrad_new(reinterpret_cast<uint8_t*>(data_ptr), length);
}

EMSCRIPTEN_BINDINGS(my_module) {

    value_object<RadialData>("RadialData")
        .field("azimuth_deg", &RadialData::azimuth_deg)
        .field("dist",        &RadialData::dist)
        .field("value",       &RadialData::value);

    register_vector<RadialData>("RadialDataVector");

    value_object<SingleTilt>("SingleTilt")
        .field("ElevationAngle", &SingleTilt::ElevationAngle)
        .field("gateSpacing",    &SingleTilt::gateSpacing)
        .field("maxDist",        &SingleTilt::maxDist)
        .field("count",           &SingleTilt::count)
        .field("Radials_REF",     &SingleTilt::Radials_REF)
        .field("Radials_VEL",     &SingleTilt::Radials_VEL)
        .field("Radials_SW",      &SingleTilt::Radials_SW)
        .field("Radials_ZDR",     &SingleTilt::Radials_ZDR)
        .field("Radials_PHI",     &SingleTilt::Radials_PHI)
        .field("Radials_RHO",     &SingleTilt::Radials_RHO);

    register_vector<SingleTilt>("SingleTiltVector");

    value_object<AllTilt>("AllTilt")
        .field("Tilts",       &AllTilt::Tilts)
        .field("julian_date", &AllTilt::julian_date);


    function("parse_nexrad", &parse_nexrad_bound);
}
