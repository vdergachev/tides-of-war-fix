
cbuffer Transfer : register(b0) { uint key; uint pause_mask; uint output_width; uint output_height; };
Texture2D image:register(t0); SamplerState sampling:register(s0);
Texture2D fresh_plain:register(t1); Texture2D record_plain:register(t2);
float4 ps_main(float4 position:SV_POSITION, float2 uv:TEXCOORD0):SV_TARGET {
    if (pause_mask) {
        int2 p = int2(position.xy * float2(640.0 / output_width, 480.0 / output_height));
        if (any(fresh_plain.Load(int3(p,0)).rgb != record_plain.Load(int3(p,0)).rgb)) discard;
    }
    float3 c=image.Sample(sampling,uv).rgb;
    uint3 q=(uint3)round(saturate(c)*255);
    uint packed=((q.r>>3)<<11)|((q.g>>2)<<5)|(q.b>>3);
    if(packed==key) discard;
    return float4(c,1);
}