// BreadCamera's own thin FSR 1 RCAS compute shader (cs_5_0). This file is ours, written for
// BreadCamera; it is not AMD's sample. It includes the vendored FidelityFX FSR 1.0.2 headers
// (ffx/ffx_a.h, ffx/ffx_fsr1.h — MIT) unchanged and supplies the load/input callbacks they require.
#define A_GPU 1
#define A_HLSL 1
#define FSR_RCAS_F 1
#include "ffx/ffx_a.h"
#include "ffx/ffx_fsr1.h"

Texture2D<float4> gInput : register(t0);  // EASU's gamma-encoded output
RWTexture2D<float4> gOut : register(u0);
cbuffer Params : register(b0) {
  uint4 gCon;  // RCAS constants (FsrRcasCon)
  uint2 gOutSize;
};

// RCAS loads the 3x3 ring in the gamma domain EASU produced; clamp at the edges.
AF4 FsrRcasLoadF(ASU2 p) {
  ASU2 c = clamp(p, ASU2(0, 0), ASU2(gOutSize) - ASU2(1, 1));
  return gInput.Load(int3(c, 0));
}
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= gOutSize.x || id.y >= gOutSize.y) return;
  AF1 r, g, b;
  FsrRcasF(r, g, b, id.xy, gCon);
  // Back to linear: the pipeline's 2.4 decode on store.
  gOut[id.xy] = float4(pow(saturate(AF3(r, g, b)), 2.4), 1);
}