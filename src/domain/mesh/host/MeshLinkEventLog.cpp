#include "domain/mesh/host/MeshLinkEventLog.h"

#include "common/Logger.h"
#include "common/metrics/MetricsRegistry.h"
#include "common/PbrCompat.h"

#include <cstdio>
#include <sstream>

namespace pbr {

namespace {

logging::Logger& MeshLinkLog() {
  static logging::Logger logger = logging::getLogger("MeshLink");
  return logger;
}

const char* PathKindName(const pp::amp::LinkPathKind kind) {
  switch (kind) {
  case pp::amp::LinkPathKind::Punched:
    return "punched";
  case pp::amp::LinkPathKind::Carrier:
    return "carrier";
  case pp::amp::LinkPathKind::Direct:
    break;
  }
  return "direct";
}

/** Operator metrics (docs/contracts/NODE_METRICS.md § Links): lifecycle counts, no peer identities. */
void CountLinkEvent(const pp::amp::LinkEvent& event) {
  MetricsRegistry& r = MetricsRegistry::Global();
  switch (event.kind) {
  case pp::amp::LinkEvent::Kind::Connected:
    r.Counter("pp_link_connects_total", "Amp links connected, by path and direction.",
              {{"path", PathKindName(event.path_kind)}, {"direction", event.outbound ? "outbound" : "inbound"}})
        .Inc();
    break;
  case pp::amp::LinkEvent::Kind::Dropped:
    r.Counter("pp_link_drops_total", "Amp links dropped, by reason; stage=attempt never connected.",
              {{"reason", pp::amp::LinkDropReasonName(event.reason)},
               {"stage", event.was_connected ? "connected" : "attempt"}})
        .Inc();
    break;
  case pp::amp::LinkEvent::Kind::PathChanged:
    r.Counter("pp_link_path_changes_total", "Amp link remote endpoint migrations.").Inc();
    break;
  }
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
      << " peer=" << (event.peer_id.empty() ? "-" : event.peer_id) << " path=" << PathKindName(event.path_kind)
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

void InstallMeshLinkMetrics(pp::amp::MeshRuntime& runtime) {
  (void)runtime.AddLinkEventListener([](const pp::amp::LinkEvent& event) { CountLinkEvent(event); });
  // Round trips (acks of first sends) into one histogram; the observer runs on the io strand.
  MetricHistogram& rtt = MetricsRegistry::Global().Histogram(
      "pp_amp_rtt_seconds", "Amp round trips (acks of never-retransmitted reliable packets).",
      {0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 2.5});
  runtime.WithIoLock([&]() {
    runtime.GetEndpoint().SetRttObserver([&rtt](int64_t rtt_ms) { rtt.Observe(static_cast<double>(rtt_ms) / 1000.0); });
  });
}

} // namespace pbr
