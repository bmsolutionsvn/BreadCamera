cbuffer Params : register(b0) {
  float4 gA;
  float4 gB;
  uint2 gOutSize;
  uint2 gSrcSize;
};

Texture2D<float4> gSrc : register(t0);
RWTexture2D<float4> gOut : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= gOutSize.x || id.y >= gOutSize.y) return;
  float sigma = max(gA.z, 0.5);
  int r = min((int)ceil(sigma * 3.0), 40);
  float4 sum = 0;
  float wsum = 0;
  for (int i = -r; i <= r; ++i) {
    int2 p = clamp(int2(id.xy) + int2(gA.xy) * i, int2(0, 0), int2(gOutSize) - 1);
    float w = exp(-(i * i) / (2.0 * sigma * sigma));
    sum += w * gSrc[p];
    wsum += w;
  }
  gOut[id.xy] = sum / wsum;
}
