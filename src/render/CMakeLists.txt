# ff7vr_render: D3D11 hooks inside the game, the XR session and the virtual
# screen. Linked into the loader DLL automatically when this target exists
# (FF7VR_HAVE_RENDER).

# NVAPI SDK R615 (MIT): headers and nvapi64.lib, for variable rate shading
# (foveated rendering). The library only loads the driver's nvapi64.dll at run
# time; without an NVIDIA driver NvAPI_Initialize fails and the feature stays off.
include(FetchContent)
set(FF7VR_NVAPI_COMMIT "70d337db9186e968eab622f7e786de7e437faf3d")
if(NOT DEFINED FETCHCONTENT_SOURCE_DIR_NVAPI AND EXISTS "${FETCHCONTENT_BASE_DIR}/nvapi-src/.git/HEAD")
    # A checkout already at the pinned commit is used without network access.
    execute_process(COMMAND git -C "${FETCHCONTENT_BASE_DIR}/nvapi-src" rev-parse HEAD
                    OUTPUT_VARIABLE _nvapi_head OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_nvapi_head STREQUAL FF7VR_NVAPI_COMMIT)
        set(FETCHCONTENT_SOURCE_DIR_NVAPI "${FETCHCONTENT_BASE_DIR}/nvapi-src")
    endif()
endif()
FetchContent_Declare(nvapi
    GIT_REPOSITORY https://github.com/NVIDIA/nvapi.git
    GIT_TAG        ${FF7VR_NVAPI_COMMIT} # R615 developer SDK
    GIT_SHALLOW    FALSE
    SOURCE_SUBDIR  _no_cmake_
)
FetchContent_MakeAvailable(nvapi)
add_library(ff7vr_nvapi INTERFACE)
target_include_directories(ff7vr_nvapi SYSTEM INTERFACE ${nvapi_SOURCE_DIR})
target_link_libraries(ff7vr_nvapi INTERFACE ${nvapi_SOURCE_DIR}/amd64/nvapi64.lib)

add_library(ff7vr_render STATIC
    include/ff7vr/render/render.h
    src/d3d11_hooks.h
    src/d3d11_hooks.cpp
    src/foveation.h
    src/foveation.cpp
    src/timing.h
    src/timing.cpp
    src/xr_controller.h
    src/xr_controller.cpp
    src/render_module.cpp
)
target_include_directories(ff7vr_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ff7vr_render PUBLIC ff7vr_core ff7vr_xr PRIVATE d3d11 dxgi ff7vr_nvapi)
ff7vr_set_warnings(ff7vr_render)
