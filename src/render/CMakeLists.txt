# ff7vr_render: D3D11 hooks inside the game, the XR session and the virtual
# screen. Linked into the loader DLL automatically when this target exists
# (FF7VR_HAVE_RENDER).

add_library(ff7vr_render STATIC
    include/ff7vr/render/render.h
    src/d3d11_hooks.h
    src/d3d11_hooks.cpp
    src/timing.h
    src/timing.cpp
    src/xr_controller.h
    src/xr_controller.cpp
    src/render_module.cpp
)
target_include_directories(ff7vr_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ff7vr_render PUBLIC ff7vr_core ff7vr_xr PRIVATE d3d11 dxgi)
ff7vr_set_warnings(ff7vr_render)
