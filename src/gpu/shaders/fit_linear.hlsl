Texture2D<float4> gSrc : register(t0);
RWTexture2D<float4> gOut : register(u0);
SamplerState gLinear : register(s0);
cbuffer Params : register(b0) {
  float4 gRow0;
  float4 gRow1;
  float4 gRow2;
  uint2 gOutSize;
  uint2 gSrcSize;
};

// Fits the source inside the output keeping its aspect (spec §3.2); bars are black.
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= gOutSize.x || id.y >= gOutSize.y) return;
  float s = min(float(gOutSize.x) / gSrcSize.x, float(gOutSize.y) / gSrcSize.y);
  float2 size = float2(gSrcSize) * s;
  float2 p = float2(id.xy) + 0.5 - (float2(gOutSize) - size) * 0.5;
  if (p.x < 0 || p.y < 0 || p.x >= size.x || p.y >= size.y) {
    gOut[id.xy] = float4(0, 0, 0, 1);
    return;
  }
  gOut[id.xy] = gSrc.SampleLevel(gLinear, p / size, 0);
}
