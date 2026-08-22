// Mesh shader: the OPAQUE 3D pass. One directional light, LAMBERT diffuse plus
// a constant ambient term, one albedo texture.
//
// DELIBERATELY NOT PBR. The GGX/metallic-roughness material model belongs to
// the Deadlock-class renderer arc, and a half-version here would be a second
// material model to reconcile with it later. Lambert + ambient is the whole
// lighting model in this file, and that is a decision, not a placeholder.
//
// Colors are LINEAR and may exceed 1.0 -- the canvas is RGBA16F and the tonemap
// node is what turns them display-referred (same contract as sprite.hlsl).
//
// MATRIX PACKING. Every float4x4 here is COLUMN-MAJOR, which is dxc's default
// for both the DXIL and SPIR-V targets and is exactly glm::mat4's memory layout
// (m[c] is column c), so the C++ side memcpys a glm::mat4 in with no transpose.
// mul(M, v) is therefore the ordinary M * v.
//
// UNITS are METERS (MKS) -- MeshBuilder emits meters and the camera's nearZ/
// farZ are meters.

// THE NORMAL MATRIX (normalMatrixCol0/1/2), Task 8/F2a. Packed as three
// float4 COLUMNS rather than a bare float3x3 for the same reason
// MeshFrameCB below packs g_lightDirection etc. as float4 instead of float3:
// each column gets its own explicit 16-byte slot (12 bytes used, 4 padding)
// so the C++ struct (MeshNode.cpp's PackedNormalMatrix) and this layout agree
// byte-for-byte with no compiler-packing ambiguity between the two. Together
// with `model` and `baseColor` this is EXACTLY 128 bytes -- Vulkan's
// GUARANTEED MINIMUM maxPushConstantsSize, with ZERO headroom left. See
// MeshNode.cpp's MeshRootConstants comment before adding another field here.
struct MeshConstants
{
    float4x4 model;             // model -> world
    float4   baseColor;         // linear tint, multiplies the albedo sample
    float4   normalMatrixCol0;  // NormalMatrixFor(model)'s columns 0..2, xyz
    float4   normalMatrixCol1;  // used / w padding -- see the comment above
    float4   normalMatrixCol2;
};

#if SPIRV
[[vk::push_constant]] ConstantBuffer<MeshConstants> g_PC;
#define g_model            g_PC.model
#define g_baseColor        g_PC.baseColor
#define g_normalMatrixCol0 g_PC.normalMatrixCol0
#define g_normalMatrixCol1 g_PC.normalMatrixCol1
#define g_normalMatrixCol2 g_PC.normalMatrixCol2
#else
cbuffer MeshConstantsCB : register(b0)
{
    MeshConstants g_PCData;
}
#define g_model            g_PCData.model
#define g_baseColor        g_PCData.baseColor
#define g_normalMatrixCol0 g_PCData.normalMatrixCol0
#define g_normalMatrixCol1 g_PCData.normalMatrixCol1
#define g_normalMatrixCol2 g_PCData.normalMatrixCol2
#endif

// PER-FRAME, and an ordinary descriptor-set constant buffer rather than more
// root constants: the view-projection alone is 64 bytes and root/push-constant
// budget is the scarcest thing in a pipeline layout. b1 in the implicit space0,
// which is where MeshNode's descriptor set is (THE REGISTER-SPACE RULE,
// Batch2DNode.hpp).
cbuffer MeshFrameCB : register(b1)
{
    float4x4 g_viewProjection;
    // xyz: a UNIT vector pointing TOWARD the light, ALREADY NORMALIZED by
    // MeshNode::Record -- do NOT normalize it again here. A zero vector is the
    // legal "no directional light" value, and normalize() on one is a division
    // by zero whose NaN would propagate through N.L into every lit pixel;
    // dotting against the zero vector instead gives 0, i.e. ambient only. The
    // CPU is the only place that case can be checked, and normalizing there
    // costs one normalize per frame rather than one per pixel here.
    float4   g_lightDirection;
    float4   g_lightColor;       // rgb: linear radiance
    float4   g_ambient;          // rgb: the constant ambient term
};

struct VSInput
{
    float3 pos    : POSITION;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD0;
};

struct VSOutput
{
    float4 pos    : SV_Position;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD0;
};

VSOutput vs_main(VSInput input)
{
    VSOutput output;
    const float4 world = mul(g_model, float4(input.pos, 1.0));
    output.pos = mul(g_viewProjection, world);
    // THE INVERSE TRANSPOSE (Task 8/F2a), not the upper 3x3: a surface
    // tangent scales WITH the object, so for dot(normal, tangent) to stay
    // zero after a NON-UNIFORM scale the normal has to scale by the INVERSE
    // along each axis, not the same factor `model` applies to a position.
    // MeshNode::NormalMatrixFor computes this once per instance on the CPU
    // (glm::transpose(glm::inverse(upper 3x3)), with a singular-model guard
    // that returns identity rather than feeding this shader a NaN) and
    // MeshNode::Record pushes its three columns above -- built as an explicit
    // weighted sum of those columns, NOT float3x3(col0,col1,col2), because
    // HLSL's matrix-from-vectors constructor fills ROWS from its arguments,
    // which would silently transpose this: mul(M,v) == v.x*col0 + v.y*col1 +
    // v.z*col2 is the definition of a column-major matrix-vector product, and
    // stating it this way is correct on BOTH the SPIR-V and DXIL targets with
    // no dependence on either compiler's matrix-packing defaults.
    output.normal = input.normal.x * g_normalMatrixCol0.xyz
                   + input.normal.y * g_normalMatrixCol1.xyz
                   + input.normal.z * g_normalMatrixCol2.xyz;
    output.uv     = input.uv;
    return output;
}

Texture2D    g_Albedo  : register(t0);
SamplerState g_Sampler : register(s0);

float4 ps_main(VSOutput input) : SV_Target0
{
    const float3 n      = normalize(input.normal);
    // NOT normalized here -- see g_lightDirection's comment. The zero vector is
    // the "no directional light" case and dot() handles it; normalize() would
    // not.
    const float  ndotl  = saturate(dot(n, g_lightDirection.xyz));
    const float4 albedo = g_Albedo.Sample(g_Sampler, input.uv) * g_baseColor;
    const float3 lit    = albedo.rgb * (g_ambient.rgb + g_lightColor.rgb * ndotl);
    return float4(lit, albedo.a);
}
