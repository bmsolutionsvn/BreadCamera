// C1 Task 3 — BreadCallInput. The offline cases need no server; the live case is
// gated behind Req::LiveCall ("no LiveKit server").
#define DOCTEST_CONFIG_DOUBLE_STRINGIFY
#include <doctest/doctest.h>

// winsock2.h must precede the Windows headers pulled in by gpu/device.h.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "gpu/device.h"
#include "io/breadcall_input.h"
#include "io/input.h"
#include "support/requires.h"

using namespace std::chrono_literals;

namespace {
// A structurally valid LiveKit token (three base64url segments). It is not a
// real credential; anything that needs the server is skipped.
constexpr const char* kTokenShape = "eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiJ4In0.c2lnbmF0dXJl";

[[maybe_unused]] std::optional<std::string> envVar(const char* name) {
  char* buf = nullptr;
  size_t len = 0;
  if (_dupenv_s(&buf, &len, name) != 0 || buf == nullptr) return std::nullopt;
  std::string v(buf);
  std::free(buf);
  return v;
}

struct Sink : bc::InputSink {
  std::mutex m;
  std::condition_variable cv;
  std::vector<bc::SignalState> states;
  std::vector<std::optional<bc::ErrorInfo>> errors;
  int video = 0;
  std::optional<bc::PixelLayout> layout;
  uint32_t width = 0;
  uint32_t height = 0;

  void onVideo(bc::VideoFrameIn f) override {
    std::lock_guard l(m);
    ++video;
    layout = f.layout;
    width = f.tex->width;
    height = f.tex->height;
    cv.notify_all();
  }
  void onAudio(bc::AudioBlock) override {}
  void onState(bc::SignalState s, std::optional<bc::ErrorInfo> e) override {
    std::lock_guard l(m);
    states.push_back(s);
    errors.push_back(std::move(e));
    cv.notify_all();
  }
  bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds t) {
    std::unique_lock l(m);
    return cv.wait_for(l, t, pred);
  }
  // Called under waitFor's lock (predicate) and from the test thread after
  // stop(); no concurrent writer then, so it must not take m itself.
  int count(bc::SignalState s) {
    return static_cast<int>(std::count(states.begin(), states.end(), s));
  }
  std::optional<bc::ErrorInfo> lastError() {
    for (auto it = errors.rbegin(); it != errors.rend(); ++it)
      if (*it) return *it;
    return std::nullopt;
  }
};

struct Fixture {
  std::unique_ptr<bc::GpuDevice> gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool{*gpu};
};

#if BC_WITH_LIVECALL
// A one-shot TCP listener that accepts the WebSocket handshake and answers with
// HTTP 401 — how LiveKit rejects a well-formed but expired/unauthorised token.
// It lets the terminal auth path be tested without a LiveKit server.
class RejectingHandshakeServer {
 public:
  RejectingHandshakeServer() {
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_ == INVALID_SOCKET) return;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        listen(listen_, 4) == SOCKET_ERROR) {
      return;
    }
    int len = sizeof(addr);
    if (getsockname(listen_, reinterpret_cast<sockaddr*>(&addr), &len) == 0) port_ = ntohs(addr.sin_port);
    thread_ = std::thread([this] { serve(); });
  }
  ~RejectingHandshakeServer() { stop(); }
  RejectingHandshakeServer(const RejectingHandshakeServer&) = delete;
  RejectingHandshakeServer& operator=(const RejectingHandshakeServer&) = delete;

  int port() const { return static_cast<int>(port_); }

  void stop() {
    if (listen_ != INVALID_SOCKET) {
      closesocket(listen_);
      listen_ = INVALID_SOCKET;
    }
    if (thread_.joinable()) thread_.join();
  }

 private:
  void serve() {
    for (int i = 0; i < 4; ++i) {  // allow a couple of connection attempts
      SOCKET c = accept(listen_, nullptr, nullptr);
      if (c == INVALID_SOCKET) return;
      char buf[4096];
      (void)recv(c, buf, sizeof(buf), 0);  // drain the request headers (best effort)
      const char* resp = "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
      (void)send(c, resp, static_cast<int>(std::strlen(resp)), 0);
      closesocket(c);
    }
  }

  SOCKET listen_ = INVALID_SOCKET;
  unsigned short port_ = 0;
  std::thread thread_;
};
#endif  // BC_WITH_LIVECALL
}  // namespace

#if !BC_WITH_LIVECALL
// Without libdatachannel the input is not built: it must report input-config
// rather than reference the client. The live behaviour is covered by the ON build.
TEST_CASE("BreadCallInput: reports input-config when LiveKit support is not built") {
  Fixture f;
  bc::BreadCallInput in(*f.gpu, f.pool, "ws://127.0.0.1:7880", kTokenShape, 1);
  Sink sink;
  in.start(sink);
  REQUIRE(sink.waitFor([&] { return sink.count(bc::SignalState::Offline) > 0; }, 5s));
  in.stop();
  CHECK(sink.states.back() == bc::SignalState::Offline);
  auto e = sink.lastError();
  REQUIRE(e);
  CHECK(e->code == "input-config");
}
#else
TEST_CASE("BreadCallInput: a malformed url reports input-config") {
  Fixture f;
  bc::BreadCallInput in(*f.gpu, f.pool, "this is not a url", kTokenShape, 1);
  Sink sink;
  in.start(sink);
  REQUIRE(sink.waitFor([&] { return sink.count(bc::SignalState::Offline) > 0; }, 5s));
  in.stop();
  CHECK(sink.states.back() == bc::SignalState::Offline);
  auto e = sink.lastError();
  REQUIRE(e);
  CHECK(e->code == "input-config");
}

TEST_CASE("BreadCallInput: a bad token reports input-config and does not retry") {
  Fixture f;
  bc::BreadCallInput in(*f.gpu, f.pool, "ws://127.0.0.1:7880", "not-a-jwt", 1);
  Sink sink;
  in.start(sink);
  REQUIRE(sink.waitFor([&] { return sink.count(bc::SignalState::Offline) > 0; }, 5s));
  in.stop();
  auto e = sink.lastError();
  REQUIRE(e);
  CHECK(e->code == "input-config");
  CHECK(sink.count(bc::SignalState::Connecting) == 1);  // terminal: no reconnect
}

TEST_CASE("BreadCallInput: a well-formed token the server rejects reports terminal input-config") {
  // The token is a structurally valid JWT, so the failure is the server's
  // decision (LiveKit checks the token at the WebSocket upgrade): terminal
  // input-config, not an endless retry.
  Fixture f;
  RejectingHandshakeServer server;
  REQUIRE(server.port() > 0);
  bc::BreadCallInput in(*f.gpu, f.pool, "ws://127.0.0.1:" + std::to_string(server.port()), kTokenShape, 1);
  Sink sink;
  in.start(sink);
  REQUIRE(sink.waitFor([&] { return sink.count(bc::SignalState::Offline) > 0; }, 10s));
  in.stop();
  auto e = sink.lastError();
  REQUIRE(e);
  CHECK(e->code == "input-config");
  CHECK(sink.count(bc::SignalState::Connecting) == 1);  // terminal: no reconnect
}

TEST_CASE("BreadCallInput: a slot with no guest reports breadcall-no-guest") {
  Fixture f;
  bc::BreadCallInput in(*f.gpu, f.pool, "ws://127.0.0.1:7880", kTokenShape, 0);
  Sink sink;
  in.start(sink);
  REQUIRE(sink.waitFor([&] { return sink.count(bc::SignalState::Offline) > 0; }, 5s));
  in.stop();
  CHECK(sink.states.back() == bc::SignalState::Offline);
  auto e = sink.lastError();
  REQUIRE(e);
  CHECK(e->code == "breadcall-no-guest");
}

TEST_CASE("BreadCallInput: no server reports input-failed and retries, no crash") {
  Fixture f;
  bc::BreadCallInput in(*f.gpu, f.pool, "ws://127.0.0.1:1", kTokenShape, 1);
  Sink sink;
  in.start(sink);
  REQUIRE(sink.waitFor([&] { return sink.count(bc::SignalState::Connecting) >= 2; }, 15s));
  auto t0 = std::chrono::steady_clock::now();
  in.stop();
  CHECK(std::chrono::steady_clock::now() - t0 < 2000ms);
  REQUIRE(sink.states.size() >= 3);
  CHECK(sink.states[1] == bc::SignalState::Offline);
  REQUIRE(sink.errors.size() >= 2);
  REQUIRE(sink.errors[1]);
  CHECK(sink.errors[1]->code == "input-failed");
}

TEST_CASE("BreadCallInput: decodes a guest slot's H.264 to BGRA8"
          BC_REQUIRES(bc::test::Req::LiveCall)) {
  Fixture f;
  const std::string url = envVar("BC_LIVECALL_URL").value_or("");
  const std::string token = envVar("BC_LIVECALL_TOKEN").value_or("");
  const int slot = std::stoi(envVar("BC_LIVECALL_SLOT").value_or("1"));
  bc::BreadCallInput in(*f.gpu, f.pool, url, token, slot);
  Sink sink;
  in.start(sink);
  const bool got = sink.waitFor([&] { return sink.video > 0; }, 30s);
  if (!got) {
    auto e = sink.lastError();
    std::fprintf(stderr, "[breadcall live] no video; last error: %s: %s\n",
                 e ? e->code.c_str() : "(none)", e ? e->message.c_str() : "");
  }
  in.stop();
  REQUIRE(got);
  std::fprintf(stderr, "[breadcall live] decoded %d frame(s), first %ux%u BGRA8\n", sink.video, sink.width,
               sink.height);
  CHECK(sink.video > 0);
  CHECK(sink.layout == bc::PixelLayout::BGRA8);
  CHECK(sink.width > 0);
  CHECK(sink.height > 0);
  CHECK(sink.count(bc::SignalState::Live) >= 1);
}
#endif  // BC_WITH_LIVECALL
