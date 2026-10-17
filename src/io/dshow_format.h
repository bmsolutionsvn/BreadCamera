#pragma once
#include <guiddef.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bc {
// True when `subtype` is a raw video format dshowToBgra8 can convert: RGB32, RGB24, NV12, YUY2, UYVY.
bool dshowSubtypeSupported(const GUID& subtype);

// The bytes in one row of `subtype` at `width` (NV12 = the luma stride, not the 1.5x frame size).
// 0 for an unsupported subtype.
int dshowRowBytes(const GUID& subtype, int width);

// The bytes one full sample of `subtype` occupies at `width`x`height`, given `rowBytes` per row.
// NV12 adds its interleaved chroma plane (`rowBytes*height*3/2`). 0 for an unsupported subtype.
std::size_t dshowFrameBytes(const GUID& subtype, int width, int height, int rowBytes);

// One DirectShow video sample -> packed BGRA8, width*4 bytes per row, top-down.// `stride` is the source row size in bytes; `bottomUp` means the buffer's first row is the bottom.
// `bt709` picks the Rec.709 matrix, else Rec.601. Throws std::runtime_error for an unsupported
// subtype.
std::vector<uint8_t> dshowToBgra8(const GUID& subtype, const uint8_t* src, int width, int height,
                                  int stride, bool bottomUp, bool bt709);
}  // namespace bc
