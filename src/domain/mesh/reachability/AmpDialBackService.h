#pragma once

#include "amp/link/PeerLinkManager.h"
#include "domain/mesh/reachability/DialBackTypes.h"
#include "common/CodedFailure.h"
#include "common/Error.h"
#include "common/PbrCompat.h"

#include "amp/L3/ChannelSession.h"

#include <cstdint>
#include <functional>
#include <vector>
#include <memory>
#include <string>
#include <vector>

namespace pbr {

/**
 * Amp L4 dial-back (`/pp-browser/reach/1.0.0`) for reachability chrome (D8).
 * Client asks a seed to dial advertised ADP listen multiaddrs; seed replies with ok/dialed/error.
 *
 * Errors follow docs/contracts/CODED_FAILURE.md — wrap PeerLinkManager failures at this owning layer.
 */
class AmpDialBackService {
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

  AmpDialBackService(pp::amp::PeerLinkManager& links, IoPump io_pump = {}, WorkerPost post_worker = {});
  ~AmpDialBackService();

  AmpDialBackService(const AmpDialBackService&) = delete;
  AmpDialBackService& operator=(const AmpDialBackService&) = delete;

  void Start(bool register_handler = true);
  /** Shared `/pp-browser/reach/1.0.0` demux — already-bound session + first DATA. */
  void ServeInbound(std::shared_ptr<pp::amp::ChannelSession> session, std::vector<uint8_t> body);
  void Stop();
  bool IsStarted() const { return started_; }

  /**
   * Ask `seed_peer_key` (must have a registered ADP endpoint) to dial `target_multiaddrs`.
   * Blocks with IoPump until response or timeout.
   */
  ProbeRoe Probe(const std::string& seed_peer_key, const std::vector<std::string>& target_multiaddrs,
                 int timeout_ms = 8000);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  pp::amp::PeerLinkManager& links_;
  IoPump io_pump_;
  WorkerPost post_worker_;
  bool started_ = false;
};

} // namespace pbr
