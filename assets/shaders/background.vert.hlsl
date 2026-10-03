float4 main(uint id : SV_VertexID) : SV_Position
{
    float2 position = id == 0 ? float2(-1, -1)
        : (id == 1 ? float2(-1, 3) : float2(3, -1));
    return float4(position, 0, 1);
}
