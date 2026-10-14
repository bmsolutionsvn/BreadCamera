// C1 Task 5 — the C1 acceptance: BreadCamera pulls two BreadCall guests from a
// local LiveKit room as cameras, and slot 2's input keeps working when a
// different publisher takes guest_2 (slot stability).
//
// The two guests are real publishers: a loopback TCP feeder streams an H.264
// clip to the LiveKit CLI (`lk room join --publish h264://…`), which joins the
// room as `guest_1` / `guest_2`. The receiver tokens are hidden, subscribe-only
// grants minted with `lk token create`. Nothing is faked — the inputs run the
// same lkc/BreadCallInput path as production.
//
// Gated behind Req::LiveCall (a live server, per the task brief) and
// Req::LiveCallGuests (the `lk` CLI the harness drives).
#define DOCTEST_CONFIG_DOUBLE_STRINGIFY
#include <doctest/doctest.h>

// winsock2.h must precede the Windows headers pulled in by gpu/device.h.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "gpu/device.h"
#include "io/breadcall_input.h"
#include "io/input.h"
#include "support/h264_black720.h"
#include "support/requires.h"

using namespace std::chrono_literals;

#if BC_WITH_LIVECALL
namespace {
std::optional<std::string> envVar(const char* name) {
  char* buf = nullptr;
  size_t len = 0;
  if (_dupenv_s(&buf, &len, name) != 0 || buf == nullptr) return std::nullopt;
  std::string v(buf);
  std::free(buf);
  return v;
}

std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
  return s.substr(a, b - a);
}

// The LiveKit HTTP API URL (`lk --url`) from the receiver WebSocket URL.
std::string httpUrl(std::string url) {
  if (url.rfind("wss://", 0) == 0) return "https://" + url.substr(6);
  if (url.rfind("ws://", 0) == 0) return "http://" + url.substr(5);
  return url;
}

std::filesystem::path scratch(const std::string& leaf) {
  wchar_t buf[MAX_PATH];
  GetTempPathW(MAX_PATH, buf);
  auto dir = std::filesystem::path(buf) / ("bc_c1_acc_" + std::to_string(GetCurrentProcessId()));
  std::filesystem::create_directories(dir);
  return dir / leaf;
}

// A spawned child process; kills on destruction so a REQUIRE abort leaves none.
class Child {
 public:
  Child() = default;
  ~Child() { kill(); if (proc_) CloseHandle(proc_); }
  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;
  Child(Child&& o) noexcept : proc_(o.proc_), pid_(o.pid_) { o.proc_ = nullptr; }
  Child& operator=(Child&& o) noexcept {
    if (this != &o) {
      kill();
      if (proc_) CloseHandle(proc_);
      proc_ = o.proc_;
      pid_ = o.pid_;
      o.proc_ = nullptr;
    }
    return *this;
  }
  explicit operator bool() const { return proc_ != nullptr; }
  bool running() const { return proc_ && WaitForSingleObject(proc_, 0) == WAIT_TIMEOUT; }
  int exitCode() const {
    DWORD code = 0;
    return proc_ && GetExitCodeProcess(proc_, &code) ? static_cast<int>(code) : -1;
  }
  void kill() {
    if (!proc_) return;
    if (running()) TerminateProcess(proc_, 1);
    WaitForSingleObject(proc_, 3000);
  }
  DWORD pid() const { return pid_; }

 private:
  friend Child spawn(const std::wstring&, const std::wstring&, const std::filesystem::path&,
                     const std::filesystem::path&);
  HANDLE proc_ = nullptr;
  DWORD pid_ = 0;
};

Child spawn(const std::wstring& exe, const std::wstring& args, const std::filesystem::path& out,
            const std::filesystem::path& err) {
  Child c;
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  const auto openW = [&](const std::filesystem::path& p, DWORD access, DWORD disp) -> HANDLE {
    if (p.empty()) return CreateFileW(L"NUL", access, FILE_SHARE_READ, &sa, disp, FILE_ATTRIBUTE_NORMAL, nullptr);
    return CreateFileW(p.wstring().c_str(), access, FILE_SHARE_READ, &sa, disp, FILE_ATTRIBUTE_NORMAL, nullptr);
  };
  HANDLE outH = openW(out, GENERIC_WRITE, CREATE_ALWAYS);
  HANDLE errH = openW(err, GENERIC_WRITE, CREATE_ALWAYS);
  HANDLE inH = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  STARTUPINFOW si{sizeof(si)};
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = outH;
  si.hStdError = errH;
  si.hStdInput = inH;
  PROCESS_INFORMATION pi{};
  std::wstring cmd = L"\"" + exe + L"\"" + (args.empty() ? L"" : L" " + args);
  if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    CloseHandle(pi.hThread);
    c.proc_ = pi.hProcess;
    c.pid_ = pi.dwProcessId;
  }
  for (HANDLE h : {outH, errH, inH})
    if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
  return c;
}

struct RunResult {
  int code = -1;
  std::string out;
};

// Runs `lk` to completion and returns its exit code and stdout.
RunResult runCapture(const std::wstring& exe, const std::wstring& args, const std::string& tag) {
  const auto out = scratch(tag + ".out");
  const auto err = scratch(tag + ".err");
  Child c = spawn(exe, args, out, err);
  RunResult r;
  if (!c) return r;
  for (int i = 0; i < 1200 && c.running(); ++i) std::this_thread::sleep_for(25ms);
  std::ifstream f(out, std::ios::binary);
  r.out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  r.code = c.running() ? -1 : c.exitCode();
  return r;
}

// A loopback TCP server that streams the embedded H.264 clip to one connection
// (the LiveKit CLI's `h264://host:port` publisher).
class Feeder {
 public:
  Feeder() {
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_ == INVALID_SOCKET) return;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(listen_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(listen_, 1) != 0) return;
    int len = sizeof(a);
    if (getsockname(listen_, reinterpret_cast<sockaddr*>(&a), &len) == 0) port_ = ntohs(a.sin_port);
    thread_ = std::thread([this] { run(); });
  }
  ~Feeder() { stop(); }
  Feeder(const Feeder&) = delete;
  Feeder& operator=(const Feeder&) = delete;

  int port() const { return port_; }
  bool ok() const { return port_ != 0; }

  void stop() {
    running_ = false;
    if (listen_ != INVALID_SOCKET) {
      closesocket(listen_);
      listen_ = INVALID_SOCKET;
    }
    {
      std::lock_guard l(m_);
      if (client_ != INVALID_SOCKET) {
        shutdown(client_, SD_BOTH);
        closesocket(client_);
        client_ = INVALID_SOCKET;
      }
    }
    if (thread_.joinable()) thread_.join();
  }

 private:
  void run() {
    SOCKET c = accept(listen_, nullptr, nullptr);
    if (c == INVALID_SOCKET) return;
    {
      std::lock_guard l(m_);
      client_ = c;
    }
    const char* data = reinterpret_cast<const char*>(bc::test::kH264Black720);
    const int size = static_cast<int>(bc::test::kH264Black720Size);
    while (running_) {
      if (send(c, data, size, 0) <= 0) break;
      std::this_thread::sleep_for(100ms);
    }
  }

  SOCKET listen_ = INVALID_SOCKET;
  SOCKET client_ = INVALID_SOCKET;
  int port_ = 0;
  std::atomic<bool> running_{true};
  std::mutex m_;
  std::thread thread_;
};

struct Sink : bc::InputSink {
  std::mutex m;
  std::condition_variable cv;
  std::vector<bc::SignalState> states;
  std::vector<std::optional<bc::ErrorInfo>> errors;
  int video = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  std::optional<bc::PixelLayout> layout;

  void onVideo(bc::VideoFrameIn f) override {
    std::lock_guard l(m);
    ++video;
    width = f.tex->width;
    height = f.tex->height;
    layout = f.layout;
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
  int count(bc::SignalState s) {
    std::lock_guard l(m);
    return static_cast<int>(std::count(states.begin(), states.end(), s));
  }
  int videoCount() {
    std::lock_guard l(m);
    return video;
  }
  std::optional<bc::ErrorInfo> lastError() {
    std::lock_guard l(m);
    for (auto it = errors.rbegin(); it != errors.rend(); ++it)
      if (*it) return *it;
    return std::nullopt;
  }
};

// Runs `lk room participants get` for one identity; present() is false when the
// participant is gone (the CLI exits non-zero).
struct ParticipantQuery {
  bool present = false;
  bool hasVideo = false;
  std::string raw;
};

ParticipantQuery queryParticipant(const std::wstring& lkExe, const std::string& http, const std::string& room,
                                  const std::string& identity) {
  std::wstring args = L"room participants get --dev --url " + widen(http) + L" -r " + widen(room) + L" -i " +
                      widen(identity);
  RunResult r = runCapture(lkExe, args, "part_" + identity + ".txt");
  ParticipantQuery q;
  q.raw = r.out;
  q.present = r.code == 0 && r.out.find("\"identity\"") != std::string::npos;
  q.hasVideo = r.out.find("mimeType") != std::string::npos;
  return q;
}

bool waitForVideo(const std::wstring& lkExe, const std::string& http, const std::string& room,
                  const std::string& identity, std::chrono::milliseconds timeout, std::string& lastRaw) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    ParticipantQuery q = queryParticipant(lkExe, http, room, identity);
    lastRaw = q.raw;
    if (q.hasVideo) return true;
    std::this_thread::sleep_for(500ms);
  }
  return false;
}

Child spawnGuest(const std::wstring& lkExe, const std::string& http, const std::string& room,
                 const std::string& identity, int port) {
  std::wstring args = L"room join --dev --url " + widen(http) + L" -r " + widen(room) + L" -i " +
                      widen(identity) + L" --publish h264://127.0.0.1:" + std::to_wstring(port) + L" --fps 30";
  return spawn(lkExe, args, {}, {});
}

std::string mintReceiverToken(const std::wstring& lkExe, const std::string& http, const std::string& room,
                              const std::string& identity) {
  std::wstring args = L"token create --dev --url " + widen(http) + L" -r " + widen(room) + L" -i " +
                      widen(identity) +
                      L" --join --grant \"{\\\"canPublish\\\":false,\\\"canSubscribe\\\":true,\\\"hidden\\\":true}\""
                      L" --token-only";
  RunResult r = runCapture(lkExe, args, "token_" + identity + ".txt");
  // Keep the token: a JWT is the last line of stdout (errors go to stderr).
  std::string best;
  size_t start = 0;
  while (start <= r.out.size()) {
    size_t nl = r.out.find('\n', start);
    std::string line = trim(r.out.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
    if (line.find('.') != std::string::npos && line.size() > 40) best = line;
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
  return best;
}

struct Fixture {
  std::unique_ptr<bc::GpuDevice> gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool{*gpu};
};

// Deletes the room on scope exit (declared after the values it reads).
struct RoomGuard {
  std::function<void()> f;
  ~RoomGuard() {
    if (f) f();
  }
};

// True when every named receiver holds no published track: BreadCamera sends
// nothing back to the room (roadmap R5).
bool receiversPublishNothing(const std::wstring& lkExe, const std::string& http, const std::string& room,
                             const std::vector<std::string>& identities, std::string& detail) {
  bool ok = true;
  for (const auto& id : identities) {
    ParticipantQuery q = queryParticipant(lkExe, http, room, id);
    detail += id + (q.present ? " present" : " MISSING") + (q.hasVideo ? " PUBLISHES" : " no-tracks") + "; ";
    ok = ok && q.present && !q.hasVideo;
  }
  return ok;
}
}  // namespace

TEST_CASE("C1 acceptance: two simultaneous guests are two BreadCamera cameras"
          BC_REQUIRES(bc::test::Req::LiveCall)
          BC_REQUIRES(bc::test::Req::LiveCallGuests)) {
  const std::string url = envVar("BC_LIVECALL_URL").value_or("");
  const std::string http = httpUrl(url);
  const std::wstring lkExe = std::filesystem::path(envVar("BC_LIVECALL_LK").value_or("")).wstring();
  const std::string room = "c1acc_two_" + std::to_string(GetCurrentProcessId());
  RoomGuard roomGuard{[&] {
    (void)runCapture(lkExe, L"room delete --dev --url " + widen(http) + L" " + widen(room), "room_del_two.txt");
  }};

  Fixture f;

  // Two guests publish a synthetic H.264 track each; every resource cleans itself
  // up on destruction, so a REQUIRE abort mid-case leaves nothing behind.
  Feeder feed1, feed2;
  REQUIRE(feed1.ok());
  REQUIRE(feed2.ok());
  Child guest1 = spawnGuest(lkExe, http, room, "guest_1", feed1.port());
  Child guest2 = spawnGuest(lkExe, http, room, "guest_2", feed2.port());
  REQUIRE(guest1);
  REQUIRE(guest2);

  std::string raw;
  // The acceptance needs a real track per slot, not just a participant.
  if (!waitForVideo(lkExe, http, room, "guest_1", 45s, raw) ||
      !waitForVideo(lkExe, http, room, "guest_2", 45s, raw)) {
    MESSAGE("a guest never published a video track; last participants output:\n" << raw);
    REQUIRE(false);
  }

  const std::string token1 = mintReceiverToken(lkExe, http, room, "breadcamera_c1_a");
  const std::string token2 = mintReceiverToken(lkExe, http, room, "breadcamera_c1_b");
  REQUIRE(token1.find('.') != std::string::npos);
  REQUIRE(token2.find('.') != std::string::npos);

  bc::BreadCallInput in1(*f.gpu, f.pool, url, token1, 1);
  bc::BreadCallInput in2(*f.gpu, f.pool, url, token2, 2);
  Sink s1, s2;
  in1.start(s1);
  in2.start(s2);

  s1.waitFor([&] { return s1.video > 0; }, 60s);
  s2.waitFor([&] { return s2.video > 0; }, 60s);
  MESSAGE("two guests: slot1 " << s1.videoCount() << " frames / " << s1.count(bc::SignalState::Live)
                               << " live, slot2 " << s2.videoCount() << " frames / "
                               << s2.count(bc::SignalState::Live) << " live");

  // BreadCamera sends nothing back: the two receivers hold no published tracks.
  std::string detail;
  CHECK(receiversPublishNothing(lkExe, http, room, {"breadcamera_c1_a", "breadcamera_c1_b"}, detail));
  MESSAGE("receivers: " << detail);

  REQUIRE(s1.videoCount() > 0);
  REQUIRE(s1.count(bc::SignalState::Live) >= 1);
  REQUIRE(s2.videoCount() > 0);
  REQUIRE(s2.count(bc::SignalState::Live) >= 1);
  CHECK(s1.layout == bc::PixelLayout::BGRA8);
  CHECK(s2.layout == bc::PixelLayout::BGRA8);
  CHECK(s1.width > 0);
  CHECK(s2.width > 0);

  in1.stop();
  in2.stop();
}

TEST_CASE("C1 acceptance: slot 2's camera stays slot 2's across a guest change"
          BC_REQUIRES(bc::test::Req::LiveCall)
          BC_REQUIRES(bc::test::Req::LiveCallGuests)) {
  const std::string url = envVar("BC_LIVECALL_URL").value_or("");
  const std::string http = httpUrl(url);
  const std::wstring lkExe = std::filesystem::path(envVar("BC_LIVECALL_LK").value_or("")).wstring();
  const std::string room = "c1acc_slot_" + std::to_string(GetCurrentProcessId());
  RoomGuard roomGuard{[&] {
    (void)runCapture(lkExe, L"room delete --dev --url " + widen(http) + L" " + widen(room), "room_del_slot.txt");
  }};

  Fixture f;
  Feeder feed1;
  REQUIRE(feed1.ok());
  Child guest2 = spawnGuest(lkExe, http, room, "guest_2", feed1.port());
  REQUIRE(guest2);

  std::string raw;
  if (!waitForVideo(lkExe, http, room, "guest_2", 45s, raw)) {
    MESSAGE("guest_2 never published a video track; last output:\n" << raw);
    REQUIRE(false);
  }

  const std::string token = mintReceiverToken(lkExe, http, room, "breadcamera_c1_slot2");
  REQUIRE(token.find('.') != std::string::npos);
  bc::BreadCallInput in(*f.gpu, f.pool, url, token, 2);
  Sink s;
  in.start(s);
  REQUIRE(s.waitFor([&] { return s.video > 0; }, 60s));
  REQUIRE(s.count(bc::SignalState::Live) >= 1);
  const int liveBefore = s.count(bc::SignalState::Live);
  MESSAGE("slot 2 first guest live: " << s.videoCount() << " frames " << s.width << "x" << s.height);

  // A *different* publisher takes guest_2: kill the first, then start a second
  // one on the same slot. LiveKit lingers an abruptly-killed participant for a
  // while, so settle by watching the frames go quiet instead of waiting for the
  // participant to disappear; a same-identity join replaces the stale one.
  guest2.kill();
  feed1.stop();

  // Let slot 2's frames go quiet, so the next frames can only come from the
  // replacement guest.
  int quiet = s.videoCount();
  for (int i = 0, same = 0; i < 40 && same < 6; ++i) {
    std::this_thread::sleep_for(250ms);
    const int v = s.videoCount();
    same = (v == quiet) ? same + 1 : 0;
    quiet = v;
  }
  const int atSwap = s.videoCount();

  Feeder feed2;
  REQUIRE(feed2.ok());
  Child guest2b = spawnGuest(lkExe, http, room, "guest_2", feed2.port());
  REQUIRE(guest2b);
  if (!waitForVideo(lkExe, http, room, "guest_2", 45s, raw)) {
    MESSAGE("the replacement guest_2 never published a video track; last output:\n" << raw);
    REQUIRE(false);
  }

  // The same slot-2 input delivers frames from the new guest_2.
  REQUIRE(s.waitFor([&] { return s.video > atSwap + 2; }, 45s));
  CHECK(s.count(bc::SignalState::Live) >= liveBefore);
  MESSAGE("slot stability: same slot-2 input " << atSwap << " -> " << s.videoCount()
                                               << " frames across the guest change");

  // BreadCamera sends nothing back.
  std::string detail;
  CHECK(receiversPublishNothing(lkExe, http, room, {"breadcamera_c1_slot2"}, detail));
  MESSAGE("receiver: " << detail);

  in.stop();
}
#endif  // BC_WITH_LIVECALL