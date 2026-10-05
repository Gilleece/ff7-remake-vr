# ff7vr_xr: XR backend abstraction (OpenXR D3D11 backend + Null backend).
# Static library, C++20, static CRT. Knows nothing about Unreal Engine.
#
# Builds inside the root project (add_subdirectory(src/xr)) and standalone
# through tools/xr_smoke, which adds this directory itself when the target is
# missing.

cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(ff7vr_xr LANGUAGES C CXX)
endif()

if(TARGET ff7vr_xr)
  return()
endif()

if(NOT WIN32)
  message(FATAL_ERROR "ff7vr_xr is Windows/D3D11 only")
endif()

# Static CRT everywhere (/MT, /MTd). A parent may already have set this.
if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

get_filename_component(FF7VR_XR_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

# ---------------------------------------------------------------------------
# Dependencies, pinned by commit. Sources live in FETCHCONTENT_BASE_DIR (the
# parent may set it; default third_party/_fetched) and are shared by every
# build tree. When a source tree is already there at the pinned commit it is
# used as is, without touching the network; otherwise it is fetched.
# Build directories always stay inside the current build tree.
# ---------------------------------------------------------------------------
if(NOT DEFINED FETCHCONTENT_BASE_DIR)
  set(FETCHCONTENT_BASE_DIR "${FF7VR_XR_REPO_ROOT}/third_party/_fetched" CACHE PATH "FetchContent base dir")
endif()
include(FetchContent)

# OpenXR-SDK release-1.1.63
set(FF7VR_OPENXR_COMMIT "f2448a8797c85814aa892efc1ab8707900fbcc78")
# stb master 2025-05 (only stb_image_write.h is used)
set(FF7VR_STB_COMMIT "2c980bb59875b0d32144a71867fbdebb2f77cd20")

# Use an already fetched checkout if it is at the pinned commit.
function(ff7vr_xr_use_existing name commit)
  string(TOUPPER "${name}" _up)
  set(_dir "${FETCHCONTENT_BASE_DIR}/${name}-src")
  if(DEFINED FETCHCONTENT_SOURCE_DIR_${_up} OR NOT EXISTS "${_dir}/.git/HEAD")
    return()
  endif()
  file(STRINGS "${_dir}/.git/HEAD" _head LIMIT_COUNT 1)
  if(_head STREQUAL commit)
    set(FETCHCONTENT_SOURCE_DIR_${_up} "${_dir}" PARENT_SCOPE)
  endif()
endfunction()

if(NOT TARGET openxr_loader)
  ff7vr_xr_use_existing(openxr_sdk ${FF7VR_OPENXR_COMMIT})
  # Static loader, no layers, no tests.
  set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
  set(BUILD_SDK_TESTS OFF CACHE BOOL "" FORCE)
  set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
  set(BUILD_LOADER ON CACHE BOOL "" FORCE)
  set(BUILD_ALL_EXTENSIONS OFF CACHE BOOL "" FORCE)
  set(BUILD_WITH_SYSTEM_JSONCPP OFF CACHE BOOL "" FORCE)
  set(OPENXR_DEBUG_POSTFIX "" CACHE STRING "" FORCE)
  FetchContent_Declare(openxr_sdk
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG        ${FF7VR_OPENXR_COMMIT}
    SOURCE_DIR     "${FETCHCONTENT_BASE_DIR}/openxr_sdk-src"
    BINARY_DIR     "${CMAKE_BINARY_DIR}/_deps/openxr_sdk-build"
    SUBBUILD_DIR   "${CMAKE_BINARY_DIR}/_deps/openxr_sdk-subbuild"
    EXCLUDE_FROM_ALL
  )
  FetchContent_MakeAvailable(openxr_sdk)
  # The SDK links its static loader against the DLL CRT; we use the static CRT.
  set_property(TARGET openxr_loader PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

ff7vr_xr_use_existing(stb ${FF7VR_STB_COMMIT})
FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG        ${FF7VR_STB_COMMIT}
  SOURCE_DIR     "${FETCHCONTENT_BASE_DIR}/stb-src"
  BINARY_DIR     "${CMAKE_BINARY_DIR}/_deps/stb-build"
  SUBBUILD_DIR   "${CMAKE_BINARY_DIR}/_deps/stb-subbuild"
)
FetchContent_MakeAvailable(stb)  # header only; stb has no CMakeLists, so this only populates

# ---------------------------------------------------------------------------
# Shaders: compiled at build time with fxc into headers (no d3dcompiler at runtime).
# ---------------------------------------------------------------------------
include("${CMAKE_CURRENT_LIST_DIR}/cmake/ff7vr_fxc.cmake")

set(_shader_out "${CMAKE_CURRENT_BINARY_DIR}/generated/ff7vr_xr_shaders")
ff7vr_compile_shader(OUT_VAR _blit_vs SOURCE "${CMAKE_CURRENT_LIST_DIR}/src/shaders/blit.hlsl"
  PROFILE vs_5_0 ENTRY VSMain VAR g_ff7vr_blit_vs OUTPUT "${_shader_out}/blit_vs.h")
ff7vr_compile_shader(OUT_VAR _blit_ps SOURCE "${CMAKE_CURRENT_LIST_DIR}/src/shaders/blit.hlsl"
  PROFILE ps_5_0 ENTRY PSMain VAR g_ff7vr_blit_ps OUTPUT "${_shader_out}/blit_ps.h")
ff7vr_compile_shader(OUT_VAR _quad_vs SOURCE "${CMAKE_CURRENT_LIST_DIR}/src/shaders/quad.hlsl"
  PROFILE vs_5_0 ENTRY VSMain VAR g_ff7vr_quad_vs OUTPUT "${_shader_out}/quad_vs.h")
ff7vr_compile_shader(OUT_VAR _quad_ps SOURCE "${CMAKE_CURRENT_LIST_DIR}/src/shaders/quad.hlsl"
  PROFILE ps_5_0 ENTRY PSMain VAR g_ff7vr_quad_ps OUTPUT "${_shader_out}/quad_ps.h")

add_library(ff7vr_xr STATIC
  include/ff7vr/xr/xr.h
  include/ff7vr/xr/xr_math.h
  src/xr_math.cpp
  src/xr_common.h
  src/xr_common.cpp
  src/runtime_select.cpp
  src/d3d11_blitter.h
  src/d3d11_blitter.cpp
  src/capture.h
  src/capture.cpp
  src/compositor.h
  src/compositor.cpp
  src/backend_base.h
  src/backend_base.cpp
  src/null_backend.cpp
  src/openxr_backend.cpp
  src/shaders/blit.hlsl
  src/shaders/quad.hlsl
  ${_blit_vs} ${_blit_ps} ${_quad_vs} ${_quad_ps}
)
set_source_files_properties(src/shaders/blit.hlsl src/shaders/quad.hlsl PROPERTIES HEADER_FILE_ONLY TRUE)
target_compile_features(ff7vr_xr PUBLIC cxx_std_20)
target_include_directories(ff7vr_xr
  PUBLIC  "${CMAKE_CURRENT_LIST_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src" "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_include_directories(ff7vr_xr SYSTEM PRIVATE "${stb_SOURCE_DIR}")
target_compile_definitions(ff7vr_xr PRIVATE XR_USE_PLATFORM_WIN32 XR_USE_GRAPHICS_API_D3D11)
if(COMMAND ff7vr_set_warnings)
  ff7vr_set_warnings(ff7vr_xr)  # project-wide flags when built from the root project
else()
  target_compile_options(ff7vr_xr PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /EHsc /wd4324)
  target_compile_definitions(ff7vr_xr PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)
endif()
target_link_libraries(ff7vr_xr
  PUBLIC  d3d11 dxgi
  PRIVATE openxr_loader advapi32)
set_property(TARGET ff7vr_xr PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
