Texture2D<float> gLuma : register(t0);
Texture2D<float2> gChroma : register(t1);
RWTexture2D<float4> gOut : register(u0);
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
  float y = gLuma[id.xy];
  float2 uv = (float2(id.xy) + 0.5) / float2(gOutSize);
  float2 c = gChroma.SampleLevel(gLinear, uv, 0);
  float4 yuv1 = float4(y, c.x, c.y, 1.0);
  float3 rgb = float3(dot(gRow0, yuv1), dot(gRow1, yuv1), dot(gRow2, yuv1));
  gOut[id.xy] = float4(pow(saturate(rgb), 2.4), 1.0);
}
