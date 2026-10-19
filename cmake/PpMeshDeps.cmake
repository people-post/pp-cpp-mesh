# pp-cpp-common, pp-cpp-crypto and pp-cpp-amp: release tags through FetchContent, or a local checkout
# through PP_CPP_<NAME>_SOURCE_DIR. A consumer that already has the targets (pp-browser) keeps its own.
# There is no implicit sibling-checkout fallback: a stale ../<repo> must not build silently.

include(FetchContent)

function(pp_mesh_dependency name target repo tag)
  string(TOUPPER "${name}" _upper)
  string(REPLACE "-" "_" _upper "${_upper}")
  set(${_upper}_SOURCE_DIR "" CACHE PATH "Optional local checkout of ${name} (overrides FetchContent)")
  set(${_upper}_GIT_TAG "${tag}" CACHE STRING "Release tag on ${name}")
  if(TARGET ${target})
    return()
  endif()
  string(REPLACE "-" "_" _content "${name}")
  if(${_upper}_SOURCE_DIR)
    add_subdirectory("${${_upper}_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/${_content}-build" EXCLUDE_FROM_ALL)
  else()
    FetchContent_Declare(${_content} GIT_REPOSITORY "${repo}" GIT_TAG "${${_upper}_GIT_TAG}")
    FetchContent_MakeAvailable(${_content})
  endif()
  if(NOT TARGET ${target})
    message(FATAL_ERROR "pp-cpp-mesh requires target ${target} from ${name}")
  endif()
endfunction()

set(PP_COMMON_BUILD_TESTS OFF CACHE BOOL "Build pp-cpp-common tests")
set(PP_CRYPTO_BUILD_TESTS OFF CACHE BOOL "Build pp-cpp-crypto tests")
set(PP_AMP_BUILD_TESTS OFF CACHE BOOL "Build pp-cpp-amp tests")

# Order matters: pp-cpp-amp takes pp_common / pp_crypto from here when they already exist.
pp_mesh_dependency(pp-cpp-common pp_common
  https://github.com/people-post/pp-cpp-common.git v1.2.0)
pp_mesh_dependency(pp-cpp-crypto pp_crypto
  https://github.com/people-post/pp-cpp-crypto.git v1.0.0)
pp_mesh_dependency(pp-cpp-amp pp_amp_link
  https://github.com/people-post/pp-cpp-amp.git v2.24.0)

if(PP_MESH_WITH_UPNP AND NOT (ANDROID OR IOS) AND NOT TARGET libminiupnpc-static)
  set(UPNPC_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(UPNPC_BUILD_STATIC ON CACHE BOOL "" FORCE)
  set(UPNPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(UPNPC_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
  set(UPNPC_NO_INSTALL ON CACHE BOOL "" FORCE)
  add_subdirectory("${PP_MESH_SOURCE_ROOT}/third_party/miniupnpc"
                   "${CMAKE_BINARY_DIR}/_deps/pp_mesh_miniupnpc-build" EXCLUDE_FROM_ALL)
endif()
