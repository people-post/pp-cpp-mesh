#pragma once

#include "common/DirectoryTypes.h"
#include "common/Error.h"

#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Abstract directory / phone-book client (HTTP, Amp, failover, mocks). */
class IDirectoryClient {
public:
  virtual ~IDirectoryClient() = default;
  virtual Roe<std::vector<DirectoryHit>> SearchPeople(const std::string& query) = 0;
  virtual Roe<DirectoryHit> LookupRelayUser(const std::string& relay_user_id) = 0;
  virtual Roe<DirectoryHit> LookupByAccount(const std::string& account_id) = 0;
  /** Infra / pp-node listings (N027). Default empty for mocks / older providers. */
  virtual Roe<std::vector<MeshNodeHit>> ListMeshNodes() {
    return std::vector<MeshNodeHit>{};
  }
};

}  // namespace pbr
