#include "mesh/host/LocalNetworkChange.h"

namespace pp::mesh {

LocalNetworkReaction DecideLocalNetworkReaction(const LocalNetworkChange& change) {
  LocalNetworkReaction reaction;
  if (!change.online) {
    return reaction;
  }
  const bool moved = change.attachment_changed || !change.was_online;
  reaction.probe_links = moved;
  reaction.reprobe_reachability = moved;
  return reaction;
}

} // namespace pp::mesh
