cmake_minimum_required(VERSION 3.22.1)

project(splatscan_core CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(
        splatscan_core
        SHARED
        native_bridge.cpp
        core/math_types.h
        gs/gaussian_params.cpp
        gs/raster_cpu.cpp
        vio/feature_tracker.cpp
        vio/pose_solver.cpp
        engine/scan_engine.cpp
)

target_include_directories(splatscan_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_options(
        splatscan_core
        PRIVATE
        -O3
        -ffast-math
        -fvisibility=hidden
        -Wall
)

find_library(log_lib log)
find_library(android_lib android)

target_link_libraries(
        splatscan_core
        ${log_lib}
        ${android_lib}
)