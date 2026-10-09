#include "shell/process_launcher.h"

#include <cwctype>
#include <fstream>
#include <thread>

#include <nlohmann/json.hpp>

namespace bc {
std::optional<Discovery> readDiscovery(const std::filesystem::path& file) {
  std::ifstream f(file);
  if (!f) return std::nullopt;
  try {
    auto j = nlohmann::json::parse(f);
    Discovery d{j.at("pid").get<DWORD>(), j.at("port").get<int>(), j.at("token").get<std::string>()};
    if (d.port <= 0 || d.port > 65535 || d.token.size() < 16) return std::nullopt;
    return d;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::wstring> processImageBaseName(DWORD pid) {
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return std::nullopt;
  wchar_t buf[MAX_PATH];
  DWORD size = MAX_PATH;
  const BOOL ok = QueryFullProcessImageNameW(h, 0, buf, &size);
  CloseHandle(h);
  if (!ok) return std::nullopt;
  std::wstring name = std::filesystem::path(buf).filename().wstring();
  for (auto& c : name) c = static_cast<wchar_t>(towlower(c));
  return name;
}

bool isEngineProcess(DWORD pid) {
  auto name = processImageBaseName(pid);
  return name && *name == L"breadcamera-engine.exe";
}

bool engineInstanceRunning() {
  HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\BreadCameraEngine");
  if (!m) return false;
  CloseHandle(m);
  return true;
}

std::optional<Discovery> waitForDiscovery(const std::filesystem::path& file,
                                          std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    if (auto d = readDiscovery(file)) return d;
    if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

ProcessLauncher::~ProcessLauncher() {
  if (process_) CloseHandle(process_);  // never kills: the engine outlives the shell
}

bool ProcessLauncher::isRunning() {
  return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
}

void ProcessLauncher::launch() {
  if (process_) {
    CloseHandle(process_);
    process_ = nullptr;
  }
  std::wstring cmd = L"\"" + exe_.wstring() + L"\" " + args_;
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe_.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      exe_.parent_path().c_str(), &si, &pi)) {
    return;  // stays not-running; the supervisor retries with its policy
  }
  CloseHandle(pi.hThread);
  process_ = pi.hProcess;
  pid_ = pi.dwProcessId;
}

void ProcessLauncher::kill() {
  if (!process_) return;
  TerminateProcess(process_, 1);
  WaitForSingleObject(process_, 2000);
}

void ProcessLauncher::adopt(DWORD pid) {
  HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return;
  if (process_) CloseHandle(process_);
  process_ = h;
  pid_ = pid;
}
}  // namespace bc
