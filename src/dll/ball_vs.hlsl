// 32x32 local trail tiles, one placement per instance in draw order.
cbuffer Placement : register(b0) { float4 rects[1024]; };
struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};
VertexOutput vs_main(uint id : SV_VertexID, uint instance : SV_InstanceID)
{
    VertexOutput result;
    float2 corner = float2(id & 1, id >> 1);
    float4 rect = rects[instance];
    float2 xy = rect.xy + corner * rect.zw;
    result.position = float4(xy.x * 2 - 1, 1 - xy.y * 2, 0, 1);
    result.uv = (float2(instance & 31, instance >> 5) + corner) / 32;
    return result;
}
