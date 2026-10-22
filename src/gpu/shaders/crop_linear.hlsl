// Auto frame: samples a sub-rectangle of the linear picture into a texture of the same size,
// so every later stage (colour, matte, upscale, outputs) sees the framed picture unchanged.

cbuffer Params : register(b0)
{
	float4 row0;
	float4 row1;
	float4 row2;
	uint outW;
	uint outH;
	uint srcW;
	uint srcH;
	float4 rect;
};

Texture2D<float4> src : register(t0);
SamplerState samp : register(s0);
RWTexture2D<float4> dst : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= outW || id.y >= outH)
		return;

	float2 uv = rect.xy + (float2(id.xy) + 0.5) / float2(outW, outH) * rect.zw;
	dst[id.xy] = src.SampleLevel(samp, uv, 0);
}
