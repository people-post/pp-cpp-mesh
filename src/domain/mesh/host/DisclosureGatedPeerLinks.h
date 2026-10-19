#pragma once

#include "common/privacy/AddressDisclosure.h"
#include "domain/mesh/host/MeshPorts.h"

#include <memory>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Peer links for product transports that talk to people (chat, attachments, history), limited to the
 * address-disclosure audience (projects/privacy T1): a peer outside it is reached only over a link
 * that already exists (P003) — no new dial, so it never learns our address from us. Everything else
 * passes through. Relays and seeds use the plain links (their operators see addresses: T4).
 */
class DisclosureGatedPeerLinks : public IChatPeerLinks {
public:
  /** `inner` and `gate` are not owned and outlive this. */
  DisclosureGatedPeerLinks(IChatPeerLinks& inner, const AddressDisclosureGate& gate);

  std::optional<std::string> PreferredMultiaddr(const std::string& peer_id) const override;
  Roe<void> RegisterEndpoint(const std::string& peer_key, const std::string& multiaddr) override;
  Roe<void> RegisterEndpoints(const std::string& peer_key, const std::vector<std::string>& multiaddrs) override;
  void EnsureAssociation(const std::string& peer_key, LinkCb on_complete) override;
  void OpenChannel(const std::string& peer_key, const std::string& protocol_id, pp::amp::ChannelPolicy policy,
                   ChannelCb on_complete) override;
  void EstablishNestedOverCarrier(const std::string& peer_key, std::shared_ptr<pp::amp::ChannelSession> carrier,
                                  bool initiator, LinkCb on_complete) override;
  void SetProtocolHandler(const std::string& protocol_id, ProtocolHandler handler) override;
  void RemoveProtocolHandler(const std::string& protocol_id) override;
  MeshPeerLinkSnapshot GetLinkSnapshot(const std::string& peer_key) const override;
  pp::amp::LinkSnapshotEx SnapshotByPeerId(const std::string& peer_id) const override;
  bool IsConnectedRelayed(const std::string& peer_id) const override;
  bool IsConnected(const std::string& peer_key) const override;
  bool IsReachable(const std::string& peer_id) const override;
  void MarkWarm(const std::string& peer_key) override;
  void ClearDialBackoff(const std::string& peer_key) override;
  void AbortInflightDial(const std::string& peer_key) override;
  void DropLink(const std::string& peer_key) override;
  void WhenChannelOpen(const std::string& peer_key, uint32_t channel_id, int64_t deadline_ms,
                       std::function<void(bool ok)> done) override;
  std::shared_ptr<pp::amp::ChannelSession> BindChannel(const std::string& peer_key, uint32_t channel_id,
                                                        pp::amp::ChannelPolicy policy,
                                                        pp::amp::ChannelSession::FrameHandler on_frame,
                                                        pp::amp::ChannelSession::ClosedCallback on_closed) override;

  /** A new dial to `peer_key` is allowed: in the audience, or a link to it is already up. */
  bool MayDial(const std::string& peer_key) const;

private:
  IChatPeerLinks& inner_;
  const AddressDisclosureGate& gate_;
};

} // namespace pbr
