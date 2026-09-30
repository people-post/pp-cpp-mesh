#pragma once

#include "common/Module.h"
#include "domain/mesh/connectivity/MeshConnectivity.h"
#include "domain/mesh/l4/media_relay/client/IMediaRelayClient.h"
#include "domain/mesh/media_plane/MediaRelayAttach.h"

#include <memory>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The media_relay client shared by calls (group hop) and broadcast (media-client-layers L015),
 * built on `MeshConnectivity` (its dial registry and service reach reach the relays). One client
 * session per mesh host (L013).
 *
 * The owner sequences rewires: dependents holding `RelayClient()` (or `RelayAttachPorts()`) must be
 * detached before `Wire`, `ResetRelayClient` or teardown replace it.
 *
 * Threading: like `MeshConnectivity`, on the Connectivity owner; lifecycle edges run there and the
 * caller waits. `RelayClient()` / `RelayAttachPorts()` are read by consumers at their bind points.
 */
class MeshMediaRelay : public Module {
public:
  /** `connectivity` must outlive this. */
  explicit MeshMediaRelay(MeshConnectivity& connectivity);
  ~MeshMediaRelay() override;
  MeshMediaRelay(const MeshMediaRelay&) = delete;
  MeshMediaRelay& operator=(const MeshMediaRelay&) = delete;

  /** (Re)create the relay client from the running mesh. */
  void Wire();
  /** Tests / harness without MeshHost objects: `relay` stands in for the Amp client (not owned; null = the wired one). */
  void BindTestRelay(IMediaRelayClient* relay);
  void ResetRelayClient();
  /** Owner teardown. */
  void Clear();

  IMediaRelayClient* RelayClient() const { return test_relay_ ? test_relay_ : media_relay_client_.get(); }
  /** media_relay client + dial + service reach, for `AttachToMediaRelayAsync` users. */
  MediaRelayAttachPorts RelayAttachPorts() const;
  /** True when the mesh runs a started Amp media_relay coordinator. */
  bool AmpRelayAvailable() const { return connectivity_.AmpClientsUp(); }

private:
  MeshConnectivity& connectivity_;
  std::unique_ptr<IMediaRelayClient> media_relay_client_;
  IMediaRelayClient* test_relay_ = nullptr;
};

} // namespace pbr
