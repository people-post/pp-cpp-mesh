pp_mesh_add_folder_library(discovery
  SOURCES
    MeshDirectoryCache.cpp
    NameDirectory.cpp
    MeshNodeHitCodec.cpp
    AmpDirectoryProtocol.cpp
    serve/DirectoryServer.cpp
    client/DirectoryClient.cpp
  DEPS dht shared)

if(PP_MESH_BUILD_TESTS)
  add_subdirectory(tests)
endif()
