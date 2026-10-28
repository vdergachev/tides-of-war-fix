Texture2D current_bottom : register(t0);
Texture2D previous_terrain : register(t1);
Texture2D final_frame : register(t2);
Texture2D current_terrain : register(t3);
cbuffer Terrain : register(b0) {
    float4 view_rect;
    float4 shifts;
    float4 pixel_scale;
    float4 unused;
#ifdef WAKE
    float4 wake_field;
    float4 wake_camera;
#endif
};
#ifdef WAKE
Texture2D<int2> ripple : register(t4);
Texture2D<float> foam : register(t5);
float4 apply_wake(float4 color, int2 pixel) {
    int3 rgb = int3(round(color.rgb * 255));
    // Prototype shade always masks ripples, even when wake.land is disabled.
    if (rgb.b > rgb.r + 8 && rgb.b > rgb.g * 1.08) {
        int2 cell = int2(floor((float2(pixel) + wake_camera.xy - view_rect.xy) / wake_field.z) - wake_field.xy);
        int2 heights = ripple.Load(int3(cell, 0));
        int offset = int(round(lerp(float(heights.x), float(heights.y), wake_field.w)));
        if (offset != 0) {
            uint packed = uint(((rgb.r >> 3) << 11) | ((rgb.g >> 2) << 5) | (rgb.b >> 3));
            packed = (packed + offset) & 65535;
            int3 v = int3(packed >> 11, (packed >> 5) & 63, packed & 31);
            rgb = int3((v.r << 3) | (v.r >> 2), (v.g << 2) | (v.g >> 4), (v.b << 3) | (v.b >> 2));
        }
    }
    int alpha = int(round(foam.Load(int3(pixel, 0)) * 255));
    rgb = int3((uint3(rgb) * uint(255 - alpha) + uint(255 * alpha + 127)) / 255u);
    return float4(float3(rgb) / 255, color.a);
}
#endif
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
#ifdef WAKE
        float4 bottom = current_bottom.Load(int3(int2(current), 0));
        // Display.draw_wake is composited through the prototype's 8-pixel inset.
        if (all(p >= view_rect.xy + 8) && all(p < view_rect.zw - 8)) {
            return apply_wake(bottom, int2(current));
        }
        return bottom;
#else
        return current_bottom.Load(int3(int2(current), 0));
#endif
    }
    float2 previous = p - shifts.zw;
    if (covered(previous)) {
        return previous_terrain.Load(int3(int2(previous), 0));
    }
    // A diagonal corner can be absent in both records; repeat the nearest edge texel.
    current = clamp(current, view_rect.xy, view_rect.zw - 1);
    return current_terrain.Load(int3(int2(current), 0));
}
