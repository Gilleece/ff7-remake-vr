# ff7vr: the DLL the game loads. Built as xinput1_3.dll (see docs/dev-harness.md
# for why) and deployed to End\Binaries\Win64 next to ff7remake_.exe.

add_library(ff7vr SHARED
    dllmain.cpp
    startup.cpp
    xinput_proxy.cpp
    dev_input.cpp
    xinput1_3.def
    version.rc
)
set_target_properties(ff7vr PROPERTIES
    OUTPUT_NAME xinput1_3
    PDB_NAME xinput1_3
)
target_link_libraries(ff7vr PRIVATE ff7vr_core ff7vr_buildinfo)
ff7vr_set_warnings(ff7vr)

# Optional modules (src/engine, src/render, src/xr, src/dev): link them and tell the code which exist.
foreach(_mod engine render xr dev)
    string(TOUPPER ${_mod} _MOD)
    if(TARGET ff7vr_${_mod})
        target_link_libraries(ff7vr PRIVATE ff7vr_${_mod})
        target_compile_definitions(ff7vr PRIVATE FF7VR_HAVE_${_MOD}=1)
    else()
        target_compile_definitions(ff7vr PRIVATE FF7VR_HAVE_${_MOD}=0)
    endif()
endforeach()

# Copy the dev ini next to the DLL so tools/dev/deploy.ps1 finds everything in one place.
add_custom_command(TARGET ff7vr POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/ff7vr.ini $<TARGET_FILE_DIR:ff7vr>/ff7vr.ini
    VERBATIM
)
