# Shared wire / decisions at the root; serve/ = the hop side, client/ = the attaching side.
pp_mesh_add_folder_library(media_relay
  SOURCES
    
    MediaRelayLogic.cpp
    MediaRelayBundleLogic.cpp
    MediaRelayVideoLevels.cpp
    serve/MediaRelayAttachSm.cpp
    serve/MediaRelayServer.cpp
    client/MediaRelayClientCoordinator.cpp
    client/AmpMediaRelayClient.cpp
    
  DEPS shared)

if(PP_MESH_BUILD_TESTS)
  add_subdirectory(tests)
endif()
