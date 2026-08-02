#include "feature/messaging/MobileEphemeralListenGate.h"

namespace pbr {

bool ShouldStartMobileEphemeralListen(const MobileEphemeralListenInput& in) {
  if (in.ephemeral_active) {
    return false;
  }
  return in.is_mobile && in.messaging_ready && in.node_runtime_running && in.on_wifi &&
         in.foreground && in.active_local_call;
}

bool ShouldStopMobileEphemeralListen(const MobileEphemeralListenInput& in) {
  if (!in.ephemeral_active) {
    return false;
  }
  return !in.is_mobile || !in.messaging_ready || !in.node_runtime_running || !in.on_wifi ||
         !in.foreground || !in.active_local_call;
}

} // namespace pbr
