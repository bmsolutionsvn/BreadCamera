Texture2D<float4> gSrc : register(t0);
RWTexture2D<float4> gOut : register(u0);
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
  float4 c = gSrc[id.xy];
  gOut[id.xy] = float4(pow(saturate(c.rgb), 2.4), c.a);
}
