#pragma once
#include <doctest/doctest.h>

namespace bc::test {
enum class Req { HardwareGpu, Ndi, H264Encoder, Maxine, ChartDetect, CaptureDevice, DShowDevice, Omt, DeckLink, Aja, VcamInstalled, Onnx, LiveCall, LiveCallGuests };
// True when the requirement is missing. Prints the reason to stderr once per requirement
// so a skipped test is visible in the log (spec §7.2).
bool missing(Req r);
}  // namespace bc::test

#define BC_REQUIRES(r) *doctest::skip(::bc::test::missing(r))
