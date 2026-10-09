#pragma once
#include <atomic>
#include <filesystem>
#include <string>
#include <thread>

namespace bc::test {
// MPEG-TS with a moving pattern (mpeg2video) and a 1 kHz tone (mp2, 48 kHz stereo).
// LGPL-native encoders only.
void writeTestTs(const std::filesystem::path& path, int width, int height, int fps, int seconds,
                 bool withAudio = true);

// Re-muxes a TS file to `url` (e.g. "srt://127.0.0.1:47901?mode=caller") in real time on
// its own thread. Retries the connection until the receiver is up or stop() is called.
class TsPusher {
 public:
  TsPusher(std::filesystem::path file, std::string url, bool loop);
  ~TsPusher();
  void stop();
  bool failed() const { return failed_; }
  std::string error() const { return error_; }

 private:
  void run();
  static int interrupt(void* self);
  std::filesystem::path file_;
  std::string url_;
  bool loop_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> failed_{false};
  std::string error_;
  std::thread thread_;
};
}  // namespace bc::test
