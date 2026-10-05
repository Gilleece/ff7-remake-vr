# ff7vr_dev: in-game development and measurement tools (frame timer for the
# benchmark). Linked into the loader DLL automatically when this target exists.

add_library(ff7vr_dev STATIC
    cvars.cpp
    dev.cpp
    frame_timer.cpp
)
target_include_directories(ff7vr_dev PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ff7vr_dev PUBLIC ff7vr_core PRIVATE d3d11 dxgi)
ff7vr_set_warnings(ff7vr_dev)
