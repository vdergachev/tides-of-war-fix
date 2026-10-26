Texture2D frame_texture : register(t0);
SamplerState point_sampler : register(s0);

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOutput vs_main(uint id : SV_VertexID)
{
    VertexOutput result;
    result.uv = float2(id & 1, id >> 1);
    result.position = float4(result.uv.x * 2 - 1, 1 - result.uv.y * 2, 0, 1);
    return result;
}

float4 ps_main(VertexOutput input) : SV_Target
{
    return frame_texture.Sample(point_sampler, input.uv);
}
