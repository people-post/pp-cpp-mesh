#pragma once

#include "common/Error.h"
#include "common/ValueJson.h"

#include <cstdint>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Which video levels a relay carries (operator config; peer-scoped-broadcast B009). Levels are
 * opaque ordered integers (docs/contracts/MEDIA_CHANNELS.md).
 */
struct MediaRelayVideoPolicy {
  /** Levels this relay serves; empty = any. */
  std::vector<uint8_t> serve_levels;
  /** At most this many levels per publisher; 0 = no limit. */
  int carry_levels = 0;
  /** Refuse a publisher offering none of `serve_levels` (default: carry its closest level instead). */
  bool strict = false;
};

/** What a publisher can produce: the levels, and how many at once. Empty levels = no video. */
struct MediaRelayVideoOffer {
  std::vector<uint8_t> levels;
  int parallel = 1;
};

/**
 * The levels a relay carries for one publisher (ascending): the highest offered levels it serves,
 * up to what both sides allow; when it serves none of them, the one offered level closest to what it
 * serves (the higher on a tie) — or an error when the relay is strict. An offer without levels gets
 * none (never an error).
 */
Roe<std::vector<uint8_t>> ChooseCarriedVideoLevels(const MediaRelayVideoOffer& offer,
                                                   const MediaRelayVideoPolicy& policy);

/** Levels as a JSON array of integers (quote offer / answer). */
Value VideoLevelsToJson(const std::vector<uint8_t>& levels);
/** Valid levels in `key`'s array (others skipped); empty when absent. */
std::vector<uint8_t> VideoLevelsFromJson(const Object& json, const char* key);

} // namespace pbr
