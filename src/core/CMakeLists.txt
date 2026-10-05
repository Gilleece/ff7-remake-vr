add_library(ff7vr_core STATIC
    log.cpp
    config.cpp
    module.cpp
    pattern.cpp
    hook.cpp
    crash.cpp
    dev_commands.cpp
)
target_include_directories(ff7vr_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ff7vr_core PUBLIC minhook PRIVATE version)
ff7vr_set_warnings(ff7vr_core)

# Self-test / benchmark executable (not deployed). Run: build/<name>/src/core/ff7vr_core_tests.exe
add_executable(ff7vr_core_tests tests/core_tests.cpp)
target_link_libraries(ff7vr_core_tests PRIVATE ff7vr_core)
ff7vr_set_warnings(ff7vr_core_tests)
