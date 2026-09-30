cmake_minimum_required(VERSION 3.24)
project(eventree LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(eventree INTERFACE)
add_library(eventree::eventree ALIAS eventree)
target_include_directories(eventree INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(eventree INTERFACE cxx_std_20)

if(PROJECT_IS_TOP_LEVEL)
  include(CTest)
  if(BUILD_TESTING)
    add_subdirectory(tests)
  endif()
  if (BUILD_APPS)
    add_subdirectory(apps)
  endif()
endif()