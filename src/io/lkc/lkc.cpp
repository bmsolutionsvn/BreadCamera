#include "io/lkc/lkc.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if BC_WITH_LIVECALL
#include <rtc/rtc.hpp>
#endif

namespace bc::lkc {
namespace {

int64_t nowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool debugOn() {
  static const bool on = [] {
    char* buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, "BC_LKC_DEBUG") != 0) return false;
    const bool set = buf != nullptr && buf[0] != '\0';
    std::free(buf);
    return set;
  }();
  return on;
}
void dbg(const char* fmt, ...) {
  if (!debugOn()) return;
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
  std::fputc('\n', stderr);
}

// ---- input validation (shared by both build modes) -------------------------
// A LiveKit access token is a JWT: three non-empty base64url segments.
bool tokenLooksValid(const std::string& t) {
  int dots = 0;
  size_t seg = 0;
  for (char c : t) {
    unsigned char u = static_cast<unsigned char>(c);
    if (c == '.') {
      if (seg == 0) return false;
      ++dots;
      seg = 0;
    } else if (std::isalnum(u) || c == '-' || c == '_') {
      ++seg;
    } else {
      return false;
    }
  }
  return dots == 2 && seg > 0;
}

// Accepts ws/wss/http/https; rewrites http(s) to ws(s) and stops at the first
// path separator. Returns false with a reason for an unusable URL.
bool buildWsUrl(const std::string& url, std::string& out, std::string& err) {
  auto pos = url.find("://");
  if (pos == std::string::npos) {
    err = "url has no scheme: " + url;
    return false;
  }
  const std::string scheme = url.substr(0, pos);
  const std::string rest = url.substr(pos + 3);
  const std::string hostport = rest.substr(0, rest.find('/'));
  if (hostport.empty()) {
    err = "url has no host: " + url;
    return false;
  }
  if (scheme == "ws" || scheme == "wss") {
    out = scheme + "://" + hostport;
  } else if (scheme == "http") {
    out = "ws://" + hostport;
  } else if (scheme == "https") {
    out = "wss://" + hostport;
  } else {
    err = "unsupported url scheme '" + scheme + "'";
    return false;
  }
  return true;
}

#if BC_WITH_LIVECALL
// ---- minimal protobuf wire codec (only the LiveKit messages we need) -------
void putVarint(std::string& out, uint64_t v) {
  while (v >= 0x80) {
    out.push_back(static_cast<char>((v & 0x7f) | 0x80));
    v >>= 7;
  }
  out.push_back(static_cast<char>(v));
}
void tag(std::string& out, int field, int wire) {
  putVarint(out, (static_cast<uint64_t>(field) << 3) | static_cast<uint64_t>(wire));
}
void putLen(std::string& out, int field, const std::string& bytes) {
  tag(out, field, 2);
  putVarint(out, bytes.size());
  out += bytes;
}
void putStr(std::string& out, int field, const std::string& s) { putLen(out, field, s); }

struct Reader {
  const uint8_t* p;
  const uint8_t* end;
  bool varint(uint64_t& v) {
    v = 0;
    int shift = 0;
    while (p < end && shift < 64) {
      uint8_t b = *p++;
      v |= static_cast<uint64_t>(b & 0x7f) << shift;
      if (!(b & 0x80)) return true;
      shift += 7;
    }
    return false;
  }
  bool bytes(const uint8_t*& data, size_t& len) {
    uint64_t l;
    if (!varint(l)) return false;
    if (p + l > end) return false;
    data = p;
    len = l;
    p += l;
    return true;
  }
};

struct Field {
  int num = 0;
  int wire = 0;
  const uint8_t* data = nullptr;
  size_t len = 0;
  uint64_t v = 0;
};

bool next(Reader& r, Field& f) {
  uint64_t key;
  if (!r.varint(key)) return false;
  f.num = static_cast<int>(key >> 3);
  f.wire = static_cast<int>(key & 7);
  f.data = nullptr;
  f.len = 0;
  f.v = 0;
  if (f.wire == 0) return r.varint(f.v);
  if (f.wire == 2) return r.bytes(f.data, f.len);
  if (f.wire == 1) {
    if (r.end - r.p < 8) return false;
    r.p += 8;
    return true;
  }
  if (f.wire == 5) {
    if (r.end - r.p < 4) return false;
    r.p += 4;
    return true;
  }
  return false;
}

std::string asStr(const Field& f) {
  return std::string(reinterpret_cast<const char*>(f.data), f.len);
}

// SessionDescription { string type=1; string sdp=2; uint32 id=3; map mid_to_track_id=4 }
struct SessionDescription {
  int32_t id = 0;
  std::string type;
  std::string sdp;
  std::map<std::string, std::string> midToTrack;
};
SessionDescription parseSessionDescription(const uint8_t* d, size_t n) {
  SessionDescription sd;
  Reader r{d, d + n};
  Field f;
  while (next(r, f)) {
    if (f.wire == 2 && f.num == 1) sd.type = asStr(f);
    else if (f.wire == 2 && f.num == 2) sd.sdp = asStr(f);
    else if (f.wire == 0 && f.num == 3) sd.id = static_cast<int32_t>(f.v);
    else if (f.wire == 2 && f.num == 4) {  // map entry { key=1; value=2 }
      Reader er{f.data, f.data + f.len};
      Field ef;
      std::string k, v;
      while (next(er, ef)) {
        if (ef.wire == 2 && ef.num == 1) k = asStr(ef);
        else if (ef.wire == 2 && ef.num == 2) v = asStr(ef);
      }
      if (!k.empty()) sd.midToTrack[k] = v;
    }
  }
  return sd;
}

// ParticipantInfo { string sid=1; string identity=2; repeated TrackInfo tracks=4 }
// TrackInfo { string sid=1; enum type=2; string mime_type=11; string mid=12 }
struct ParticipantRef {
  std::string identity;
  std::vector<std::pair<std::string, std::string>> tracks;  // (sid, mid)
};
ParticipantRef parseParticipant(const uint8_t* d, size_t n) {
  ParticipantRef p;
  Reader r{d, d + n};
  Field f;
  while (next(r, f)) {
    if (f.wire == 2 && f.num == 2) {
      p.identity = asStr(f);
    } else if (f.wire == 2 && f.num == 4) {  // TrackInfo
      std::string sid, mid;
      Reader tr{f.data, f.data + f.len};
      Field tf;
      while (next(tr, tf)) {
        if (tf.wire == 2 && tf.num == 1) sid = asStr(tf);
        else if (tf.wire == 2 && tf.num == 12) mid = asStr(tf);
      }
      p.tracks.emplace_back(sid, mid);
    }
  }
  return p;
}

// ---- tiny SDP reader: per-mid kind + the H.264/Opus payload types ----------
struct MediaSection {
  std::string kind;
  std::set<int> h264;
  int opus = -1;
};
std::map<std::string, MediaSection> parseSdpMedia(const std::string& sdp) {
  std::map<std::string, MediaSection> out;
  std::string curMid;
  std::string pendingKind;
  bool havePending = false;
  MediaSection* cur = nullptr;
  size_t i = 0;
  while (i <= sdp.size()) {
    size_t eol = sdp.find('\n', i);
    std::string line = sdp.substr(i, eol == std::string::npos ? std::string::npos : eol - i);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("m=", 0) == 0) {
      size_t sp = line.find(' ');
      pendingKind = line.substr(2, sp == std::string::npos ? std::string::npos : sp - 2);
      havePending = true;
      cur = nullptr;
    } else if (line.rfind("a=mid:", 0) == 0 && havePending) {
      curMid = line.substr(6);
      MediaSection& sec = out[curMid];
      sec.kind = pendingKind;
      cur = &sec;
      havePending = false;
    } else if (cur && line.rfind("a=rtpmap:", 0) == 0) {
      int pt = std::atoi(line.c_str() + 9);
      std::string lower = line;
      for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (lower.find(" h264/") != std::string::npos) cur->h264.insert(pt);
      else if (lower.find(" opus/") != std::string::npos) cur->opus = pt;
    }
    if (eol == std::string::npos) break;
    i = eol + 1;
  }
  return out;
}

// ---- outbound SignalRequest encoders --------------------------------------
std::string encodeAnswer(const std::string& sdp, int32_t offerId) {
  std::string sd;
  putStr(sd, 1, "answer");
  putStr(sd, 2, sdp);
  tag(sd, 3, 0);
  putVarint(sd, static_cast<uint64_t>(std::max(0, offerId)));
  std::string req;
  putLen(req, 2, sd);  // SignalRequest.answer = 2
  return req;
}
std::string encodeTrickle(const std::string& candidateJson) {
  std::string sd;
  putStr(sd, 1, candidateJson);
  tag(sd, 2, 0);
  putVarint(sd, 1);  // SignalTarget.SUBSCRIBER = 1
  std::string req;
  putLen(req, 3, sd);  // SignalRequest.trickle = 3
  return req;
}
std::string encodePing(int64_t tsMs) {
  std::string ping;
  tag(ping, 1, 0);
  putVarint(ping, static_cast<uint64_t>(tsMs));
  std::string req;
  putLen(req, 16, ping);  // SignalRequest.ping_req = 16
  return req;
}
// The legacy `int64 ping = 14`; the JS SDK sends it alongside `ping_req` for
// compatibility, and the server resets its ping timeout on it.
std::string encodePingLegacy(int64_t tsMs) {
  std::string req;
  tag(req, 14, 0);
  putVarint(req, static_cast<uint64_t>(tsMs));
  return req;
}

// Minimal JSON string-field picker for LiveKit's trickle candidateInit.
std::string jsonStringField(const std::string& json, const std::string& key) {
  const std::string needle = "\"" + key + "\"";
  auto k = json.find(needle);
  if (k == std::string::npos) return {};
  auto colon = json.find(':', k + needle.size());
  if (colon == std::string::npos) return {};
  auto q1 = json.find('"', colon + 1);
  if (q1 == std::string::npos) return {};
  std::string v;
  for (size_t i = q1 + 1; i < json.size(); ++i) {
    char c = json[i];
    if (c == '\\' && i + 1 < json.size()) {
      char n = json[++i];
      v.push_back(n == 'n' ? '\n' : n == 'r' ? '\r' : n == 't' ? '\t' : n);
    } else if (c == '"') {
      break;
    } else {
      v.push_back(c);
    }
  }
  return v;
}
#endif  // BC_WITH_LIVECALL
}  // namespace

// ---------------------------------------------------------------------------
struct Client::Impl {
  std::string url_;
  std::string token_;

  std::mutex m_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> queue_;
  std::atomic<bool> stop_{false};
  std::thread worker_;

  std::function<void(VideoSample)> onVideo_;
  std::function<void(AudioSample)> onAudio_;
  std::function<void(SignalState, std::optional<ErrorInfo>)> onState_;
  SignalState state_ = SignalState::Connecting;
  std::optional<ErrorInfo> lastErr_;
  bool haveState_ = false;

  std::string wantIdentity_;

  Impl(std::string url, std::string token) : url_(std::move(url)), token_(std::move(token)) {
    worker_ = std::thread([this] { workerMain(); });
  }
  ~Impl() { stop(); }

  void stop() {
    {
      std::lock_guard lk(m_);
      if (stop_.exchange(true)) return;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
  }

  // Runs fn on the single delivery thread; dropped once stopping.
  void post(std::function<void()> fn) {
    {
      std::lock_guard lk(m_);
      if (stop_) return;
      queue_.push_back(std::move(fn));
    }
    cv_.notify_all();
  }

  void emitState(SignalState s, std::optional<ErrorInfo> e) {
    std::function<void(SignalState, std::optional<ErrorInfo>)> cb;
    {
      std::lock_guard lk(m_);
      state_ = s;
      lastErr_ = e;
      haveState_ = true;
      cb = onState_;
    }
    if (cb) cb(s, e);
  }

  void setState(std::function<void(SignalState, std::optional<ErrorInfo>)> cb) {
    std::function<void(SignalState, std::optional<ErrorInfo>)> call;
    SignalState s = SignalState::Connecting;
    std::optional<ErrorInfo> e;
    {
      std::lock_guard lk(m_);
      onState_ = std::move(cb);
      if (haveState_ && onState_) {
        call = onState_;
        s = state_;
        e = lastErr_;
      }
    }
    if (call) call(s, e);  // replay the last state so registration never races
  }

  void workerMain() {
    if (!tokenLooksValid(token_)) {
      emitState(SignalState::Offline,
                ErrorInfo{"lkc-config", "the token is not a LiveKit access token (JWT)"});
      return;
    }
    std::string wsBase, err;
    if (!buildWsUrl(url_, wsBase, err)) {
      emitState(SignalState::Offline, ErrorInfo{"lkc-unreachable", err});
      return;
    }
#if BC_WITH_LIVECALL
    connectLoop(wsBase);
#else
    (void)wsBase;
    emitState(SignalState::Offline,
              ErrorInfo{"lkc-unavailable", "LiveKit support is not built (BC_WITH_LIVECALL=0)"});
#endif
  }

#if BC_WITH_LIVECALL
  std::shared_ptr<rtc::WebSocket> ws_;
  std::shared_ptr<rtc::PeerConnection> pc_;
  std::shared_ptr<rtc::Track> videoTrack_;
  std::shared_ptr<rtc::Track> audioTrack_;
  std::vector<std::shared_ptr<rtc::DataChannel>> dcs_;
  std::map<std::string, MediaSection> midMedia_;
  std::map<std::string, std::string> midIdentity_;
  std::map<std::string, std::string> trackIdentity_;  // TrackInfo.sid -> participant
  std::vector<uint8_t> fu_;
  int pingIntervalMs_ = 5000;
  bool joined_ = false;
  std::chrono::steady_clock::time_point nextPing_;

  void connectLoop(const std::string& wsBase) {
    emitState(SignalState::Connecting, std::nullopt);
    const std::string url = wsBase + "/rtc?access_token=" + token_ +
                            "&auto_subscribe=1&sdk=cpp&version=0.1.0&protocol=9";
    // Seed the ping clock on the worker before opening; every callback below
    // hops onto the worker so no shared state is touched off-thread.
    nextPing_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(pingIntervalMs_);
    ws_ = std::make_shared<rtc::WebSocket>();
    ws_->onOpen([this] {
      post([this] {
        nextPing_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(pingIntervalMs_);
        dbg("[lkc] ws open");
      });
    });
    ws_->onError([this](std::string e) {
      dbg("[lkc] ws error: %s", e.c_str());
      post([this, e] { emitState(SignalState::Offline, ErrorInfo{"lkc-unreachable", e}); });
    });
    ws_->onClosed([this] {
      post([this] {
        dbg("[lkc] ws closed joined=%d", joined_ ? 1 : 0);
        if (stop_) return;
        const bool up = joined_;
        emitState(SignalState::Offline,
                  ErrorInfo{up ? "lkc-media" : "lkc-config",
                            up ? "the signalling connection closed"
                               : "the server refused the connection (check the token)"});
      });
    });
    ws_->onMessage([this](rtc::message_variant msg) {
      auto* b = std::get_if<rtc::binary>(&msg);
      if (!b) return;
      std::string bytes(reinterpret_cast<const char*>(b->data()), b->size());
      post([this, bytes = std::move(bytes)] { handleSignal(bytes); });
    });
    try {
      ws_->open(url);
    } catch (const std::exception& e) {
      emitState(SignalState::Offline, ErrorInfo{"lkc-unreachable", e.what()});
      return;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!stop_) {
      std::unique_lock lk(m_);
      cv_.wait_for(lk, std::chrono::milliseconds(50), [this] { return stop_ || !queue_.empty(); });
      std::deque<std::function<void()>> work;
      work.swap(queue_);
      const bool stopping = stop_;
      lk.unlock();
      for (auto& fn : work) fn();
      if (stopping) break;
      if (ws_ && ws_->isOpen() && std::chrono::steady_clock::now() >= nextPing_) {
        sendSignal(encodePingLegacy(nowUs() / 1000));
        sendSignal(encodePing(nowUs() / 1000));
        nextPing_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(pingIntervalMs_);
      }
      if (!joined_ && std::chrono::steady_clock::now() > deadline) {
        emitState(SignalState::Offline,
                  ErrorInfo{"lkc-unreachable", "no JoinResponse from the server"});
        break;
      }
    }
    teardown();
  }

  void sendSignal(const std::string& body) {
    if (!body.empty())
      dbg("[lkc] -> send %02x (%zu bytes)", static_cast<unsigned char>(body[0]), body.size());
    std::shared_ptr<rtc::WebSocket> ws;
    {
      std::lock_guard lk(m_);
      ws = ws_;
    }
    if (!ws || !ws->isOpen()) return;
    ws->send(rtc::binary(reinterpret_cast<const std::byte*>(body.data()),
                         reinterpret_cast<const std::byte*>(body.data()) + body.size()));
  }

  std::shared_ptr<rtc::PeerConnection> ensurePc() {
    if (pc_) return pc_;
    rtc::Configuration cfg;
    cfg.disableAutoNegotiation = false;
    cfg.forceMediaTransport = true;  // allocate SRTP before any track exists
    pc_ = std::make_shared<rtc::PeerConnection>(cfg);
    pc_->onLocalDescription([this](rtc::Description d) {
      const std::string type = d.typeString();
      std::string sdp = std::string(d);
      dbg("[lkc] local description %s (%zu bytes)", type.c_str(), sdp.size());
      // Build and send the answer on the worker; the offer id is the one
      // handleSignal captured, read here on the worker (not off-thread).
      post([this, type, sdp = std::move(sdp)] {
        if (type == "answer") sendSignal(encodeAnswer(sdp, latestOfferId_));
      });
    });
    pc_->onLocalCandidate([this](rtc::Candidate c) {
      // `Candidate::operator string()` prefixes "a=", which LiveKit's SDP parser
      // rejects ("invalid ice-char token: ="); candidateInit wants the bare form.
      std::string json = "{\"candidate\":\"" + c.candidate() + "\",\"sdpMid\":\"" + c.mid() +
                         "\",\"sdpMLineIndex\":0,\"usernameFragment\":null}";
      post([this, json = std::move(json)] { sendSignal(encodeTrickle(json)); });
    });
    pc_->onStateChange([this](rtc::PeerConnection::State s) {
      using S = rtc::PeerConnection::State;
      post([this, s] {
        if (s == S::Connected) {
          emitState(SignalState::Live, std::nullopt);
        } else if (s == S::Disconnected || s == S::Failed || s == S::Closed) {
          emitState(SignalState::Offline, ErrorInfo{"lkc-media", "the media connection was lost"});
        }
      });
    });
    pc_->onTrack([this](std::shared_ptr<rtc::Track> t) {
      // Hold the track until the worker runs: a released track drops its m-line.
      post([this, t] {
        const std::string mid = t->mid();
        auto it = midMedia_.find(mid);
        const std::string kind = it == midMedia_.end() ? std::string() : it->second.kind;
        if (kind == "audio") {
          audioTrack_ = t;
        } else {
          videoTrack_ = t;
        }
        t->onMessage([this, mid](rtc::message_variant msg) {
          auto* b = std::get_if<rtc::binary>(&msg);
          if (!b) return;
          std::string bytes(reinterpret_cast<const char*>(b->data()), b->size());
          post([this, mid, bytes = std::move(bytes)] { handleRtp(mid, bytes); });
        });
      });
    });
    pc_->onDataChannel([this](std::shared_ptr<rtc::DataChannel> dc) {
      post([this, dc = std::move(dc)] { dcs_.push_back(dc); });
    });
    return pc_;
  }

  int32_t latestOfferId_ = 0;

  void handleSignal(const std::string& bytes) {
    Reader r{reinterpret_cast<const uint8_t*>(bytes.data()),
             reinterpret_cast<const uint8_t*>(bytes.data()) + bytes.size()};
    Field f;
    while (next(r, f)) {
      if (f.wire != 2) continue;
      dbg("[lkc] sig field %d", f.num);
      switch (f.num) {
        case 1: {  // JoinResponse
          joined_ = true;
          Reader jr{f.data, f.data + f.len};
          Field jf;
          while (next(jr, jf)) {
            if (jf.wire == 0 && jf.num == 11) pingIntervalMs_ = static_cast<int>(jf.v) * 1000;
            else if (jf.wire == 2 && jf.num == 3) rememberParticipant(parseParticipant(jf.data, jf.len));
          }
          emitState(SignalState::Connecting, std::nullopt);
          break;
        }
        case 3: {  // offer (server-initiated subscriber PC)
          auto sd = parseSessionDescription(f.data, f.len);
          if (sd.type != "offer") break;
          // Captured on the worker; the answer posted from onLocalDescription
          // reads it here too, never on libdatachannel's thread.
          latestOfferId_ = sd.id;
          midMedia_ = parseSdpMedia(sd.sdp);
          for (const auto& [mid, sid] : sd.midToTrack)
            if (auto t = trackIdentity_.find(sid); t != trackIdentity_.end())
              midIdentity_[mid] = t->second;
          auto p = ensurePc();
          try {
            p->setRemoteDescription(rtc::Description(sd.sdp, "offer"));
          } catch (const std::exception& e) {
            emitState(SignalState::Offline, ErrorInfo{"lkc-media", e.what()});
          }
          break;
        }
        case 4: {  // trickle
          Reader tr{f.data, f.data + f.len};
          Field tf;
          std::string cand;
          while (next(tr, tf))
            if (tf.wire == 2 && tf.num == 1) cand = asStr(tf);
          if (cand.empty() || !pc_) break;
          std::string c = jsonStringField(cand, "candidate");
          std::string mid = jsonStringField(cand, "sdpMid");
          if (!c.empty()) {
            try {
              pc_->addRemoteCandidate(rtc::Candidate(c, mid));
            } catch (const std::exception&) {
            }
          }
          break;
        }
        case 5: {  // ParticipantUpdate (incremental: repeated ParticipantInfo = 1)
          Reader pr{f.data, f.data + f.len};
          Field pf;
          while (next(pr, pf))
            if (pf.wire == 2 && pf.num == 1) rememberParticipant(parseParticipant(pf.data, pf.len));
          break;
        }
        case 8: {  // LeaveRequest { can_reconnect=1; DisconnectReason reason=2; Action action=3 }
          Reader lr{f.data, f.data + f.len};
          Field lf;
          unsigned long long reason = 0, action = 0;
          while (next(lr, lf)) {
            if (lf.wire == 0 && lf.num == 2) reason = lf.v;
            if (lf.wire == 0 && lf.num == 3) action = lf.v;
          }
          dbg("[lkc] LeaveRequest reason=%llu action=%llu", reason, action);
          break;
        }
        default:
          break;
      }
    }
  }

  void rememberParticipant(const ParticipantRef& p) {
    for (const auto& [sid, mid] : p.tracks) {
      if (!sid.empty()) trackIdentity_[sid] = p.identity;
      if (!mid.empty()) midIdentity_[mid] = p.identity;
    }
  }

  bool wanted(const std::string& mid) {
    std::string want;
    {
      std::lock_guard lk(m_);
      want = wantIdentity_;
    }
    if (want.empty()) return true;
    auto it = midIdentity_.find(mid);
    return it == midIdentity_.end() ? true : it->second == want;  // unfilterable mid is delivered
  }

  void handleRtp(const std::string& mid, const std::string& bytes) {
    if (!wanted(mid)) return;
    auto sit = midMedia_.find(mid);
    if (sit == midMedia_.end()) return;
    const MediaSection& sec = sit->second;
    if (bytes.size() < 12) return;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(bytes.data());
    const int cc = p[0] & 0x0f;
    const bool ext = (p[0] & 0x10) != 0;
    const int pt = p[1] & 0x7f;
    const uint32_t rtpTs = (static_cast<uint32_t>(p[4]) << 24) | (static_cast<uint32_t>(p[5]) << 16) |
                           (static_cast<uint32_t>(p[6]) << 8) | p[7];
    size_t off = 12 + 4 * static_cast<size_t>(cc);
    if (ext) {
      if (bytes.size() < off + 4) return;
      const size_t words = (p[off + 2] << 8) | p[off + 3];
      off += 4 + 4 * words;
    }
    if (off > bytes.size()) return;

    if (sec.kind == "audio") {
      if (pt != sec.opus) return;
      AudioSample s;
      s.opus.assign(p + off, p + bytes.size());
      s.rtpTimestamp = rtpTs;
      s.receiveUs = nowUs();
      std::function<void(AudioSample)> cb;
      {
        std::lock_guard lk(m_);
        cb = onAudio_;
      }
      if (cb) cb(std::move(s));
      return;
    }
    if (sec.kind != "video" || sec.h264.count(pt) == 0) return;
    depacketizeH264(p + off, bytes.size() - off, rtpTs);
  }

  void deliverVideo(std::vector<uint8_t> annexB, uint32_t rtpTs) {
    VideoSample s;
    s.annexB = std::move(annexB);
    s.rtpTimestamp = rtpTs;
    s.receiveUs = nowUs();
    std::function<void(VideoSample)> cb;
    {
      std::lock_guard lk(m_);
      cb = onVideo_;
    }
    if (cb) cb(std::move(s));
  }

  void depacketizeH264(const uint8_t* pay, size_t len, uint32_t rtpTs) {
    if (len < 1) return;
    const int nal = pay[0] & 0x1f;
    if (nal >= 1 && nal <= 23) {  // single NAL unit
      std::vector<uint8_t> au{0, 0, 0, 1};
      au.insert(au.end(), pay, pay + len);
      deliverVideo(std::move(au), rtpTs);
    } else if (nal == 24) {  // STAP-A
      std::vector<uint8_t> au;
      size_t i = 1;
      while (i + 2 <= len) {
        const size_t nalLen = (pay[i] << 8) | pay[i + 1];
        i += 2;
        if (i + nalLen > len) break;
        au.insert(au.end(), {0, 0, 0, 1});
        au.insert(au.end(), pay + i, pay + i + nalLen);
        i += nalLen;
      }
      if (!au.empty()) deliverVideo(std::move(au), rtpTs);
    } else if (nal == 28) {  // FU-A
      if (len < 2) return;
      const bool start = pay[1] & 0x80;
      const bool end = pay[1] & 0x40;
      const uint8_t type = pay[1] & 0x1f;
      if (start) {
        fu_ = {0, 0, 0, 1};
        fu_.push_back(static_cast<uint8_t>((pay[0] & 0xe0) | type));
      }
      fu_.insert(fu_.end(), pay + 2, pay + len);
      if (end && !fu_.empty()) {
        std::vector<uint8_t> au = std::move(fu_);
        fu_.clear();
        deliverVideo(std::move(au), rtpTs);
      }
    }
  }

  void teardown() {
    std::shared_ptr<rtc::WebSocket> ws;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> video;
    std::shared_ptr<rtc::Track> audio;
    std::vector<std::shared_ptr<rtc::DataChannel>> dcs;
    {
      std::lock_guard lk(m_);
      ws = ws_;
      pc = std::move(pc_);
      video = std::move(videoTrack_);
      audio = std::move(audioTrack_);
      dcs = std::move(dcs_);
      ws_.reset();  // reset shared state under the mutex sendSignal() reads it with
    }
    if (ws) {
      try {
        ws->close();
      } catch (...) {
      }
    }
    // ws/pc/video/audio/dcs destruct here, outside m_: a libdatachannel
    // destructor that synchronously fires a callback can still post().
  }
#endif  // BC_WITH_LIVECALL
};

// ---------------------------------------------------------------------------
Client::Client(std::string url, std::string token)
    : impl_(std::make_unique<Impl>(std::move(url), std::move(token))) {}
Client::~Client() = default;

void Client::onVideo(std::function<void(VideoSample)> cb) {
  std::lock_guard lk(impl_->m_);
  impl_->onVideo_ = std::move(cb);
}
void Client::onAudio(std::function<void(AudioSample)> cb) {
  std::lock_guard lk(impl_->m_);
  impl_->onAudio_ = std::move(cb);
}
void Client::onState(std::function<void(SignalState, std::optional<ErrorInfo>)> cb) {
  impl_->setState(std::move(cb));
}
void Client::subscribe(std::string participantIdentity) {
  std::lock_guard lk(impl_->m_);
  impl_->wantIdentity_ = std::move(participantIdentity);
}
}  // namespace bc::lkc