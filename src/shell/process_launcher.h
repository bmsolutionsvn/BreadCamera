#pragma once
#include <windows.h>

#include <chrono>
#include <filesystem>
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
  DWORD pid() const { return pid_; }

 private:
  std::filesystem::path exe_;
  std::wstring args_;
  HANDLE process_ = nullptr;
  DWORD pid_ = 0;
};
}  // namespace bc
