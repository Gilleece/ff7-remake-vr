# Build-time HLSL compilation with fxc.exe from the Windows SDK.
# ff7vr_compile_shader(OUT_VAR <var> SOURCE <hlsl> PROFILE <vs_5_0> ENTRY <main> VAR <c array name> OUTPUT <header>)
#
# fxc is found on PATH (VS developer shell), through WindowsSdkVerBinPath, or
# under the Windows 10/11 SDK root read from the registry.

if(NOT FF7VR_FXC)
  set(_fxc_hints "")
  if(DEFINED ENV{WindowsSdkVerBinPath})
    list(APPEND _fxc_hints "$ENV{WindowsSdkVerBinPath}/x64")
  endif()
  cmake_host_system_information(RESULT _kits_root
    QUERY WINDOWS_REGISTRY "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots" VALUE KitsRoot10 VIEW 64_32)
  if(_kits_root)
    file(TO_CMAKE_PATH "${_kits_root}" _kits_root)
    file(GLOB _fxc_globbed "${_kits_root}/bin/10.*/x64/fxc.exe")
    list(SORT _fxc_globbed COMPARE NATURAL ORDER DESCENDING)
    foreach(_f IN LISTS _fxc_globbed)
      get_filename_component(_d "${_f}" DIRECTORY)
      list(APPEND _fxc_hints "${_d}")
    endforeach()
  endif()
  find_program(FF7VR_FXC NAMES fxc HINTS ${_fxc_hints} REQUIRED)
endif()

function(ff7vr_compile_shader)
  cmake_parse_arguments(A "" "OUT_VAR;SOURCE;PROFILE;ENTRY;VAR;OUTPUT" "" ${ARGN})
  get_filename_component(_dir "${A_OUTPUT}" DIRECTORY)
  file(MAKE_DIRECTORY "${_dir}")
  add_custom_command(
    OUTPUT "${A_OUTPUT}"
    COMMAND "${FF7VR_FXC}" /nologo /O3 /WX /T ${A_PROFILE} /E ${A_ENTRY} /Vn ${A_VAR} /Fh "${A_OUTPUT}" "${A_SOURCE}"
    DEPENDS "${A_SOURCE}"
    COMMENT "fxc ${A_PROFILE} ${A_ENTRY} ${A_SOURCE}"
    VERBATIM)
  set(${A_OUT_VAR} "${A_OUTPUT}" PARENT_SCOPE)
endfunction()
