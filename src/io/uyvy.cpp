#include "io/uyvy.h"

extern "C" {
#include <libswscale/swscale.h>
}

#include <stdexcept>

namespace bc {
namespace {
SwsContext* make(int w, int h, AVPixelFormat from, AVPixelFormat to, bool bt709, bool toYuv) {
  SwsContext* s = sws_getContext(w, h, from, w, h, to, SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (!s) throw std::runtime_error("cannot create a pixel converter");
  const int* cs = sws_getCoefficients(bt709 ? SWS_CS_ITU709 : SWS_CS_ITU601);
  // srcRange/dstRange: 0 = limited (MPEG), 1 = full (JPEG). RGB is full, SDI YUV is limited.
  sws_setColorspaceDetails(s, cs, toYuv ? 1 : 0, cs, toYuv ? 0 : 1, 0, 1 << 16, 1 << 16);
  return s;
}
}  // namespace

std::vector<uint8_t> uyvyToBgra(const uint8_t* uyvy, int w, int h, int rowBytes, bool bt709) {
  std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4);
  SwsContext* s = make(w, h, AV_PIX_FMT_UYVY422, AV_PIX_FMT_BGRA, bt709, false);
  const uint8_t* src[1] = {uyvy};
  const int srcStride[1] = {rowBytes};
  uint8_t* dst[1] = {out.data()};
  const int dstStride[1] = {w * 4};
  sws_scale(s, src, srcStride, 0, h, dst, dstStride);
  sws_freeContext(s);
  return out;
}

void bgraToUyvy(const uint8_t* bgra, int w, int h, uint8_t* out, int outRowBytes, bool bt709) {
  SwsContext* s = make(w, h, AV_PIX_FMT_BGRA, AV_PIX_FMT_UYVY422, bt709, true);
  const uint8_t* src[1] = {bgra};
  const int srcStride[1] = {w * 4};
  uint8_t* dst[1] = {out};
  const int dstStride[1] = {outRowBytes};
  sws_scale(s, src, srcStride, 0, h, dst, dstStride);
  sws_freeContext(s);
}
}  // namespace bc