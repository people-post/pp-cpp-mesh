#include "domain/mesh/host/MeshLinkEventLog.h"

#include <gtest/gtest.h>

namespace {

TEST(MeshLinkEventLogTest, FormatsDroppedWithReasonAndAge) {
  pp::amp::LinkEvent event;
  event.kind = pp::amp::LinkEvent::Kind::Dropped;
  event.dial_key = "carrier:QmRelay:3";
  event.peer_id = "QmCaller";
  event.transport = pp::amp::TransportClass::Carrier;
  event.handle.id.value = 7;
  event.handle.generation = 2;
  event.reason = pp::amp::LinkDropReason::CarrierClosed;
  event.was_connected = true;

  EXPECT_EQ(pbr::FormatLinkEventForLog(event),
            "link dropped key=carrier:QmRelay:3 peer=QmCaller transport=carrier dir=in id=7.2 "
            "reason=carrier-closed was_connected=1");
}

TEST(MeshLinkEventLogTest, FormatsPathChangeEndpoints) {
  pp::amp::LinkEvent event;
  event.kind = pp::amp::LinkEvent::Kind::PathChanged;
  event.dial_key = "QmPeer";
  event.outbound = true;
  event.previous_remote = pp::adp::IpEndpoint::V4(203, 0, 113, 5, 4001);
  std::array<uint8_t, 16> v6{};
  v6[0] = 0x20;
  v6[1] = 0x01;
  v6[2] = 0x0d;
  v6[3] = 0xb8;
  v6[15] = 0x01;
  event.remote = pp::adp::IpEndpoint::V6(v6, 4002);

  EXPECT_EQ(pbr::FormatLinkEventForLog(event),
            "link path-changed key=QmPeer peer=- transport=adp dir=out id=0.0 from=203.0.113.5:4001 "
            "to=[2001:db8:0:0:0:0:0:1]:4002");
}

} // namespace
