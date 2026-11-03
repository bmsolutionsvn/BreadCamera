Texture2D<float4> gSrc : register(t0);  // R8G8B8A8_UNORM; bytes are U, Y0, V, Y1
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
  // One texel is a pixel pair: U = r, V = b, the even pixel's Y = g and the odd pixel's Y = a.
  float4 c = gSrc[uint2(id.x / 2, id.y)];
  float y = (id.x & 1) ? c.a : c.g;
  float4 yuv1 = float4(y, c.r, c.b, 1.0);
  float3 rgb = float3(dot(gRow0, yuv1), dot(gRow1, yuv1), dot(gRow2, yuv1));
  gOut[id.xy] = float4(pow(saturate(rgb), 2.4), 1.0);
}