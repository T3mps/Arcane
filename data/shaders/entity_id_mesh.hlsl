// Entity-id pass, MESH half (F4 spec s7.1): rasterise a resident mesh's
// triangles through mvp and write the drawable's 1-based id. Depth-tested
// (LESS, write) against the pick pass's own D32 transient, AFTER the 2D
// silhouettes drew depth-off -- so meshes resolve among themselves by depth
// and always over sprites, which is the main pass's order reproduced.
//
// The vertex input is MeshVertex (Mesh/MeshBuilder.hpp) verbatim -- the SAME
// resident buffers MeshNode draws, bound by PickNode -- so NORMAL and TEXCOORD
// are declared and unused. The 80-byte root block matches entity_id.hlsl's so
// one pipeline layout serves both pipelines.
//
// MATRIX PACKING: float4x4 is COLUMN-MAJOR (dxc's default on both targets),
// so glm's mat4 uploads verbatim and mul(M, v) is the ordinary M * v.

struct MeshIdConstants
{
    float4x4 mvp;
    uint     id;
    uint     pad0;   // three SCALARS, not a uint3: a vector member would be 16-byte
    uint     pad1;   // aligned under cbuffer/push-constant packing and push the block
    uint     pad2;   // to 96 bytes, past the 80-byte root-constant range on both backends
};

#if SPIRV
[[vk::push_constant]] ConstantBuffer<MeshIdConstants> g_PC;
#define g_mvp g_PC.mvp
#define g_id  g_PC.id
#else
cbuffer MeshIdConstantsCB : register(b0)
{
    MeshIdConstants g_PCData;
}
#define g_mvp g_PCData.mvp
#define g_id  g_PCData.id
#endif

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;      // unused: the input layout is MeshVertex's
    float2 uv       : TEXCOORD;    // unused
};

struct VSOutput
{
    float4 pos : SV_Position;
};

VSOutput vs_main(VSInput input)
{
    VSOutput o;
    o.pos = mul(g_mvp, float4(input.position, 1.0));
    return o;
}

uint ps_main(VSOutput input) : SV_Target0
{
    return g_id;
}
