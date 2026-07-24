// ENGINE-OWNED material template: sprite surface (Slice 8). NOT compiled by
// compile-shaders.bat -- the substitution slots below make this a template,
// not a shader. The material pipeline (Arcane/Material/MaterialSource.cpp)
// stitches two slots (careful: ANY percent-brace sequence in this file is a
// slot, comments included):
//   MATERIAL_CBUFFER <- generated from the snippet's //@param decls for the
//       SPRITE register map (GlobalParams.hpp): cbuffer Material : register(b1)
//       + Texture2D t1..N. MaterialSampler (s0) is declared HERE, not there.
//   MATERIAL_BODY    <- the designer snippet defining float4 shade(Varyings)
// and hands the result to the runtime ShaderCompiler (DXIL + SPIR-V).
//
// The vertex stage and push constants mirror sprite.hlsl EXACTLY -- the
// Batcher2D feeds the same vertex stream (pos px, uv, linear color) and the
// same b0 push-constant block to every 2D pipeline. The Globals cbuffer layout
// MUST stay in lockstep with Arcane::GlobalParams (one 16-byte register), here
// at b2 (b0 = push constants, b1 = material params). Reserved snippet names:
// Time, DeltaTime, ViewportSize, MaterialSampler, SpriteTexture.

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

%{MATERIAL_CBUFFER}

cbuffer Globals : register(b2)
{
    float  Time;           // seconds since app start
    float  DeltaTime;      // seconds, last frame
    float2 ViewportSize;   // pixels
};

Texture2D    SpriteTexture   : register(t0);   // the sprite's own texture (white 1x1 when untextured)
SamplerState MaterialSampler : register(s0);

struct VSInput
{
    float2 pos   : POSITION;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
};

struct Varyings
{
    float4 pos   : SV_Position;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;     // the sprite's tint (linear, may exceed 1)
};

Varyings vs_main(VSInput input)
{
    Varyings o;
    o.pos = float4(input.pos.x * g_invHalfViewport.x - 1.0,
                   1.0 - input.pos.y * g_invHalfViewport.y,
                   0.0, 1.0);
    o.uv = input.uv;
    o.color = input.color;
    return o;
}

%{MATERIAL_BODY}

float4 ps_main(Varyings v) : SV_Target0
{
    return shade(v);
}
