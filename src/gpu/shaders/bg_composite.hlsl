cbuffer Params : register(b0) {
  float4 gA;
  float4 gB;
  uint2 gOutSize;
  uint2 gSrcSize;
};

Texture2D<float4> gFrame : register(t0);
Texture2D<float> gAlpha : register(t1);
Texture2D<float4> gBg : register(t2);
RWTexture2D<float4> gOut : register(u0);
SamplerState gLinear : register(s0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= gOutSize.x || id.y >= gOutSize.y) return;
  float4 fg = gFrame[id.xy];
  float a = gAlpha[id.xy];
  int mode = (int)gA.x;
  if (mode == 1) {
    gOut[id.xy] = float4(fg.rgb, a);
    return;
  }
  float2 uv = (float2(id.xy) + 0.5) / float2(gOutSize);
  float3 bg;
  if (mode == 2) {
    float4 b = gBg.SampleLevel(gLinear, uv, 0);
    bg = b.w > 1e-4 ? b.rgb / b.w : fg.rgb;
  } else if (mode == 4) {
    bg = gBg.SampleLevel(gLinear, uv, 0).rgb;
  } else {
    bg = gB.rgb;
  }
  gOut[id.xy] = float4(lerp(bg, fg.rgb, a), 1.0);
}
