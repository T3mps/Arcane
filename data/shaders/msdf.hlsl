// MSDF glyph shader: median-of-3 distance reconstruction with screen-space
// AA (Chlumsky). The batcher emits a textured quad in canvas pixels; the
// pixel shader samples the glyph atlas and reconstructs coverage. Shares
// sprite.hlsl's vertex layout and BatchConstants block (the ONE 80-byte b0
// every 2D pipeline binds), but takes only the SCREEN path: Glyph() records
// canvas pixels and no world-space text submission exists (F4 plan 1, T4).
// Compiled separately so the artifacts stay self-contained per pipeline.
//
// kPxRange/kAtlasSize are compile-time constants MIRRORED in TextSystem.cpp
// (the glyph atlas generator) -- change BOTH together or text edges break.

struct BatchConstants
{
    float4x4 viewProj;         // world -> clip; declared for the shared b0 shape, unused here
    float2   invHalfViewport;  // 2.0 / (canvasW, canvasH)
    uint     worldSpace;       // always 0 for a glyph span
    float    pad;
};

#if SPIRV
[[vk::push_constant]] ConstantBuffer<BatchConstants> g_PC;   // 80 bytes <= Vulkan's 128 minimum
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
    float3 pos   : POSITION;   // Batch2DVertex::pos (vec3); z is 0 on the screen path
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
