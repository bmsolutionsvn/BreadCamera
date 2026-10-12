#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct AVCodecContext;

namespace bc {
struct OpenedEncoder {
  AVCodecContext* ctx = nullptr;  // free with avcodec_free_context
  std::string name;
};
// spec §7.2: NVIDIA first, then AMD, Intel, Media Foundation.
const std::vector<std::string>& h264EncoderOrder();
// Low-latency, no B-frames, NV12 input. Throws std::runtime_error listing every attempt.
OpenedEncoder openH264Encoder(int width, int height, int fpsNum, int fpsDen, int64_t bitRate, int gop, bool globalHeader);
}  // namespace bc