pp_mesh_add_folder_library(media_plane
  SOURCES
    MeshMediaRelay.cpp
    MediaRelayAttach.cpp
  DEPS connectivity reach host reachability media_relay)

if(PP_MESH_BUILD_TESTS)
  add_subdirectory(tests)
endif()
