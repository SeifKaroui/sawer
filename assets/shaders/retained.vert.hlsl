cbuffer Matrices : register(b0, space1)
{
    row_major float4x4 Projection;
    row_major float4x4 Model;
};
cbuffer BoardTheme : register(b1, space1)
{
    float4 Sources[7];
    float4 Targets[7];
    float4 ThemeState;
};
struct Input { float2 position : TEXCOORD0; float4 color : TEXCOORD1; };
struct Output { float4 color : TEXCOORD0; float4 position : SV_Position; };
Output main(Input input)
{
    Output output;
    output.color = input.color;
    if (ThemeState.x > 0) {
        for (int index = 0; index < 7; ++index) {
            if (all(abs(input.color.rgb - Sources[index].rgb) < 0.001)) {
                output.color.rgb = lerp(input.color.rgb, Targets[index].rgb, ThemeState.x);
                break;
            }
        }
    }
    output.position = mul(mul(float4(input.position, 0, 1), Model), Projection);
    return output;
}
