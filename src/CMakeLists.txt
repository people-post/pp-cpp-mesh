# One static library per folder; the DEPS below are the allowed include edges
# (scripts/check_layers.sh keeps the two in step). Bottom to top:
#
#   types          (the vocabulary product code shares: directory, hops, relay scope, disclosure,
#                   config — pp-cpp-common only, so an app's lower layers can use it without Amp)
#   shared (shared/ + l4/shared/)
#   reachability   dht <- discovery   circuit <- media_relay
#   host           (MeshHost: composes the services above)
#   reach          (reach / rendezvous / punch walk over MeshHost)
#   connectivity   (MeshConnectivity: dial registry, listen book, circuit reach, rendezvous, punch walk)
#   media_plane    (MeshMediaRelay, MediaRelayAttach)
#   testing        (in-memory Amp harnesses for this repo's tests and consumers' — not in pp_mesh)
#
# `pp_mesh` aggregates them for consumers.

# Every folder but types sits on Amp and the vocabulary.
function(_pp_mesh_library name)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPS" ${ARGN})
  pp_mesh_add_library(${name}
    SOURCES ${ARG_SOURCES}
    DEPS types ${ARG_DEPS}
    LIBS pp_crypto pp_amp_l1 pp_amp_link)
endfunction()

pp_mesh_add_library(types
  SOURCES
    types/AddressDisclosure.cpp
    types/MeshHopDial.cpp
    types/PublicAddress.cpp
  LIBS pp_common)

_pp_mesh_library(shared
  SOURCES
    l4/shared/MediaFrameBody.cpp)

set(_pp_mesh_reachability_sources
  reachability/Reachability.cpp
  reachability/ReachabilityEngine.cpp
  reachability/NatTraversal.cpp
  reachability/LanMdnsDiscovery.cpp
  reachability/dial_back/DialBackTypes.cpp
  reachability/dial_back/serve/DialBackServer.cpp
  reachability/dial_back/client/DialBackClient.cpp
  reachability/AmpObservedAddrs.cpp
  reachability/punch/PunchLogic.cpp
  reachability/punch/PunchBurst.cpp
  reachability/punch/PunchLinkOps.cpp
  reachability/punch/serve/PunchServer.cpp
  reachability/punch/client/PunchClientCoordinator.cpp
  reachability/MobileEphemeralListenGate.cpp)
if(WIN32)
  list(APPEND _pp_mesh_reachability_sources
    reachability/LanMdnsSocket_Win32.cpp
    reachability/ReachabilityNetIf_Win32.cpp)
else()
  list(APPEND _pp_mesh_reachability_sources
    reachability/LanMdnsSocket_Posix.cpp
    reachability/ReachabilityNetIf_Posix.cpp)
endif()
_pp_mesh_library(reachability
  SOURCES ${_pp_mesh_reachability_sources}
  DEPS shared)
if(WIN32)
  target_link_libraries(pp_mesh_reachability PUBLIC ws2_32 iphlpapi)
endif()
if(TARGET libminiupnpc-static)
  target_link_libraries(pp_mesh_reachability PRIVATE libminiupnpc-static)
  target_compile_definitions(pp_mesh_reachability PRIVATE PP_MESH_HAS_MINIUPNPC)
endif()

_pp_mesh_library(dht
  SOURCES
    dht/DhtRecordCodec.cpp
    dht/DhtRecordStore.cpp
    dht/DhtRateLimiter.cpp
    dht/AmpDhtProtocol.cpp
    dht/serve/DhtServer.cpp
    dht/client/DhtClient.cpp
  DEPS shared)

_pp_mesh_library(discovery
  SOURCES
    discovery/MeshDirectoryCache.cpp
    discovery/NameDirectory.cpp
    discovery/MeshNodeHitCodec.cpp
    discovery/AmpDirectoryProtocol.cpp
    discovery/serve/DirectoryServer.cpp
    discovery/client/DirectoryClient.cpp
  DEPS dht shared)

# circuit: shared wire / policy at the root; serve/ = the relay side, client/ = bridges and reserves.
_pp_mesh_library(circuit
  SOURCES
    l4/circuit/CircuitBundleLogic.cpp
    l4/circuit/serve/CircuitRelayServer.cpp
    l4/circuit/client/CircuitClientCoordinator.cpp
    l4/circuit/client/AmpCircuitHopRegistry.cpp
  DEPS shared)

# media_relay: shared wire / decisions at the root; serve/ = the hop side, client/ = the attaching side.
_pp_mesh_library(media_relay
  SOURCES
    l4/media_relay/MediaRelayFrames.cpp
    l4/media_relay/MediaRelayLogic.cpp
    l4/media_relay/MediaRelayBundleLogic.cpp
    l4/media_relay/MediaRelayVideoLevels.cpp
    l4/media_relay/serve/MediaRelayAttachSm.cpp
    l4/media_relay/serve/MediaRelayServer.cpp
    l4/media_relay/client/MediaRelayClientCoordinator.cpp
    l4/media_relay/client/AmpMediaRelayClient.cpp
    l4/media_relay/client/MediaRelayFrameCrypto.cpp
  DEPS circuit shared)

_pp_mesh_library(host
  SOURCES
    host/AmpLinkConfig.cpp
    host/LocalNetworkChange.cpp
    host/MeshHost.cpp
    host/MeshLinkEventLog.cpp
    host/DisclosureGatedPeerLinks.cpp
    host/MeshPorts.cpp
    host/MeshPumpThread.cpp
  DEPS reachability dht discovery circuit media_relay shared)

_pp_mesh_library(reach
  SOURCES
    reach/AmpCircuitHopReach.cpp
    reach/PeerReachCoordinator.cpp
    reach/PunchIntroducerWalk.cpp
    reach/SignalingPunchExchange.cpp
    reach/CircuitRendezvousCoordinator.cpp
  DEPS host reachability circuit shared)

_pp_mesh_library(connectivity
  SOURCES
    connectivity/MeshConnectivity.cpp
  DEPS reach host reachability)

_pp_mesh_library(media_plane
  SOURCES
    media_plane/MeshMediaRelay.cpp
    media_plane/MediaRelayAttach.cpp
  DEPS connectivity reach host reachability media_relay)

_pp_mesh_library(testing
  SOURCES
    testing/mesh_harness_support.cpp
    testing/mesh_test_harness.cpp
    testing/mesh_triple_harness.cpp
  DEPS host shared)

# Everything but the test harness.
add_library(pp_mesh INTERFACE)
target_link_libraries(pp_mesh INTERFACE
  pp_mesh_media_plane
  pp_mesh_connectivity
  pp_mesh_reach
  pp_mesh_host
  pp_mesh_media_relay
  pp_mesh_circuit
  pp_mesh_discovery
  pp_mesh_dht
  pp_mesh_reachability
  pp_mesh_shared
  pp_mesh_types)
