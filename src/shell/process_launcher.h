#pragma once
#include <windows.h>

#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

#include "shell/supervisor.h"

namespace bc {
struct Discovery {
  DWORD pid = 0;
  int port = 0;
  std::string token;
};
// %LOCALAPPDATA%\BreadCamera\engine.json written by the engine (Task 11).
std::optional<Discovery> readDiscovery(const std::filesystem::path& file);

// The image basename of a live process, lower-cased; nullopt when the pid cannot be opened.
std::optional<std::wstring> processImageBaseName(DWORD pid);
// True only for a live process whose image is breadcamera-engine.exe: engine.json
// survives an unclean exit, so the pid it names may already have been reused.
bool isEngineProcess(DWORD pid);
// True when some process holds the engine's single-instance mutex.
bool engineInstanceRunning();
// Waits for a discovery file another process is still writing; nullopt at the timeout.
std::optional<Discovery> waitForDiscovery(const std::filesystem::path& file,
                                          std::chrono::milliseconds timeout);

class ProcessLauncher final : public EngineLauncher {
 public:
  ProcessLauncher(std::filesystem::path exe, std::wstring args) : exe_(std::move(exe)), args_(std::move(args)) {}
  ~ProcessLauncher() override;
  bool isRunning() override;
  void launch() override;
  void kill() override;
  void adopt(DWORD pid);
  DWORD pid() const;
  struct Identity {
    DWORD pid = 0;
    HANDLE process = nullptr;
  };
  // One consistent pid + process-handle pair, read under the launcher's lock. The handle is a
  // duplicate the caller owns (CloseHandle it), so a supervisor relaunch cannot close it while a
  // layer handle is copied out of that exact process.
  Identity identity() const;

 private:
  std::filesystem::path exe_;
  std::wstring args_;
  HANDLE process_ = nullptr;
  DWORD pid_ = 0;
  mutable std::mutex m_;
};
}  // namespace bc
