#pragma once

#include "amp/link/PeerLinkManager.h"
#include "domain/mesh/reachability/PunchTypes.h"
#include "common/ValueJson.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/** Encode/decode Amp Coordinated Punch JSON frames (`v` + `op`). */
std::string EncodePunchConnect(const PunchConnectRequest& req);
std::string EncodePunchOffer(const PunchOffer& msg);
std::string EncodePunchCandidates(const PunchCandidates& msg);
std::string EncodePunchSync(const PunchSync& msg);
std::string EncodePunchResult(const PunchResult& msg);

std::optional<std::string> PunchOp(const Object& root);

std::optional<PunchConnectRequest> DecodePunchConnect(const Object& root);
std::optional<PunchOffer> DecodePunchOffer(const Object& root);
std::optional<PunchCandidates> DecodePunchCandidates(const Object& root);
std::optional<PunchSync> DecodePunchSync(const Object& root);
std::optional<PunchResult> DecodePunchResult(const Object& root);

/** Keep only ADP multiaddrs; cap list length. */
std::vector<std::string> SanitizePunchAddrs(const std::vector<std::string>& addrs, size_t max_addrs = 8);

/** Epoch still open relative to start + window. */
bool PunchWindowOpen(int64_t start_ms, int window_ms, int64_t now_ms);

/** Upsert punch winner under PeerId (and multiaddr PeerId if different) for SoftMigrate IsDialable. */
void PublishPunchWinnerAddrs(pp::amp::PeerLinkManager& links, const std::string& known_peer_id,
                             const std::string& winner_multiaddr);

/**
 * Prefer a connected contact with an endpoint, else any contact with endpoint,
 * then the same for seeds. Skips exclude_peer_id.
 */
std::optional<std::string> PickPunchIntroducer(
    const std::vector<std::string>& contact_peer_ids, const std::vector<std::string>& seed_peer_ids,
    const std::string& exclude_peer_id, const std::function<bool(const std::string&)>& has_endpoint,
    const std::function<bool(const std::string&)>& is_connected);


} // namespace pbr
