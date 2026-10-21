pp_mesh_add_folder_library(reach
  SOURCES
    AmpCircuitHopReach.cpp
    PeerReachCoordinator.cpp
    PunchIntroducerWalk.cpp
    SignalingPunchExchange.cpp
    CircuitRendezvousCoordinator.cpp
  DEPS host reachability circuit shared)

if(PP_MESH_BUILD_TESTS)
  add_subdirectory(tests)
endif()
