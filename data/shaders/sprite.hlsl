// Batch shader: textured/colored quads. Untextured primitives bind the
// 1x1 white texture. Positions arrive in canvas pixels (y down); the push
// constants carry 2/viewport to reach clip space. Colors are LINEAR and
// may exceed 1.0 (HDR canvas).

struct BatchConstants
{
    float2 invHalfViewport;   // 2.0 / (canvasW, canvasH)
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

Texture2D    g_Texture : register(t0);
SamplerState g_Sampler : register(s0);

float4 ps_main(VSOutput input) : SV_Target0
{
    return g_Texture.Sample(g_Sampler, input.uv) * input.color;
}
