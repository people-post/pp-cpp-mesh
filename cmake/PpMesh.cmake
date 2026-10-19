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

# pp_mesh_add_library(<name> SOURCES … [DEPS …] [LIBS …]): pp_mesh_<name>, the library of the folder
# this is called from; SOURCES are relative to it. DEPS name other folder libraries (without the
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

# pp_mesh_add_folder_library: pp_mesh_add_library for a folder above the vocabulary — it sits on
# types and Amp. Every folder but types/ uses this one.
function(pp_mesh_add_folder_library name)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPS" ${ARGN})
  pp_mesh_add_library(${name}
    SOURCES ${ARG_SOURCES}
    DEPS types ${ARG_DEPS}
    LIBS pp_crypto pp_amp_l1 pp_amp_link)
endfunction()

# pp_mesh_add_tests(<library> [LINK_LIBS …]): the *_test.cpp of the folder this is called from (a
# library's tests/) as one GoogleTest executable, <library>_test. A test sits with the highest
# library it includes; pp_mesh_testing (the in-memory Amp harnesses) is test support, like GoogleTest.
function(pp_mesh_add_tests lib)
  cmake_parse_arguments(ARG "" "" "LINK_LIBS" ${ARGN})
  file(GLOB _sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")
  if(NOT _sources)
    message(FATAL_ERROR "pp_mesh_add_tests(${lib}): no *_test.cpp in ${CMAKE_CURRENT_SOURCE_DIR}")
  endif()
  add_executable(${lib}_test ${_sources})
  target_link_libraries(${lib}_test PRIVATE GTest::gtest GTest::gtest_main ${lib} ${ARG_LINK_LIBS})
  pp_mesh_apply_warnings(${lib}_test)
  include(GoogleTest)
  gtest_discover_tests(${lib}_test DISCOVERY_TIMEOUT 60)
endfunction()
