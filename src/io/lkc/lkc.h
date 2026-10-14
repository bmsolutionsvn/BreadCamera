#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/signal_state.h"

namespace bc::lkc {
// A decoded-codec sample as it arrives from the room. The client does no
// decoding (spec D5): video is delivered as H.264 Annex-B access units and
// audio as one Opus packet, for the caller's FFmpeg to decode.
struct VideoSample {
  std::vector<uint8_t> annexB;
  uint32_t rtpTimestamp = 0;
  int64_t receiveUs = 0;
};
struct AudioSample {
  std::vector<uint8_t> opus;
  uint32_t rtpTimestamp = 0;
  int64_t receiveUs = 0;
};

// The shared LiveKit client (spec D1, §7): WebSocket signalling + one
// subscriber PeerConnection on libdatachannel. It joins a room, subscribes to
// one participant's tracks and exposes them through callbacks. There is no
// publish path in the API by design (R5) and the token is subscribe-only.
//
// Threading contract: one worker thread owns the transport. Every
// libdatachannel callback is marshalled onto that worker, so the user
// callbacks (onVideo/onAudio/onState) are always invoked on the worker thread,
// never on libdatachannel's internal threads. The public methods are safe to
// call from any thread. Because the callbacks run on the worker:
//   - a callback must not block (it stalls the transport);
//   - a callback must not destroy the Client — ~Client joins the worker, so
//     deleting the Client from inside its own callback deadlocks.
// Built only when BC_WITH_LIVECALL=1; otherwise construction reports
// `lkc-unavailable`.
class Client {
 public:
  Client(std::string url, std::string token);
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // Registering a callback after a state is already known replays the last
  // state immediately, so no event is lost to a registration race.
  void onVideo(std::function<void(VideoSample)> cb);
  void onAudio(std::function<void(AudioSample)> cb);
  void onState(std::function<void(SignalState, std::optional<ErrorInfo>)> cb);

  // Deliver only this participant's tracks. Empty (the default) delivers every
  // subscribed track. Safe to call before or after the connection is up.
  void subscribe(std::string participantIdentity);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace bc::lkc