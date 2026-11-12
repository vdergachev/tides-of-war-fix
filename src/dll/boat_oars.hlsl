cbuffer Vertices : register(b0) { float4 data[576]; };
struct Output {
    float4 position : SV_Position;
    float4 color : COLOR0;
};
Output vs_main(uint id : SV_VertexID)
{
    Output result;
    float2 xy = data[id * 2].xy / float2(640, 480);
    result.position = float4(xy.x * 2 - 1, 1 - xy.y * 2, 0, 1);
    result.color = data[id * 2 + 1];
    return result;
}
float4 ps_main(Output input) : SV_Target { return input.color; }
