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
//
// Task 8/10 spent the one headroom this budget had without growing it:
// normalMatrixCol0.w -- otherwise always zero, like col1/col2's own `.w` --
// carries the per-instance MATERIAL SLOT into this bindless array below, as
// RAW BITS (asuint/asfloat). See ps_main.
struct MeshConstants
{
    float4x4 model;             // model -> world
    float4   baseColor;         // linear tint; the WHOLE albedo when the material
                                 // slot below is kMeshInvalidMaterialSlot (the flat path)
    float4   normalMatrixCol0;  // xyz: NormalMatrixFor(model) col0. w: the packed
                                 // MATERIAL SLOT (Task 8/10) -- raw bits, never a float
    float4   normalMatrixCol1;  // xyz used / w padding, always zero
    float4   normalMatrixCol2;  // xyz used / w padding, always zero
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
// budget is the scarcest thing in a pipeline layout. b1 in space1 (Task 8/10
// moved it there from the implicit space0 -- see MeshNode.cpp's
// CreateBindings, THE REGISTER-SPACE RULE: adding a root SAMPLER for the
// bindless array below meant root items and every ordinary descriptor set
// had to become distinct spaces, where before only root CONSTANTS shared
// space0 with the one set that existed).
cbuffer MeshFrameCB : register(b1, space1)
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
    // Arcane::NormalMatrixFor (a free function in MeshNode.hpp, not a member
    // of MeshNode) computes this once per instance on the CPU
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

// THE BINDLESS MATERIAL TABLE (Task 8/10). A FIXED-size array -- not an
// unbounded/runtime one -- because BindlessTable's capacity is decided once
// at MeshNode creation and never resized (BindlessTable.hpp's SLOT POLICY).
// kMeshBindlessCapacity MUST EQUAL MeshNode.cpp's own kBindlessCapacity
// literal EXACTLY: CreateBindings() there sizes the matching descriptor
// range to it. There is no compiler that checks the two agree, so this
// comment (and its mirror in MeshNode.cpp) is the whole of that contract.
// t0 in space2, matching MeshNode.cpp's bindlessSetDesc (THE REGISTER-SPACE
// RULE, this file's MeshFrameCB comment above).
#define kMeshBindlessCapacity 256
Texture2D<float4> g_BindlessTextures[kMeshBindlessCapacity] : register(t0, space2);

// THE ONE IMMUTABLE SAMPLER (Task 10's slice-one sampler strategy): a ROOT/
// static sampler baked into the pipeline layout (MeshNode.cpp's
// CreateBindings, RootSamplerDesc) rather than a descriptor-set entry -- s0
// stays in the implicit space0, where this shader's OTHER root item (b0)
// already is, which is exactly what THE REGISTER-SPACE RULE requires once a
// layout carries a root sampler.
SamplerState g_Sampler : register(s0);

// BindlessTable::kInvalidSlot (BindlessTable.hpp), restated here because
// HLSL cannot include a C++ header. Keep numerically identical.
#define kMeshInvalidMaterialSlot 0xFFFFFFFFu

float4 ps_main(VSOutput input) : SV_Target0
{
    const float3 n      = normalize(input.normal);
    // NOT normalized here -- see g_lightDirection's comment. The zero vector is
    // the "no directional light" case and dot() handles it; normalize() would
    // not.
    const float  ndotl  = saturate(dot(n, g_lightDirection.xyz));

    // THE MATERIAL SLOT (Task 8/10), unpacked as RAW BITS from
    // normalMatrixCol0.w -- see MeshConstants' own comment (and
    // MeshNode.hpp's MeshInstance::materialSlot) for why this is asuint,
    // never a float read: `kMeshInvalidMaterialSlot` is a NaN bit pattern
    // and only ever safe reinterpreted, not converted.
    const uint slot = asuint(g_normalMatrixCol0.w);

    // kMeshInvalidMaterialSlot selects F2a's ORIGINAL flat path BIT-FOR-
    // BIT: the old code sampled a 1x1 opaque-white texel and multiplied it
    // by g_baseColor, and 1.0 * x is exact under IEEE-754, so using
    // g_baseColor directly is that same result with the now-redundant
    // sample removed, not an approximation of it. Any other slot must have
    // come from THIS pipeline's own MeshNode::AddMaterial. The index is
    // wrapped in NonUniformResourceIndex even though it is uniform across
    // one draw's invocations (a root/push-constant value, not a per-pixel
    // varying): it is still a SHADER-COMPUTED index into the array rather
    // than a compile-time constant, which is the standard bindless-
    // indexing idiom on both backends.
    const float4 albedo = (slot == kMeshInvalidMaterialSlot)
        ? g_baseColor
        : g_BindlessTextures[NonUniformResourceIndex(slot)].Sample(g_Sampler, input.uv) * g_baseColor;

    const float3 lit    = albedo.rgb * (g_ambient.rgb + g_lightColor.rgb * ndotl);
    return float4(lit, albedo.a);
}
