cbuffer Background : register(b0, space3)
{
    float4 Board;
    float4 Minor;
    float4 Major;
    float4 Axis;
    float4 Dots;
    float4 View;       // logical width, height, pattern, unused
    float4 Scale;      // physical/logical x, y, lattice spacing, base spacing
    float4 Phase;      // x/y modulo major lattice; diagonal intercept phases
    float4 Axes;       // camera in screen units, minor/major widths
    float4 Marks;      // axis width, dot radius, diagonal period, unused
    float4 BoardRect;  // board limits in logical screen coordinates
};

float periodic_distance(float value, float period)
{
    return abs((frac(value / period + 0.5) - 0.5) * period);
}

float coverage(float distance, float width)
{
    return saturate((width * 0.5 - distance) * min(Scale.x, Scale.y) + 0.5);
}

float3 grid_line(float3 color, float value, float axis_value, bool majors, bool axis)
{
    color = lerp(color, Minor.rgb, coverage(periodic_distance(value, Scale.z), Axes.z));
    if (majors) color = lerp(color, Major.rgb,
        coverage(periodic_distance(value, Scale.z * 5.0), Axes.w));
    if (axis) color = lerp(color, Axis.rgb, coverage(abs(axis_value), Marks.x));
    return color;
}

float4 main(float4 position : SV_Position) : SV_Target0
{
    float2 screen = position.xy / Scale.xy;
    if (screen.x < BoardRect.x || screen.y < BoardRect.y
        || screen.x > BoardRect.z || screen.y > BoardRect.w) discard;
    float2 offset = screen - View.xy * 0.5;
    float2 lattice = offset + Phase.xy;
    uint pattern = (uint)View.z;
    float3 color = Board.rgb;
    if (pattern == 2 || pattern == 3 || pattern == 4) {
        bool majors = pattern != 4;
        color = grid_line(color, lattice.x, offset.x + Axes.x, majors, true);
        color = grid_line(color, lattice.y, offset.y + Axes.y, majors, true);
    }
    if (pattern == 6 || pattern == 7 || pattern == 8)
        color = grid_line(color, lattice.y, offset.y + Axes.y, false, pattern == 7);
    if (pattern == 1 || pattern == 4) {
        float2 distance = float2(periodic_distance(lattice.x, Scale.z),
            periodic_distance(lattice.y, Scale.z));
        float radius_distance = Marks.y < 2.0 ? max(distance.x, distance.y) : length(distance);
        color = lerp(color, Dots.rgb, coverage(radius_distance, Marks.y * 2.0));
    }
    if (pattern == 5 || pattern == 7) {
        float slope = pattern == 5 ? 1.0 : 1.7320508;
        float normal = sqrt(1.0 + slope * slope);
        float first = periodic_distance(offset.y - slope * offset.x + Phase.z, Marks.z) / normal;
        float second = periodic_distance(offset.y + slope * offset.x + Phase.w, Marks.z) / normal;
        color = lerp(color, Minor.rgb, coverage(first, Axes.z));
        color = lerp(color, Minor.rgb, coverage(second, Axes.z));
    }
    return float4(color, 1.0);
}
