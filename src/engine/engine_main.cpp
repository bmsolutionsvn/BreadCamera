#include <windows.h>
#include <shlobj.h>

#include <ixwebsocket/IXNetSystem.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#include "core/show_store.h"
#include "core/state_store.h"
#include "core/utf8.h"
#include "engine/adapter_factory.h"
#include "engine/command_router.h"
#include "engine/control_server.h"
#include "engine/engine.h"
#include "engine/matte.h"
#include "engine/preview_service.h"
#include "gpu/compositor.h"
#include "gpu/converter.h"
#include "gpu/device.h"
#include "io/monitors.h"

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
  bc::StateStore state;
  bc::DefaultAdapterFactory factory(*gpu, pool);
  bc::ShowStore store(showDir);
  auto loaded = store.load();
  state.set("/engine/gpu", gpu->info().adapterName);
  state.set("/engine/warnings/show", loaded.warning);

  bc::Engine engine(conv, comp, state, factory);
  auto matte = bc::makeMatteBackend(*gpu, pool, conv);
  engine.setMatteBackend(std::move(matte.backend), matte.unavailableReason);
  engine.apply(loaded.show);
  bc::ControlServer server(port, token);
  bc::PreviewHub hub;
  bc::PreviewService previews(conv, state, hub, [&](const bc::ClientId& c, std::vector<uint8_t> m) { server.sendBinary(c, m); });
  engine.setFrameTap([&](uint64_t tick, const auto& frames) { previews.onTick(tick, frames); });

  struct Fx : bc::CommandRouter::Effects {
    bc::Engine& engine;
    bc::ShowStore& store;
    bc::ControlServer& server;
    bc::PreviewHub& hub;
    bc::PreviewService& previews;
    Fx(bc::Engine& e, bc::ShowStore& s, bc::ControlServer& sv, bc::PreviewHub& h, bc::PreviewService& p)
        : engine(e), store(s), server(sv), hub(h), previews(p) {}
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
    void shutdown() override { g_quit = true; }
  } fx(engine, store, server, hub, previews);
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
