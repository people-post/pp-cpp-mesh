#include "domain/mesh/host/MeshLinkEventLog.h"

#include "common/Logger.h"
#include "common/PbrCompat.h"

#include <cstdio>
#include <sstream>

namespace pbr {

namespace {

logging::Logger& MeshLinkLog() {
  static logging::Logger logger = logging::getLogger("MeshLink");
  return logger;
}

const char* TransportName(const pp::amp::TransportClass transport) {
  return transport == pp::amp::TransportClass::Carrier ? "carrier" : "adp";
}

} // namespace

std::string FormatLinkEndpointForLog(const pp::adp::IpEndpoint& endpoint) {
  std::ostringstream out;
  if (endpoint.family == pp::adp::IpEndpoint::Family::V4) {
    out << static_cast<int>(endpoint.addr[0]) << '.' << static_cast<int>(endpoint.addr[1]) << '.'
        << static_cast<int>(endpoint.addr[2]) << '.' << static_cast<int>(endpoint.addr[3]);
  } else {
    out << '[';
    for (int i = 0; i < 8; ++i) {
      char group[8];
      std::snprintf(group, sizeof(group), "%x", (endpoint.addr[2 * i] << 8) | endpoint.addr[2 * i + 1]);
      out << (i ? ":" : "") << group;
    }
    out << ']';
  }
  out << ':' << endpoint.port;
  return out.str();
}

std::string FormatLinkEventForLog(const pp::amp::LinkEvent& event) {
  std::ostringstream out;
  out << "link " << pp::amp::LinkEventKindName(event.kind) << " key=" << event.dial_key
      << " peer=" << (event.peer_id.empty() ? "-" : event.peer_id) << " transport=" << TransportName(event.transport)
      << " dir=" << (event.outbound ? "out" : "in") << " id=" << event.handle.id.value << '.'
      << event.handle.generation;
  if (event.previous_remote) {
    out << " from=" << FormatLinkEndpointForLog(*event.previous_remote);
  }
  if (event.remote) {
    out << (event.previous_remote ? " to=" : " remote=") << FormatLinkEndpointForLog(*event.remote);
  }
  if (event.kind == pp::amp::LinkEvent::Kind::Dropped) {
    out << " reason=" << pp::amp::LinkDropReasonName(event.reason)
        << " was_connected=" << (event.was_connected ? 1 : 0);
    if (event.last_rx_age_ms >= 0) {
      out << " last_rx_age_ms=" << event.last_rx_age_ms;
    }
  }
  return out.str();
}

void InstallMeshLinkEventLog(pp::amp::MeshRuntime& runtime) {
  (void)runtime.AddLinkEventListener([](const pp::amp::LinkEvent& event) {
    const std::string line = FormatLinkEventForLog(event);
    if (event.kind != pp::amp::LinkEvent::Kind::Dropped) {
      MeshLinkLog().info << line;
    } else if (event.was_connected) {
      MeshLinkLog().warning << line;
    } else {
      MeshLinkLog().debug << line;
    }
  });
}

} // namespace pbr
