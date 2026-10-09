#include "core/show_store.h"

#include <windows.h>

#include <fstream>

namespace bc {
namespace {
nlohmann::json readJson(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) throw ShowError("cannot open " + p.string());
  try {
    return nlohmann::json::parse(f);
  } catch (const nlohmann::json::exception& e) {
    throw ShowError(p.filename().string() + " is not valid JSON: " + e.what());
  }
}
}  // namespace

ShowStore::ShowStore(std::filesystem::path dir, std::chrono::milliseconds debounce)
    : dir_(std::move(dir)), debounce_(debounce), thread_([this] { run(); }) {}

ShowStore::~ShowStore() {
  flush();
  {
    std::lock_guard l(m_);
    stop_ = true;
  }
  cv_.notify_all();
  thread_.join();
}

ShowStore::Loaded ShowStore::load() {
  std::string mainProblem;
  if (std::filesystem::exists(mainPath())) {
    try {
      return {showFromJson(readJson(mainPath())), From::Main, ""};
    } catch (const std::exception& e) {
      mainProblem = e.what();
    }
  }
  if (std::filesystem::exists(lkgPath())) {
    try {
      std::string why = mainProblem.empty() ? "show.json is missing" : "show.json unreadable (" + mainProblem + ")";
      return {showFromJson(readJson(lkgPath())), From::LastKnownGood, why + "; loaded the last-known-good copy"};
    } catch (const std::exception& e) {
      return {Show{}, From::Default, std::string("show files unreadable (") + e.what() + "); started with an empty show"};
    }
  }
  if (!mainProblem.empty()) {
    return {Show{}, From::Default, "show.json unreadable (" + mainProblem + "); started with an empty show"};
  }
  return {Show{}, From::Default, ""};
}

void ShowStore::saveNow(const Show& show) {
  std::lock_guard fl(fileMutex_);
  std::filesystem::create_directories(dir_);
  const auto text = showToJson(show, SecretMode::Encrypt).dump(2);
  const auto tmp = dir_ / "show.json.tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f << text;
    f.flush();
    if (!f) throw ShowError("cannot write " + tmp.string());
  }
  if (!MoveFileExW(tmp.c_str(), mainPath().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    throw ShowError("cannot replace show.json (error " + std::to_string(GetLastError()) + ")");
  }
  showFromJson(readJson(mainPath()));  // parse back before promoting to last-known-good
  CopyFileW(mainPath().c_str(), lkgPath().c_str(), FALSE);
  std::lock_guard l(m_);
  ++saveCount_;
  lastError_.clear();
}

void ShowStore::scheduleSave(Show show) {
  {
    std::lock_guard l(m_);
    pending_ = std::move(show);
    lastChange_ = std::chrono::steady_clock::now();
  }
  cv_.notify_all();
}

void ShowStore::flush() {
  std::optional<Show> s;
  {
    std::lock_guard l(m_);
    s = std::move(pending_);
    pending_.reset();
  }
  if (!s) return;
  try {
    saveNow(*s);
  } catch (const std::exception& e) {
    std::lock_guard l(m_);
    lastError_ = e.what();
  }
}

void ShowStore::run() {
  std::unique_lock l(m_);
  while (!stop_) {
    if (!pending_) {
      cv_.wait(l);
      continue;
    }
    const auto due = lastChange_ + debounce_;
    if (cv_.wait_until(l, due, [&] { return stop_ || !pending_ || lastChange_ + debounce_ > due; })) {
      continue;  // stopped, flushed elsewhere, or rescheduled
    }
    Show s = std::move(*pending_);
    pending_.reset();
    l.unlock();
    try {
      saveNow(s);
    } catch (const std::exception& e) {
      std::lock_guard el(m_);
      lastError_ = e.what();
    }
    l.lock();
  }
}

uint64_t ShowStore::saveCount() const {
  std::lock_guard l(m_);
  return saveCount_;
}

std::string ShowStore::lastSaveError() const {
  std::lock_guard l(m_);
  return lastError_;
}
}  // namespace bc
