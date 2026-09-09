#pragma once

#include "amp/link/PeerLinkManager.h"
#include "domain/mesh/reachability/DialBackTypes.h"
#include "common/CodedFailure.h"
#include "common/Error.h"
#include "common/PbrCompat.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pbr {

/**
 * Amp L4 dial-back (`/pp-browser/reach/1.0.0`) for reachability chrome (D8).
 * Client asks a seed to dial advertised ADP listen multiaddrs; seed replies with ok/dialed/error.
 *
 * Errors follow docs/contracts/CODED_FAILURE.md — wrap PeerLinkManager failures at this owning layer.
 *
 * Prefer ProbeAsync (A022-style). Sync Probe parks until done; with MeshPump running leave IoPump
 * empty so waiters do not Tick. Optional IoPost schedules channel-open polls on MeshRuntime.
 */
class AmpDialBackProtocol {
public:
  enum class Err : int32_t {
    Ok = 0,
    NotStarted,
    EndpointNotRegistered,
    InvalidRequest,
    LinkFailed,
    Timeout,
    ChannelFailed,
    ProtocolError,
    Generic,
  };

  using Failure = CodedFailure<Err>;
  using ProbeRoe = CodedRoe<DialBackProbeResult, Err>;

  /** Map immediate link-manager failure → dial-back Err (never inspect ADP/PeerLink codes). */
  static Failure WrapLinkFailure(const pp::amp::PeerLinkManager::Failure& child);

  using IoPump = std::function<void()>;
  using WorkerPost = std::function<void(std::function<void()>)>;
  /** Queue work for MeshRuntime::PostToIo (channel-open poll). */
  using IoPost = std::function<void(std::function<void()>)>;

  AmpDialBackProtocol(pp::amp::PeerLinkManager& links, IoPump io_pump = {}, WorkerPost post_worker = {},
                      IoPost post_io = {});
  ~AmpDialBackProtocol();

  AmpDialBackProtocol(const AmpDialBackProtocol&) = delete;
  AmpDialBackProtocol& operator=(const AmpDialBackProtocol&) = delete;

  void Start();
  void Stop();
  bool IsStarted() const { return started_; }

  /**
   * Ask `seed_peer_key` (must have a registered ADP endpoint) to dial `target_multiaddrs`.
   * Non-blocking; completion via `on_done` (may run on Amp io / MeshControl / caller).
   */
  void ProbeAsync(const std::string& seed_peer_key, const std::vector<std::string>& target_multiaddrs,
                  std::function<void(ProbeRoe)> on_done, int timeout_ms = 8000);

  /** Blocks until ProbeAsync settles (tests / reachability worker). */
  ProbeRoe Probe(const std::string& seed_peer_key, const std::vector<std::string>& target_multiaddrs,
                 int timeout_ms = 8000);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  pp::amp::PeerLinkManager& links_;
  IoPump io_pump_;
  WorkerPost post_worker_;
  IoPost post_io_;
  bool started_ = false;
};

} // namespace pbr
