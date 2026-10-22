// The reachability probe against a seed the mesh is already linked to.
#include "mesh/reachability/ReachabilityEngine.h"
#include "mesh/reachability/dial_back/client/DialBackClient.h"
#include "mesh/reachability/dial_back/serve/DialBackServer.h"

#include "mesh/testing/mesh_test_harness.h"

#include <gtest/gtest.h>

namespace pp::mesh {
namespace {

// The probe dials the seed under its PeerId — the key the rest of the mesh uses — so it rides the
// link that is already up instead of opening a second one that would drop it.
TEST(ReachabilityEngineProbeTest, ItUsesTheLinkToTheSeedThatIsAlreadyUp) {
  auto created = pp::mesh::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto h = std::move(*created);
  ASSERT_TRUE(static_cast<bool>(h->mgr_a().RegisterEndpoint(h->peer_id_b, h->ma_b)));

  DialBackServer seed(*h->runtime_b);
  auto pump = [&]() { h->PumpBoth(); };
  DialBackClient dial_back(*h->runtime_a, pump);
  seed.Start();
  dial_back.Start();

  bool linked = false;
  h->mgr_a().EnsureAssociation(h->peer_id_b, [&](pp::amp::PeerLinkManager::LinkRoe link) { linked = bool(link); });
  h->PumpUntil([&]() { return linked; });
  ASSERT_TRUE(linked);
  const pp::amp::LinkHandle before = h->mgr_a().GetSnapshotByPeerId(h->peer_id_b).handle;

  ReachabilityEngine engine;
  AmpReachabilityProbeDeps deps;
  deps.links = &h->mgr_a();
  deps.dial_back = &dial_back;
  deps.amp_listen_multiaddr = h->ma_a;
  deps.local_peer_id = h->peer_id_a;
  deps.bootstrap_peers = {h->ma_b};
  deps.io_pump = pump;
  engine.RunProbeBlocking(deps);

  EXPECT_TRUE(engine.Snapshot().signals.seed_dial_ok) << engine.Snapshot().signals.seed_dial_error;
  EXPECT_FALSE(h->mgr_a().GetLinkSnapshot("reachability:seed").has_endpoint) << "no second key for the seed";
  h->PumpBoth();
  EXPECT_TRUE(h->mgr_a().IsConnectedToPeerId(h->peer_id_b));
  EXPECT_TRUE(h->mgr_a().GetSnapshotByPeerId(h->peer_id_b).handle == before) << "the same link, not a replacement";

  dial_back.Stop();
  seed.Stop();
}

} // namespace
} // namespace pp::mesh
