// Batch shader: textured/colored quads. Untextured primitives bind the
// 1x1 white texture. Two projection paths, selected PER SPAN by the push
// constants (F4 plan 1, Task 4):
//   * SCREEN (worldSpace == 0): positions are canvas pixels (y down) and
//     2/viewport reaches clip space -- the original mapping, unchanged;
//   * WORLD  (worldSpace != 0): positions are world metres (+Y up) and the
//     view-projection reaches clip space, so a tilted or distant quad gets
//     perspective-correct UVs (which projecting on the CPU would not give).
// Colors are LINEAR and may exceed 1.0 (HDR canvas).
//
// MATRIX PACKING: float4x4 is COLUMN-MAJOR (dxc's default on both targets),
// which is glm::mat4's memory layout, so Batch2DNode memcpys the glm matrix
// in with no transpose and mul(M, v) is the ordinary M * v (mesh.hlsl's rule).

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

Texture2D    g_Texture : register(t0);
SamplerState g_Sampler : register(s0);

float4 ps_main(VSOutput input) : SV_Target0
{
    return g_Texture.Sample(g_Sampler, input.uv) * input.color;
}
