#include "mesh/l4/circuit/client/AmpCircuitHopRegistry.h"

#include <gtest/gtest.h>

namespace pp::mesh {
namespace {

TEST(AmpCircuitHopRegistryTest, KeyIncludesProtocol) {
  EXPECT_EQ(AmpCircuitHopRegistry::Key("QmA", "/test/realtime/1.0.0"), "QmA\x1f/test/realtime/1.0.0");
  EXPECT_NE(AmpCircuitHopRegistry::Key("QmA", "/test/datagram-relay/1.0.0"),
            AmpCircuitHopRegistry::Key("QmA", "/test/realtime/1.0.0"));
}

} // namespace
} // namespace pp::mesh
