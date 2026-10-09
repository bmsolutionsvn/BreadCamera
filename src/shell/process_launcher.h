#pragma once
#include <windows.h>

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
