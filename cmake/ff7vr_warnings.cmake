# Project-wide warning and code-generation settings for our own targets.
# Third-party targets do not get these.

function(ff7vr_set_warnings target)
    target_compile_options(${target} PRIVATE
        /W4
        /permissive-
        /utf-8
        /Zc:__cplusplus
        /Zc:preprocessor
        /EHsc
        /w44265   # class has virtual functions but destructor is not virtual
        /w44062   # enumerator not handled in switch
        /wd4324   # structure padded due to alignment specifier
    )
    if(FF7VR_WARNINGS_AS_ERRORS)
        target_compile_options(${target} PRIVATE /WX)
    endif()
    target_compile_definitions(${target} PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
        _WIN32_WINNT=0x0A00
    )
endfunction()

option(FF7VR_WARNINGS_AS_ERRORS "Treat warnings in ff7vr targets as errors" ON)

# PDBs for release builds too: crash reports are resolved against them.
string(APPEND CMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO " /OPT:REF /OPT:ICF")
string(APPEND CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO " /OPT:REF /OPT:ICF")
