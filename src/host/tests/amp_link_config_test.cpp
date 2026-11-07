#include "mesh/host/AmpLinkConfig.h"

#include <gtest/gtest.h>

using pp::mesh::AmpLinkTable;
using pp::mesh::MakeProductAmpLinkConfig;

// A device keeps Amp's small table; a host others connect to gets one link per client, or it
// refuses newcomers once a handful of devices and dead links fill it ("its link table is full").
TEST(AmpLinkConfigTest, AServerHasRoomForItsClients) {
  const pp::amp::PeerLinkConfig device = MakeProductAmpLinkConfig();
  const pp::amp::PeerLinkConfig server = MakeProductAmpLinkConfig(AmpLinkTable::Server);
  EXPECT_EQ(device.max_links, pp::amp::PeerLinkConfig{}.max_links);
  EXPECT_EQ(device.max_pending_inbound, pp::amp::PeerLinkConfig{}.max_pending_inbound);
  EXPECT_EQ(server.max_links, pp::mesh::kServerMaxLinks);
  EXPECT_EQ(server.max_pending_inbound, pp::mesh::kServerMaxPendingInbound);
  EXPECT_LT(server.max_pending_inbound, server.max_links) << "a handshake flood still cannot fill the table";
  EXPECT_EQ(server.keepalive_warm_interval, device.keepalive_warm_interval);
}

// The operator sizes a server's table (mesh.capacity.links); pending handshakes stay under half of it.
TEST(AmpLinkConfigTest, AServerTableTakesTheOperatorsSize) {
  const pp::amp::PeerLinkConfig big = MakeProductAmpLinkConfig(AmpLinkTable::Server, 20000);
  EXPECT_EQ(big.max_links, 20000u);
  EXPECT_EQ(big.max_pending_inbound, pp::mesh::kServerMaxPendingInbound);
  const pp::amp::PeerLinkConfig small = MakeProductAmpLinkConfig(AmpLinkTable::Server, 8);
  EXPECT_EQ(small.max_links, 8u);
  EXPECT_EQ(small.max_pending_inbound, 4u) << "a handshake flood still cannot fill the table";
  EXPECT_EQ(MakeProductAmpLinkConfig(AmpLinkTable::Device, 20000).max_links, pp::amp::PeerLinkConfig{}.max_links)
      << "a device keeps Amp's table";
}

// Amp's endpoint accepts a connection per link and per pending handshake: a big table is not capped at Amp's own
// default (the stress lab's 8000 parked clients stopped at 4096), and a device's endpoint is as before.
TEST(AmpLinkConfigTest, TheEndpointTakesWhatTheTableHolds) {
  const pp::amp::PeerLinkConfig big = MakeProductAmpLinkConfig(AmpLinkTable::Server, 20000);
  EXPECT_EQ(pp::mesh::AcceptedConnectionsFor(big), 20000u + big.max_pending_inbound);
  EXPECT_EQ(pp::mesh::AcceptedConnectionsFor(MakeProductAmpLinkConfig()), pp::mesh::kAmpDefaultAcceptedConnections);
}
