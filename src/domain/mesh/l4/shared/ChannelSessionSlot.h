#pragma once

#include "amp/L3/ChannelSession.h"
#include "amp/link/PeerLink.h"

#include <memory>

namespace pbr {

/**
 * Parent-owned ChannelSession slot teardown ([A027]).
 * Moves the slot out, then CloseQuiet + ReleaseHandlers only when the session is still
 * bound to a live Connected PeerLink mux; otherwise OrphanFromMux (dangling mux after
 * PeerLink drop → SIGFPE/SIGSEGV on hash%).
 */
inline void CloseQuietSlot(std::shared_ptr<pp::amp::ChannelSession>& slot, pp::amp::PeerLink* link) {
  auto session = std::move(slot);
  if (!session) {
    return;
  }
  if (link && link->Mux() && link->Phase() == pp::amp::PeerLinkPhase::Connected &&
      session->Mux() == link->Mux()) {
    session->CloseQuiet();
    session->ReleaseHandlers();
  } else {
    session->OrphanFromMux();
  }
}

} // namespace pbr
