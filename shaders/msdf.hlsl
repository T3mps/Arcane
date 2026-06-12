// MSDF glyph shader: median-of-3 distance reconstruction with screen-space
// AA (Chlumsky). The batcher emits a textured quad in canvas pixels; the
// pixel shader samples the glyph atlas and reconstructs coverage. Shares
// sprite.hlsl's vertex layout and clip transform (compiled separately so the
// artifacts stay self-contained per pipeline).
//
// kPxRange/kAtlasSize are compile-time constants MIRRORED in TextSystem.cpp
// (the glyph atlas generator) -- change BOTH together or text edges break.

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

// MIRRORED in TextSystem.cpp (kPxRange / kAtlasSize). The atlas generator
// maps each glyph's distance field over kPxRange pixels at the atlas glyph
// scale, into a kAtlasSize square atlas; these reconstruct that contract.
static const float kPxRange   = 6.0;
static const float kAtlasSize = 1024.0;

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

float Median3(float a, float b, float c)
{
    return max(min(a, b), min(max(a, b), c));
}

float4 ps_main(VSOutput input) : SV_Target0
{
    float3 msd = g_Texture.Sample(g_Sampler, input.uv).rgb;
    float sd = Median3(msd.r, msd.g, msd.b);

    // Chlumsky screen-px-range: how many screen pixels one SDF unit spans.
    float2 unitRange = (kPxRange / kAtlasSize).xx;
    float2 screenTexSize = 1.0 / fwidth(input.uv);
    float screenPxRange = max(0.5 * dot(unitRange, screenTexSize), 1.0);

    float screenPxDistance = screenPxRange * (sd - 0.5);
    float alpha = saturate(screenPxDistance + 0.5);
    if (alpha <= 0.0)
        discard;
    return float4(input.color.rgb, input.color.a * alpha);
}
