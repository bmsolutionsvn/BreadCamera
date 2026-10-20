// BreadCamera's own thin FSR 1 EASU compute shader (cs_5_0). This file is ours, written for
// BreadCamera; it is not AMD's sample. It includes the vendored FidelityFX FSR 1.0.2 headers
// (ffx/ffx_a.h, ffx/ffx_fsr1.h — MIT) unchanged and supplies the gather callbacks they require.
#define A_GPU 1
#define A_HLSL 1
#define FSR_EASU_F 1
#include "ffx/ffx_a.h"
#include "ffx/ffx_fsr1.h"

Texture2D<float4> gInput : register(t0);
RWTexture2D<float4> gOut : register(u0);
SamplerState gLinear : register(s0);
cbuffer Params : register(b0) {
  uint4 gCon0;  // EASU constants (FsrEasuCon)
  uint4 gCon1;
  uint4 gCon2;
  uint4 gCon3;
  uint2 gOutSize;
};

// EASU reads through gather4, one channel at a time, in the display-referred gamma domain: the
// pipeline's linear frame is gamma-encoded (1/2.4) on load here. The sampler clamps at the edges.
AF4 FsrEasuRF(AF2 p) { return pow(saturate(gInput.GatherRed(gLinear, p)), 1.0 / 2.4); }
AF4 FsrEasuGF(AF2 p) { return pow(saturate(gInput.GatherGreen(gLinear, p)), 1.0 / 2.4); }
AF4 FsrEasuBF(AF2 p) { return pow(saturate(gInput.GatherBlue(gLinear, p)), 1.0 / 2.4); }

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= gOutSize.x || id.y >= gOutSize.y) return;
  AF3 pix;
  FsrEasuF(pix, id.xy, gCon0, gCon1, gCon2, gCon3);
  gOut[id.xy] = float4(pix, 1);
}