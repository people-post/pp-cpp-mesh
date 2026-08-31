# Product libp2p glue (base layer). Fork lives in src/lib/libp2p.

include(pp_lib_paths)

set(PP_BASE_P2P_SOURCES
  Libp2pHost.cpp
  PeerAddressBook.cpp
  CircuitBridgeTarget.cpp
  IdentifyIntegrationService.cpp
  AdvertisedAddrPublisher.cpp
  PeerSessionManager.cpp
  PeerIdUtil.cpp
  NodeRuntime.cpp
  MeshHost.cpp
  DialBackService.cpp
  StreamJsonFrame.cpp
  StreamFrameIo.cpp
  Libp2pScheduler.cpp
  Reachability.cpp
  ReachabilityService.cpp
  NatTraversal.cpp
  CircuitRelayService.cpp
  CircuitRelayRuntime.cpp
  MediaRelayService.cpp
  MediaRelayHostInbound.cpp
  MediaRelayClientAttach.cpp
  MediaRelayFrames.cpp
  MediaRelayAttachSm.cpp
  MediaRelayLogic.cpp
  LanMdnsDiscovery.cpp
  CallMediaDirectService.cpp
  CallMediaSession.cpp
  CallMediaLegCoordinator.cpp
  CallMediaBundleLogic.cpp
  CallMediaSessionLogic.cpp
  CallMediaFrameCrypto.cpp
  CallMediaAdpKey.cpp
  CallMediaAdpPath.cpp
)

if(WIN32)
  list(APPEND PP_BASE_P2P_SOURCES
    LanMdnsSocket_Win32.cpp
    ReachabilityNetIf_Win32.cpp)
else()
  list(APPEND PP_BASE_P2P_SOURCES
    LanMdnsSocket_Posix.cpp
    ReachabilityNetIf_Posix.cpp)
endif()

pp_browser_add_base_library(pp_base_p2p
  SOURCES
    ${PP_BASE_P2P_SOURCES}
  PUBLIC_LIBS
    pp_base_data
    pp_base_crypto
    pp_base_adp
    pp_base_mesh_link
    pp_base_runtime_core
    p2p
    p2p_identify
    p2p_peer_id
    p2p_wire
    pp_common)

target_include_directories(pp_base_p2p PUBLIC ${PP_LIB_LIBP2P_INCLUDE})

if(WIN32)
  target_link_libraries(pp_base_p2p PUBLIC ws2_32 iphlpapi)
endif()
if(TARGET miniupnpc-static)
  target_link_libraries(pp_base_p2p PRIVATE miniupnpc-static)
  target_compile_definitions(pp_base_p2p PUBLIC PP_BROWSER_HAS_MINIUPNPC)
  target_include_directories(pp_base_p2p PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/miniupnpc/include
    ${CMAKE_BINARY_DIR}/third_party/miniupnpc)
endif()

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
