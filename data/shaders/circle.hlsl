// SDF circle: the batcher emits a quad whose uv spans [-1, 1]; the PS
// keeps the unit disc with fwidth-based antialiasing. Shares sprite.hlsl's
// vertex layout, BatchConstants block and vs_main -- both projection paths,
// selected per span by g_worldSpace (compiled separately so the artifacts
// stay self-contained per pipeline). length(uv) is sign-agnostic, so
// CircleWorld's (-1,-1)..(1,1) corners ride this PS exactly as Circle()'s do.

struct BatchConstants
{
    float4x4 viewProj;         // world -> clip, WORLD-space spans only (column-major, glm layout)
    float2   invHalfViewport;  // 2.0 / (canvasW, canvasH), SCREEN-space spans
    uint     worldSpace;       // 1 = pos is world metres; 0 = canvas pixels (y down)
    float    pad;
};

#if SPIRV
[[vk::push_constant]] ConstantBuffer<BatchConstants> g_PC;   // 80 bytes <= Vulkan's 128 minimum
#define g_viewProj        g_PC.viewProj
#define g_invHalfViewport g_PC.invHalfViewport
#define g_worldSpace      g_PC.worldSpace
#else
cbuffer BatchConstantsCB : register(b0)
{
    BatchConstants g_PCData;
}
#define g_viewProj        g_PCData.viewProj
#define g_invHalfViewport g_PCData.invHalfViewport
#define g_worldSpace      g_PCData.worldSpace
#endif

struct VSInput
{
    float3 pos   : POSITION;
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
    if (g_worldSpace != 0)
        output.pos = mul(g_viewProj, float4(input.pos, 1.0));
    else
        output.pos = float4(input.pos.x * g_invHalfViewport.x - 1.0,
                            1.0 - input.pos.y * g_invHalfViewport.y, 0.0, 1.0);
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
