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

/** A hop's refusal, as the client's failure code: by its reply's `code` only. A missing or unknown code is a plain
 * failure (Generic), never read from the text. */
inline MediaRelayErr MediaRelayErrFromCode(const std::string_view code) {
  if (code == MediaRelayRefusalWire(MediaRelayRefusal::Busy)) {
    return MediaRelayErr::HopBusy;
  }
  if (code == MediaRelayRefusalWire(MediaRelayRefusal::AuthFailed)) {
    return MediaRelayErr::AuthFailed;
  }
  if (code == MediaRelayRefusalWire(MediaRelayRefusal::UnknownQuote)) {
    return MediaRelayErr::UnknownQuote;
  }
  if (code == MediaRelayRefusalWire(MediaRelayRefusal::Refused) ||
      code == MediaRelayRefusalWire(MediaRelayRefusal::NotReady) ||
      code == MediaRelayRefusalWire(MediaRelayRefusal::BadRequest)) {
    return MediaRelayErr::HopRefused;
  }
  return MediaRelayErr::Generic;
}

} // namespace pp::mesh
