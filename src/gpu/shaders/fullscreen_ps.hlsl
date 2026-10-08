Texture2D<float4> gSrc : register(t0);
SamplerState gLinear : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  float4 c = gSrc.Sample(gLinear, uv);
  return float4(pow(saturate(c.rgb), 1.0 / 2.4), 1.0);
}
