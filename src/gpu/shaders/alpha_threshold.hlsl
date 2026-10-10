cbuffer Params : register(b0) {
  float4 gA;
  float4 gB;
  uint2 gOutSize;
  uint2 gSrcSize;
};

Texture2D<float> gRaw : register(t0);
RWTexture2D<float> gOut : register(u0);
SamplerState gLinear : register(s0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= gOutSize.x || id.y >= gOutSize.y) return;
  float2 uv = (float2(id.xy) + 0.5) / float2(gOutSize);
  float raw = gRaw.SampleLevel(gLinear, uv, 0);
  gOut[id.xy] = smoothstep(gA.x - 0.1, gA.x + 0.1, raw);
}
