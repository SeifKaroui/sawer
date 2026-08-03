#include "colored_triangle.hlsli"

float4 main(VSOutput input) : SV_Target0
{
    return input.Color;
}

