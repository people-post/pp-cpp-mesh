#pragma once

#include "base/data/Config.h"
#include "base/net/ServiceClients.h"
#include "common/DirectoryTypes.h"
#include "common/Error.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

/**
 * Normalized phone-book capabilities (N029).
 * HTTP / Amp / future chain providers map into this shape.
 */
struct NameCapabilities {
  bool circuit_relay = false;
  bool media_relay = false;
  bool dht = false;
  /** Prep for Phase C — no chain runtime required to advertise. */
  bool ledger_gateway = false;
};

/**
 * Stable name-directory record (N029 frozen fields).
 * `name` is the human key (prefer account_id); PeerId is mesh identity;
 * endpoints/multiaddrs are dial hints.
 */
struct NameRecord {
  std::string name;
  std::string account_id;
  std::string peer_id;
  std::vector<DirectoryEndpoint> endpoints;
  std::vector<std::string> multiaddrs;
  std::string entity_kind;
  NameCapabilities capabilities;
  int64_t seq = 0;
  std::string expires_at;
  std::string nickname;
  std::string relay_user_id;
};

/** Abstract phone book — backends: HTTP now, Amp later, chain later (N029). */
class INameDirectory {
public:
  virtual ~INameDirectory() = default;

  /** Resolve a human name / Account ID to a directory record. */
  virtual Roe<NameRecord> Resolve(const std::string& name) = 0;

  /**
   * List service records by kind (`mesh_node`, …).
   * Empty kind defaults to `mesh_node`.
   */
  virtual Roe<std::vector<NameRecord>> ListService(const std::string& kind) = 0;
};

NameCapabilities NameCapabilitiesFromAd(const MeshCapabilitiesAd& ad);
MeshCapabilitiesAd MeshCapabilitiesAdFromName(const NameCapabilities& caps);

NameRecord NameRecordFromMeshNodeHit(const MeshNodeHit& hit);
NameRecord NameRecordFromDirectoryHit(const DirectoryHit& hit);

std::vector<NameRecord> NameRecordsFromMeshNodeHits(const std::vector<MeshNodeHit>& hits);

/** Flatten NameRecords to per-PeerId hop-cache rows (preserves N029 fields). */
std::vector<MeshDirectoryNode> MeshDirectoryNodesFromNameRecords(const std::vector<NameRecord>& records);

/** HTTP (or any IDirectoryClient) adapter for INameDirectory. */
class DirectoryClientNameDirectory : public INameDirectory {
public:
  explicit DirectoryClientNameDirectory(IDirectoryClient& directory);

  Roe<NameRecord> Resolve(const std::string& name) override;
  Roe<std::vector<NameRecord>> ListService(const std::string& kind) override;

private:
  IDirectoryClient& directory_;
};

} // namespace pbr
