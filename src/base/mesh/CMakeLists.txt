# PeerId derivation (retained mesh PeerId + ML-DSA wire). Separate from product MeshHost
# so mesh/link tests can use real PeerIds without linking pp_base_mesh (SRC_LAYOUT: mesh ↛ p2p).

include(pp_lib_paths)

pp_browser_add_base_library(pp_base_peer_id
  SOURCES
    PeerIdUtil.cpp
  PUBLIC_LIBS
    pp_base_crypto
    p2p_peer_id
    p2p_wire
    pp_common)

target_include_directories(pp_base_peer_id PUBLIC ${PP_LIB_LIBP2P_INCLUDE})

# Product Amp mesh glue (base layer). Libp2p Host/Yamux/Noise L4 retired from product (A017/D10).

set(PP_BASE_MESH_SOURCES
  MeshHost.cpp
  LengthPrefixedCodec.cpp
  Reachability.cpp
  ReachabilityService.cpp
  NatTraversal.cpp
  MediaRelayFrames.cpp
  MediaRelayAttachSm.cpp
  MediaRelayLogic.cpp
  LanMdnsDiscovery.cpp
  CallMediaLegCoordinator.cpp
  CallMediaAmpTransport.cpp
  CallMediaBundleLogic.cpp
  CallMediaSessionLogic.cpp
  CallMediaFrameCrypto.cpp
  AmpCircuitHopRegistry.cpp
  CircuitBundleLogic.cpp
  CircuitTunnelCoordinator.cpp
  MediaRelayBundleLogic.cpp
  AmpMediaRelayCoordinator.cpp
  AmpDialBackService.cpp
)

if(WIN32)
  list(APPEND PP_BASE_MESH_SOURCES
    LanMdnsSocket_Win32.cpp
    ReachabilityNetIf_Win32.cpp)
else()
  list(APPEND PP_BASE_MESH_SOURCES
    LanMdnsSocket_Posix.cpp
    ReachabilityNetIf_Posix.cpp)
endif()

pp_browser_add_base_library(pp_base_mesh
  SOURCES
    ${PP_BASE_MESH_SOURCES}
  PUBLIC_LIBS
    pp_base_data
    pp_base_crypto
    pp_base_adp
    pp_base_mesh_link
    pp_base_peer_id
    pp_base_runtime_core
    pp_common)

if(WIN32)
  target_link_libraries(pp_base_mesh PUBLIC ws2_32 iphlpapi)
endif()
if(TARGET miniupnpc-static)
  target_link_libraries(pp_base_mesh PRIVATE miniupnpc-static)
  target_compile_definitions(pp_base_mesh PUBLIC PP_BROWSER_HAS_MINIUPNPC)
  target_include_directories(pp_base_mesh PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/miniupnpc/include
    ${CMAKE_BINARY_DIR}/third_party/miniupnpc)
endif()

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
