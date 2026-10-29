
cbuffer Params : register(b0) {
    uint textured; uint alpha_test; uint alpha_func; float alpha_ref;
    uint specular; uint colorkey; uint modulate_alpha; uint padding;
};
struct Input { float2 xy:POSITION; float2 uv:TEXCOORD0; float4 color:COLOR0; float4 spec:COLOR1; };
struct Output { float4 xy:SV_POSITION; float2 uv:TEXCOORD0; nointerpolation float4 color:COLOR0;
                nointerpolation float4 spec:COLOR1; };
Output vs_main(Input i) {
    Output o; o.xy=float4(i.xy,0,1); o.uv=i.uv; o.color=i.color; o.spec=i.spec; return o;
}
Texture2D image:register(t0); SamplerState sampling:register(s0);
float4 ps_main(Output i):SV_TARGET {
    float4 c=i.color;
    if(textured) {
        float4 t=image.Sample(sampling,i.uv);
        if(colorkey && t.a==0) discard;
        c.rgb*=t.rgb;
        c.a=modulate_alpha ? c.a*t.a : t.a;
    }
    if(specular) c.rgb=saturate(c.rgb+i.spec.rgb);
    if(alpha_test) {
        bool accepted=alpha_func==8 || (alpha_func==2 && c.a<alpha_ref) ||
          (alpha_func==3 && c.a==alpha_ref) || (alpha_func==4 && c.a<=alpha_ref) ||
          (alpha_func==5 && c.a>alpha_ref) || (alpha_func==6 && c.a!=alpha_ref) ||
          (alpha_func==7 && c.a>=alpha_ref);
        if(!accepted) discard;
    }
    return c;
}