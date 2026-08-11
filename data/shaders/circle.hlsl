// SDF circle: the batcher emits a quad whose uv spans [-1, 1]; the PS
// keeps the unit disc with fwidth-based antialiasing. Shares sprite.hlsl's
// vertex layout and vs_main (compiled separately so the artifacts stay
// self-contained per pipeline).

struct BatchConstants
{
    float2 invHalfViewport;
    float2 pad;
};

#if SPIRV
[[vk::push_constant]] ConstantBuffer<BatchConstants> g_PC;
#define g_invHalfViewport g_PC.invHalfViewport
#else
cbuffer BatchConstantsCB : register(b0)
{
    BatchConstants g_PCData;
}
#define g_invHalfViewport g_PCData.invHalfViewport
#endif

struct VSInput
{
    float2 pos   : POSITION;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
};

struct VSOutput
{
    float4 pos   : SV_Position;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
};

VSOutput vs_main(VSInput input)
{
    VSOutput output;
    output.pos = float4(input.pos.x * g_invHalfViewport.x - 1.0,
                        1.0 - input.pos.y * g_invHalfViewport.y,
                        0.0, 1.0);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}

float4 ps_main(VSOutput input) : SV_Target0
{
    float dist = length(input.uv);
    float aa = fwidth(dist);
    float alpha = 1.0 - smoothstep(1.0 - aa, 1.0, dist);
    if (alpha <= 0.0)
        discard;
    return float4(input.color.rgb, input.color.a * alpha);
}
