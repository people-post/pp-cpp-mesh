#include "domain/mesh/tests/support/mesh_test_harness.h"

namespace pbr::test {

pp::Roe<std::unique_ptr<AmpMeshHarness>> AmpMeshHarness::Create() {
  auto harness = std::make_unique<AmpMeshHarness>();
  harness->clock = std::make_shared<pp::adp::VirtualClock>(1'000'000);
  harness->hub = pp::adp::MemoryDatagramIo::MakeHub();
  harness->addr_a = pp::adp::IpEndpoint::V4(10, 0, 0, 1, 1000);
  harness->addr_b = pp::adp::IpEndpoint::V4(10, 0, 0, 2, 2000);
  harness->io_a = std::make_shared<pp::adp::MemoryDatagramIo>(harness->hub, harness->addr_a);
  harness->io_b = std::make_shared<pp::adp::MemoryDatagramIo>(harness->hub, harness->addr_b);
  harness->ep_a = std::make_unique<pp::adp::Endpoint>(harness->io_a, harness->clock);
  harness->ep_b = std::make_unique<pp::adp::Endpoint>(harness->io_b, harness->clock);
  harness->ep_b->SetAcceptEnabled(true);

  auto alice_keys = pp::MlDsa::GenerateKeyPair();
  auto bob_keys = pp::MlDsa::GenerateKeyPair();
  if (!alice_keys || !bob_keys) {
    return pp::Error("amp mesh harness: keygen failed");
  }
  harness->alice.ml_dsa_secret_key = std::move(alice_keys->secret_key);
  harness->alice.ml_dsa_public_key = std::move(alice_keys->public_key);
  harness->bob.ml_dsa_secret_key = std::move(bob_keys->secret_key);
  harness->bob.ml_dsa_public_key = std::move(bob_keys->public_key);

  auto alice_id = DeriveTestPeerId(harness->alice.ml_dsa_public_key);
  auto bob_id = DeriveTestPeerId(harness->bob.ml_dsa_public_key);
  if (!alice_id || !bob_id) {
    return pp::Error("amp mesh harness: peer id derivation failed");
  }
  harness->peer_id_a = *alice_id;
  harness->peer_id_b = *bob_id;

  const auto link_config = AmpMeshTestLinkConfig();
  harness->runtime_a = std::make_unique<pp::amp::MeshRuntime>(*harness->ep_a, harness->alice, harness->peer_id_a,
                                                          link_config);
  harness->runtime_b = std::make_unique<pp::amp::MeshRuntime>(*harness->ep_b, harness->bob, harness->peer_id_b,
                                                          link_config);
  harness->runtime_a->Start();
  harness->runtime_b->Start();
  harness->chat_links_a = pbr::NewAmpChatPeerLinks(harness->runtime_a->Links());
  harness->chat_links_b = pbr::NewAmpChatPeerLinks(harness->runtime_b->Links());

  auto ma_b = pp::amp::FormatAdpMultiaddr(harness->addr_b, harness->peer_id_b);
  auto ma_a = pp::amp::FormatAdpMultiaddr(harness->addr_a, harness->peer_id_a);
  if (!ma_b || !ma_a) {
    return pp::Error("amp mesh harness: multiaddr format failed");
  }
  harness->ma_a = *ma_a;
  harness->ma_b = *ma_b;
  return harness;
}

void AmpMeshHarness::PumpBoth() {
  runtime_a->Pump();
  runtime_b->Pump();
  runtime_a->Tick();
  runtime_b->Tick();
}

void AmpMeshHarness::PumpUntil(const std::function<bool()>& done, const size_t max_rounds) {
  for (size_t i = 0; i < max_rounds && !done(); ++i) {
    PumpBoth();
  }
}

} // namespace pbr::test
