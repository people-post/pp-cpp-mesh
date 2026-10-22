#pragma once

#include "common/CodedFailure.h"

#include <cstdint>
#include <string_view>

namespace pp::mesh {

/** Why a media-relay quote, attach or send failed (docs/ERRORS.md). */
enum class MediaRelayErr : int32_t {
  Ok = 0,
  NotStarted,
  /** A request missing what it needs (quote id, call id). */
  BadRequest,
  /** No address for the hop. */
  HopNoEndpoint,
  /** Reaching the hop failed (the link's chain is in `message`). */
  LinkFailed,
  /** The channel to the hop did not open, or a request could not be sent. */
  ChannelFailed,
  Timeout,
  /** The channel or the link to the hop went away. */
  Closed,
  Aborted,
  /** The hop's answer could not be read. */
  BadReply,
  /** Not attached to a hop (subscribe / send). */
  NotAttached,
  HopBusy,
  /** The hop refused us (scope, not ready, a request it does not take). */
  HopRefused,
  AuthFailed,
  UnknownQuote,
  /** The caller's own gate declined the quote (e.g. its price). */
  QuoteDeclined,
  Generic,
};
using MediaRelayFailure = CodedFailure<MediaRelayErr>;
template <typename T>
using MediaRelayRoe = CodedRoe<T, MediaRelayErr>;

/** Why a hop refuses a request: the `code` of its error reply. */
enum class MediaRelayRefusal : int32_t {
  Ok = 0,
  Busy,
  /** Scope (a stranger). */
  Refused,
  NotReady,
  AuthFailed,
  UnknownQuote,
  BadRequest,
  Generic,
};

inline const char* MediaRelayRefusalWire(const MediaRelayRefusal refusal) {
  switch (refusal) {
  case MediaRelayRefusal::Busy:
    return "busy";
  case MediaRelayRefusal::Refused:
    return "refused";
  case MediaRelayRefusal::NotReady:
    return "not_ready";
  case MediaRelayRefusal::AuthFailed:
    return "auth_failed";
  case MediaRelayRefusal::UnknownQuote:
    return "unknown_quote";
  case MediaRelayRefusal::BadRequest:
    return "bad_request";
  case MediaRelayRefusal::Ok:
  case MediaRelayRefusal::Generic:
    break;
  }
  return "";
}

/** A hop's refusal, as the client's failure code. Hops before the `code` field sent text only: it is read here. */
inline MediaRelayErr MediaRelayErrFromReply(const std::string_view code, const std::string_view legacy_text) {
  MediaRelayRefusal refusal = MediaRelayRefusal::Generic;
  bool known = false;
  for (const MediaRelayRefusal candidate :
       {MediaRelayRefusal::Busy, MediaRelayRefusal::Refused, MediaRelayRefusal::NotReady, MediaRelayRefusal::AuthFailed,
        MediaRelayRefusal::UnknownQuote, MediaRelayRefusal::BadRequest}) {
    if (!code.empty() && code == MediaRelayRefusalWire(candidate)) {
      refusal = candidate;
      known = true;
    }
  }
  if (!known && code.empty()) {
    const auto has = [legacy_text](const char* needle) { return legacy_text.find(needle) != std::string_view::npos; };
    refusal = has("busy")            ? MediaRelayRefusal::Busy
              : has("stranger")      ? MediaRelayRefusal::Refused
              : has("not ready")     ? MediaRelayRefusal::NotReady
              : has("auth failed")   ? MediaRelayRefusal::AuthFailed
              : has("unknown quote") ? MediaRelayRefusal::UnknownQuote
                                     : MediaRelayRefusal::Generic;
  }
  switch (refusal) {
  case MediaRelayRefusal::Busy:
    return MediaRelayErr::HopBusy;
  case MediaRelayRefusal::AuthFailed:
    return MediaRelayErr::AuthFailed;
  case MediaRelayRefusal::UnknownQuote:
    return MediaRelayErr::UnknownQuote;
  default:
    return MediaRelayErr::HopRefused;
  }
}

} // namespace pp::mesh
