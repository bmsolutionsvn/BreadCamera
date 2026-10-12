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
  float sigma = gA.z;
  if (sigma < 0.01) {
    gOut[id.xy] = gSrc[id.xy];
    return;
  }
  int r = min((int)ceil(sigma * 3.0), 24);
  float sum = 0, wsum = 0;
  for (int i = -r; i <= r; ++i) {
    int2 p = clamp(int2(id.xy) + int2(gA.xy) * i, int2(0, 0), int2(gOutSize) - 1);
    float w = exp(-(i * i) / (2.0 * sigma * sigma));
    sum += w * gSrc[p];
    wsum += w;
  }
  gOut[id.xy] = sum / wsum;
}
