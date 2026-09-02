#pragma once

#include "amp/L1/Clock.h"
#include "amp/L1/Endpoint.h"
#include "amp/L1/MemoryDatagramIo.h"
#include "crypto/MlDsa.h"
#include "amp/link/AdpMultiaddr.h"
#include "amp/link/MeshRuntime.h"
#include "domain/mesh/host/MeshPorts.h"
#include "domain/mesh/tests/support/mesh_harness_support.h"

#include <functional>
#include <memory>
#include <string>

namespace pbr::test {

/** Two-peer MemoryDatagramIo harness for AMP L4 protocol tests. */
struct AmpMeshHarness {
  std::shared_ptr<pp::adp::VirtualClock> clock;
  std::shared_ptr<pp::adp::MemoryDatagramHub> hub;
  std::shared_ptr<pp::adp::MemoryDatagramIo> io_a;
  std::shared_ptr<pp::adp::MemoryDatagramIo> io_b;
  std::unique_ptr<pp::adp::Endpoint> ep_a;
  std::unique_ptr<pp::adp::Endpoint> ep_b;
  pp::adp::IpEndpoint addr_a;
  pp::adp::IpEndpoint addr_b;
  pp::amp::MshIdentity alice;
  pp::amp::MshIdentity bob;
  std::unique_ptr<pp::amp::MeshRuntime> runtime_a;
  std::unique_ptr<pp::amp::MeshRuntime> runtime_b;
  std::string peer_id_a;
  std::string peer_id_b;
  std::string ma_a;
  std::string ma_b;
  std::unique_ptr<pbr::IChatPeerLinks> chat_links_a;
  std::unique_ptr<pbr::IChatPeerLinks> chat_links_b;

  pp::amp::PeerLinkManager& mgr_a() { return runtime_a->Links(); }
  pp::amp::PeerLinkManager& mgr_b() { return runtime_b->Links(); }
  pbr::IChatPeerLinks& chat_a() { return *chat_links_a; }
  pbr::IChatPeerLinks& chat_b() { return *chat_links_b; }

  static pp::Roe<std::unique_ptr<AmpMeshHarness>> Create();

  void PumpBoth();
  void PumpUntil(const std::function<bool()>& done, size_t max_rounds = 500);
};

} // namespace pbr::test
