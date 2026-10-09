# upload_probe: measures how fast the graphics card moves data between system memory and its
# own memory, from a process of its own (docs/benchmarking.md, "Video memory and slow phases").

cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(upload_probe LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(upload_probe main.cpp)
if(COMMAND ff7vr_set_warnings)
  ff7vr_set_warnings(upload_probe)
else()
  target_compile_options(upload_probe PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_definitions(upload_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)
endif()
target_link_libraries(upload_probe PRIVATE d3d11 dxgi)
set_property(TARGET upload_probe PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
