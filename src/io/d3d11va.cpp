#include "io/d3d11va.h"

#include <cstring>
#include <stdexcept>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include "gpu/upload.h"

namespace bc {
namespace {
std::string averr(int rc) {
  char buf[AV_ERROR_MAX_STRING_SIZE];
  av_strerror(rc, buf, sizeof(buf));
  return buf;
}

bool isYuv420(AVPixelFormat fmt) {
  return fmt == AV_PIX_FMT_YUV420P || fmt == AV_PIX_FMT_YUVJ420P;
}
}  // namespace

AVBufferRef* makeD3d11vaDevice(GpuDevice& gpu) {
  AVBufferRef* ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
  if (!ref) throw std::runtime_error("d3d11va device allocation failed");
  auto* dctx = reinterpret_cast<AVHWDeviceContext*>(ref->data);
  auto* d3d = static_cast<AVD3D11VADeviceContext*>(dctx->hwctx);
  d3d->device = gpu.device();
  d3d->device->AddRef();
  // FFmpeg takes our context lock, so its decode calls never interleave with ours.
  d3d->lock_ctx = &gpu.contextMutex();
  d3d->lock = [](void* m) { static_cast<std::recursive_mutex*>(m)->lock(); };
  d3d->unlock = [](void* m) { static_cast<std::recursive_mutex*>(m)->unlock(); };
  const int rc = av_hwdevice_ctx_init(ref);
  if (rc < 0) {
    av_buffer_unref(&ref);
    throw std::runtime_error("d3d11va init failed: " + averr(rc));
  }
  return ref;
}

AVPixelFormat pickD3d11Format(AVCodecContext*, const AVPixelFormat* fmts) {
  for (auto* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
    if (*p == AV_PIX_FMT_D3D11) return *p;
  }
  for (auto* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
    if (!(av_pix_fmt_desc_get(*p)->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
  }
  return AV_PIX_FMT_NONE;
}

bool copyDecodedFrame(GpuDevice& gpu, TexturePool& pool, const AVFrame* f, uint32_t w, uint32_t h,
                      VideoFrameIn& out) {
  if (f->format != AV_PIX_FMT_D3D11) return false;
  auto* src = reinterpret_cast<ID3D11Texture2D*>(f->data[0]);
  const UINT index = static_cast<UINT>(reinterpret_cast<intptr_t>(f->data[1]));
  D3D11_TEXTURE2D_DESC d{};
  src->GetDesc(&d);
  out.layout = d.Format == DXGI_FORMAT_P010 ? PixelLayout::P010 : PixelLayout::NV12;
  out.tex = pool.acquire(w, h, d.Format, D3D11_BIND_SHADER_RESOURCE);
  const D3D11_BOX box{0, 0, 0, w, h, 1};
  auto lock = gpu.lock();
  gpu.context()->CopySubresourceRegion(out.tex->tex.Get(), 0, 0, 0, 0, src, index, &box);
  return true;
}

bool uploadSoftwareFrame(GpuDevice& gpu, TexturePool& pool, const AVFrame* f, uint32_t w, uint32_t h,
                         SwsContext*& sws, std::vector<uint8_t>& scratch, VideoFrameIn& out) {
  if (w == 0 || h == 0) return false;
  const auto fmt = static_cast<AVPixelFormat>(f->format);
  if (isYuv420(fmt)) {
    // Interleave U and V into NV12: the only CPU pass over the picture, and only over chroma.
    scratch.resize(static_cast<size_t>(w) * h / 2);
    for (uint32_t y = 0; y < h / 2; ++y) {
      const uint8_t* u = f->data[1] + static_cast<ptrdiff_t>(y) * f->linesize[1];
      const uint8_t* v = f->data[2] + static_cast<ptrdiff_t>(y) * f->linesize[2];
      uint8_t* dst = scratch.data() + static_cast<size_t>(y) * w;
      for (uint32_t x = 0; x < w / 2; ++x) {
        dst[2 * x] = u[x];
        dst[2 * x + 1] = v[x];
      }
    }
    out.tex = uploadNv12Planes(gpu, pool, f->data[0], scratch.data(), w, h);
    out.layout = PixelLayout::NV12;
    return true;
  }

  const int cs = f->height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601;
  sws = sws_getCachedContext(sws, f->width, f->height, fmt, static_cast<int>(w), static_cast<int>(h),
                             AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (!sws) throw std::runtime_error("cannot convert pixel format");
  sws_setColorspaceDetails(sws, sws_getCoefficients(cs), f->color_range == AVCOL_RANGE_JPEG,
                           sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
  scratch.resize(static_cast<size_t>(w) * h * 4);
  uint8_t* dst[1] = {scratch.data()};
  const int dstStride[1] = {static_cast<int>(w * 4)};
  sws_scale(sws, f->data, f->linesize, 0, f->height, dst, dstStride);
  out.tex = uploadBgra(gpu, pool, scratch.data(), w * 4, w, h);
  out.layout = PixelLayout::BGRA8;
  out.fullRange = true;
  return true;
}

GpuDecoder::GpuDecoder(GpuDevice& gpu, TexturePool& pool, AVCodecID codecId, bool hardware)
    : gpu_(gpu), pool_(pool) {
  const AVCodec* codec = avcodec_find_decoder(codecId);
  if (!codec) throw std::runtime_error(std::string("unsupported codec ") + avcodec_get_name(codecId));
  ctx_ = avcodec_alloc_context3(codec);
  if (!ctx_) throw std::runtime_error("cannot allocate the decoder");
  if (hardware && gpu.supportsVideoDecode()) {
    try {
      hw_ = makeD3d11vaDevice(gpu);
    } catch (const std::exception&) {
      hw_ = nullptr;  // fall back to software decode
    }
  }
  if (hw_) {
    ctx_->hw_device_ctx = av_buffer_ref(hw_);
    ctx_->get_format = &pickD3d11Format;
    ctx_->extra_hw_frames = 8;
  }
  ctx_->pkt_timebase = {1, 1'000'000};
  const int rc = avcodec_open2(ctx_, codec, nullptr);
  if (rc < 0) {
    avcodec_free_context(&ctx_);
    av_buffer_unref(&hw_);
    throw std::runtime_error("cannot open decoder: " + averr(rc));
  }
  frame_ = av_frame_alloc();
  pkt_ = av_packet_alloc();
  if (!frame_ || !pkt_) {
    av_frame_free(&frame_);
    av_packet_free(&pkt_);
    avcodec_free_context(&ctx_);
    av_buffer_unref(&hw_);
    throw std::runtime_error("cannot allocate decoder buffers");
  }
}

GpuDecoder::~GpuDecoder() {
  if (sws_) sws_freeContext(sws_);
  av_frame_free(&frame_);
  av_packet_free(&pkt_);
  avcodec_free_context(&ctx_);
  av_buffer_unref(&hw_);
}

void GpuDecoder::drain(const std::function<void(VideoFrameIn)>& emit, int64_t fallbackPtsUs) {
  for (;;) {
    const int rc = avcodec_receive_frame(ctx_, frame_);
    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
    if (rc < 0) break;
    const uint32_t w = static_cast<uint32_t>(frame_->width) & ~1u;
    const uint32_t h = static_cast<uint32_t>(frame_->height) & ~1u;
    if (w != 0 && h != 0) {
      VideoFrameIn out;
      out.ptsUs = fallbackPtsUs;
      out.fullRange = frame_->color_range == AVCOL_RANGE_JPEG;
      if (!copyDecodedFrame(gpu_, pool_, frame_, w, h, out)) {
        uploadSoftwareFrame(gpu_, pool_, frame_, w, h, sws_, scratch_, out);
      } else {
        hardware_ = true;
      }
      emit(std::move(out));
    }
    av_frame_unref(frame_);
  }
}

void GpuDecoder::decode(std::span<const uint8_t> au, int64_t ptsUs,
                        const std::function<void(VideoFrameIn)>& emit) {
  if (!ctx_ || au.empty()) return;
  if (av_new_packet(pkt_, static_cast<int>(au.size())) < 0) return;
  std::memcpy(pkt_->data, au.data(), au.size());
  pkt_->pts = pkt_->dts = ptsUs;
  const int rc = avcodec_send_packet(ctx_, pkt_);
  av_packet_unref(pkt_);
  if (rc < 0) return;  // a partial or first-fragment NAL: the caller counts empty emissions
  drain(emit, ptsUs);
}

void GpuDecoder::flush(const std::function<void(VideoFrameIn)>& emit) {
  if (!ctx_) return;
  avcodec_send_packet(ctx_, nullptr);
  drain(emit, 0);
  reset();
}

void GpuDecoder::reset() {
  if (ctx_) avcodec_flush_buffers(ctx_);
}
}  // namespace bc