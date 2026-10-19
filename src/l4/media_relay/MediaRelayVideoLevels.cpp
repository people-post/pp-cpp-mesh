#include "domain/mesh/l4/media_relay/MediaRelayVideoLevels.h"

#include "common/media/MediaChannel.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

/** Valid levels, unique, highest first. */
std::vector<uint8_t> Normalized(const std::vector<uint8_t>& levels) {
  std::vector<uint8_t> out;
  for (const uint8_t level : levels) {
    if (IsVideoLevel(level) && std::find(out.begin(), out.end(), level) == out.end()) {
      out.push_back(level);
    }
  }
  std::sort(out.begin(), out.end(), std::greater<>());
  return out;
}

int Distance(const uint8_t level, const std::vector<uint8_t>& served) {
  int best = 255;
  for (const uint8_t s : served) {
    best = std::min(best, std::abs(static_cast<int>(level) - static_cast<int>(s)));
  }
  return best;
}

} // namespace

Roe<std::vector<uint8_t>> ChooseCarriedVideoLevels(const MediaRelayVideoOffer& offer,
                                                   const MediaRelayVideoPolicy& policy) {
  const std::vector<uint8_t> offered = Normalized(offer.levels);
  if (offered.empty()) {
    return std::vector<uint8_t>{};
  }
  const std::vector<uint8_t> served = Normalized(policy.serve_levels);
  size_t limit = static_cast<size_t>(std::max(1, offer.parallel));
  if (policy.carry_levels > 0) {
    limit = std::min(limit, static_cast<size_t>(policy.carry_levels));
  }

  std::vector<uint8_t> carried;
  for (const uint8_t level : offered) {  // highest first
    if (carried.size() < limit &&
        (served.empty() || std::find(served.begin(), served.end(), level) != served.end())) {
      carried.push_back(level);
    }
  }
  if (carried.empty()) {
    if (policy.strict) {
      return Error("video level not served by this relay");
    }
    // Carry what the publisher has, closest to what this relay serves (the higher on a tie).
    uint8_t closest = offered.front();
    for (const uint8_t level : offered) {
      if (Distance(level, served) < Distance(closest, served)) {
        closest = level;
      }
    }
    carried.push_back(closest);
  }
  std::sort(carried.begin(), carried.end());
  return carried;
}

Value VideoLevelsToJson(const std::vector<uint8_t>& levels) {
  std::vector<Value> out;
  for (const uint8_t level : levels) {
    out.emplace_back(int64_t{level});
  }
  return makeArray(std::move(out));
}

std::vector<uint8_t> VideoLevelsFromJson(const Object& json, const char* key) {
  std::vector<uint8_t> out;
  const Array* arr = json.getArray(key);
  if (!arr) {
    return out;
  }
  for (const Value& row : arr->elements) {
    if (const auto* level = std::get_if<int64_t>(&row); level && IsVideoLevel(static_cast<int>(*level))) {
      out.push_back(static_cast<uint8_t>(*level));
    }
  }
  return out;
}

uint8_t PublishVideoLevel(const std::vector<uint8_t>& offered, const std::vector<uint8_t>& carried) {
  uint8_t level = 0;
  for (const uint8_t candidate : carried) {
    if (candidate > level && std::find(offered.begin(), offered.end(), candidate) != offered.end()) {
      level = candidate;
    }
  }
  return level;
}

} // namespace pbr
