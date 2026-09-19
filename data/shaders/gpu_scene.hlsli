// The GPU scene row (F3, spec s5.1). MIRRORS Arcane::GpuInstance
// (ArcaneClient/src/Arcane/Render/GpuSceneTypes.hpp) field for field: 240
// bytes -- change both or neither. Shared by mesh.hlsl and (plan 2)
// mesh_cull.hlsl.
//
// LAYOUT: read through a StructuredBuffer<GpuInstance>, whose element layout
// is the C++ struct's natural layout -- every float4x4 / float4 is 16-byte
// aligned and the four trailing uints pack to one 16-byte slot, so the 240
// bytes land field-for-field with no cbuffer-style padding rules involved.
// The matrices are COLUMN-MAJOR (dxc's default for both DXIL and SPIR-V,
// and glm::mat4's memory layout), same as mesh.hlsl's own MATRIX PACKING note.
#ifndef ARCANE_GPU_SCENE_HLSLI
#define ARCANE_GPU_SCENE_HLSLI

struct GpuInstance
{
    float4x4 model;
    float4x4 prevModel;
    float4   normal0;        // xyz: NormalMatrixFor(model) columns (CPU-computed, R8)
    float4   normal1;
    float4   normal2;
    float4   boundsMin;      // world AABB; w unused
    float4   boundsMax;      // w = alphaCutoff for masked rows (plan 2)
    float4   baseColor;
    uint     materialSlot;   // kMeshInvalidMaterialSlot = the flat path
    uint     batch;
    uint     flags;
    uint     pad;
};

#define kGpuInstanceFlagTeleported 1u

#endif
