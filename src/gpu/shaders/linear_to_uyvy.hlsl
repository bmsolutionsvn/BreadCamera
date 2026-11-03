Texture2D<float4> gSrc : register(t0);
RWTexture2D<unorm float4> gOut : register(u0);  // R8G8B8A8_UNORM; bytes are U, Y0, V, Y1
SamplerState gLinear : register(s0);
cbuffer Params : register(b0) {
  float4 gRow0;  // Y row
  float4 gRow1;  // Cb row
  float4 gRow2;  // Cr row
  uint2 gOutSize;  // packed: width / 2 x height
  uint2 gSrcSize;
};

float3 toYuv(float4 c) {
  float3 g = pow(saturate(c.rgb), 1.0 / 2.4);
  return float3(dot(gRow0.xyz, g) + gRow0.w, dot(gRow1.xyz, g) + gRow1.w,
                dot(gRow2.xyz, g) + gRow2.w);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= gOutSize.x || id.y >= gOutSize.y) return;
  float2 size = (float2)gSrcSize;
  float2 uv = float2(2 * id.x + 0.5, id.y + 0.5) / size;
  float3 a = toYuv(gSrc.SampleLevel(gLinear, uv, 0));
  float3 b = toYuv(gSrc.SampleLevel(gLinear, uv + float2(1.0 / size.x, 0.0), 0));
  // One texel is a pixel pair: chroma averaged, each pixel keeps its own luma.
  gOut[id.xy] = float4((a.y + b.y) * 0.5, a.x, (a.z + b.z) * 0.5, b.x);
}