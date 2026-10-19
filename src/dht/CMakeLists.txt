pp_mesh_add_folder_library(dht
  SOURCES
    DhtRecordCodec.cpp
    DhtRecordStore.cpp
    DhtRateLimiter.cpp
    AmpDhtProtocol.cpp
    serve/DhtServer.cpp
    client/DhtClient.cpp
  DEPS shared)

if(PP_MESH_BUILD_TESTS)
  add_subdirectory(tests)
endif()
