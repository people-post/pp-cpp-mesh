#pragma once

#include "mesh/types/DirectoryTypes.h"
#include "common/Error.h"
#include "common/Value.h"
namespace pp::mesh {

common::Object MeshNodeHitToObject(const MeshNodeHit& hit);
Roe<MeshNodeHit> MeshNodeHitFromObject(const common::Object& object);
common::Value MeshNodeHitsToJsonArray(const std::vector<MeshNodeHit>& hits);
Roe<std::vector<MeshNodeHit>> MeshNodeHitsFromJsonArray(const common::Array& nodes);

} // namespace pp::mesh
