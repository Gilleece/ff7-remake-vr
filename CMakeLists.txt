cmake_minimum_required(VERSION 3.25)

# One shared dependency cache for every build directory. Must be set before any
# FetchContent call.
set(FETCHCONTENT_BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/_fetched" CACHE PATH
    "Shared FetchContent cache for all build directories")
# Once a dependency is fetched, do not hit the network again on reconfigure.
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "")

project(ff7vr LANGUAGES C CXX)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "ff7vr builds with MSVC x64 only (use tools/dev/build.ps1)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static CRT (/MT, /MTd) everywhere, including third-party targets.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ff7vr_warnings)   # ff7vr_set_warnings(<target>)
include(ff7vr_deps)       # pinned third-party dependencies
include(ff7vr_buildinfo)  # generated build identity header

add_subdirectory(src/core)

# Optional modules: added when the directory has a CMakeLists.txt. Each can be
# switched off with -DFF7VR_WITH_<NAME>=OFF (e.g. FF7VR_WITH_XR) to build the
# rest while one module is broken.
foreach(_dir src/xr tools/xr_smoke src/engine src/render src/dev)
    get_filename_component(_name ${_dir} NAME)
    string(TOUPPER ${_name} _NAME)
    option(FF7VR_WITH_${_NAME} "Build ${_dir} if present" ON)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_dir}/CMakeLists.txt")
        message(STATUS "ff7vr: ${_dir} not present, skipped")
    elseif(NOT FF7VR_WITH_${_NAME})
        message(STATUS "ff7vr: ${_dir} disabled (FF7VR_WITH_${_NAME}=OFF)")
    else()
        message(STATUS "ff7vr: adding ${_dir}")
        add_subdirectory(${_dir})
    endif()
endforeach()

# The loader links whatever optional modules exist, so it goes last.
add_subdirectory(src/loader)
