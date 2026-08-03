Texture2D ImageTexture : register(t0, space2);
SamplerState ImageSampler : register(s0, space2);

struct PSInput { float4 color : TEXCOORD0; float2 uv : TEXCOORD1; };

float4 main(PSInput input) : SV_Target0
{
    return input.color * ImageTexture.Sample(ImageSampler, input.uv);
}
