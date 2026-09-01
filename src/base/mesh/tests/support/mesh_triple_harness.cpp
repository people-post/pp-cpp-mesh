#include "base/mesh/tests/support/mesh_triple_harness.h"

namespace pbr::test {

Roe<std::unique_ptr<AmpMeshTripleHarness>> AmpMeshTripleHarness::Create() {
  auto harness = std::make_unique<AmpMeshTripleHarness>();
  harness->clock = std::make_shared<adp::VirtualClock>(1'000'000);
  harness->hub = adp::MemoryDatagramIo::MakeHub();
  harness->addr_a = adp::IpEndpoint::V4(10, 0, 0, 1, 1000);
  harness->addr_r = adp::IpEndpoint::V4(10, 0, 0, 3, 3000);
  harness->addr_b = adp::IpEndpoint::V4(10, 0, 0, 2, 2000);
  harness->io_a = std::make_shared<adp::MemoryDatagramIo>(harness->hub, harness->addr_a);
  harness->io_r = std::make_shared<adp::MemoryDatagramIo>(harness->hub, harness->addr_r);
  harness->io_b = std::make_shared<adp::MemoryDatagramIo>(harness->hub, harness->addr_b);
  harness->ep_a = std::make_unique<adp::Endpoint>(harness->io_a, harness->clock);
  harness->ep_r = std::make_unique<adp::Endpoint>(harness->io_r, harness->clock);
  harness->ep_b = std::make_unique<adp::Endpoint>(harness->io_b, harness->clock);
  harness->ep_r->SetAcceptEnabled(true);
  harness->ep_b->SetAcceptEnabled(true);

  auto alice_keys = pp::MlDsa::GenerateKeyPair();
  auto relay_keys = pp::MlDsa::GenerateKeyPair();
  auto bob_keys = pp::MlDsa::GenerateKeyPair();
  if (!alice_keys || !relay_keys || !bob_keys) {
    return Error("amp triple harness: keygen failed");
  }
  harness->alice.ml_dsa_secret_key = std::move(alice_keys->secret_key);
  harness->alice.ml_dsa_public_key = std::move(alice_keys->public_key);
  harness->relay.ml_dsa_secret_key = std::move(relay_keys->secret_key);
  harness->relay.ml_dsa_public_key = std::move(relay_keys->public_key);
  harness->bob.ml_dsa_secret_key = std::move(bob_keys->secret_key);
  harness->bob.ml_dsa_public_key = std::move(bob_keys->public_key);

  auto alice_id = DeriveTestPeerId(harness->alice.ml_dsa_public_key);
  auto relay_id = DeriveTestPeerId(harness->relay.ml_dsa_public_key);
  auto bob_id = DeriveTestPeerId(harness->bob.ml_dsa_public_key);
  if (!alice_id || !relay_id || !bob_id) {
    return Error("amp triple harness: peer id derivation failed");
  }
  harness->peer_id_a = *alice_id;
  harness->peer_id_r = *relay_id;
  harness->peer_id_b = *bob_id;

  const auto link_config = AmpMeshTestLinkConfig();
  harness->runtime_a =
      std::make_unique<amp::MeshRuntime>(*harness->ep_a, harness->alice, harness->peer_id_a, link_config);
  harness->runtime_r =
      std::make_unique<amp::MeshRuntime>(*harness->ep_r, harness->relay, harness->peer_id_r, link_config);
  harness->runtime_b =
      std::make_unique<amp::MeshRuntime>(*harness->ep_b, harness->bob, harness->peer_id_b, link_config);
  harness->runtime_a->Start();
  harness->runtime_r->Start();
  harness->runtime_b->Start();

  auto ma_a = amp::FormatAdpMultiaddr(harness->addr_a, harness->peer_id_a);
  auto ma_r = amp::FormatAdpMultiaddr(harness->addr_r, harness->peer_id_r);
  auto ma_b = amp::FormatAdpMultiaddr(harness->addr_b, harness->peer_id_b);
  if (!ma_a || !ma_r || !ma_b) {
    return Error("amp triple harness: multiaddr format failed");
  }
  harness->ma_a = *ma_a;
  harness->ma_r = *ma_r;
  harness->ma_b = *ma_b;
  return harness;
}

void AmpMeshTripleHarness::PumpAll() {
  runtime_a->Pump();
  runtime_r->Pump();
  runtime_b->Pump();
  runtime_a->Tick();
  runtime_r->Tick();
  runtime_b->Tick();
}

void AmpMeshTripleHarness::PumpUntil(const std::function<bool()>& done, const size_t max_rounds) {
  for (size_t i = 0; i < max_rounds && !done(); ++i) {
    PumpAll();
  }
}

} // namespace pbr::test
