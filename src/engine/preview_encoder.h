#pragma once
#include <atomic>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "gpu/converter.h"

struct AVCodecContext;
struct AVFrame;
struct SwsContext;

namespace bc {
struct PreviewPacket {
  std::vector<uint8_t> annexB;
  bool keyframe = false;
  int64_t ptsUs = 0;
};

std::vector<uint8_t> avccToAnnexB(std::span<const uint8_t> avcc);
bool hasNalType(std::span<const uint8_t> annexB, int type);
std::vector<uint8_t> encodePreviewMessage(std::string_view cameraId, const PreviewPacket& p);

// Low-latency H.264 for UI previews. Uses the first encoder that opens (spec §7.2).
class PreviewEncoder {
 public:
  PreviewEncoder(int width, int height, int fps);
  ~PreviewEncoder();
  PreviewEncoder(const PreviewEncoder&) = delete;
  PreviewEncoder& operator=(const PreviewEncoder&) = delete;
  static const std::vector<std::string>& encoderOrder();
  const std::string& encoderName() const { return name_; }
  void forceKeyframe() { forceKey_ = true; }
  std::vector<PreviewPacket> encode(const CpuImage& bgra, int64_t ptsUs);

 private:
  int w_, h_;
  std::string name_;
  AVCodecContext* ctx_ = nullptr;
  AVFrame* frame_ = nullptr;
  SwsContext* sws_ = nullptr;
  std::vector<uint8_t> headers_;
  int64_t index_ = 0;
  std::atomic<bool> forceKey_{false};
};
}  // namespace bc
