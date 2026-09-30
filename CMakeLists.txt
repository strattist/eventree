cmake_minimum_required(VERSION 4.2)

if (NOT CMAKE_BUILD_TYPE) 
    set(CMAKE_BUILD_TYPE RELEASE)
endif()

project(eventree VERSION 0.0.0 LANGUAGES CXX)

find_package(CLI11 REQUIRED)
find_package(MetavisionPSEEHWLayer REQUIRED)
find_package(MetavisionSDK COMPONENTS base core stream REQUIRED)

add_executable(raw_file_inspector ${PROJECT_SOURCE_DIR}/raw_file_inspector.cpp)
target_link_libraries(raw_file_inspector 
    PRIVATE 
        CLI11::CLI11 
        Metavision::PSEEHWLayer 
        MetavisionSDK::base 
        MetavisionSDK::core 
        MetavisionSDK::stream
)