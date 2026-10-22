#include <windows.h>
#include <shlobj.h>

#include <ixwebsocket/IXNetSystem.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>

#include "core/show_store.h"
#include "core/state_store.h"
#include "core/utf8.h"
#include "engine/adapter_factory.h"
#include "engine/command_router.h"
#include "engine/color_calibration_service.h"
#include "engine/control_server.h"
#include "engine/engine.h"
#include "engine/face_detector.h"
#include "engine/gpu_tools.h"
#include "engine/matte.h"
#include "engine/preview_service.h"
#include "gpu/color_grader.h"
#include "gpu/compositor.h"
#include "gpu/converter.h"
#include "gpu/device.h"
#include "gpu/rtx_video.h"
#include "gpu/upscaler.h"
#include "io/aja.h"
#include "io/decklink.h"
#include "io/mf_capture_input.h"
#include "io/monitors.h"
#if BC_WITH_NDI
#include "io/ndi_discovery.h"
#endif

namespace {
using nlohmann::json;
std::atomic<bool> g_quit{false};

std::filesystem::path knownFolder(REFKNOWNFOLDERID id) {
  PWSTR p = nullptr;
  SHGetKnownFolderPath(id, 0, nullptr, &p);
  std::filesystem::path out(p);
  CoTaskMemFree(p);
  return out;
}

void writeDiscovery(const std::filesystem::path& file, int port, const std::string& token) {
  std::filesystem::create_directories(file.parent_path());
  const auto tmp = file.wstring() + L".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    f << json{{"pid", GetCurrentProcessId()}, {"port", port}, {"token", token}}.dump();
  }
  MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  int port = 47810;
  std::string token;
  std::filesystem::path showDir = knownFolder(FOLDERID_RoamingAppData) / L"BreadCamera" / L"shows";
  std::filesystem::path discovery = knownFolder(FOLDERID_LocalAppData) / L"BreadCamera" / L"engine.json";
  for (int i = 1; i + 1 < argc; i += 2) {
    std::wstring k = argv[i], v = argv[i + 1];
    if (k == L"--port") {
      try {
        port = std::stoi(v);
      } catch (...) {
        std::wcerr << L"--port must be a whole number\n";
        return 2;
      }
    } else if (k == L"--token") token = bc::toUtf8(v);
    else if (k == L"--show-dir") showDir = v;
    else if (k == L"--discovery") discovery = v;
    else {
      std::wcerr << L"unknown argument " << k << L"\n";
      return 2;
    }
  }
  if (argc > 1 && (argc - 1) % 2 != 0) {
    std::cerr << "every option needs a value\n";
    return 2;
  }
  if (token.size() < 16 || port <= 0 || port > 65535) {
    std::cerr << "usage: breadcamera-engine --token <at least 16 chars> [--port N] [--show-dir D] [--discovery F]\n";
    return 2;
  }
  HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\BreadCameraEngine");
  if (GetLastError() == ERROR_ALREADY_EXISTS) return 3;
  SetConsoleCtrlHandler([](DWORD) -> BOOL { g_quit = true; return TRUE; }, TRUE);
  ix::initNetSystem();

  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool(*gpu);
  bc::Converter conv(*gpu, pool);
  bc::Compositor comp(*gpu, pool);
  bc::ColorGrader grader(*gpu, pool);
  bc::Upscaler up(*gpu, pool);
  // The RTX Video SDK path (spec D4). Null on every non-NVIDIA PC, with the exact reason the UI
  // shows on the Clean up row; a missing SDK never fails the build (spec §7.2).
  std::string rtxWhy;
  auto rtx = bc::RtxVideo::create(*gpu, pool, conv, rtxWhy);
  // Auto frame's AI detector. A missing model or OpenCV module only costs this one stage.
  std::unique_ptr<bc::FaceDetector> faces;
#if BC_WITH_FACE_DETECT
  try {
    faces = std::make_unique<bc::FaceDetector>(conv, bc::defaultFaceModel());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[breadcamera] auto frame is unavailable: %s\n", e.what());
  }
#endif
  bc::StateStore state;
  bc::DefaultAdapterFactory factory(*gpu, pool);
  bc::ShowStore store(showDir);
  auto loaded = store.load();
  state.set("/engine/gpu", gpu->info().adapterName);
  state.set("/engine/warnings/show", loaded.warning);

  bc::Engine engine(bc::GpuTools{.conv = conv, .comp = comp, .grader = grader, .up = &up, .rtx = rtx.get(), .gpu = gpu.get(), .faces = faces.get()}, state,
                    factory);
  auto matte = bc::makeMatteBackend(*gpu, pool, conv);
  engine.setMatteBackend(std::move(matte.backend), matte.unavailableReason);
  engine.apply(loaded.show);
  bc::ColorCalibrationService calib(engine, showDir / L"luts");
  bc::ControlServer server(port, token);
  bc::PreviewHub hub;
  bc::PreviewService previews(state, hub, [&](const bc::ClientId& c, std::vector<uint8_t> m) { server.sendBinary(c, m); });
  engine.setFrameTap([&](uint64_t tick, const std::vector<bc::FrameTap>& frames) { previews.onTick(tick, frames); });
  // D5: a visible preview forces its camera's one readback even when no output needs CPU. The
  // hub owns the wanted set and the tap fires after the tick, so push the wanted-set in here.
  engine.setPreviewWanted([&hub](const std::string& id) {
    for (const auto& key : hub.wanted()) {
      if (key.first == id) return true;
    }
    return false;
  });

  struct Fx : bc::CommandRouter::Effects {
    bc::Engine& engine;
    bc::ShowStore& store;
    bc::ControlServer& server;
    bc::PreviewHub& hub;
    bc::PreviewService& previews;
    bc::ColorCalibrationService& calib;
    bc::RtxVideo* rtx;
    std::string cleanupWhy;
    Fx(bc::Engine& e, bc::ShowStore& s, bc::ControlServer& sv, bc::PreviewHub& h, bc::PreviewService& p,
       bc::ColorCalibrationService& c, bc::RtxVideo* r, std::string why)
        : engine(e), store(s), server(sv), hub(h), previews(p), calib(c), rtx(r), cleanupWhy(std::move(why)) {}
    void showChanged(const bc::Show& s) override {
      engine.apply(s);
      store.scheduleSave(s);
      server.broadcast(json{{"type", "show"}, {"show", bc::showToJson(s, bc::SecretMode::Redact)}}.dump());
    }
    void subscribePreview(const bc::ClientId& c, std::vector<std::string> ids, bc::PreviewSize size) override {
      for (auto& [id, sz] : hub.subscribe(c, std::move(ids), size)) previews.requestKeyframe(id, sz);
    }
    void requestKeyframe(const std::string& id, bc::PreviewSize size) override { previews.requestKeyframe(id, size); }
    json listMonitors() override {
      json out = json::array();
      for (auto& m : bc::listMonitors()) {
        out.push_back({{"index", m.index}, {"name", m.name}, {"width", m.width}, {"height", m.height}, {"primary", m.primary}});
      }
      return out;
    }
    json listNdiSources() override {
#if BC_WITH_NDI
      return bc::listNdiSources();
#else
      return json::array();
#endif
    }
    json listCaptureDevices() override {
      json out = json::array();
      for (auto& d : bc::listCaptureDevices()) out.push_back({{"name", d.name}, {"id", d.id}});
      return out;
    }
    json listDeckLinkDevices() override {
      json out = json::array();
#if BC_WITH_DECKLINK
      for (auto& d : bc::listDeckLinkDevices()) {
        out.push_back({{"index", d.index}, {"name", d.name}, {"hasInput", d.hasInput}, {"hasOutput", d.hasOutput}, {"externalKeying", d.externalKeying}});
      }
#endif
      return out;
    }
    json listAjaDevices() override {
      json out = json::array();
#if BC_WITH_AJA
      for (auto& d : bc::listAjaDevices()) out.push_back({{"index", d.index}, {"name", d.name}});
#endif
      return out;
    }
    json findChart(const std::string& id) override { return calib.find(id); }
    json calibrateColor(const std::string& id, const std::string& chart, const bc::ChartQuad& q) override {
      auto r = calib.calibrate(id, chart, q);
      return {{"lutPath", r.lutPath}, {"deltaEBefore", r.deltaEBefore}, {"deltaE", r.deltaE}, {"rating", r.rating}, {"reference", r.reference}};
    }
    void forgetColor(const std::string& id) override { calib.forget(id); }
    // Task 4: Clean up is RTX-only. When the RTX Video SDK could not start (no NVIDIA card, no SDK
    // in this build, an old driver) the Add effect row is dimmed with this exact reason (spec D7).
    std::string stageUnavailable(const std::string& kind) override {
      if (kind == "cleanup") return rtx ? std::string() : cleanupWhy;
      return "";
    }
    void shutdown() override { g_quit = true; }
  } fx(engine, store, server, hub, previews, calib, rtx.get(), rtxWhy);
  bc::CommandRouter router(loaded.show, fx);

  server.setHandlers({
      [&](const bc::ClientId& id) {
        server.send(id, json{{"type", "snapshot"}, {"state", state.snapshot()},
                             {"show", bc::showToJson(router.show(), bc::SecretMode::Redact)}}.dump());
      },
      [&](const bc::ClientId& id) { hub.dropClient(id); },
      [&](const bc::ClientId& id, const std::string& text) { return router.handle(id, text).dump(); },
  });
  try {
    server.start();
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    return 4;
  }
  engine.start();
  writeDiscovery(discovery, port, token);

  int ticks = 0;
  while (!g_quit) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto ops = state.takePatch();
    if (!ops.empty()) server.broadcast(json{{"type", "patch"}, {"ops", ops}}.dump());
    if (++ticks % 20 == 0) {
      auto st = engine.stats();
      json cams = json::object();
      for (auto& [id, c] : st.cameras) {
        cams[id] = {{"fps", c.fps}, {"drops", c.drops}, {"repeats", c.repeats}, {"latencyMs", c.latencyMs}};
      }
      server.broadcast(json{{"type", "stats"}, {"engine", {{"overruns", st.overruns}}}, {"cameras", cams}}.dump());
      const auto err = store.lastSaveError();
      state.set("/engine/warnings/save", err.empty() ? "" : "the show could not be saved: " + err);
    }
  }
  engine.stop();
  server.stop();
  store.flush();
  std::error_code ec;
  std::filesystem::remove(discovery, ec);
  CloseHandle(single);
  return 0;
}
