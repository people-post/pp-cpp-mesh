#pragma once

#include "base/net/ServiceClients.h"
#include "common/Error.h"
#include "common/PbrCompat.h"

namespace pbr {

Object MeshNodeHitToObject(const MeshNodeHit& hit);
Roe<MeshNodeHit> MeshNodeHitFromObject(const Object& object);
Value MeshNodeHitsToJsonArray(const std::vector<MeshNodeHit>& hits);
Roe<std::vector<MeshNodeHit>> MeshNodeHitsFromJsonArray(const Array& nodes);

} // namespace pbr
