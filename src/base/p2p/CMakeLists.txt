# Product libp2p glue (base layer). Fork lives in src/lib/libp2p.

include(PpBrowserLib)
include(PpBrowserWarnings)

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
  CallMediaSessionLogic.cpp
  CallMediaFrameCrypto.cpp
)

add_library(pp_base_p2p STATIC ${PP_BASE_P2P_SOURCES})
target_include_directories(pp_base_p2p PUBLIC
  ${CMAKE_SOURCE_DIR}/src
  ${PP_LIB_LIBP2P_INCLUDE})
target_link_libraries(pp_base_p2p PUBLIC
  pp_common
  pp_base_data
  pp_base_crypto
  p2p
  p2p_identify
  p2p_peer_id
  p2p_wire
  nlohmann_json::nlohmann_json)
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
pp_browser_apply_warnings(pp_base_p2p)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
