# Shared wire / policy at the root; serve/ = the relay side, client/ = bridges and reserves.
pp_mesh_add_folder_library(circuit
  SOURCES
    CircuitBundleLogic.cpp
    serve/CircuitRelayServer.cpp
    client/CircuitClientCoordinator.cpp
    client/AmpCircuitHopRegistry.cpp
  DEPS shared)

if(PP_MESH_BUILD_TESTS)
  add_subdirectory(tests)
endif()
