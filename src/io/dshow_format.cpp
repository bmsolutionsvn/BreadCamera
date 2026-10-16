#include "io/dshow_format.h"

extern "C" {
#include <libswscale/swscale.h>
}

#include <stdexcept>

namespace bc {
namespace {
// The MEDIASUBTYPE_* GUIDs, declared here so this pure unit needs no DirectShow headers (and no
// strmiids link). Values are the fixed uuids.h constants / the FOURCC base GUID.
const GUID kRgb32 = {0xe436eb7d, 0x524f, 0x11ce, {0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70}};
const GUID kRgb24 = {0xe436eb7e, 0x524f, 0x11ce, {0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70}};
const GUID kUyvy = {0x59565955, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kYuy2 = {0x32595559, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kNv12 = {0x3231564e, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

AVPixelFormat formatFor(const GUID& s) {
  if (IsEqualGUID(s, kRgb32)) return AV_PIX_FMT_BGRA;
  if (IsEqualGUID(s, kRgb24)) return AV_PIX_FMT_BGR24;
  if (IsEqualGUID(s, kNv12)) return AV_PIX_FMT_NV12;
  if (IsEqualGUID(s, kYuy2)) return AV_PIX_FMT_YUYV422;
  if (IsEqualGUID(s, kUyvy)) return AV_PIX_FMT_UYVY422;
  return AV_PIX_FMT_NONE;
}

bool isRgb(AVPixelFormat f) { return f == AV_PIX_FMT_BGRA || f == AV_PIX_FMT_BGR24; }
}  // namespace

bool dshowSubtypeSupported(const GUID& subtype) { return formatFor(subtype) != AV_PIX_FMT_NONE; }

int dshowRowBytes(const GUID& subtype, int width) {
  switch (formatFor(subtype)) {
    case AV_PIX_FMT_BGRA: return width * 4;
    case AV_PIX_FMT_BGR24: return width * 3;
    case AV_PIX_FMT_NV12: return width;  // luma stride; the interleaved chroma shares it
    case AV_PIX_FMT_YUYV422:
    case AV_PIX_FMT_UYVY422: return width * 2;
    default: return 0;
  }
}

std::vector<uint8_t> dshowToBgra8(const GUID& subtype, const uint8_t* src, int width, int height,
                                  int stride, bool bottomUp, bool bt709) {
  const AVPixelFormat from = formatFor(subtype);
  if (from == AV_PIX_FMT_NONE) throw std::runtime_error("unsupported DirectShow subtype");
  if (width <= 0 || height <= 0 || stride <= 0) throw std::runtime_error("invalid picture size");

  SwsContext* ctx = sws_getContext(width, height, from, width, height, AV_PIX_FMT_BGRA, SWS_BILINEAR,
                                   nullptr, nullptr, nullptr);
  if (!ctx) throw std::runtime_error("cannot create a pixel converter");
  const int* cs = sws_getCoefficients(bt709 ? SWS_CS_ITU709 : SWS_CS_ITU601);
  // srcRange: 1 = full (RGB), 0 = limited (camera YUV). dstRange: 1 = full (BGRA).
  sws_setColorspaceDetails(ctx, cs, isRgb(from) ? 1 : 0, cs, 1, 0, 1 << 16, 1 << 16);

  std::vector<uint8_t> out(static_cast<size_t>(width) * height * 4);
  const uint8_t* srcPtr = src;
  int srcStride = stride;
  if (bottomUp) {  // the buffer's first row is the bottom; swscale flips on a negative stride
    srcPtr = src + static_cast<size_t>(stride) * (height - 1);
    srcStride = -stride;
  }
  const uint8_t* in[4] = {srcPtr, nullptr, nullptr, nullptr};
  int inStride[4] = {srcStride, 0, 0, 0};
  if (from == AV_PIX_FMT_NV12) {  // two-plane: the interleaved UV plane follows the luma plane
    in[1] = srcPtr + static_cast<ptrdiff_t>(srcStride) * height;
    inStride[1] = srcStride;
  }
  uint8_t* dst[1] = {out.data()};
  const int dstStride[1] = {width * 4};
  sws_scale(ctx, in, inStride, 0, height, dst, dstStride);
  sws_freeContext(ctx);
  return out;
}
}  // namespace bc
