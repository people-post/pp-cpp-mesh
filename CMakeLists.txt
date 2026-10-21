cmake_minimum_required(VERSION 3.24)
project(pp-cpp-mesh VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PP_MESH_BUILD_TESTS "Build pp-cpp-mesh tests" OFF)
option(PP_MESH_WARNINGS_AS_ERRORS "Treat compiler warnings as errors for pp-cpp-mesh targets" ON)
# UPnP port mapping (reachability) through the vendored miniupnpc. Off: pp-browser never built it in
# (its CMake looked for a target name miniupnpc does not define), so on is a product decision, not a
# move. No UPnP on mobile either way.
option(PP_MESH_WITH_UPNP "Map ports with UPnP (vendored miniupnpc; not on Android / iOS)" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(PP_MESH_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
include(PpMesh)
include(PpMeshDeps)

if(PP_MESH_BUILD_TESTS)
  enable_testing()
  if(NOT TARGET GTest::gtest)
    include(FetchContent)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
      googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG v1.14.0
    )
    FetchContent_MakeAvailable(googletest)
  endif()
endif()

add_subdirectory(src)

include(GNUInstallDirs)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
