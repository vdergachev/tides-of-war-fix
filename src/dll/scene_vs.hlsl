cbuffer Placement : register(b0) { float4 rect; };
struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};
VertexOutput vs_main(uint id : SV_VertexID)
{
    VertexOutput result;
    result.uv = float2(id & 1, id >> 1);
    float2 xy = rect.xy + result.uv * rect.zw;
    result.position = float4(xy.x * 2 - 1, 1 - xy.y * 2, 0, 1);
    return result;
}
