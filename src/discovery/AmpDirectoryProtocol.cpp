#include "domain/mesh/discovery/AmpDirectoryProtocol.h"

#include "common/PbrCompat.h"

namespace pbr {

AmpDirectoryProtocol::AmpDirectoryProtocol(pp::amp::MeshRuntime& runtime, IoPump io_pump, WorkerPost post_worker)
    : server_(runtime, std::move(post_worker)), client_(runtime, std::move(io_pump)) {}

void AmpDirectoryProtocol::Configure(AmpDirectoryProtocolConfig config) {
  server_.Configure(config);
  client_.Configure(config);
}

void AmpDirectoryProtocol::Start() {
  server_.Start();
  client_.Start();
}

void AmpDirectoryProtocol::Stop() {
  client_.Stop();
  server_.Stop();
}

AmpDirectoryClient::AmpDirectoryClient(AmpDirectoryProtocol& service) : service_(service) {}

Roe<std::vector<DirectoryHit>> AmpDirectoryClient::SearchPeople(const std::string& /*query*/) {
  return Error("Amp directory does not support person search (use HTTP failover)");
}

Roe<DirectoryHit> AmpDirectoryClient::LookupRelayUser(const std::string& /*relay_user_id*/) {
  return Error("Amp directory does not support person lookup (use HTTP failover)");
}

Roe<DirectoryHit> AmpDirectoryClient::LookupByAccount(const std::string& /*account_id*/) {
  return Error("Amp directory does not support account lookup (use HTTP failover)");
}

Roe<std::vector<MeshNodeHit>> AmpDirectoryClient::ListMeshNodes() {
  auto result = service_.ListMeshNodes();
  if (!result) {
    return Error(result.error().message);
  }
  return std::move(*result);
}

} // namespace pbr
