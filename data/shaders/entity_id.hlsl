// Entity-id (hit-proxy) pass, 2D half: rasterize each pickable silhouette's
// BOUNDING QUAD and, in the PS, discard fragments outside the analytic shape,
// writing the drawable's 1-based uint id to an R32_UINT target (0 == background).
// NO tessellation -- circle/capsule coverage is analytic in the PS (the
// v1-simplest option per the plan); a Quad or Box drawable covers its whole
// bounding quad.
//
// Positions arrive in WORLD space (F4 plan 2); the root block carries the
// frame's view-projection. local/radius/halfLen are METRES -- the analytic
// coverage test below is unit-agnostic, so it is unchanged. The depth test is
// OFF for this half (PickOutlineNodes.cpp): the 2D silhouettes resolve a
// contested pixel by submission order (the output merger is primitive-ordered,
// the drawables are emitted back to front), and the MESH half that follows
// (entity_id_mesh.hlsl) is depth-tested against a cleared depth, so a mesh
// always owns a pixel it shares with a sprite.
//
// MATRIX PACKING: float4x4 is COLUMN-MAJOR (dxc's default on both targets),
// so glm's column-major mat4 uploads verbatim and mul(M, v) is the ordinary
// M * v (mesh.hlsl's rule).
//
// Vertex inputs use DISTINCT custom semantics (POSITION / LOCAL / SHAPEPARAM /
// KINDID), NOT TEXCOORD0/1/2: the attribute name is used verbatim as the D3D
// SemanticName at SemanticIndex 0, and Vulkan input locations are assigned by
// declaration order -- so the C++ attribute array order (PickOutlineNodes.cpp)
// MUST match this struct's member order.

struct BatchConstants
{
    float4x4 viewProj;
    float4   pad;        // 80 bytes total: the same block size as entity_id_mesh.hlsl's, ONE pipeline layout serves both
};

#if SPIRV
[[vk::push_constant]] ConstantBuffer<BatchConstants> g_PC;
#define g_viewProj g_PC.viewProj
#else
cbuffer BatchConstantsCB : register(b0)
{
    BatchConstants g_PCData;
}
#define g_viewProj g_PCData.viewProj
#endif

struct VSInput
{
    float3 pos   : POSITION;     // WORLD-space corner of the bounding quad
    float2 local : LOCAL;        // shape-local coords (unrotated), metres
    float2 rl    : SHAPEPARAM;   // (radius, halfLen), metres
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
    output.pos   = mul(g_viewProj, float4(input.pos, 1.0));
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
