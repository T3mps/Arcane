// Entity-id (hit-proxy) pass: rasterize each pickable entity's BOUNDING QUAD
// and, in the PS, discard fragments outside the analytic shape, writing the
// drawable's 1-based uint id to an R32_UINT target (0 == background). Positions
// arrive in canvas pixels (y-down); the push constant carries 2/viewport to
// reach clip space (matches sprite.hlsl). NO tessellation -- circle/capsule
// coverage is analytic in the PS (the v1-simplest option per the plan); a Quad
// or Box drawable covers its whole bounding quad. Front-most wins by submission
// order: PickBuffer draws back-to-front and the output merger is
// primitive-ordered, so the last-drawn silhouette owns a contested pixel.
//
// Vertex inputs use DISTINCT custom semantics (POSITION / LOCAL / SHAPEPARAM /
// KINDID), NOT TEXCOORD0/1/2: nvrhi uses the attribute name verbatim as the
// D3D SemanticName at SemanticIndex 0 (it does not split trailing digits), and
// assigns Vulkan input locations by declaration order -- so the C++ attribute
// array order (PickBuffer.cpp) MUST match this struct's member order.

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
    float2 pos   : POSITION;     // canvas px: the rotated bounding-quad corner
    float2 local : LOCAL;        // shape-local coords (unrotated), canvas px
    float2 rl    : SHAPEPARAM;   // (radius, halfLen), canvas px
    uint2  ki    : KINDID;       // (kind, id): kind 0=Quad 1=Circle 2=Capsule 3=Box
};

struct VSOutput
{
    float4 pos   : SV_Position;
    float2 local : TEXCOORD0;
    float2 rl    : TEXCOORD1;
    nointerpolation uint2 ki : TEXCOORD2;
};

VSOutput vs_main(VSInput input)
{
    VSOutput output;
    output.pos = float4(input.pos.x * g_invHalfViewport.x - 1.0,
                        1.0 - input.pos.y * g_invHalfViewport.y,
                        0.0, 1.0);
    output.local = input.local;
    output.rl    = input.rl;
    output.ki    = input.ki;
    return output;
}

uint ps_main(VSOutput input) : SV_Target0
{
    const uint  kind   = input.ki.x;
    const uint  id     = input.ki.y;
    const float radius = input.rl.x;

    if (kind == 1u)            // Circle: keep the inscribed disc of `radius`.
    {
        if (length(input.local) > radius)
            discard;
    }
    else if (kind == 2u)      // Capsule: within `radius` of the central segment.
    {
        const float  halfLen = input.rl.y;
        const float2 p       = input.local;
        const float  cx      = clamp(p.x, -halfLen, halfLen);
        const float2 d       = float2(p.x - cx, p.y);
        if (length(d) > radius)
            discard;
    }
    // Quad (0) / Box (3): the whole bounding quad is covered -- no discard.

    return id;
}
