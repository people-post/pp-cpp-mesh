#include "mesh/types/ServiceLoad.h"

#include <algorithm>
#include <chrono>

namespace pp::mesh {

namespace {

std::size_t Index(const MeshService service) {
  return static_cast<std::size_t>(service);
}

} // namespace

const char *ServiceLoadWire(const ServiceLoad load) {
  switch (load) {
  case ServiceLoad::Open:
    return "open";
  case ServiceLoad::Busy:
    return "busy";
  case ServiceLoad::Full:
    return "full";
  case ServiceLoad::Draining:
    return "draining";
  case ServiceLoad::Unknown:
    break;
  }
  return "";
}

ServiceLoad ServiceLoadFromWire(const std::string_view wire) {
  if (wire == "open") {
    return ServiceLoad::Open;
  }
  if (wire == "busy") {
    return ServiceLoad::Busy;
  }
  if (wire == "full") {
    return ServiceLoad::Full;
  }
  if (wire == "draining") {
    return ServiceLoad::Draining;
  }
  return ServiceLoad::Unknown;
}

const char *MeshServiceName(const MeshService service) {
  switch (service) {
  case MeshService::Park:
    return "park";
  case MeshService::Relay:
    return "relay";
  case MeshService::MediaRelay:
    return "media_relay";
  }
  return "";
}

int ServiceLoadRank(const ServiceLoad load) {
  switch (load) {
  case ServiceLoad::Open:
    return 0;
  case ServiceLoad::Unknown:
    return 1;
  case ServiceLoad::Busy:
    return 2;
  case ServiceLoad::Full:
  case ServiceLoad::Draining:
    return 3;
  }
  return 1;
}

void SetLoadField(common::Object &reply, const ServiceLoad load) {
  if (const char *word = ServiceLoadWire(load); *word != '\0') {
    reply.set("load", std::string(word));
  }
}

ServiceLoad LoadField(const common::Object &reply) {
  return ServiceLoadFromWire(reply.getString("load").value_or(""));
}

ServiceLoad NextServiceLoad(const ServiceLoad prev, const double pressure,
                            const bool draining,
                            const ServiceLoadThresholds &t) {
  if (draining) {
    return ServiceLoad::Draining;
  }
  const bool was_full = prev == ServiceLoad::Full;
  const bool was_busy = prev == ServiceLoad::Busy || was_full;
  if (pressure >= t.full_enter || (was_full && pressure >= t.full_leave)) {
    return ServiceLoad::Full;
  }
  if (pressure >= t.busy_enter || (was_busy && pressure >= t.busy_leave)) {
    return ServiceLoad::Busy;
  }
  return ServiceLoad::Open;
}

void ServiceLoadMonitor::SetPressure(const MeshService service,
                                     const std::string_view source,
                                     const double pressure) {
  std::lock_guard lock(mu_);
  Service &s = services_[Index(service)];
  s.sources[std::string(source)] = std::clamp(pressure, 0.0, 1.0);
  RecomputeLocked(s);
}

void ServiceLoadMonitor::SetDraining(const bool draining) {
  std::lock_guard lock(mu_);
  draining_ = draining;
  for (Service &s : services_) {
    if (!draining && s.level == ServiceLoad::Draining) {
      s.level = ServiceLoad::Open; // back to the pressure's own level
    }
    RecomputeLocked(s);
  }
}

bool ServiceLoadMonitor::Draining() const {
  std::lock_guard lock(mu_);
  return draining_;
}

ServiceLoad ServiceLoadMonitor::Level(const MeshService service) const {
  std::lock_guard lock(mu_);
  return services_[Index(service)].level;
}

double ServiceLoadMonitor::Pressure(const MeshService service) const {
  std::lock_guard lock(mu_);
  return WorstLocked(services_[Index(service)]);
}

double ServiceLoadMonitor::WorstLocked(const Service &service) {
  double worst = 0.0;
  for (const auto &[source, pressure] : service.sources) {
    worst = std::max(worst, pressure);
  }
  return worst;
}

void ServiceLoadMonitor::RecomputeLocked(Service &service) {
  service.level = NextServiceLoad(service.level, WorstLocked(service),
                                  draining_, thresholds_);
}

int64_t PeerLoadBook::NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void PeerLoadBook::Note(const std::string &peer_id, const MeshService service,
                        const ServiceLoad load, const int64_t now_ms) {
  if (peer_id.empty() || load == ServiceLoad::Unknown) {
    return; // an older node says nothing: keep what a newer reply said
  }
  std::lock_guard lock(mu_);
  if (!entries_.contains(peer_id) && entries_.size() >= kMaxPeers) {
    auto oldest = std::min_element(
        entries_.begin(), entries_.end(), [](const auto &a, const auto &b) {
          return a.second.touched_ms < b.second.touched_ms;
        });
    entries_.erase(oldest);
  }
  Entry &entry = entries_[peer_id];
  entry.said[Index(service)] = Said{load, now_ms};
  entry.touched_ms = now_ms;
}

ServiceLoad PeerLoadBook::Load(const std::string &peer_id,
                               const MeshService service,
                               const int64_t now_ms) const {
  std::lock_guard lock(mu_);
  const auto it = entries_.find(peer_id);
  if (it == entries_.end()) {
    return ServiceLoad::Unknown;
  }
  const Said &said = it->second.said[Index(service)];
  return now_ms - said.at_ms <= kFreshMs ? said.load : ServiceLoad::Unknown;
}

void PeerLoadBook::OrderBest(std::vector<std::string> &peer_ids,
                             const MeshService service,
                             const int64_t now_ms) const {
  std::stable_sort(peer_ids.begin(), peer_ids.end(),
                   [&](const std::string &a, const std::string &b) {
                     return ServiceLoadRank(Load(a, service, now_ms)) <
                            ServiceLoadRank(Load(b, service, now_ms));
                   });
}

} // namespace pp::mesh
