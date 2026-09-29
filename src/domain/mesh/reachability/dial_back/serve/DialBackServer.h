#pragma once

#include "amp/link/MeshRuntime.h"
#include "domain/mesh/reachability/dial_back/DialBackTypes.h"
#include "common/PbrCompat.h"

#include <memory>

namespace pbr {

/**
 * Serving side of Amp dial-back (`/pp-browser/reach/1.0.0`, D8): a seed answers `op=probe` by
 * dialing the asker's advertised ADP multiaddrs in order and replying ok / dialed / error, plus
 * the asker's reflexive address as this seed sees it (B26). Registered whenever Amp is up.
 */
class DialBackServer {
public:
  explicit DialBackServer(pp::amp::MeshRuntime& runtime);
  ~DialBackServer();

  DialBackServer(const DialBackServer&) = delete;
  DialBackServer& operator=(const DialBackServer&) = delete;

  void Start();
  void Stop();
  bool IsStarted() const { return started_; }

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  pp::amp::MeshRuntime& runtime_;
  bool started_ = false;
};

} // namespace pbr
