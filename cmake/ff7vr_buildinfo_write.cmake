# Script mode: cmake -DSRC_DIR=... -DOUT=... -DBUILD_TYPE=... -P this_file
find_package(Git QUIET)
set(_commit "nogit")
set(_dirty "")
set(_version "unknown")
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
        WORKING_DIRECTORY ${SRC_DIR} OUTPUT_VARIABLE _commit
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        set(_commit "nogit")
    endif()
    execute_process(COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=no
        WORKING_DIRECTORY ${SRC_DIR} OUTPUT_VARIABLE _status
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_status)
        set(_dirty "-dirty")
    endif()
    # Release version from the nearest tag, e.g. "v1.2" or "v1.2-3-gabc1234-dirty".
    execute_process(COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
        WORKING_DIRECTORY ${SRC_DIR} OUTPUT_VARIABLE _describe
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _rc)
    if(_rc EQUAL 0 AND _describe)
        set(_version "${_describe}")
    endif()
endif()
# Numeric parts for the DLL's version resource ("v1.2" -> 1,2,0).
set(_vmaj 0)
set(_vmin 0)
set(_vpat 0)
if(_version MATCHES "^v?([0-9]+)[.]([0-9]+)([.]([0-9]+))?")
    set(_vmaj ${CMAKE_MATCH_1})
    set(_vmin ${CMAKE_MATCH_2})
    if(CMAKE_MATCH_4)
        set(_vpat ${CMAKE_MATCH_4})
    endif()
endif()
string(TIMESTAMP _time "%Y-%m-%dT%H:%M:%S" UTC)

set(_content "#pragma once\n#define FF7VR_VERSION \"${_version}\"\n#define FF7VR_VERSION_NUM ${_vmaj},${_vmin},${_vpat},0\n#define FF7VR_GIT_COMMIT \"${_commit}${_dirty}\"\n#define FF7VR_BUILD_TIME_UTC \"${_time}Z\"\n#define FF7VR_BUILD_TYPE \"${BUILD_TYPE}\"\n")

# Only rewrite when the commit changes, to avoid rebuilding every time.
# The time is therefore the time of the first build at this commit state.
set(_old "")
if(EXISTS ${OUT})
    file(READ ${OUT} _old)
endif()
string(REGEX REPLACE "#define FF7VR_BUILD_TIME_UTC \"[^\"]*\"\n" "" _old_cmp "${_old}")
string(REGEX REPLACE "#define FF7VR_BUILD_TIME_UTC \"[^\"]*\"\n" "" _new_cmp "${_content}")
if(NOT _old_cmp STREQUAL _new_cmp)
    file(WRITE ${OUT} "${_content}")
endif()
