#pragma once

#include "base/adp/Clock.h"
#include "base/adp/Endpoint.h"
#include "base/adp/MemoryDatagramIo.h"
#include "base/crypto/MlDsa.h"
#include "base/mesh/link/AdpMultiaddr.h"
#include "base/mesh/link/MeshRuntime.h"
#include "base/mesh/tests/support/mesh_harness_support.h"

#include <functional>
#include <memory>
#include <string>

namespace pbr::test {

/** Three-peer MemoryDatagramIo harness (client A, relay R, target B) for AMP circuit tests. */
struct AmpMeshTripleHarness {
  std::shared_ptr<adp::VirtualClock> clock;
  std::shared_ptr<adp::MemoryDatagramHub> hub;
  std::shared_ptr<adp::MemoryDatagramIo> io_a;
  std::shared_ptr<adp::MemoryDatagramIo> io_r;
  std::shared_ptr<adp::MemoryDatagramIo> io_b;
  std::unique_ptr<adp::Endpoint> ep_a;
  std::unique_ptr<adp::Endpoint> ep_r;
  std::unique_ptr<adp::Endpoint> ep_b;
  adp::IpEndpoint addr_a;
  adp::IpEndpoint addr_r;
  adp::IpEndpoint addr_b;
  amp::MshIdentity alice;
  amp::MshIdentity relay;
  amp::MshIdentity bob;
  std::unique_ptr<amp::MeshRuntime> runtime_a;
  std::unique_ptr<amp::MeshRuntime> runtime_r;
  std::unique_ptr<amp::MeshRuntime> runtime_b;
  std::string peer_id_a;
  std::string peer_id_r;
  std::string peer_id_b;
  std::string ma_a;
  std::string ma_r;
  std::string ma_b;

  amp::PeerLinkManager& mgr_a() { return runtime_a->Links(); }
  amp::PeerLinkManager& mgr_r() { return runtime_r->Links(); }
  amp::PeerLinkManager& mgr_b() { return runtime_b->Links(); }

  static Roe<std::unique_ptr<AmpMeshTripleHarness>> Create();

  void PumpAll();
  void PumpUntil(const std::function<bool()>& done, size_t max_rounds = 800);
};

} // namespace pbr::test
