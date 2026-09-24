cmake_minimum_required(VERSION 3.22.1)

project(splatscan_core CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 着色器：GLSL 源码 → SPIR-V → 内嵌头文件。构建机需要 glslangValidator。
find_program(GLSLANG_VALIDATOR NAMES glslangValidator glslang)
if(NOT GLSLANG_VALIDATOR)
    message(FATAL_ERROR "缺少着色器编译器：请安装 glslang-tools（apt-get install -y glslang-tools）")
endif()
find_program(PYTHON3 NAMES python3 python)
if(NOT PYTHON3)
    message(FATAL_ERROR "缺少 python3：生成着色器头文件需要它")
endif()

set(SHADER_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/render/shaders)
set(SHADER_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_BINARY_DIR})

set(SHADER_FILES splat.comp blit.vert blit.frag)
set(SPV_OUTPUTS "")
foreach(shader ${SHADER_FILES})
    add_custom_command(
            OUTPUT ${SHADER_BINARY_DIR}/${shader}.spv
            COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.1
                    -o ${SHADER_BINARY_DIR}/${shader}.spv
                    ${SHADER_SOURCE_DIR}/${shader}
            DEPENDS ${SHADER_SOURCE_DIR}/${shader}
            COMMENT "编译着色器 ${shader}"
            VERBATIM
    )
    list(APPEND SPV_OUTPUTS ${SHADER_BINARY_DIR}/${shader}.spv)
endforeach()

add_custom_command(
        OUTPUT ${SHADER_BINARY_DIR}/shaders_spv.h
        COMMAND ${PYTHON3} ${CMAKE_CURRENT_SOURCE_DIR}/render/gen_shader_header.py
                ${SHADER_BINARY_DIR} ${SHADER_BINARY_DIR}/shaders_spv.h
        DEPENDS ${SPV_OUTPUTS} ${CMAKE_CURRENT_SOURCE_DIR}/render/gen_shader_header.py
        COMMENT "生成着色器头文件"
        VERBATIM
)

add_library(
        splatscan_core
        SHARED
        native_bridge.cpp
        core/math_types.h
        gs/gaussian_params.cpp
        gs/raster_cpu.cpp
        gs/tile_builder.cpp
        vio/feature_tracker.cpp
        vio/pose_solver.cpp
        engine/scan_engine.cpp
        render/shader_registry.cpp
        render/vulkan_renderer.cpp
        ${SHADER_BINARY_DIR}/shaders_spv.h
)

target_include_directories(
        splatscan_core
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${SHADER_BINARY_DIR}
)

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
find_library(vulkan_lib vulkan)

target_link_libraries(
        splatscan_core
        ${log_lib}
        ${android_lib}
        ${vulkan_lib}
)