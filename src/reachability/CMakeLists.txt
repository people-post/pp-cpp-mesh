set(_sources
  Reachability.cpp
  ReachabilityEngine.cpp
  NatTraversal.cpp
  LanMdnsDiscovery.cpp
  dial_back/DialBackTypes.cpp
  dial_back/serve/DialBackServer.cpp
  dial_back/client/DialBackClient.cpp
  AmpObservedAddrs.cpp
  punch/PunchLogic.cpp
  punch/PunchBurst.cpp
  punch/PunchLinkOps.cpp
  punch/serve/PunchServer.cpp
  punch/client/PunchClientCoordinator.cpp
  MobileEphemeralListenGate.cpp)
if(WIN32)
  list(APPEND _sources LanMdnsSocket_Win32.cpp ReachabilityNetIf_Win32.cpp)
else()
  list(APPEND _sources LanMdnsSocket_Posix.cpp ReachabilityNetIf_Posix.cpp)
endif()
pp_mesh_add_folder_library(reachability
  SOURCES ${_sources}
  DEPS shared)
if(WIN32)
  target_link_libraries(pp_mesh_reachability PUBLIC ws2_32 iphlpapi)
endif()
if(TARGET libminiupnpc-static)
  target_link_libraries(pp_mesh_reachability PRIVATE libminiupnpc-static)
  target_compile_definitions(pp_mesh_reachability PRIVATE PP_MESH_HAS_MINIUPNPC)
endif()

if(PP_MESH_BUILD_TESTS)
  add_subdirectory(tests)
endif()
