# xr_smoke: standalone D3D11 test app for the XR layer (ff7vr_xr).
#
# Standalone:   cmake -S tools/xr_smoke -B build/xr_smoke -G Ninja && cmake --build build/xr_smoke
# In the root project it is added after src/xr and reuses that target.

cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(xr_smoke LANGUAGES C CXX)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
  endif()
endif()

get_filename_component(_xr_dir "${CMAKE_CURRENT_LIST_DIR}/../../src/xr" ABSOLUTE)
if(NOT TARGET ff7vr_xr)
  add_subdirectory("${_xr_dir}" "${CMAKE_CURRENT_BINARY_DIR}/ff7vr_xr")
endif()

include("${_xr_dir}/cmake/ff7vr_fxc.cmake")
set(_gen "${CMAKE_CURRENT_BINARY_DIR}/generated/xr_smoke_shaders")
ff7vr_compile_shader(OUT_VAR _pattern_vs SOURCE "${CMAKE_CURRENT_LIST_DIR}/pattern.hlsl"
  PROFILE vs_5_0 ENTRY VSMain VAR g_pattern_vs OUTPUT "${_gen}/pattern_vs.h")
ff7vr_compile_shader(OUT_VAR _pattern_ps SOURCE "${CMAKE_CURRENT_LIST_DIR}/pattern.hlsl"
  PROFILE ps_5_0 ENTRY PSMain VAR g_pattern_ps OUTPUT "${_gen}/pattern_ps.h")

add_executable(xr_smoke main.cpp pattern.hlsl ${_pattern_vs} ${_pattern_ps})
set_source_files_properties(pattern.hlsl PROPERTIES HEADER_FILE_ONLY TRUE)
target_include_directories(xr_smoke PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
# stb_image (PNG read-back for verification) comes from the same pinned stb checkout as ff7vr_xr.
include(FetchContent)
FetchContent_GetProperties(stb)
target_include_directories(xr_smoke SYSTEM PRIVATE "${stb_SOURCE_DIR}")
if(COMMAND ff7vr_set_warnings)
  ff7vr_set_warnings(xr_smoke)
else()
  target_compile_options(xr_smoke PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
  target_compile_definitions(xr_smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)
endif()
target_link_libraries(xr_smoke PRIVATE ff7vr_xr d3d11 dxgi)
set_property(TARGET xr_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
