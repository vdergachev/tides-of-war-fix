Texture2D current_bottom : register(t0);
Texture2D previous_terrain : register(t1);
Texture2D final_frame : register(t2);
Texture2D current_terrain : register(t3);
cbuffer Terrain : register(b0) {
    float4 view_rect;
    float4 shifts;
    float4 pixel_scale;
    float4 unused;
};
bool covered(float2 p) {
    return all(p >= view_rect.xy) && all(p < view_rect.zw);
}
float4 ps_main(float4 position : SV_Position) : SV_Target
{
    float2 p = position.xy * pixel_scale.xy;
    if (!covered(p)) {
        return final_frame.Load(int3(int2(p), 0));
    }
    float2 current = p - shifts.xy;
    if (covered(current)) {
        return current_bottom.Load(int3(int2(current), 0));
    }
    float2 previous = p - shifts.zw;
    if (covered(previous)) {
        return previous_terrain.Load(int3(int2(previous), 0));
    }
    // A diagonal corner can be absent in both records; repeat the nearest edge texel.
    current = clamp(current, view_rect.xy, view_rect.zw - 1);
    return current_terrain.Load(int3(int2(current), 0));
}
