#pragma once

namespace pbr {

struct MobileEphemeralListenInput {
  bool is_mobile = false;
  bool messaging_ready = false;
  bool node_runtime_running = false;
  bool on_wifi = false;
  bool foreground = false;
  bool active_local_call = false;
  /** When true, includes joined sessions and foreground incoming ring (N025). */
  bool ephemeral_active = false;
};

bool ShouldStartMobileEphemeralListen(const MobileEphemeralListenInput& in);
bool ShouldStopMobileEphemeralListen(const MobileEphemeralListenInput& in);

} // namespace pbr
