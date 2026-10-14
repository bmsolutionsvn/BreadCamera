// bc_soak: spec §8.3 soak + §8.4 fault injection against the real engine executable.
//
// Adaptations from the M5 task-3 brief (controller ruling M5-1), all to match the real
// M1/M2 code:
//   * The supervisor is fed heartbeats (EngineClient calls onAlive on every inbound frame).
//     The brief polled the supervisor but never beat it, so its 3 s heartbeat timeout would
//     have killed a healthy engine ~11 s in; the real Supervisor (src/shell/supervisor.h)
//     requires heartbeat() "on every ping reply".
//   * The 1280x720 per-output format is set on OutputConfig::width/height (the real model,
//     spec §4.3); the brief put width/height in params, but SrtOutputOptions ignores them
//     and Camera::endTick sizes the frame from OutputConfig.
//   * OutputConfig fields are assigned explicitly instead of aggregate-initialised so the
//     format fields cannot be skipped silently.
#include <windows.h>
#include <psapi.h>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/show.h"
#include "core/show_store.h"
#include "core/stage_params.h"
#include "core/utf8.h"
#include "engine/control_server.h"
#include "gpu/device.h"
#include "gpu/texture_pool.h"
#include "io/ffmpeg_input.h"
#include "io/srt_params.h"
#include "shell/process_launcher.h"
#include "shell/supervisor.h"
#include "support/test_media.h"

namespace fs = std::filesystem;
using nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {
constexpr int kCams = 4;
constexpr int kPort = 47970;

// Orphan protection (M5 review): if this tool dies abnormally (Ctrl-C, console close, an
// unhandled exception), kill the engine we launched and drop the temp show dir. Otherwise a
// stale breadcamera-engine.exe keeps holding Local\BreadCameraEngine and the next run exits 2.
std::atomic<bc::ProcessLauncher*> g_launcher{nullptr};
fs::path g_soakDir;
std::atomic<bool> g_soakDirSet{false};

void orphanCleanup() {
  if (auto* l = g_launcher.load(); l && l->isRunning()) l->kill();
  if (g_soakDirSet.load()) {
    std::error_code ec;
    fs::remove_all(g_soakDir, ec);
  }
}

BOOL WINAPI soakCtrlHandler(DWORD type) {
  switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
      orphanCleanup();
      ExitProcess(1);
    default:
      return FALSE;
  }
}

void soakTerminateHandler() {
  orphanCleanup();
  ExitProcess(1);
}

struct Args {
  int minutes = 30;
  bool faults = false;
  std::string out;
};

Args parseArgs(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    if (k == "--minutes" && i + 1 < argc) a.minutes = std::stoi(argv[++i]);
    else if (k == "--faults") a.faults = true;
    else if (k == "--out" && i + 1 < argc) a.out = argv[++i];
  }
  if (a.out.empty()) a.out = (fs::temp_directory_path() / "bcam-soak.csv").string();
  return a;
}

// Control-API client: replies matched by id; state kept from snapshot + patches; last stats.
class EngineClient {
 public:
  EngineClient(int port, const std::string& token, std::function<void()> onAlive = {})
      : onAlive_(std::move(onAlive)) {
    ws_.setUrl("ws://127.0.0.1:" + std::to_string(port) + "/?token=" + token);
    ws_.setMinWaitBetweenReconnectionRetries(250);
    ws_.setMaxWaitBetweenReconnectionRetries(500);
    ws_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& m) { onMessage(m); });
    ws_.start();
  }
  ~EngineClient() { ws_.stop(); }

  std::optional<json> request(const std::string& cmd, json args = json::object(),
                              std::chrono::milliseconds timeout = 2000ms) {
    int id;
    {
      std::lock_guard l(m_);
      id = ++next_;
    }
    ws_.send(json{{"id", id}, {"cmd", cmd}, {"args", args}}.dump());
    std::unique_lock l(m_);
    if (!cv_.wait_for(l, timeout, [&] { return replies_.count(id) > 0; })) return std::nullopt;
    json r = replies_[id];
    replies_.erase(id);
    if (!r.value("ok", false)) return std::nullopt;
    return r["result"];
  }
  json state() {
    std::lock_guard l(m_);
    return state_;
  }
  json stats() {
    std::lock_guard l(m_);
    return stats_;
  }
  std::string at(const std::string& ptr) {
    std::lock_guard l(m_);
    json::json_pointer p(ptr);
    return state_.contains(p) && state_.at(p).is_string() ? state_.at(p).get<std::string>() : "";
  }

 private:
  void onMessage(const ix::WebSocketMessagePtr& m) {
    // Any inbound frame proves the engine is alive: feed the supervisor's heartbeat.
    if (onAlive_) onAlive_();
    if (m->type != ix::WebSocketMessageType::Message || m->binary) return;
    json j = json::parse(m->str, nullptr, false);
    if (!j.is_object()) return;
    std::lock_guard l(m_);
    const std::string type = j.value("type", "");
    if (type == "snapshot") {
      state_ = j["state"];
    } else if (type == "patch") {
      for (auto& op : j["ops"]) {
        json::json_pointer p(op["path"].get<std::string>());
        if (op["op"] == "replace") state_[p] = op["value"];
        else if (state_.contains(p)) state_.at(p.parent_pointer()).erase(p.back());
      }
    } else if (type == "stats") {
      stats_ = j;
    } else if (j.contains("id") && j["id"].is_number_integer()) {
      replies_[j["id"].get<int>()] = j;
      cv_.notify_all();
    }
  }
  ix::WebSocket ws_;
  std::function<void()> onAlive_;
  std::mutex m_;
  std::condition_variable cv_;
  int next_ = 0;
  std::map<int, json> replies_;
  json state_ = json::object();
  json stats_ = json::object();
};

struct Counter : bc::InputSink {
  std::atomic<int64_t> frames{0};
  void onVideo(bc::VideoFrameIn) override { ++frames; }
  void onAudio(bc::AudioBlock) override {}
  void onState(bc::SignalState, std::optional<bc::ErrorInfo>) override {}
};

std::string camId(int i) { return "soak" + std::to_string(i); }

bc::Show buildShow() {
  bc::Show show;
  for (int i = 0; i < kCams; ++i) {
    bc::CameraConfig c;
    c.id = camId(i);
    c.name = "Soak " + std::to_string(i + 1);
    c.input.kind = "srt";
    c.input.params = {{"mode", "listener"}, {"host", "127.0.0.1"}, {"port", 47971 + i}};
    c.stages = {{"color", bc::normalizeStageParams("color", {{"enabled", true}, {"exposure", 0.1}})},
                {"matte", bc::normalizeStageParams("matte", {{"enabled", true}})},
                {"background", bc::normalizeStageParams("background", {{"mode", "blur"}})}};
    // The per-output format lives on OutputConfig (spec §4.3); params carry only the SRT keys.
    bc::OutputConfig srt;
    srt.id = "srt" + std::to_string(i);
    srt.kind = "srt";
    srt.keyed = false;
    srt.enabled = true;
    srt.params = {{"mode", "listener"}, {"host", "127.0.0.1"}, {"port", 47981 + i}, {"bitrateKbps", 6000}};
    srt.width = 1280;
    srt.height = 720;
    c.outputs.push_back(std::move(srt));
    if (i == 0) {
      bc::OutputConfig fs;
      fs.id = "fs0";
      fs.kind = "fullscreen";
      fs.keyed = false;
      fs.enabled = true;
      fs.params = {{"monitor", 0}};
      c.outputs.push_back(std::move(fs));
    }
    show.cameras.push_back(std::move(c));
  }
  return show;
}

double workingSetMb(DWORD pid) {
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  if (!h) return 0;
  PROCESS_MEMORY_COUNTERS pmc{};
  GetProcessMemoryInfo(h, &pmc, sizeof(pmc));
  CloseHandle(h);
  return static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
}

bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout) {
  const auto end = Clock::now() + timeout;
  while (Clock::now() < end) {
    if (pred()) return true;
    std::this_thread::sleep_for(100ms);
  }
  return pred();
}

struct Rig {
  fs::path dir, clip;
  std::vector<std::unique_ptr<bc::test::TsPusher>> pushers;
  std::unique_ptr<bc::GpuDevice> gpu;
  std::unique_ptr<bc::TexturePool> pool;
  std::vector<std::unique_ptr<bc::FfmpegInput>> readers;
  Counter counts[kCams];

  void startPusher(int i) {
    pushers[i] = std::make_unique<bc::test::TsPusher>(clip, "srt://127.0.0.1:" + std::to_string(47971 + i) + "?mode=caller", true);
  }
  void startReader(int i) {
    readers[i] = std::make_unique<bc::FfmpegInput>(
        *gpu, *pool, bc::srtOptionsFromParams({{"mode", "caller"}, {"host", "127.0.0.1"}, {"port", 47981 + i}}));
    readers[i]->start(counts[i]);
  }
};

// §8.4: input killed → hold → slate → live on return.
std::string faultInputLoss(Rig& rig, EngineClient& e) {
  const std::string p = "/cameras/" + camId(0) + "/phase";
  rig.pushers[0].reset();
  bool held = false;
  const bool slated = waitFor([&] {
    held = held || e.at(p) == "holding";
    return e.at(p) == "slate";
  }, 9000ms);
  rig.startPusher(0);
  const bool back = waitFor([&] { return e.at(p) == "live"; }, 10000ms);
  if (!held || !slated || !back) return "input-loss fault: held=" + std::to_string(held) + " slated=" + std::to_string(slated) + " back=" + std::to_string(back);
  return "";
}

// §8.4: one receiver killed → the others keep receiving.
std::string faultReceiverLoss(Rig& rig) {
  rig.readers[1]->stop();
  int64_t before[kCams];
  for (int i = 0; i < kCams; ++i) before[i] = rig.counts[i].frames;
  std::this_thread::sleep_for(30s);
  std::string err;
  for (int i : {0, 2, 3}) {
    if (rig.counts[i].frames - before[i] < 30 * 50) err += "receiver " + std::to_string(i) + " slowed while receiver 1 was down; ";
  }
  rig.startReader(1);
  return err;
}

// §8.4: a stage forced to fail → bypassed/errored for that camera only, frames continue.
std::string faultStage(Rig& rig, EngineClient& e) {
  const auto cam = camId(2);
  e.request("setStage", {{"cameraId", cam}, {"stage", "background"}, {"params", {{"mode", "replace"}, {"image", "C:/definitely/missing.png"}}}});
  const bool flagged = waitFor([&] { return e.at("/cameras/" + cam + "/stages/background/error/code") == "background-image"; }, 4000ms);
  // "Frames continue" must be sampled over a fixed span: the missing-file loadImageBgra throws
  // immediately, so the error flag lands within ~100 ms and a before/after around the setStage
  // round-trip spans only a handful of frames even at full rate (the check would always fail).
  // Sample a clean 2 s window while the failing stage is bypassed instead. Criterion unchanged.
  const int64_t before = rig.counts[2].frames;
  std::this_thread::sleep_for(2s);
  const int64_t flowed = rig.counts[2].frames - before;
  const bool flowing = flowed > 100;
  e.request("setStage", {{"cameraId", cam}, {"stage", "background"}, {"params", {{"mode", "blur"}, {"image", ""}}}});
  const bool othersOk = e.at("/cameras/" + camId(1) + "/stages/background/error/code").empty();
  if (!flagged || !flowing || !othersOk) return "stage fault: flagged=" + std::to_string(flagged) + " flowing=" + std::to_string(flowing) + " frames2s=" + std::to_string(flowed) + " othersOk=" + std::to_string(othersOk);
  return "";
}

// §8.4: engine killed → back with its show in < 3 s.
std::string faultEngineKill(bc::ProcessLauncher& launcher, EngineClient& e) {
  launcher.kill();
  const auto t0 = Clock::now();
  const bool back = waitFor([&] { return e.request("ping", json::object(), 300ms).has_value(); }, 10000ms);
  const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0);
  auto show = e.request("getShow");
  const bool showOk = show && (*show)["cameras"].size() == kCams;
  if (!back || took >= 3000ms || !showOk) return "engine-kill fault: back=" + std::to_string(back) + " took=" + std::to_string(took.count()) + "ms showOk=" + std::to_string(showOk);
  return "";
}
}  // namespace

int main(int argc, char** argv) {
  // Install the orphan guard before anything can launch or create state (see orphanCleanup).
  SetConsoleCtrlHandler(soakCtrlHandler, TRUE);
  std::set_terminate(soakTerminateHandler);
  const Args args = parseArgs(argc, argv);
  ix::initNetSystem();
  Rig rig;
  rig.dir = fs::temp_directory_path() / ("bcam-soak-" + std::to_string(GetCurrentProcessId()));
  fs::create_directories(rig.dir);
  g_soakDir = rig.dir;
  g_soakDirSet = true;
  {
    bc::ShowStore store(rig.dir / "shows");
    store.saveNow(buildShow());
  }
  wchar_t self[MAX_PATH];
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  const fs::path engineExe = fs::path(self).parent_path() / "breadcamera-engine.exe";
  if (!fs::exists(engineExe)) {
    std::cerr << "setup: " << engineExe.string() << " not found\n";
    orphanCleanup();
    return 2;
  }
  const std::string token = bc::randomToken();
  bc::ProcessLauncher launcher(engineExe, L"--port " + std::to_wstring(kPort) + L" --token " + bc::toWide(token) + L" --show-dir \"" +
                                             (rig.dir / "shows").wstring() + L"\" --discovery \"" + (rig.dir / "engine.json").wstring() + L"\"");
  g_launcher = &launcher;
  bc::Supervisor sup(launcher);
  std::atomic<bool> supervising{true};
  std::thread supThread([&] {
    while (supervising) {
      sup.poll(Clock::now());
      std::this_thread::sleep_for(100ms);
    }
  });
  // The supervisor's heartbeat comes from the control socket: any inbound frame proves the
  // engine is alive, so the restart policy only fires on a real hang or the kill fault.
  EngineClient engine(kPort, token, [&] { sup.heartbeat(Clock::now()); });
  if (!waitFor([&] { return engine.request("ping", json::object(), 500ms).has_value(); }, 15000ms)) {
    std::cerr << "setup: the engine did not answer within 15 s\n";
    supervising = false;
    supThread.join();
    launcher.kill();
    orphanCleanup();
    return 2;
  }
  for (const char* kind : {"ndi", "omt"}) {
    for (int i = 0; i < kCams; ++i) {
      if (!engine.request("addOutput", {{"cameraId", camId(i)}, {"output", {{"kind", kind}}}})) std::cout << "note: " << kind << " output refused on " << camId(i) << "\n";
    }
  }
  rig.clip = rig.dir / "clip.ts";
  bc::test::writeTestTs(rig.clip, 1920, 1080, 60, 10);
  rig.pushers.resize(kCams);
  rig.readers.resize(kCams);
  for (int i = 0; i < kCams; ++i) rig.startPusher(i);
  rig.gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  rig.pool = std::make_unique<bc::TexturePool>(*rig.gpu);
  for (int i = 0; i < kCams; ++i) rig.startReader(i);

  std::ofstream csv(args.out);
  csv << "seconds,overruns,camera,fps,latencyMs,phase,matte,workingSetMB,receiverFrames\n";
  std::vector<std::string> failures;
  std::map<std::string, std::vector<double>> latencies;
  const auto start = Clock::now();
  const auto end = start + std::chrono::minutes(args.minutes);
  auto nextFault = start + 5min;
  int faultIndex = 0, samples = 0;
  int64_t lastOverruns = -1;
  double baselineWs = 0, lastWs = 0;
  auto baselineAt = start + 10min;
  bool liveSeen[kCams] = {};  // per-camera liveness floor: each must reach phase `live` once
  while (Clock::now() < end) {
    std::this_thread::sleep_for(10s);
    const double t = std::chrono::duration<double>(Clock::now() - start).count();
    const json st = engine.stats();
    const int64_t overruns = st.contains("engine") ? st["engine"].value("overruns", int64_t{0}) : 0;
    if (lastOverruns >= 0 && overruns > lastOverruns) failures.push_back("tick overruns (+" + std::to_string(overruns - lastOverruns) + ") at t=" + std::to_string(static_cast<int>(t)) + " s");
    lastOverruns = overruns;
    lastWs = workingSetMb(launcher.pid());
    if (baselineWs == 0 && Clock::now() >= baselineAt) baselineWs = lastWs;
    for (int i = 0; i < kCams; ++i) {
      const auto id = camId(i);
      const json c = st.contains("cameras") && st["cameras"].contains(id) ? st["cameras"][id] : json::object();
      const double lat = c.value("latencyMs", 0.0);
      latencies[id].push_back(lat);
      const std::string phase = engine.at("/cameras/" + id + "/phase");
      if (phase == "live") liveSeen[i] = true;
      csv << t << ',' << overruns << ',' << id << ',' << c.value("fps", 0.0) << ',' << lat << ',' << phase << ','
          << engine.at("/cameras/" + id + "/stages/matte/state") << ',' << lastWs << ',' << rig.counts[i].frames << '\n';
    }
    csv.flush();
    ++samples;
    if (args.faults && Clock::now() >= nextFault) {
      std::string r;
      switch (faultIndex++ % 4) {
        case 0: r = faultInputLoss(rig, engine); break;
        case 1: r = faultReceiverLoss(rig); break;
        case 2: r = faultStage(rig, engine); break;
        case 3:
          r = faultEngineKill(launcher, engine);
          baselineWs = 0;  // a new process: take the next sample as the baseline so §8.3 still evaluates
          baselineAt = Clock::now();
          break;
      }
      if (!r.empty()) failures.push_back(r);
      std::cout << "fault " << (faultIndex - 1) % 4 << ": " << (r.empty() ? "ok" : r) << "\n";
      lastOverruns = -1;  // a fault window may overrun once while things reconnect
      nextFault = Clock::now() + 5min;
    }
  }

  if (samples == 0) failures.push_back("no samples collected");
  // Liveness floor (M5 review #4): answering ping is not proof the pipeline carried a frame.
  // Every camera must have reached phase `live` and the SRT readers must have received frames;
  // a run where the pushers never bound is a failed run, not a pass.
  int64_t totalFrames = 0;
  for (int i = 0; i < kCams; ++i) {
    totalFrames += rig.counts[i].frames;
    if (!liveSeen[i]) failures.push_back(camId(i) + ": never reached phase live during the run");
    if (rig.pushers[i] && rig.pushers[i]->failed()) failures.push_back("pusher " + camId(i) + " failed: " + rig.pushers[i]->error());
  }
  const int64_t livenessFloor = static_cast<int64_t>(kCams) * samples;  // ~1 frame per camera per sample
  std::cout << "liveness: " << totalFrames << " frames across " << kCams << " SRT readers (floor " << livenessFloor << ")\n";
  if (totalFrames < livenessFloor) failures.push_back("liveness: only " + std::to_string(totalFrames) + " frames received across the SRT readers (< " + std::to_string(livenessFloor) + " floor)");
  for (auto& [id, v] : latencies) {
    if (v.empty()) continue;
    std::sort(v.begin(), v.end());
    const double median = v[v.size() / 2];
    std::cout << id << ": median latency " << median << " ms\n";
    if (median > 50.0) failures.push_back(id + ": median latency " + std::to_string(median) + " ms > 50");
  }
  if (baselineWs > 0) {
    if (lastWs > baselineWs * 1.05) failures.push_back("memory grew from " + std::to_string(baselineWs) + " MB to " + std::to_string(lastWs) + " MB");
    std::cout << "memory: evaluated, working set " << baselineWs << " → " << lastWs << " MB (bound 105%)\n";
  } else {
    std::cout << "memory: not evaluated (no baseline sample; the run ended before the 10-minute baseline window)\n";
  }
  std::cout << "samples " << samples << ", CSV " << args.out << "\n";

  for (auto& r : rig.readers) if (r) r->stop();
  for (auto& p : rig.pushers) if (p) p->stop();
  sup.stopSupervising();
  engine.request("shutdown");
  waitFor([&] { return !launcher.isRunning(); }, 5000ms);
  if (launcher.isRunning()) launcher.kill();
  supervising = false;
  supThread.join();

  for (auto& f : failures) std::cout << "FAIL: " << f << "\n";
  std::cout << (failures.empty() ? "PASS\n" : "FAIL\n");
  orphanCleanup();
  return failures.empty() ? 0 : 1;
}