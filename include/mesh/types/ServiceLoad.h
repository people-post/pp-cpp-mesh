#pragma once

#include "common/Value.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pp::mesh {

/**
 * How busy a node is, per service it offers, as the node itself judges it
 * (docs/PEER_REACH.md § Service load). The node derives the level from whatever
 * is tight for it — links, reservations, bridges, media sessions, and what its
 * operator feeds in (CPU, uplink) — and sends the level, never the numbers, in
 * its replies (`"load"`). A client acts on the level only: the node's rule can
 * change without one.
 */

/** What a client picks a node for. */
enum class MeshService : uint8_t {
  /** Parking: a circuit reservation, the link that holds it. */
  Park,
  /** Relaying a 1:1 call's circuit (a bridge). */
  Relay,
  /** Hosting a group call (media_relay). */
  MediaRelay,
};
inline constexpr std::size_t kMeshServiceCount = 3;

enum class ServiceLoad : uint8_t {
  /** Not said (an older node), or what it said is stale. */
  Unknown,
  /** Prefer it. */
  Open,
  /** Use it when nothing better is around. */
  Busy,
  /** Send it nothing new: it refuses new work (code busy). What it already
     serves goes on. */
  Full,
  /** Move off it when convenient: it takes nothing new, and is going away. */
  Draining,
};

/** The wire word ("open" / "busy" / "full" / "draining"); empty for Unknown. */
const char* ServiceLoadWire(ServiceLoad load);
/** A word a newer node may send that this one does not know reads Unknown. */
ServiceLoad ServiceLoadFromWire(std::string_view wire);
const char* MeshServiceName(MeshService service);
/** The node takes no new work for it (Full, Draining). */
inline bool ServiceLoadRefusesNew(const ServiceLoad load) {
  return load == ServiceLoad::Full || load == ServiceLoad::Draining;
}
/** For ordering candidates, best first: Open 0, Unknown 1, Busy 2, Full /
 * Draining 3. */
int ServiceLoadRank(ServiceLoad load);
/** A reply's `"load"`: set when the node says a level. */
void SetLoadField(common::Object& reply, ServiceLoad load);
class ServiceLoadMonitor;
/**
 * A reply's `"load"` (the level of the service replying), and with `monitor` its `"loads"`: every service this node
 * serves, `{"park": "open", "media_relay": "full", …}` — so a client that parks here knows before its first group
 * call whether this hop takes one.
 */
void SetLoadFields(common::Object& reply, ServiceLoad load, const ServiceLoadMonitor* monitor);
/** MeshServiceName's inverse; false for a name this node does not know. */
bool MeshServiceFromName(std::string_view name, MeshService& out);
/** A reply's `"load"`; Unknown when absent (an older node) or not a known word.
 */
ServiceLoad LoadField(const common::Object& reply);

/** Where a pressure turns a level on and off: a gap between the two, so a node
 * near a line does not flap. */
struct ServiceLoadThresholds {
  double busy_enter = 0.70;
  double busy_leave = 0.60;
  double full_enter = 0.95;
  double full_leave = 0.90;
};

/**
 * The pressure of a socket's kernel receive drops (datagrams a second the kernel threw away for want of buffer): 0
 * when none, 1 from kDropsPerSecondFull. Any steady drop means the pump does not keep up; a burst is smoothed by the
 * caller.
 */
inline constexpr double kDropsPerSecondFull = 500.0;
double DropRatePressure(double drops_per_second);

/** The level after `prev` for `pressure` (0 idle .. 1 at capacity), draining or
 * not. Pure. */
ServiceLoad NextServiceLoad(ServiceLoad prev, double pressure, bool draining,
                            const ServiceLoadThresholds& thresholds = {});

/**
 * The node's own levels. Each service's pressure is the worst of its sources
 * ("reservations", "links", "cpu"…, 0..1); its level follows NextServiceLoad.
 * The servers that admit work read the level, so what a node says and what it
 * does are one thing: Full is exactly when it refuses new work. Thread-safe.
 */
class ServiceLoadMonitor {
public:
  explicit ServiceLoadMonitor(ServiceLoadThresholds thresholds = {}) : thresholds_(thresholds) {}

  /** `source`'s pressure on `service` now (clamped to 0..1). */
  void SetPressure(MeshService service, std::string_view source, double pressure);
  /** The operator's draining (pp-node's state): every service takes nothing
   * new. */
  void SetDraining(bool draining);
  bool Draining() const;
  ServiceLoad Level(MeshService service) const;
  /** The worst source's pressure, for logs and tests. */
  double Pressure(MeshService service) const;
  /** Whether this node serves `service` (its replies' `"loads"` name only those). */
  void SetServed(MeshService service, bool served);
  bool Served(MeshService service) const;

private:
  struct Service {
    std::unordered_map<std::string, double> sources;
    ServiceLoad level = ServiceLoad::Open;
    bool served = false;
  };
  void RecomputeLocked(Service& service);
  static double WorstLocked(const Service& service);

  const ServiceLoadThresholds thresholds_;
  mutable std::mutex mu_;
  std::array<Service, kMeshServiceCount> services_{};
  bool draining_ = false;
};

/**
 * What nodes said about their own load, as a client heard it in their replies.
 * Separate from PeerReachRecord, which holds only what the mesh observed. A
 * level is good for kFreshMs, then reads Unknown; at most kMaxPeers peers (the
 * oldest goes). Thread-safe.
 */
class PeerLoadBook {
public:
  static constexpr int64_t kFreshMs = 120'000;
  static constexpr std::size_t kMaxPeers = 512;
  /** The book's clock (steady, ms). */
  static int64_t NowMs();

  void Note(const std::string& peer_id, MeshService service, ServiceLoad load, int64_t now_ms);
  ServiceLoad Load(const std::string& peer_id, MeshService service, int64_t now_ms) const;
  /** The peer said it takes nothing new for `service`, and that is still fresh.
   */
  bool RefusesNew(const std::string& peer_id, MeshService service, int64_t now_ms) const {
    return ServiceLoadRefusesNew(Load(peer_id, service, now_ms));
  }
  /** `peer_ids` best first for `service` (ServiceLoadRank), keeping their order
   * within a rank. */
  void OrderBest(std::vector<std::string>& peer_ids, MeshService service, int64_t now_ms) const;
  /**
   * What `peer_id` said of `service` is forgotten; its next reply says it again. The circuit client does it when a
   * park it held on `peer_id` ended with the link: a phone whose seed was full and then crashed skipped the restarted,
   * empty seed for the 2 min the old "full" stayed fresh (pp-browser STRESS_LAB, 2026-11-10).
   */
  void Forget(const std::string& peer_id, MeshService service);

private:
  struct Said {
    ServiceLoad load = ServiceLoad::Unknown;
    int64_t at_ms = 0;
  };
  struct Entry {
    std::array<Said, kMeshServiceCount> said{};
    int64_t touched_ms = 0;
  };
  mutable std::mutex mu_;
  std::unordered_map<std::string, Entry> entries_;
};

/**
 * What a reply from `peer_id` to a `replying` request says: its `"load"` for that service, and its `"loads"` for each
 * service it names.
 */
void NoteReplyLoads(PeerLoadBook& book, const std::string& peer_id, MeshService replying, const common::Object& reply,
                    int64_t now_ms);

} // namespace pp::mesh
