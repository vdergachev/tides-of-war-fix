
cbuffer Transfer : register(b0) { uint key; uint3 reserved; };
Texture2D image:register(t0); SamplerState sampling:register(s0);
float4 ps_main(float4 position:SV_POSITION, float2 uv:TEXCOORD0):SV_TARGET {
    float3 c=image.Sample(sampling,uv).rgb;
    uint3 q=(uint3)round(saturate(c)*255);
    uint packed=((q.r>>3)<<11)|((q.g>>2)<<5)|(q.b>>3);
    if(packed==key) discard;
    return float4(c,1);
}