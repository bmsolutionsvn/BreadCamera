cbuffer Params : register(b0) {
  float4 gA;
  float4 gB;
  uint2 gOutSize;
  uint2 gSrcSize;
};

Texture2D<float4> gFrame : register(t0);
Texture2D<float> gAlpha : register(t1);
RWTexture2D<float4> gOut : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= gOutSize.x || id.y >= gOutSize.y) return;
  float4 acc = 0;
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x) {
      int2 p = min(int2(id.xy) * 4 + int2(x, y), int2(gSrcSize) - 1);
      float w = 1.0 - gAlpha[p];
      acc += float4(gFrame[p].rgb * w, w);
    }
  gOut[id.xy] = acc / 16.0;  // premultiplied background colour + background weight
}
