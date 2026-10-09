Texture2D<float4> gSrc : register(t0);
RWTexture2D<unorm float4> gOut : register(u0);  // R8G8B8A8_UNORM, swizzled to BGRA bytes
SamplerState gLinear : register(s0);
cbuffer Params : register(b0) {
  float4 gRow0;
  float4 gRow1;
  float4 gRow2;
  uint2 gOutSize;
  uint2 gSrcSize;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= gOutSize.x || id.y >= gOutSize.y) return;
  float2 uv = (float2(id.xy) + 0.5) / float2(gOutSize);
  float4 c = gSrc.SampleLevel(gLinear, uv, 0);
  float3 g = pow(saturate(c.rgb), 1.0 / 2.4);
  gOut[id.xy] = float4(g.b, g.g, g.r, saturate(c.a));
}
