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
    src/fixes.cpp
    src/commands.cpp
    src/ui_layer.cpp
    src/gpu_trace.cpp
    src/movie_watch.cpp
    src/bloom_fix.cpp
    src/uobj.cpp
    src/player.cpp
    src/controls.cpp
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

# Unit tests for the camera and projection math (no game needed):
#   build/<name>/src/engine/ff7vr_engine_tests.exe   (exit 0 = pass)
add_executable(ff7vr_engine_tests tests/engine_tests.cpp src/ue_math.cpp)
target_include_directories(ff7vr_engine_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
ff7vr_set_warnings(ff7vr_engine_tests)
