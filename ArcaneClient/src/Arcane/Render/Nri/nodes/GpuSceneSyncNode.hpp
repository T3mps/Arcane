#pragma once

// GpuSceneSyncNode -- the GPU scene's per-frame writer as a graph node (F3,
// spec s8): imports the four persistent buffers, declares them CopyDst,
// and in Record hands GpuScene::Apply the frame's staged rows, this slot's
// args + visible indices + cull batches, and the ad-hoc rows. Transfer only;
// no shader.
// Declared BEFORE the mesh node -- by AddMeshNode itself (plan 1 T6, ruling
// R-A) -- which Reads the same handles (ShaderRead / IndirectArgs), so the
// graph derives the copy -> read barriers.
//
// GROWTH IS SETTLED BEFORE THE IMPORTS: AddGpuSceneSyncNode calls
// GpuScene::Reserve first, so every handle it mints names the buffer the
// frame will actually write and read (GpuScene.hpp's header block).
//
// Include order: NRI headers first, ALWAYS (NriCommon.hpp); NRIDeviceCreation.h
// explicitly because GpuSceneTypes.hpp reaches <windows.h> (GpuScene.hpp).
#include <NRI.h>
#include <Extensions/NRIDeviceCreation.h>

#include <Arcane/Base/Api.hpp>
#include <Arcane/Render/GpuSceneTypes.hpp>
#include <Arcane/Render/Nri/RenderGraph.hpp>

#include <memory>
#include <span>

namespace Arcane
{
    class NriGraphContext;

    struct GpuSceneNodeInputs
    {
        RgBuffer instances{};
        RgBuffer args{};
        RgBuffer visibleIndices{};
        RgBuffer cullBatches{};
        std::shared_ptr<GpuSceneFrameReadiness> readiness;
    };

    // `frame` may be null (no registry-backed scene this frame); `adHoc` may be
    // empty. Both are borrowed for the RenderFrame call, like FrameDesc::pickables.
    // A null context (the device-less declaration-shape drives) imports null
    // buffers, which the executor's barrier walk skips.
    ARCANE_API GpuSceneNodeInputs AddGpuSceneSyncNode(RenderGraph& graph, NriGraphContext* context,
                                                       const GpuSceneFrame* frame,
                                                       std::span<const GpuInstance> adHoc);

    // TEST-ONLY (GpuScene::EnableDebugReadback): a Copy node that reads the
    // imported instances handle into GpuScene's HOST_READBACK buffer. Declared
    // by AddMeshNode after the mesh node, and only when the readback is armed.
    ARCANE_API void AddGpuSceneDebugReadbackNode(RenderGraph& graph, NriGraphContext* context,
                                                 const GpuSceneNodeInputs& inputs);
}
