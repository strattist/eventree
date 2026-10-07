find_package(CLI11 REQUIRED)
find_package(MetavisionPSEEHWLayer REQUIRED)
find_package(MetavisionSDK COMPONENTS base core stream REQUIRED)

add_executable(raw_file_inspector raw_file_inspector.cpp)
target_link_libraries(raw_file_inspector 
    PRIVATE 
        CLI11::CLI11 
        Metavision::PSEEHWLayer 
        MetavisionSDK::base 
        MetavisionSDK::core 
        MetavisionSDK::stream
)

add_executable(reference_decoder reference_decoder.cpp)
target_link_libraries(reference_decoder
    PRIVATE
        CLI11::CLI11
        Metavision::PSEEHWLayer
        MetavisionSDK::base
        MetavisionSDK::core
        MetavisionSDK::stream
)

add_executable(comparison comparison.cpp)
target_link_libraries(comparison
    PRIVATE
        eventree::eventree
        CLI11::CLI11
        Metavision::PSEEHWLayer
        MetavisionSDK::base
        MetavisionSDK::core
        MetavisionSDK::stream
)

add_executable(benchmark benchmark.cpp)
target_link_libraries(benchmark
    PRIVATE
        eventree::eventree
        CLI11::CLI11
        Metavision::PSEEHWLayer
        MetavisionSDK::base
        MetavisionSDK::core
        MetavisionSDK::stream
)
