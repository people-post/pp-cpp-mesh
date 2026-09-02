#pragma once

#include <string>

namespace pbr {

inline constexpr const char* kDialBackProtocolId = "/pp-browser/dial-back/1.0.0";

struct DialBackProbeResult {
  bool ok = false;
  std::string dialed;
  std::string error;
};

} // namespace pbr
