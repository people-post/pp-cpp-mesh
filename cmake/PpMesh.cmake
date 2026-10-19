# Helpers for the pp-cpp-mesh folder libraries.

# Warnings for pp-cpp-mesh's own targets (not third_party/).
function(pp_mesh_apply_warnings target)
  if(MSVC)
    # CRT secure deprecations (getenv / localtime: portable code uses the ISO APIs); C4100 unused
    # parameter and C4458 member hiding, as on GCC / Clang below.
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(${target} PRIVATE /W4 /wd4100 /wd4458)
    if(PP_MESH_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU|AppleClang")
    target_compile_options(${target} PRIVATE
      -Wall
      -Wextra
      -Wno-missing-field-initializers
      -Wno-unused-parameter)
    if(PP_MESH_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

# pp_mesh_<name>: a static library of one folder. DEPS name other folder libraries (without the
# pp_mesh_ prefix) — they are the allowed include edges (scripts/check_layers.sh); LIBS are external.
function(pp_mesh_add_library name)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPS;LIBS" ${ARGN})
  set(target pp_mesh_${name})
  add_library(${target} STATIC ${ARG_SOURCES})
  target_include_directories(${target} PUBLIC
    $<BUILD_INTERFACE:${PP_MESH_SOURCE_ROOT}/include>
    $<INSTALL_INTERFACE:include>)
  set(_deps "")
  foreach(dep IN LISTS ARG_DEPS)
    list(APPEND _deps pp_mesh_${dep})
  endforeach()
  target_link_libraries(${target} PUBLIC ${_deps} ${ARG_LIBS})
  if(MSVC)
    target_compile_definitions(${target} PUBLIC NOMINMAX)
  endif()
  pp_mesh_apply_warnings(${target})
endfunction()
