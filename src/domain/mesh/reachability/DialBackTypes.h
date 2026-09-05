#pragma once

#include <string>

namespace pbr {

inline constexpr const char* kReachProtocolId = "/pp-browser/reach/1.0.0";
inline constexpr const char* kDialBackProtocolId = kReachProtocolId;

struct DialBackProbeResult {
  bool ok = false;
  std::string dialed;
  std::string error;
};

} // namespace pbr
