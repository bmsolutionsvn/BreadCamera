#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "gpu/device.h"
#include "gpu/texture_pool.h"
#include "io/input.h"

struct AVBufferRef;
struct AVFrame;
struct SwsContext;

namespace bc {
// Shared by FfmpegInput and GpuDecoder (spec §3.1 item 4).

// A d3d11va device over the engine's device, with the context lock FFmpeg's decode calls take.
// Throws std::runtime_error when the device cannot be initialised.
AVBufferRef* makeD3d11vaDevice(GpuDevice& gpu);

// get_format for a d3d11va decoder: D3D11 when offered, else the first software format.
AVPixelFormat pickD3d11Format(AVCodecContext*, const AVPixelFormat*);

// Copies a D3D11 frame into a pooled texture with CopySubresourceRegion. Sets `tex` and
// `layout`; false (and nothing set) for a software frame.
bool copyDecodedFrame(GpuDevice& gpu, TexturePool& pool, const AVFrame* f, uint32_t w, uint32_t h,
                      VideoFrameIn& out);

// Uploads a software frame: YUV420P/YUVJ420P interleave to NV12 and use the plane upload with
// `layout` NV12; any other format goes through swscale to BGRA (`sws` is cached). `scratch` is
// reused between calls. Sets `tex`, `layout` and, on the BGRA path, `fullRange`.
bool uploadSoftwareFrame(GpuDevice& gpu, TexturePool& pool, const AVFrame* f, uint32_t w, uint32_t h,
                         SwsContext*& sws, std::vector<uint8_t>& scratch, VideoFrameIn& out);

// One Annex-B access unit in, zero or more pictures out. Hardware decode on the engine's device
// when it can, software (NV12 for 4:2:0) otherwise.
class GpuDecoder {
 public:
  // codec: AV_CODEC_ID_H264 or AV_CODEC_ID_HEVC. Throws when the decoder cannot open.
  GpuDecoder(GpuDevice& gpu, TexturePool& pool, AVCodecID codec, bool hardware = true);
  ~GpuDecoder();
  GpuDecoder(const GpuDecoder&) = delete;
  GpuDecoder& operator=(const GpuDecoder&) = delete;

  void flush(const std::function<void(VideoFrameIn)>& emit);  // drain pictures the decoder still holds
  void reset();  // forget the stream (avcodec_flush_buffers): the next picture comes from a keyframe
  void decode(std::span<const uint8_t> au, int64_t ptsUs, const std::function<void(VideoFrameIn)>& emit);
  bool hardware() const { return hardware_; }  // true once a D3D11 frame came out

 private:
  void drain(const std::function<void(VideoFrameIn)>& emit, int64_t fallbackPtsUs);

  GpuDevice& gpu_;
  TexturePool& pool_;
  AVBufferRef* hw_ = nullptr;
  AVCodecContext* ctx_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVPacket* pkt_ = nullptr;
  SwsContext* sws_ = nullptr;
  std::vector<uint8_t> scratch_;
  bool hardware_ = false;
};
}  // namespace bc
