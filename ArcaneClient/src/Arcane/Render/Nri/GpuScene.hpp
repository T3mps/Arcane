#pragma once

// GpuScene -- the device half of the GPU scene (F3, spec s5): the ONE
// persistent instance buffer (GpuInstance rows, DEVICE memory, doubling
// growth), per-frame-slot indirect-args and visible-index buffers, and a
// SCRATCH row region per slot for registry-less callers (MeshDocument's
// preview, the thumbnail harvester, the [gpu] tests -- MeshSceneDesc::
// instances). Owned by NriGraphContext beside NriMeshBufferCache.
//
// Everything here is written by GpuSceneSyncNode::Record through the upload
// ring + CmdCopyBuffer -- NO compute (the UE vet: prev is CPU history).
//
// GROWTH HAPPENS AT DECLARATION TIME, NOT INSIDE RECORD (plan 1 T6, ruling
// R-B). AddGpuSceneSyncNode calls Reserve() BEFORE it imports the buffers
// into the graph, so every imported handle names the FINAL buffer and the
// graph's copy -> read barriers land on it. Reserve creates the new buffer +
// view, retires the old pair, bumps InstanceBufferGeneration() (MeshNode
// rewrites its per-slot structured view when it changes), and -- unless the
// stage is a full rebuild -- leaves a PENDING GROW-COPY for Apply: the live
// rows are copied old -> new on the command list at record time, behind an
// explicit barrier on the OLD buffer (the graph never saw that buffer; the
// NEW one is already in CopyDst by then). The old buffer stays alive until
// the fence this frame's submit signals retires -- see FlushGraves.
//
// THE BURIAL SEAM. Graveyard::Bury requires NONDECREASING fence values, and
// RenderGraph::Execute buries at DebugSubmitCount() (the LAST submitted
// value) while it runs -- so nothing may bury at "this frame's" value,
// DebugSubmitCount() + 1 (NriGraphContext::CurrentFence), BEFORE Execute
// returns. Reserve therefore PARKS its retirements, stamped with that value,
// and NriGraphContext::FlushGraves-es them right after a successful Execute,
// at which point the stamp equals DebugSubmitCount() and the lane's order
// holds. A failed or skipped Execute leaves them parked for the next one.
//
// Include order: NRI headers first, ALWAYS (NriCommon.hpp) -- and
// NRIDeviceCreation.h explicitly, because GpuSceneTypes.hpp reaches
// <windows.h> (Astra's Base.hpp -> Log.hpp) and its ERROR macro would
// corrupt nri::Message::ERROR in any later include of that header.
#include <NRI.h>
#include <Extensions/NRIDeviceCreation.h>

#include <Arcane/Base/Api.hpp>
#include <Arcane/Render/FramePacing.hpp>      // kSwapchainFramesInFlight
#include <Arcane/Render/GpuSceneTypes.hpp>
#include <Arcane/Render/Nri/RenderGraph.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace Arcane
{
    class Graveyard;
    class NriDevice;

    class ARCANE_API GpuScene
    {
    public:
        static constexpr std::uint32_t kInitialRows = 256;
        static constexpr std::uint32_t kScratchRows = 64;    // per frame slot; ad-hoc instances beyond this are dropped with one WARN

        static std::unique_ptr<GpuScene> Create(NriDevice& device);
        ~GpuScene();
        GpuScene(const GpuScene&)            = delete;
        GpuScene& operator=(const GpuScene&) = delete;

        // DECLARATION time, from AddGpuSceneSyncNode, BEFORE the imports (the
        // header block above). Grows the instance buffer to the frame's row
        // capacity (doubling) and this slot's args / visible-index buffers to
        // the frame's counts; `fence` stamps whatever it retires (see
        // FlushGraves). `adHocCount` is the ad-hoc instance count the record
        // will be handed -- it decides the one-shot scratch-overflow WARN here,
        // where the drop is decided. False (logged) if a buffer could not be
        // created; the previous buffers stay in place, but registry consumers
        // are suppressed for this frame.
        bool Reserve(const GpuSceneFrame* frame, std::size_t adHocCount, std::uint32_t frameSlot,
                     std::uint64_t fence);

        // RECORD time, from GpuSceneSyncNode's exec fn. Issues the pending
        // grow-copy (if Reserve left one), copies the staged rows and the
        // scratch rows through the ring, copies this slot's args + visible
        // indices + cull batches, and stamps the synced generation. The result
        // reports registry and ad-hoc readiness independently. Every registry
        // refusal resets SyncedGeneration() to 0, so the host's next
        // GpuSceneSync sees a generation it never acknowledged and stages a
        // full rebuild: the dropped stage's once-staged rows (a spawn, a
        // material change) have no re-dirty to rescue them.
        GpuSceneApplyResult Apply(const GpuSceneFrame* frame, std::span<const GpuInstance> adHoc,
                                  std::uint32_t frameSlot, RenderGraphNodeContext& ctx);

        // AFTER a successful Execute (NriGraphContext, beside the mesh cache's
        // eviction): buries everything Reserve retired, at the fence values it
        // stamped them with. A no-op when nothing is parked.
        void FlushGraves(Graveyard& graves);

        [[nodiscard]] nri::Buffer*     Instances() const noexcept { return m_instances; }
        [[nodiscard]] nri::Descriptor* InstancesView() const noexcept { return m_instancesView; }   // STRUCTURED_BUFFER, stride 240
        [[nodiscard]] nri::Buffer*     Args(std::uint32_t slot) const noexcept { return m_args[slot]; }
        [[nodiscard]] nri::Descriptor* ArgsStorageView(std::uint32_t slot) const noexcept { return m_argsStorageView[slot]; }
        [[nodiscard]] nri::Buffer*     VisibleIndices(std::uint32_t slot) const noexcept { return m_visible[slot]; }
        [[nodiscard]] nri::Descriptor* VisibleIndicesView(std::uint32_t slot) const noexcept { return m_visibleView[slot]; }   // STRUCTURED_BUFFER, stride 4
        [[nodiscard]] nri::Descriptor* VisibleIndicesStorageView(std::uint32_t slot) const noexcept { return m_visibleStorageView[slot]; }
        [[nodiscard]] nri::Buffer*     CullBatches(std::uint32_t slot) const noexcept { return m_cullBatches[slot]; }
        [[nodiscard]] nri::Descriptor* CullBatchesView(std::uint32_t slot) const noexcept { return m_cullBatchesView[slot]; }
        [[nodiscard]] std::uint32_t    RowCapacity() const noexcept { return m_rowCapacity; }
        [[nodiscard]] std::uint32_t    ScratchFirstRow(std::uint32_t slot) const noexcept { return m_rowCapacity + slot * kScratchRows; }
        [[nodiscard]] std::uint64_t    InstanceBufferGeneration() const noexcept { return m_instanceGeneration; }   // bumps on every grow
        [[nodiscard]] std::uint64_t    SyncedGeneration() const noexcept { return m_syncedGeneration; }   // the mirror generation the last SUCCESSFUL Apply stamped; 0 after a refusal (or never)
        void SetSyncedGeneration(std::uint64_t g) noexcept { m_syncedGeneration = g; }
        [[nodiscard]] std::uint64_t    InstanceBytes() const noexcept;                     // the whole buffer incl. scratch
        [[nodiscard]] std::uint64_t    ArgBytes(std::uint32_t slot) const noexcept;        // the slot's args buffer, whole
        [[nodiscard]] std::uint64_t    VisibleBytes(std::uint32_t slot) const noexcept;    // the slot's visible-index buffer, whole
        [[nodiscard]] std::uint64_t    CullBatchBytes(std::uint32_t slot) const noexcept;

        void Release(Graveyard& graves, std::uint64_t fence);

        // ==================== TEST-ONLY: the instance readback ====================
        // The [gpu][gpuscene] round trip (NriGraphPixelTest.cpp) reads the
        // instance buffer back through these. Off by default and never armed
        // by production code: EnableDebugReadback() makes AddMeshNode declare
        // a "gpuscene-readback" Copy node after the mesh node, which copies the
        // imported instances handle into a HOST_READBACK buffer sized to
        // InstanceBytes() (re-created on growth, the old one retired like any
        // other); ReadDebugInstances idles the device and maps it.
        bool EnableDebugReadback();
        [[nodiscard]] bool DebugReadbackEnabled() const noexcept { return m_debugReadbackEnabled; }
        bool EnsureDebugReadback(std::uint64_t fence);   // declaration time, after Reserve: grow to InstanceBytes()
        [[nodiscard]] nri::Buffer*  DebugReadbackBuffer() const noexcept { return m_debugReadback; }
        [[nodiscard]] std::uint64_t DebugReadbackBytes() const noexcept { return m_debugReadbackBytes; }
        void RecordDebugReadback(RenderGraphNodeContext& ctx, RgBuffer instances, RgBuffer readback);
        bool ReadDebugInstances(std::vector<std::uint8_t>& out);

    private:
        GpuScene() = default;
        bool CreateInstances(std::uint32_t rowCapacity);            // buffer + view for rowCapacity + kScratchRows * frames
        bool CreateSlotBuffers(std::uint32_t slot, std::uint32_t rows, std::uint32_t argCount, std::uint32_t batchCount);
        bool EnsureSlotCapacity(std::uint32_t slot, std::uint32_t rows, std::uint32_t argCount, std::uint32_t batchCount, std::uint64_t fence);
        bool CopyRows(RenderGraphNodeContext& ctx, std::span<const std::uint32_t> rows,
                      std::span<const GpuInstance> values, std::uint32_t firstRowOverride, bool contiguous);
        void Park(std::uint64_t fence, std::function<void()> destroy);

        NriDevice*       m_device = nullptr;
        nri::Buffer*     m_instances = nullptr;
        nri::Descriptor* m_instancesView = nullptr;
        std::uint32_t    m_rowCapacity = 0;
        std::uint64_t    m_instanceGeneration = 1;
        std::uint64_t    m_syncedGeneration = 0;
        nri::Buffer*     m_args[kSwapchainFramesInFlight] = {};
        nri::Descriptor* m_argsStorageView[kSwapchainFramesInFlight] = {};
        std::uint32_t    m_argCapacity[kSwapchainFramesInFlight] = {};
        nri::Buffer*     m_visible[kSwapchainFramesInFlight] = {};
        nri::Descriptor* m_visibleView[kSwapchainFramesInFlight] = {};
        nri::Descriptor* m_visibleStorageView[kSwapchainFramesInFlight] = {};
        std::uint32_t    m_visibleCapacity[kSwapchainFramesInFlight] = {};
        nri::Buffer*     m_cullBatches[kSwapchainFramesInFlight] = {};
        nri::Descriptor* m_cullBatchesView[kSwapchainFramesInFlight] = {};
        std::uint32_t    m_cullBatchCapacity[kSwapchainFramesInFlight] = {};
        bool             m_warnedScratchOverflow = false;

        // The grow-copy Reserve leaves for Apply: the retired buffer holding
        // the live rows, and how many rows it holds. The buffer is parked
        // (below), so it is alive until this frame's fence retires. A second
        // growth before Apply ran keeps the ORIGINAL source: it is the one
        // that still holds the rows.
        struct PendingGrowCopy
        {
            nri::Buffer*  source = nullptr;
            std::uint32_t rows   = 0;
        };
        PendingGrowCopy m_pendingGrowCopy;

        // Retirements stamped with the fence value the frame that retired them
        // will signal -- buried by FlushGraves after that frame's Execute.
        std::vector<std::pair<std::uint64_t, std::function<void()>>> m_parked;

        bool          m_debugReadbackEnabled = false;
        nri::Buffer*  m_debugReadback        = nullptr;
        std::uint64_t m_debugReadbackBytes   = 0;
        bool          m_debugReadbackRecorded = false;
    };

    // The NRI-free host seam (Task 8's Host/GpuSceneHost.hpp re-declares it
    // identically): the mirror generation the device last acknowledged, 0 for
    // no device scene. Exported from ArcaneClient.dll.
    ARCANE_API std::uint64_t GpuSceneSyncedGeneration(const GpuScene* device) noexcept;
}
