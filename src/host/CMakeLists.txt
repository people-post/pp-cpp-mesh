pp_mesh_add_folder_library(host
  SOURCES
    AmpLinkConfig.cpp
    LocalNetworkChange.cpp
    MeshHost.cpp
    MeshLinkEventLog.cpp
    DisclosureGatedPeerLinks.cpp
    MeshPorts.cpp
    MeshPumpThread.cpp
  DEPS reachability dht discovery circuit media_relay shared)

if(PP_MESH_BUILD_TESTS)
  add_subdirectory(tests)
endif()
