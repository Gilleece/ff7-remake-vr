# Generates ff7vr_buildinfo.h with the git commit and build time, refreshed on
# every build (not only at configure time) so the log always identifies the
# exact binary.

set(FF7VR_BUILDINFO_DIR "${CMAKE_BINARY_DIR}/generated")
set(FF7VR_BUILDINFO_HEADER "${FF7VR_BUILDINFO_DIR}/ff7vr_buildinfo.h")

add_custom_target(ff7vr_buildinfo_gen
    COMMAND ${CMAKE_COMMAND}
        -DSRC_DIR=${CMAKE_SOURCE_DIR}
        -DOUT=${FF7VR_BUILDINFO_HEADER}
        -DBUILD_TYPE=$<CONFIG>
        -P ${CMAKE_SOURCE_DIR}/cmake/ff7vr_buildinfo_write.cmake
    BYPRODUCTS ${FF7VR_BUILDINFO_HEADER}
    COMMENT "Updating build identity"
    VERBATIM
)

add_library(ff7vr_buildinfo INTERFACE)
target_include_directories(ff7vr_buildinfo INTERFACE ${FF7VR_BUILDINFO_DIR})
add_dependencies(ff7vr_buildinfo ff7vr_buildinfo_gen)
