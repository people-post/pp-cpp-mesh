#pragma once

#include "domain/mesh/reachability/PunchTypes.h"
#include "common/ValueJson.h"

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

} // namespace pbr
