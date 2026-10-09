#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "core/show.h"

namespace bc {
// show.json + show.lkg.json in one folder (spec §6.2). Writes are atomic; the
// last-known-good copy is only replaced by a file that parsed back cleanly.
class ShowStore {
 public:
  enum class From { Main, LastKnownGood, Default };
  struct Loaded {
    Show show;
    From from = From::Default;
    std::string warning;  // non-empty when the engine must show a warning
  };

  explicit ShowStore(std::filesystem::path dir,
                     std::chrono::milliseconds debounce = std::chrono::milliseconds(500));
  ~ShowStore();
  ShowStore(const ShowStore&) = delete;
  ShowStore& operator=(const ShowStore&) = delete;

  Loaded load();
  void saveNow(const Show& show);  // throws ShowError
  void scheduleSave(Show show);    // debounced, on the store's thread
  void flush();                    // writes a pending save now
  uint64_t saveCount() const;
  std::string lastSaveError() const;
  std::filesystem::path mainPath() const { return dir_ / "show.json"; }
  std::filesystem::path lkgPath() const { return dir_ / "show.lkg.json"; }

 private:
  void run();
  std::filesystem::path dir_;
  std::chrono::milliseconds debounce_;
  mutable std::mutex m_;
  std::mutex fileMutex_;
  std::condition_variable cv_;
  std::optional<Show> pending_;
  std::chrono::steady_clock::time_point lastChange_{};
  bool stop_ = false;
  uint64_t saveCount_ = 0;
  std::string lastError_;
  std::thread thread_;
};
}  // namespace bc
