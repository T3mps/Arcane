// Dear ImGui render shader. Vertices arrive in display pixels; push
// constants carry scale/translate to clip space (the standard ImGui
// orthographic transform). Vertex color is sRGB-ish UI color -- ImGui
// draws POST-tonemap into the display-referred backbuffer.

struct ImGuiConstants
{
    float2 scale;
    float2 translate;
};

#if SPIRV
[[vk::push_constant]] ConstantBuffer<ImGuiConstants> g_PC;
#define g_scale g_PC.scale
#define g_translate g_PC.translate
#else
cbuffer ImGuiConstantsCB : register(b0)
{
    ImGuiConstants g_PCData;
}
#define g_scale g_PCData.scale
#define g_translate g_PCData.translate
#endif

struct VSInput
{
    float2 pos : POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};

struct VSOutput
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};

VSOutput vs_main(VSInput input)
{
    VSOutput output;
    output.pos = float4(input.pos * g_scale + g_translate, 0.0, 1.0);
    output.uv = input.uv;
    output.col = input.col;
    return output;
}

Texture2D    g_Texture : register(t0);
SamplerState g_Sampler : register(s0);

float4 ps_main(VSOutput input) : SV_Target0
{
    return input.col * g_Texture.Sample(g_Sampler, input.uv);
}
