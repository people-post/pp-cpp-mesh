#pragma once

#include "common/Value.h"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace pp::mesh {

/**
 * The wire `code` a server answers a request in a version it does not read with (docs/ERRORS.md § Versions).
 * The refusal names the versions the server reads in `supported_versions`, so the asker can say "update needed".
 */
inline constexpr const char* kBadVersionCode = "bad_version";

/**
 * A request's version, read before anything else in it (its op, its crypto), when it is newer than `ours`.
 * Absent, negative or not an integer reads as `ours`: v9.0.0 sends `v`:1 / `version`:1 and is served as it always
 * was; only a version above ours is refused.
 */
inline std::optional<uint64_t> NewerWireVersion(const common::Object& root, const char* key, const uint64_t ours) {
  const std::optional<uint64_t> version = root.getNonNegInt(key);
  if (version && *version > ours) {
    return version;
  }
  return std::nullopt;
}

/** `supported_versions` (or `key`): the versions this side reads, on a `bad_version` refusal. */
inline void SetSupportedVersions(common::Object& reply, const std::vector<int64_t>& versions,
                                 const char* key = "supported_versions") {
  std::vector<common::Value> items;
  items.reserve(versions.size());
  for (const int64_t version : versions) {
    items.emplace_back(version);
  }
  reply.set(key, common::makeArray(std::move(items)));
}

/**
 * The versions a `bad_version` refusal names (`supported_versions`, or `key`), as text for a message ("1, 2"); empty
 * when it names none (a v9.0.0 server's refusal).
 */
inline std::string SupportedVersionsText(const common::Object& reply, const char* key = "supported_versions") {
  std::string text;
  if (const common::Array* list = reply.getArray(key)) {
    for (const common::Value& item : list->elements) {
      if (const auto* version = std::get_if<int64_t>(&item)) {
        text += (text.empty() ? "" : ", ") + std::to_string(*version);
      }
    }
  }
  return text;
}

} // namespace pp::mesh
