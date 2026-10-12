Texture2D<float4> gFrame : register(t0);
Texture3D<float4> gLut : register(t1);
RWTexture2D<float4> gOut : register(u0);
SamplerState gLinear : register(s0);
cbuffer Params : register(b0) {
  float4 gA;  // exposure multiplier, WB r, g, b
  float4 gB;  // lift, gamma, gain, saturation
  float4 gC;  // useLut, lutSize
  uint2 gOutSize;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= gOutSize.x || id.y >= gOutSize.y) return;
  float4 c = gFrame[id.xy];
  float3 lin = c.rgb;
  if (gC.x > 0.5) {
    float n = gC.y;
    float3 g = pow(saturate(lin), 1.0 / 2.4);
    g = gLut.SampleLevel(gLinear, (g * (n - 1.0) + 0.5) / n, 0).rgb;
    lin = pow(saturate(g), 2.4);
  }
  lin *= gA.x * gA.yzw;
  float3 g2 = pow(saturate(lin), 1.0 / 2.4);
  g2 = gB.z * (g2 + gB.x * (1.0 - g2));
  g2 = pow(saturate(g2), 1.0 / gB.y);
  lin = pow(saturate(g2), 2.4);
  float luma = dot(lin, float3(0.2126, 0.7152, 0.0722));
  lin = max(luma + (lin - luma) * gB.w, 0.0);
  gOut[id.xy] = float4(lin, c.a);
}
