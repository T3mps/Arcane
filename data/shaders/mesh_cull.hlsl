// Fine GPU frustum cull. GpuInstance is shared verbatim with mesh.hlsl.
#include "gpu_scene.hlsli"

struct GpuCullBatch
{
    uint firstOutput;
    uint capacity;
    uint argIndex;
    uint emitted;
};

// Matches Arcane::DrawIndexedArgs / nri::DrawIndexedDesc (20 bytes).
struct DrawIndexedArgs
{
    uint indexNum;
    uint instanceNum;
    uint baseIndex;
    int  baseVertex;
    uint baseInstance;
};

struct MeshCullConstants
{
    uint rowCount;
    uint batchCount;
    uint cullEnabled;
    uint pad;
    float4 planes[6];
};

#if SPIRV
[[vk::push_constant]] ConstantBuffer<MeshCullConstants> g_Cull;
#else
cbuffer MeshCullCB : register(b0)
{
    MeshCullConstants g_Cull;
};
#endif

StructuredBuffer<GpuInstance> instances : register(t0, space1);
StructuredBuffer<GpuCullBatch> batches : register(t1, space1);
RWStructuredBuffer<uint> visibleIndices : register(u0, space1);
RWStructuredBuffer<DrawIndexedArgs> args : register(u1, space1);

bool Contains(in GpuInstance instance)
{
    [unroll]
    for (uint i = 0; i < 6; ++i)
    {
        const float3 n = g_Cull.planes[i].xyz;
        const float3 vertex = float3(n.x >= 0.0 ? instance.boundsMax.x : instance.boundsMin.x,
                                     n.y >= 0.0 ? instance.boundsMax.y : instance.boundsMin.y,
                                     n.z >= 0.0 ? instance.boundsMax.z : instance.boundsMin.z);
        if (dot(n, vertex) + g_Cull.planes[i].w < 0.0)
            return false;
    }
    return true;
}

[numthreads(64, 1, 1)]
void cs_main(uint id : SV_DispatchThreadID)
{
    if (id >= g_Cull.rowCount)
        return;
    const GpuInstance instance = instances[id];
    if ((instance.flags & kGpuInstanceFlagLive) == 0 || instance.batch >= g_Cull.batchCount)
        return;
    const GpuCullBatch batch = batches[instance.batch];
    if (batch.emitted == 0)
        return;
    if (g_Cull.cullEnabled != 0 && !Contains(instance))
        return;
    uint oldCount;
    InterlockedAdd(args[batch.argIndex].instanceNum, 1, oldCount);
    if (oldCount < batch.capacity)
        visibleIndices[batch.firstOutput + oldCount] = id;
}
