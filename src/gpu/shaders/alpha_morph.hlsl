cbuffer Params : register(b0) {
  float4 gA;
  float4 gB;
  uint2 gOutSize;
  uint2 gSrcSize;
};

Texture2D<float> gSrc : register(t0);
RWTexture2D<float> gOut : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= gOutSize.x || id.y >= gOutSize.y) return;
  int r = (int)gA.z;
  bool dilate = gA.w > 0.5;
  float v = dilate ? 0.0 : 1.0;
  for (int i = -r; i <= r; ++i) {
    int2 p = clamp(int2(id.xy) + int2(gA.xy) * i, int2(0, 0), int2(gOutSize) - 1);
    float s = gSrc[p];
    v = dilate ? max(v, s) : min(v, s);
  }
  gOut[id.xy] = v;
}
