# ff7vr_engine: bindings to the game's Unreal Engine 4.18 fork. Installs our stereo
# rendering device into the engine, builds the per-eye cameras and sizes the eye render
# target. See docs/engine-module.md.

add_library(ff7vr_engine STATIC
    src/engine.cpp
    src/addresses.cpp
    src/stereo_device.cpp
    src/ue_math.cpp
    src/fixed_host.cpp
    src/rhi_command.cpp
    src/mirror.cpp
    src/cvars.cpp
    src/graphics.cpp
    src/fixes.cpp
    src/commands.cpp
    src/ui_layer.cpp
    src/gpu_trace.cpp
    src/shaders.cpp
    src/movie_watch.cpp
    src/audio_listener.cpp
    src/bloom_fix.cpp
    src/distortion_fix.cpp
    src/lens_vignette.cpp
    src/uobj.cpp
    src/player.cpp
    src/controls.cpp
    src/snap_turn.cpp
    src/menu_items.cpp
    src/head_move.cpp
    src/sce_pad.cpp
    src/scene_depth.cpp
    src/late_update.cpp
)
target_include_directories(ff7vr_engine
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ff7vr_engine PUBLIC ff7vr_core PRIVATE d3d11 dxgi)
ff7vr_set_warnings(ff7vr_engine)

# With the render module in the build, the eye size, views and eye texture go through its
# XR session (src/render_host.cpp). src/render is added after this directory, so test for
# it the way the root CMakeLists.txt does.
if(EXISTS "${PROJECT_SOURCE_DIR}/src/render/CMakeLists.txt" AND (NOT DEFINED FF7VR_WITH_RENDER OR FF7VR_WITH_RENDER))
    target_sources(ff7vr_engine PRIVATE src/render_host.cpp)
    target_link_libraries(ff7vr_engine PRIVATE ff7vr_render)
    target_compile_definitions(ff7vr_engine PRIVATE FF7VR_ENGINE_WITH_RENDER=1)
endif()

# Optional: NVIDIA DLSS in place of the game's temporal anti-aliasing (src/dlss.cpp,
# docs/dlss.md). Off by default. The NVIDIA DLSS SDK has its own licence and is not part of
# this repository: with -DFF7VR_DLSS=ON it is taken from FF7VR_DLSS_SDK_DIR, or fetched
# into third_party/_fetched/dlss-sdk (github.com/NVIDIA/DLSS) if that folder is missing.
# tools/dev/build.ps1 -With dlss / -Without dlss pass FF7VR_WITH_DLSS: the same switch.
if(DEFINED FF7VR_WITH_DLSS)
    set(FF7VR_DLSS ${FF7VR_WITH_DLSS} CACHE BOOL "Build the DLSS anti-aliasing (needs the NVIDIA DLSS SDK)" FORCE)
endif()
option(FF7VR_DLSS "Build the DLSS anti-aliasing (needs the NVIDIA DLSS SDK)" OFF)
if(FF7VR_DLSS)
    set(FF7VR_DLSS_SDK_DIR "${FETCHCONTENT_BASE_DIR}/dlss-sdk" CACHE PATH "NVIDIA DLSS SDK checkout")
    if(NOT EXISTS "${FF7VR_DLSS_SDK_DIR}/include/nvsdk_ngx.h")
        include(FetchContent)
        FetchContent_Declare(dlss_sdk
            GIT_REPOSITORY https://github.com/NVIDIA/DLSS.git
            GIT_TAG        374959484e79a640feaba44c93ac8cfb0a03f5b5 # DLSS 310.9.1 SDK
            SOURCE_DIR     "${FF7VR_DLSS_SDK_DIR}"
            SOURCE_SUBDIR  _no_cmake_
        )
        FetchContent_MakeAvailable(dlss_sdk)
    endif()
    set(_ngx_lib "${FF7VR_DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_s$<$<CONFIG:Debug>:_dbg>.lib")
    if(EXISTS "${FF7VR_DLSS_SDK_DIR}/include/nvsdk_ngx.h")
        message(STATUS "ff7vr: DLSS built in (SDK ${FF7VR_DLSS_SDK_DIR})")
        target_sources(ff7vr_engine PRIVATE src/dlss.cpp)
        target_include_directories(ff7vr_engine SYSTEM PRIVATE "${FF7VR_DLSS_SDK_DIR}/include")
        target_link_libraries(ff7vr_engine PRIVATE "${_ngx_lib}")
        target_compile_definitions(ff7vr_engine PRIVATE FF7VR_ENGINE_WITH_DLSS=1)
    else()
        message(WARNING "ff7vr: FF7VR_DLSS=ON but no DLSS SDK at ${FF7VR_DLSS_SDK_DIR}; built without DLSS")
    endif()
endif()

# Unit tests for the camera and projection math (no game needed):
#   build/<name>/src/engine/ff7vr_engine_tests.exe   (exit 0 = pass)
add_executable(ff7vr_engine_tests tests/engine_tests.cpp src/ue_math.cpp)
target_include_directories(ff7vr_engine_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
ff7vr_set_warnings(ff7vr_engine_tests)
