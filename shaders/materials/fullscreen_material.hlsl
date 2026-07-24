// ENGINE-OWNED material template: fullscreen surface. NOT compiled by
// compile-shaders.bat -- the substitution slots below make this a template,
// not a shader. The material pipeline (Arcane/Material/MaterialSource.cpp)
// stitches two slots (careful: ANY percent-brace sequence in this file is a
// slot, comments included):
//   MATERIAL_CBUFFER <- generated from the snippet's //@param decls:
//       cbuffer Material : register(b0) + Texture2D tN + MaterialSampler s0
//   MATERIAL_BODY    <- the designer snippet defining float4 shade(Varyings)
// and hands the result to the runtime ShaderCompiler (DXIL + SPIR-V; plain
// register() declarations work on both targets -- the -fvk-*-shift flags in
// ShaderConventions.hpp translate them for Vulkan).
//
// The Globals cbuffer layout MUST stay in lockstep with Arcane::GlobalParams
// (Arcane/Material/GlobalParams.hpp): one 16-byte register at b1.
// Its member names (Time, DeltaTime, ViewportSize) are reserved words for
// snippet params -- the //@param parser rejects them.

%{MATERIAL_CBUFFER}

cbuffer Globals : register(b1)
{
    float  Time;           // seconds since app start
    float  DeltaTime;      // seconds, last frame
    float2 ViewportSize;   // pixels
};

struct Varyings
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

// Fullscreen triangle from SV_VertexID -- no vertex buffer (tonemap.hlsl shape).
Varyings vs_main(uint vertexId : SV_VertexID)
{
    Varyings o;
    o.uv = float2((vertexId << 1) & 2, vertexId & 2);
    o.pos = float4(o.uv.x * 2.0 - 1.0, 1.0 - o.uv.y * 2.0, 0.0, 1.0);
    return o;
}

%{MATERIAL_BODY}

float4 ps_main(Varyings v) : SV_Target0
{
    return shade(v);
}
