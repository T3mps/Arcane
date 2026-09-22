// Include order: NRI first, ALWAYS (NriCommon.hpp explains the ERROR clash).
#include <NRI.h>

#include <Arcane/Render/Nri/GpuScene.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/Nri/BindlessTable.hpp>
#include <Arcane/Render/Nri/Graveyard.hpp>
#include <Arcane/Render/Nri/NriCommon.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/Nri/NriUploadRing.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace Arcane
{
    static_assert(kGpuInvalidMaterialSlot == BindlessTable::kInvalidSlot,
                  "GpuSceneTypes.hpp restates BindlessTable::kInvalidSlot -- keep them equal");
    static_assert(sizeof(DrawIndexedArgs) == sizeof(nri::DrawIndexedDesc), "DrawIndexedArgs mirrors nri::DrawIndexedDesc");
    static_assert(offsetof(DrawIndexedArgs, indexNum)     == offsetof(nri::DrawIndexedDesc, indexNum));
    static_assert(offsetof(DrawIndexedArgs, instanceNum)  == offsetof(nri::DrawIndexedDesc, instanceNum));
    static_assert(offsetof(DrawIndexedArgs, baseIndex)    == offsetof(nri::DrawIndexedDesc, baseIndex));
    static_assert(offsetof(DrawIndexedArgs, baseVertex)   == offsetof(nri::DrawIndexedDesc, baseVertex));
    static_assert(offsetof(DrawIndexedArgs, baseInstance) == offsetof(nri::DrawIndexedDesc, baseInstance));

    namespace
    {
        constexpr std::uint64_t kRowBytes = sizeof(GpuInstance);
        constexpr std::uint64_t kRingAlign = 16;

        // The visibility readback packs the args region and the visible-index
        // region into ONE buffer; the second region starts at this alignment so
        // both copies land on an offset every backend accepts.
        constexpr std::uint64_t kVisibilityRegionAlign = 256;

        [[nodiscard]] constexpr std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment) noexcept
        {
            return ((value + alignment - 1) / alignment) * alignment;
        }

        bool CreateBuffer(NriDevice& device, nri::MemoryLocation location, std::uint64_t bytes,
                          std::uint32_t stride, nri::BufferUsageBits usage, const char* name, nri::Buffer*& out)
        {
            const nri::CoreInterface& core = device.Core();
            nri::BufferDesc desc = {};
            desc.size            = bytes;
            desc.structureStride = stride;
            desc.usage           = usage;
            out = nullptr;
            if (!ARC_NRI_CHECK(core.CreateCommittedBuffer(device.Device(), location, 0.0f, desc, out)) || !out)
            {
                out = nullptr;
                ARC_ERROR("[nri-graph] GpuScene: could not create {} ({} bytes)", name, bytes);
                return false;
            }
            core.SetDebugName(out, name);
            return true;
        }

        bool CreateStructuredView(NriDevice& device, nri::Buffer* buffer, std::uint32_t stride, nri::Descriptor*& out)
        {
            nri::BufferViewDesc view = {};
            view.buffer          = buffer;
            view.type            = nri::BufferView::STRUCTURED_BUFFER;
            view.offset          = 0;
            view.size            = nri::WHOLE_SIZE;
            view.structureStride = stride;
            out = nullptr;
            if (!ARC_NRI_CHECK(device.Core().CreateBufferView(view, out)) || !out)
            {
                out = nullptr;
                ARC_ERROR("[nri-graph] GpuScene: could not create a stride-{} structured view", stride);
                return false;
            }
            return true;
        }

        bool CreateStorageStructuredView(NriDevice& device, nri::Buffer* buffer, std::uint32_t stride, nri::Descriptor*& out)
        {
            nri::BufferViewDesc view = {};
            view.buffer = buffer; view.type = nri::BufferView::STORAGE_STRUCTURED_BUFFER;
            view.offset = 0; view.size = nri::WHOLE_SIZE; view.structureStride = stride;
            out = nullptr;
            if (!ARC_NRI_CHECK(device.Core().CreateBufferView(view, out)) || !out)
            {
                out = nullptr;
                ARC_ERROR("[nri-graph] GpuScene: could not create a stride-{} storage structured view", stride);
                return false;
            }
            return true;
        }
    }

    std::unique_ptr<GpuScene> GpuScene::Create(NriDevice& device)
    {
        std::unique_ptr<GpuScene> s(new GpuScene());
        s->m_device = &device;
        if (!s->CreateInstances(kInitialRows))
            return nullptr;
        for (std::uint32_t slot = 0; slot < kSwapchainFramesInFlight; ++slot)
            if (!s->CreateSlotBuffers(slot, kInitialRows, kScratchRows, kInitialRows))
                return nullptr;   // ~GpuScene destroys what got made
        return s;
    }

    // Release() is THE path (it buries against a fence the owner knows); this
    // is the safety net for a Create() that failed halfway, where nothing has
    // been submitted and a direct destroy is correct.
    GpuScene::~GpuScene()
    {
        if (!m_device)
            return;
        const nri::CoreInterface& core = m_device->Core();
        if (m_instances || m_debugReadback)
            ARC_WARN("[nri-graph] GpuScene destroyed with live NRI objects -- its owner skipped Release()");
        if (m_instancesView) core.DestroyDescriptor(m_instancesView);
        if (m_instances)     core.DestroyBuffer(m_instances);
        for (std::uint32_t s = 0; s < kSwapchainFramesInFlight; ++s)
        {
            if (m_cullBatchesView[s]) core.DestroyDescriptor(m_cullBatchesView[s]);
            if (m_cullBatches[s])     core.DestroyBuffer(m_cullBatches[s]);
            if (m_visibleStorageView[s]) core.DestroyDescriptor(m_visibleStorageView[s]);
            if (m_visibleView[s]) core.DestroyDescriptor(m_visibleView[s]);
            if (m_visible[s])     core.DestroyBuffer(m_visible[s]);
            if (m_argsStorageView[s]) core.DestroyDescriptor(m_argsStorageView[s]);
            if (m_args[s])        core.DestroyBuffer(m_args[s]);
        }
        if (m_debugReadback) core.DestroyBuffer(m_debugReadback);
        // The visibility ring, if it was ever armed: mark it released BEFORE
        // the buffers go, so any publish thunk the graveyard still holds (the
        // ring's state outlives this object by design) does nothing rather
        // than mapping a destroyed buffer.
        if (m_visibility)
        {
            m_visibility->released = true;
            for (VisibilityRing::Slot& s : m_visibility->slots)
            {
                if (s.buffer) core.DestroyBuffer(s.buffer);
                s = {};
            }
        }
        for (auto& parked : m_parked)
            parked.second();
        m_parked.clear();
    }

    bool GpuScene::CreateInstances(std::uint32_t rowCapacity)
    {
        const std::uint64_t rows = std::uint64_t(rowCapacity) + std::uint64_t(kScratchRows) * kSwapchainFramesInFlight;
        nri::Buffer* buffer = nullptr;
        if (!CreateBuffer(*m_device, nri::MemoryLocation::DEVICE, rows * kRowBytes, static_cast<std::uint32_t>(kRowBytes),
                          nri::BufferUsageBits::SHADER_RESOURCE, "gpuscene instances", buffer))
            return false;
        nri::Descriptor* view = nullptr;
        if (!CreateStructuredView(*m_device, buffer, static_cast<std::uint32_t>(kRowBytes), view))
        {
            m_device->Core().DestroyBuffer(buffer);
            return false;
        }
        m_instances     = buffer;
        m_instancesView = view;
        m_rowCapacity   = rowCapacity;
        return true;
    }

    std::uint64_t GpuScene::InstanceBytes() const noexcept
    {
        return (std::uint64_t(m_rowCapacity) + std::uint64_t(kScratchRows) * kSwapchainFramesInFlight) * kRowBytes;
    }

    std::uint64_t GpuScene::ArgBytes(std::uint32_t slot) const noexcept
    {
        return std::uint64_t(m_argCapacity[slot]) * sizeof(DrawIndexedArgs);
    }

    std::uint64_t GpuScene::VisibleBytes(std::uint32_t slot) const noexcept
    {
        return std::uint64_t(m_visibleCapacity[slot]) * sizeof(std::uint32_t);
    }

    std::uint64_t GpuScene::CullBatchBytes(std::uint32_t slot) const noexcept
    {
        return std::uint64_t(m_cullBatchCapacity[slot]) * sizeof(GpuCullBatch);
    }

    bool GpuScene::CreateSlotBuffers(std::uint32_t slot, std::uint32_t rows, std::uint32_t argCount, std::uint32_t batchCount)
    {
        if (!CreateBuffer(*m_device, nri::MemoryLocation::DEVICE, std::uint64_t(argCount) * sizeof(DrawIndexedArgs), sizeof(DrawIndexedArgs),
                          nri::BufferUsageBits::ARGUMENT_BUFFER | nri::BufferUsageBits::SHADER_RESOURCE | nri::BufferUsageBits::SHADER_RESOURCE_STORAGE, "gpuscene args", m_args[slot]))
            return false;
        if (!CreateStorageStructuredView(*m_device, m_args[slot], sizeof(DrawIndexedArgs), m_argsStorageView[slot]))
            return false;
        m_argCapacity[slot] = argCount;
        if (!CreateBuffer(*m_device, nri::MemoryLocation::DEVICE, std::uint64_t(rows) * sizeof(std::uint32_t), sizeof(std::uint32_t),
                          nri::BufferUsageBits::SHADER_RESOURCE | nri::BufferUsageBits::SHADER_RESOURCE_STORAGE, "gpuscene visible", m_visible[slot]))
            return false;
        if (!CreateStructuredView(*m_device, m_visible[slot], sizeof(std::uint32_t), m_visibleView[slot]))
            return false;
        if (!CreateStorageStructuredView(*m_device, m_visible[slot], sizeof(std::uint32_t), m_visibleStorageView[slot]))
            return false;
        m_visibleCapacity[slot] = rows;
        if (!CreateBuffer(*m_device, nri::MemoryLocation::DEVICE, std::uint64_t(batchCount) * sizeof(GpuCullBatch), sizeof(GpuCullBatch),
                          nri::BufferUsageBits::SHADER_RESOURCE, "gpuscene cull batches", m_cullBatches[slot]))
            return false;
        if (!CreateStructuredView(*m_device, m_cullBatches[slot], sizeof(GpuCullBatch), m_cullBatchesView[slot]))
            return false;
        m_cullBatchCapacity[slot] = batchCount;
        return true;
    }

    void GpuScene::Park(std::uint64_t fence, std::function<void()> destroy)
    {
        m_parked.emplace_back(fence, std::move(destroy));
    }

    bool GpuScene::EnsureSlotCapacity(std::uint32_t slot, std::uint32_t rows, std::uint32_t argCount, std::uint32_t batchCount, std::uint64_t fence)
    {
        if (rows <= m_visibleCapacity[slot] && argCount <= m_argCapacity[slot] && batchCount <= m_cullBatchCapacity[slot])
            return true;

        std::uint32_t newRows = std::max(m_visibleCapacity[slot], 1u), newArgs = std::max(m_argCapacity[slot], 1u), newBatches = std::max(m_cullBatchCapacity[slot], 1u);
        while (newRows < rows) newRows *= 2;
        while (newArgs < argCount) newArgs *= 2;
        while (newBatches < batchCount) newBatches *= 2;

        // Build the replacements FIRST, so a refusal leaves the slot exactly as
        // it was (still drawable, still the size the last frame used).
        nri::Buffer* oldArgs = m_args[slot]; nri::Descriptor* oldArgsView = m_argsStorageView[slot];
        nri::Buffer* oldVis = m_visible[slot]; nri::Descriptor* oldView = m_visibleView[slot]; nri::Descriptor* oldVisStorage = m_visibleStorageView[slot];
        nri::Buffer* oldBatches = m_cullBatches[slot]; nri::Descriptor* oldBatchView = m_cullBatchesView[slot];
        const std::uint32_t oldArgCap = m_argCapacity[slot], oldVisCap = m_visibleCapacity[slot], oldBatchCap = m_cullBatchCapacity[slot];
        m_args[slot] = nullptr; m_argsStorageView[slot] = nullptr; m_visible[slot] = nullptr; m_visibleView[slot] = nullptr; m_visibleStorageView[slot] = nullptr; m_cullBatches[slot] = nullptr; m_cullBatchesView[slot] = nullptr;
        if (!CreateSlotBuffers(slot, newRows, newArgs, newBatches))
        {
            const nri::CoreInterface& core = m_device->Core();
            if (m_cullBatchesView[slot]) core.DestroyDescriptor(m_cullBatchesView[slot]);
            if (m_cullBatches[slot]) core.DestroyBuffer(m_cullBatches[slot]);
            if (m_visibleStorageView[slot]) core.DestroyDescriptor(m_visibleStorageView[slot]);
            if (m_visibleView[slot]) core.DestroyDescriptor(m_visibleView[slot]);
            if (m_visible[slot])     core.DestroyBuffer(m_visible[slot]);
            if (m_argsStorageView[slot]) core.DestroyDescriptor(m_argsStorageView[slot]);
            if (m_args[slot])        core.DestroyBuffer(m_args[slot]);
            m_args[slot] = oldArgs; m_argsStorageView[slot] = oldArgsView; m_visible[slot] = oldVis; m_visibleView[slot] = oldView; m_visibleStorageView[slot] = oldVisStorage; m_cullBatches[slot] = oldBatches; m_cullBatchesView[slot] = oldBatchView;
            m_argCapacity[slot] = oldArgCap; m_visibleCapacity[slot] = oldVisCap; m_cullBatchCapacity[slot] = oldBatchCap;
            return false;
        }

        const nri::CoreInterface* core = &m_device->Core();
        Park(fence, [core, oldArgs, oldArgsView, oldVis, oldView, oldVisStorage, oldBatches, oldBatchView]
        {
            if (oldBatchView) core->DestroyDescriptor(oldBatchView);
            if (oldBatches) core->DestroyBuffer(oldBatches);
            if (oldVisStorage) core->DestroyDescriptor(oldVisStorage);
            if (oldView) core->DestroyDescriptor(oldView);
            if (oldVis)  core->DestroyBuffer(oldVis);
            if (oldArgsView) core->DestroyDescriptor(oldArgsView);
            if (oldArgs) core->DestroyBuffer(oldArgs);
        });
        return true;
    }

    bool GpuScene::Reserve(const GpuSceneFrame* frame, std::size_t adHocCount, std::uint32_t frameSlot,
                           std::uint64_t fence)
    {
        const auto refuse = [&]() noexcept
        {
            if (frame)
                m_syncedGeneration = 0;
            return false;
        };
        if (frameSlot >= kSwapchainFramesInFlight)
        {
            ARC_ERROR("[nri-graph] GpuScene::Reserve: frame slot {} is out of range", frameSlot);
            return refuse();
        }

        // 1. The instance buffer: grow to the mirror's high water, doubling.
        //    The new pair replaces the old one HERE, before the graph imports
        //    anything; the old one is parked (alive until this frame's fence
        //    retires) and, unless the stage rewrites every row anyway, Apply
        //    copies its live rows across on the command list.
        //
        //    A FULL RESTAGE DROPS ANY PENDING GROW-COPY, whether or not this
        //    frame grows: a growth frame that never submitted (Skipped) leaves
        //    its copy pending, and issuing it under a full rebuild would be a
        //    second transfer write over every row this frame stages anyway.
        if (frame && frame->stage.fullRebuild)
            m_pendingGrowCopy = {};
        const std::uint32_t needRows = frame ? frame->stage.rowCapacity : 0;
        if (needRows > m_rowCapacity)
        {
            std::uint32_t newCap = std::max(m_rowCapacity, 1u);
            while (newCap < needRows) newCap *= 2;

            nri::Buffer* oldBuf = m_instances; nri::Descriptor* oldView = m_instancesView;
            const std::uint32_t oldCap = m_rowCapacity;
            m_instances = nullptr; m_instancesView = nullptr;
            if (!CreateInstances(newCap))
            {
                m_instances = oldBuf; m_instancesView = oldView; m_rowCapacity = oldCap;   // keep drawing the old one
                return refuse();
            }
            const nri::CoreInterface* core = &m_device->Core();
            Park(fence, [core, oldBuf, oldView]
            {
                if (oldView) core->DestroyDescriptor(oldView);
                if (oldBuf)  core->DestroyBuffer(oldBuf);
            });
            ++m_instanceGeneration;

            if (!frame->stage.fullRebuild && !m_pendingGrowCopy.source)
                m_pendingGrowCopy = { oldBuf, oldCap };        // the rows live in the buffer just retired
            // else: a full rebuild re-stages every live row (cleared above), or
            // a growth before Apply ran -- the ORIGINAL source still holds the
            // rows and stays parked until this frame's fence, so keep it.
        }

        // 2. This slot's args + visible indices, when the frame draws. The
        //    visible list is rowCount entries by BuildGpuSceneFrame's contract;
        //    sized to whichever is larger so Apply's whole-array copy fits.
        if (frame && frame->rowCount != 0)
        {
            const std::uint32_t rows = std::max(frame->rowCount, static_cast<std::uint32_t>(frame->visibleIndices.size()));
            if (!EnsureSlotCapacity(frameSlot, rows, std::max(static_cast<std::uint32_t>(frame->args.size()), 1u),
                                    std::max(static_cast<std::uint32_t>(frame->cullBatches.size()), 1u), fence))
                return refuse();
        }

        // 3. The scratch overflow is decided here and clamped in Apply.
        if (adHocCount > kScratchRows && !m_warnedScratchOverflow)
        {
            m_warnedScratchOverflow = true;
            ARC_WARN("[nri-graph] GpuScene: {} ad-hoc instances exceed the {} scratch rows per frame slot -- the rest are dropped",
                     adHocCount, kScratchRows);
        }
        return true;
    }

    // Copies `values` into rows: either each value to its own `rows[i]`, or --
    // `contiguous` -- the whole span to consecutive rows from firstRowOverride.
    bool GpuScene::CopyRows(RenderGraphNodeContext& ctx, std::span<const std::uint32_t> rows,
                            std::span<const GpuInstance> values, std::uint32_t firstRowOverride, bool contiguous)
    {
        if (values.empty())
            return true;
        if (!contiguous && rows.size() != values.size())
        {
            ARC_ERROR("[nri-graph] GpuScene: a stage with {} rows but {} values", rows.size(), values.size());
            return false;
        }
        const std::uint64_t bytes = values.size() * kRowBytes;
        const NriUploadRing::Alloc a = ctx.ring.Allocate(bytes, kRingAlign);
        if (!a.buffer || !a.cpu)
        {
            ARC_ERROR("[nri-graph] GpuScene: the upload ring refused {} bytes of instance rows", bytes);
            return false;
        }
        std::memcpy(a.cpu, values.data(), bytes);
        if (contiguous)
        {
            ctx.core.CmdCopyBuffer(ctx.cmd, *m_instances, std::uint64_t(firstRowOverride) * kRowBytes, *a.buffer, a.offset, bytes);
            return true;
        }
        const std::uint64_t totalRows = std::uint64_t(m_rowCapacity);
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            if (rows[i] >= totalRows)
            {
                ARC_ERROR("[nri-graph] GpuScene: staged row {} is past the {}-row capacity -- Reserve did not run for this frame",
                          rows[i], totalRows);
                return false;
            }
            ctx.core.CmdCopyBuffer(ctx.cmd, *m_instances, std::uint64_t(rows[i]) * kRowBytes,
                                   *a.buffer, a.offset + i * kRowBytes, kRowBytes);
        }
        return true;
    }

    GpuSceneApplyResult GpuScene::Apply(const GpuSceneFrame* frame, std::span<const GpuInstance> adHoc,
                                        std::uint32_t frameSlot, RenderGraphNodeContext& ctx)
    {
        // EVERY refusal forgets the synced generation (F3 plan 1 review fix,
        // Important #2): the next GpuSceneSync starts by clearing this frame's
        // stage (out.Clear()), so rows staged ONCE -- a spawn, a material
        // change; only moved rows get the re-dirty -- are gone unless that
        // Sync rebuilds, and it rebuilds only when what the device
        // acknowledged differs from the mirror's generation. 0 never matches
        // (GpuSceneMirror::NextGeneration starts at 1), so the next
        // GpuSceneSync stages every live row with prev == model through the
        // machinery that already exists. The early refusals below count too: a
        // refusal anywhere means the stage did not land.
        GpuSceneApplyResult result;
        auto refuseRegistry = [&]() noexcept
        {
            m_syncedGeneration = 0;
            result.registryReady = false;
        };
        if (frameSlot >= kSwapchainFramesInFlight || !m_instances)
        {
            if (frame)
                refuseRegistry();
            return result;
        }
        const nri::CoreInterface& core = ctx.core;

        // 1. The pending grow-copy: the live rows, old -> new, on this command
        //    list. The NEW buffer is the imported one and the graph put it in
        //    CopyDst before this node; the OLD buffer was never a graph
        //    resource this frame, so its transition is recorded HERE -- the one
        //    sanctioned CmdBarrier outside RenderGraphExec.cpp (plan 1 T6,
        //    ruling R-B): its last use was a shader read by a previous frame's
        //    mesh pass, and this copy is the last thing that will ever touch it.
        if (m_pendingGrowCopy.source)
        {
            nri::BufferBarrierDesc toCopySource = {};
            toCopySource.buffer = m_pendingGrowCopy.source;
            toCopySource.before = { nri::AccessBits::SHADER_RESOURCE, nri::StageBits::ALL };
            toCopySource.after  = { nri::AccessBits::COPY_SOURCE, nri::StageBits::COPY };
            nri::BarrierDesc group = {};
            group.buffers   = &toCopySource;
            group.bufferNum = 1;
            core.CmdBarrier(ctx.cmd, group);

            const std::uint64_t bytes = std::min<std::uint64_t>(std::uint64_t(m_pendingGrowCopy.rows), m_rowCapacity) * kRowBytes;
            core.CmdCopyBuffer(ctx.cmd, *m_instances, 0, *m_pendingGrowCopy.source, 0, bytes);
            m_pendingGrowCopy = {};

            // THE WRITE-AFTER-WRITE ORDER on the NEW buffer: the staged rows
            // copied next land INSIDE the range the grow-copy just wrote (an
            // entity that moved in the frame the high water rose is the
            // common case), and two transfer writes to the same bytes carry no
            // ordering of their own -- Vulkan's sync validation reports it as
            // WRITE-AFTER-WRITE. One more barrier at the same sanctioned site,
            // copy -> copy, on the buffer the graph already holds in CopyDst.
            nri::BufferBarrierDesc copyAfterCopy = {};
            copyAfterCopy.buffer = m_instances;
            copyAfterCopy.before = { nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY };
            copyAfterCopy.after  = { nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY };
            nri::BarrierDesc order = {};
            order.buffers   = &copyAfterCopy;
            order.bufferNum = 1;
            core.CmdBarrier(ctx.cmd, order);
        }

        // 2. Scratch rows are independent of the registry stage. Upload them
        // first and retain their result even if the registry data below is
        // refused; preview/ad-hoc drawing must not disappear because a
        // registry frame was malformed or could not reserve its buffers.
        result.adHocReady = adHoc.empty();
        if (!adHoc.empty())
        {
            std::span<const GpuInstance> rows = adHoc;
            if (rows.size() > kScratchRows)
                rows = rows.subspan(0, kScratchRows);   // Reserve warned, once
            result.adHocReady = CopyRows(ctx, {}, rows, ScratchFirstRow(frameSlot), /*contiguous*/ true);
        }

        // No registry frame is a valid ad-hoc-only application. It does not
        // acknowledge or invalidate any mirror generation.
        if (!frame)
            return result;

        // The staged registry rows (dirty or tombstoned this frame).
        if (!CopyRows(ctx, frame->stage.rows, frame->stage.values, 0, /*contiguous*/ false))
        {
            refuseRegistry();
            return result;
        }

        // 3. Zeroed args plus one batch record for every stable key. The cull
        // pass is the sole counter writer; visible indices are deliberately
        // not CPU-populated, even when culling is compiled to its identity path.
        if (frame && frame->rowCount != 0)
        {
            if (frame->args.size() > m_argCapacity[frameSlot] || frame->cullBatches.size() > m_cullBatchCapacity[frameSlot])
            {
                ARC_ERROR("[nri-graph] GpuScene: slot {} buffers are smaller than this frame's args or cull batches -- Reserve did not run",
                          frameSlot);
                refuseRegistry();
                return result;
            }
            const std::uint64_t argBytes = frame->args.size() * sizeof(DrawIndexedArgs);
            const std::uint64_t batchBytes = frame->cullBatches.size() * sizeof(GpuCullBatch);
            const NriUploadRing::Alloc a = argBytes ? ctx.ring.Allocate(argBytes, kRingAlign) : NriUploadRing::Alloc{};
            const NriUploadRing::Alloc b = batchBytes ? ctx.ring.Allocate(batchBytes, kRingAlign) : NriUploadRing::Alloc{};
            if ((argBytes && (!a.buffer || !a.cpu)) || (batchBytes && (!b.buffer || !b.cpu)))
            {
                ARC_ERROR("[nri-graph] GpuScene: the upload ring refused the args/cull-batch arrays ({} + {} bytes)", argBytes, batchBytes);
                refuseRegistry();
                return result;
            }
            if (argBytes)
            {
                std::memcpy(a.cpu, frame->args.data(), argBytes);
                core.CmdCopyBuffer(ctx.cmd, *m_args[frameSlot], 0, *a.buffer, a.offset, argBytes);
            }
            if (batchBytes)
            {
                std::memcpy(b.cpu, frame->cullBatches.data(), batchBytes);
                core.CmdCopyBuffer(ctx.cmd, *m_cullBatches[frameSlot], 0, *b.buffer, b.offset, batchBytes);
            }
        }

        // 4. Acknowledge the mirror this frame wrote (spec s5.2): the next Sync
        //    against a DIFFERENT mirror generation (a swapped registry) rebuilds.
        m_syncedGeneration = frame->stage.generation;
        result.registryReady = true;
        return result;
    }

    void GpuScene::FlushGraves(Graveyard& graves)
    {
        for (auto& parked : m_parked)
            graves.Bury(parked.first, std::move(parked.second));
        m_parked.clear();
    }

    std::uint64_t GpuSceneSyncedGeneration(const GpuScene* device) noexcept
    {
        return device ? device->SyncedGeneration() : 0;   // Host/GpuSceneHost.hpp's NRI-free seam
    }

    void GpuScene::Release(Graveyard& graves, std::uint64_t fence)
    {
        const nri::CoreInterface* core = &m_device->Core();
        nri::Buffer* inst = m_instances; nri::Descriptor* instView = m_instancesView;
        nri::Buffer* readback = m_debugReadback;
        nri::Buffer* args[kSwapchainFramesInFlight]; nri::Descriptor* argsView[kSwapchainFramesInFlight]; nri::Buffer* vis[kSwapchainFramesInFlight]; nri::Descriptor* visView[kSwapchainFramesInFlight]; nri::Descriptor* visStorage[kSwapchainFramesInFlight]; nri::Buffer* batches[kSwapchainFramesInFlight]; nri::Descriptor* batchViews[kSwapchainFramesInFlight];
        for (std::uint32_t s = 0; s < kSwapchainFramesInFlight; ++s) { args[s] = m_args[s]; argsView[s] = m_argsStorageView[s]; vis[s] = m_visible[s]; visView[s] = m_visibleView[s]; visStorage[s] = m_visibleStorageView[s]; batches[s] = m_cullBatches[s]; batchViews[s] = m_cullBatchesView[s]; }
        graves.Bury(fence, [core, inst, instView, readback, args, argsView, vis, visView, visStorage, batches, batchViews]
        {
            if (instView) core->DestroyDescriptor(instView);
            if (inst)     core->DestroyBuffer(inst);
            if (readback) core->DestroyBuffer(readback);
            for (std::uint32_t s = 0; s < kSwapchainFramesInFlight; ++s)
            {
                if (batchViews[s]) core->DestroyDescriptor(batchViews[s]);
                if (batches[s]) core->DestroyBuffer(batches[s]);
                if (visStorage[s]) core->DestroyDescriptor(visStorage[s]);
                if (visView[s]) core->DestroyDescriptor(visView[s]);
                if (vis[s])     core->DestroyBuffer(vis[s]);
                if (argsView[s]) core->DestroyDescriptor(argsView[s]);
                if (args[s])    core->DestroyBuffer(args[s]);
            }
        });
        m_instances = nullptr; m_instancesView = nullptr; m_debugReadback = nullptr; m_debugReadbackBytes = 0;
        m_pendingGrowCopy = {};

        // The visibility ring (the header's contract): RELEASED FIRST, then
        // its buffers buried. A publish thunk still parked or still in the
        // graveyard reads `released` and returns, which is the only thing that
        // keeps it from mapping a buffer this burial destroys -- the thunks
        // below are buried at the SAME fence value, and this one goes first.
        if (m_visibility)
        {
            m_visibility->released = true;
            nri::Buffer* visibility[kSwapchainFramesInFlight];
            for (std::uint32_t s = 0; s < kSwapchainFramesInFlight; ++s)
            {
                visibility[s] = m_visibility->slots[s].buffer;
                m_visibility->slots[s] = {};
            }
            graves.Bury(fence, [core, visibility]
            {
                for (nri::Buffer* b : visibility)
                    if (b) core->DestroyBuffer(b);
            });
        }
        for (std::uint32_t s = 0; s < kSwapchainFramesInFlight; ++s) { m_args[s] = nullptr; m_argsStorageView[s] = nullptr; m_visible[s] = nullptr; m_visibleView[s] = nullptr; m_visibleStorageView[s] = nullptr; m_cullBatches[s] = nullptr; m_cullBatchesView[s] = nullptr; }

        // Anything still parked was retired by a frame that never submitted
        // (its stamp is fence + 1, and nothing in flight names it). Buried AT
        // `fence`, not at the stamp: the owner keeps burying at `fence` after
        // this call (its pipeline cache, its graph), and a higher value here
        // would break the lane's nondecreasing order for them.
        for (auto& parked : m_parked)
            graves.Bury(fence, std::move(parked.second));
        m_parked.clear();
    }

    // ==================== TEST-ONLY: the instance readback ====================

    bool GpuScene::EnableDebugReadback()
    {
        m_debugReadbackEnabled = true;
        return EnsureDebugReadback(0);
    }

    bool GpuScene::EnsureDebugReadback(std::uint64_t fence)
    {
        if (!m_debugReadbackEnabled)
            return false;
        const std::uint64_t need = InstanceBytes();
        if (m_debugReadback && m_debugReadbackBytes >= need)
            return true;
        if (m_debugReadback)
        {
            const nri::CoreInterface* core = &m_device->Core();
            Park(fence, [core, b = m_debugReadback] { core->DestroyBuffer(b); });
            m_debugReadback = nullptr;
            m_debugReadbackBytes = 0;
        }
        if (!CreateBuffer(*m_device, nri::MemoryLocation::HOST_READBACK, need, 0, nri::BufferUsageBits::NONE,
                          "gpuscene debug readback", m_debugReadback))
            return false;
        m_debugReadbackBytes = need;
        m_debugReadbackRecorded = false;
        return true;
    }

    void GpuScene::RecordDebugReadback(RenderGraphNodeContext& ctx, RgBuffer instances, RgBuffer readback)
    {
        nri::Buffer* source = ctx.Resolve(instances);
        nri::Buffer* dest   = ctx.Resolve(readback);
        if (!source || !dest || m_debugReadbackBytes < InstanceBytes())
        {
            ARC_ERROR("[nri-graph] GpuScene: the debug readback node could not resolve its buffers");
            return;
        }
        ctx.core.CmdCopyBuffer(ctx.cmd, *dest, 0, *source, 0, InstanceBytes());
        m_debugReadbackRecorded = true;
    }

    bool GpuScene::ReadDebugInstances(std::vector<std::uint8_t>& out)
    {
        out.clear();
        if (!m_debugReadback || !m_debugReadbackRecorded)
        {
            ARC_ERROR("[nri-graph] GpuScene: no debug readback was recorded -- EnableDebugReadback() before the frame");
            return false;
        }
        const nri::CoreInterface& core = m_device->Core();
        (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&m_device->Device()));   // the copy has to have landed
        const std::uint64_t bytes = InstanceBytes();
        const auto* mapped = static_cast<const std::uint8_t*>(core.MapBuffer(*m_debugReadback, 0, bytes));
        if (!mapped)
        {
            ARC_ERROR("[nri-graph] GpuScene: MapBuffer on the debug readback buffer returned null");
            return false;
        }
        out.assign(mapped, mapped + bytes);
        core.UnmapBuffer(*m_debugReadback);
        return true;
    }

    // ============ OPT-IN: THE DELAYED VISIBILITY READBACK RING ============
    // GpuScene.hpp's own block carries the contract; the comments here are the
    // mechanics that implement it.

    bool GpuScene::EnableVisibilityReadback()
    {
        if (m_visibility)
            return true;
        if (!m_device)
            return false;
        m_visibility = std::make_shared<VisibilityRing>();
        m_visibility->core = &m_device->Core();
        return true;
    }

    nri::Buffer* GpuScene::VisibilityReadbackBuffer(std::uint32_t slot) const noexcept
    {
        return (m_visibility && slot < kSwapchainFramesInFlight) ? m_visibility->slots[slot].buffer : nullptr;
    }

    std::uint64_t GpuScene::VisibilityReadbackBytes(std::uint32_t slot) const noexcept
    {
        return (m_visibility && slot < kSwapchainFramesInFlight) ? m_visibility->slots[slot].bytes : 0;
    }

    const GpuVisibilityReadback* GpuScene::LatestVisibility() const noexcept
    {
        if (!m_visibility || m_visibility->latest.publishCount == 0)
            return nullptr;   // nothing has completed -- NEVER a CPU count standing in for one
        return &m_visibility->latest;
    }

    // THE ONE GUARD (GpuScene.hpp's ring block, "one order"). Seqs come from
    // the one counter in declaration order, so "newer than the mark" IS "a
    // later frame than anything published": the check that keeps `latest`
    // monotonic across slots and paths by construction.
    bool GpuScene::ClaimVisibilitySeq(VisibilityRing& ring, std::uint64_t seq) noexcept
    {
        if (seq == 0 || seq <= ring.publishedMaxSeq)
            return false;   // nothing, already claimed, or OLDER than a claimed one
        ring.publishedMaxSeq = seq;
        return true;
    }

    // THE PUBLISHER OF RECORDED COPIES (GpuScene.hpp's ring block): the
    // slot-reuse path and the graveyard path both come through here, and the
    // claim is what lets them coexist without double-counting a frame.
    void GpuScene::PublishVisibility(VisibilityRing& ring, std::uint64_t seq, std::uint64_t fence,
                                     nri::Buffer* buffer, std::uint64_t argRegion,
                                     std::uint32_t argCount, std::uint32_t visibleCount)
    {
        if (ring.released || !ring.core || !buffer)
            return;
        // CLAIMED BEFORE THE READ. Either path may have got here first, and an
        // older frame may arrive after a newer one has published; both are
        // refused here. And a claim that then fails to map below is still a
        // consumed frame: the seq is spent, so no later path retries the map
        // against a buffer a growth may since have parked for destruction.
        if (!ClaimVisibilitySeq(ring, seq))
            return;
        const std::uint64_t bytes = argRegion + std::uint64_t(visibleCount) * sizeof(std::uint32_t);
        const auto* mapped = static_cast<const std::uint8_t*>(ring.core->MapBuffer(*buffer, 0, bytes));
        if (!mapped)
        {
            ARC_ERROR("[nri-graph] GpuScene: MapBuffer on the visibility readback buffer returned null -- "
                      "this frame's visibility result is dropped (the last published one stands)");
            return;
        }
        // No allocation in steady state: EnsureVisibilityReadback reserves both
        // vectors at declaration time, which is what keeps the slot-reuse path
        // (a record callback) free of one.
        ring.latest.args.resize(argCount);
        if (argCount)
            std::memcpy(ring.latest.args.data(), mapped, std::size_t(argCount) * sizeof(DrawIndexedArgs));
        ring.latest.visibleIndices.resize(visibleCount);
        if (visibleCount)
            std::memcpy(ring.latest.visibleIndices.data(), mapped + argRegion,
                        std::size_t(visibleCount) * sizeof(std::uint32_t));
        ring.core->UnmapBuffer(*buffer);
        ring.latest.fence = fence;
        ++ring.latest.publishCount;
    }

    // THE BATCH-LESS FRAME'S PUBLICATION (GpuScene.hpp's ring block): the
    // frame's seq is minted here, at declaration, exactly where a recorded
    // frame's would be, and claimed through the same guard -- so it orders
    // after every earlier frame and before every later one, and an earlier
    // frame's copy landing after this cannot overwrite it. Nothing is mapped:
    // clear() keeps the vectors' capacity, so this allocates nothing either.
    void GpuScene::PublishEmptyVisibility(std::uint64_t fence)
    {
        if (!m_visibility || m_visibility->released)
            return;
        VisibilityRing& ring = *m_visibility;
        if (!ClaimVisibilitySeq(ring, ++m_visibilitySeq))
            return;   // unreachable while the counter is the only seq source; kept so the guard stays the one rule
        ring.latest.args.clear();
        ring.latest.visibleIndices.clear();
        ring.latest.fence = fence;
        ++ring.latest.publishCount;
    }

    // DECLARATION TIME (AddGpuSceneVisibilityReadbackNode), after Reserve has
    // settled this slot's args / visible-index buffers -- the same rule the
    // whole file follows: size and grow here, never inside a record callback.
    bool GpuScene::EnsureVisibilityReadback(std::uint32_t slot, std::uint32_t argCount,
                                            std::uint32_t visibleCount, std::uint64_t fence)
    {
        if (!m_visibility || slot >= kSwapchainFramesInFlight)
        {
            ARC_ERROR("[nri-graph] GpuScene: EnsureVisibilityReadback on an unarmed ring or frame slot {} -- refused", slot);
            return false;
        }
        VisibilityRing::Slot& s = m_visibility->slots[slot];

        // The copy reads the slot's DEVICE buffers, which Reserve sized for
        // this very frame -- so a declaration asking for more than they hold,
        // or for nothing at all, is a caller's contract broken, not a frame to
        // clamp quietly: a truncated copy would publish a count missing whole
        // batches under this frame's name. Refused, loudly, and the frame then
        // publishes nothing (the last real result stands). The batch-less
        // frame never reaches here: AddGpuSceneVisibilityReadbackNode routes
        // it to PublishEmptyVisibility instead.
        const std::uint64_t argBytes     = std::uint64_t(argCount) * sizeof(DrawIndexedArgs);
        const std::uint64_t visibleBytes = std::uint64_t(visibleCount) * sizeof(std::uint32_t);
        if (argBytes == 0 || visibleBytes == 0)
        {
            ARC_ERROR("[nri-graph] GpuScene: a visibility readback declared for {} args and {} rows -- nothing to copy; "
                      "a batch-less frame publishes an empty result instead (AddGpuSceneVisibilityReadbackNode)",
                      argCount, visibleCount);
            return false;
        }
        if (argBytes > ArgBytes(slot) || visibleBytes > VisibleBytes(slot))
        {
            if (!m_warnedVisibilityClamp)
            {
                m_warnedVisibilityClamp = true;
                ARC_WARN("[nri-graph] GpuScene: the visibility readback asked for {} arg bytes and {} visible-index bytes "
                         "but frame slot {} holds {} and {} -- Reserve did not size this frame's buffers; the readback "
                         "is refused rather than truncated (reported once)",
                         argBytes, visibleBytes, slot, ArgBytes(slot), VisibleBytes(slot));
            }
            return false;
        }
        const std::uint64_t argRegion = AlignUp(argBytes, kVisibilityRegionAlign);
        const std::uint64_t need      = argRegion + visibleBytes;

        if (!s.buffer || s.bytes < need)
        {
            if (s.buffer)
            {
                const nri::CoreInterface* core = &m_device->Core();
                Park(fence, [core, b = s.buffer] { core->DestroyBuffer(b); });
                s.buffer = nullptr;
                s.bytes  = 0;
            }
            nri::Buffer* buffer = nullptr;
            if (!CreateBuffer(*m_device, nri::MemoryLocation::HOST_READBACK, need, 0, nri::BufferUsageBits::NONE,
                              "gpuscene visibility readback", buffer))
                return false;   // logged; the slot stays unarmed for this frame
            s.buffer = buffer;
            s.bytes  = need;
        }
        s.argBytes      = argBytes;
        s.argRegion     = argRegion;
        s.visibleBytes  = visibleBytes;
        s.declaredSeq   = ++m_visibilitySeq;
        s.declaredFence = fence;

        // The publisher's only allocation, hoisted out of the record callback
        // that the slot-reuse path publishes from (reserve never shrinks, so
        // after the first frames the capacity covers every count seen).
        const std::uint32_t declaredArgs    = static_cast<std::uint32_t>(argBytes / sizeof(DrawIndexedArgs));
        const std::uint32_t declaredVisible = static_cast<std::uint32_t>(visibleBytes / sizeof(std::uint32_t));
        m_visibility->latest.args.reserve(declaredArgs);
        m_visibility->latest.visibleIndices.reserve(declaredVisible);

        // PUBLICATION PATH 2, PARKED (GpuScene.hpp's ring block and the burial
        // seam): stamped with the value THIS frame's submit will signal, buried
        // by FlushGraves right after that submit, and run by a LATER frame's
        // Reap -- so it reads the buffer only once the frame that filled it has
        // retired. No wait, no poll, no per-frame flush. This is the path that
        // publishes the frames whose slot is never reused (the tail of a run);
        // under load on the present path, path 1 has usually got there first.
        //
        // Everything it needs is CAPTURED rather than re-read from the ring: a
        // growth between now and then replaces `s.buffer`, and this thunk still
        // owes its answer from the buffer the copy it belongs to actually wrote
        // (which stays alive until the same fence, parked just above).
        Park(fence, [ring = m_visibility, slot, seq = s.declaredSeq, fence, buffer = s.buffer,
                     argRegion, argCount = declaredArgs, visibleCount = declaredVisible]
        {
            VisibilityRing& r = *ring;
            if (r.released)
                return;   // the scene was released; the buffer is gone or going
            // NOT this declaration's copy any more: either the frame never
            // recorded one (a refused frame), or the slot has been re-recorded
            // since -- and re-recording publishes what it replaces, so there is
            // nothing left here to do. The seq claim inside PublishVisibility
            // is the second half of that promise.
            if (r.slots[slot].recordedSeq != seq)
                return;
            // THE ONE CASE THIS STAMP CANNOT SEE: an Execute that recorded the
            // copy and then FAILED before submitting. The publish then reads
            // whatever the buffer last held (that slot's previous frame),
            // because nothing on the CPU can tell a recorded copy from an
            // executed one. Accepted rather than plumbed around: a failed
            // Execute is already a latched render error, which is a louder
            // fact than a one-frame-stale observability count.
            PublishVisibility(r, seq, fence, buffer, argRegion, argCount, visibleCount);
        });
        return true;
    }

    // RECORD TIME: two copies onto this frame's command list, args then visible
    // indices, into the one buffer the declaration sized. Stamping
    // `recordedSeq` is what tells the parked thunk this frame's copy is real.
    void GpuScene::RecordVisibilityReadback(RenderGraphNodeContext& ctx, RgBuffer args, RgBuffer visibleIndices,
                                            std::uint32_t slot)
    {
        if (!m_visibility || slot >= kSwapchainFramesInFlight)
            return;
        VisibilityRing::Slot& s = m_visibility->slots[slot];

        // PUBLICATION PATH 1 -- THE SLOT-REUSE PUBLISH, and the reason the ring
        // does not depend on reap timing (GpuScene.hpp's ring block states the
        // present-path race this closes). Whatever this slot recorded last and
        // has not published yet is read out HERE, before the copy below
        // overwrites the buffer and the stamp below invalidates its thunk.
        //
        // IT ADDS NO WAIT. Reaching this callback means the executor is
        // recording into this frame slot, which it may only do once the frame
        // that last used the slot has retired -- the offscreen path waits for
        // that before declaration, the present path inside
        // AcquireNextTexture -- so the pending copy is complete by
        // construction. This frame's own copy is merely being RECORDED; it
        // cannot have touched the buffer yet.
        if (s.pending.seq != 0)
        {
            PublishVisibility(*m_visibility, s.pending.seq, s.pending.fence, s.pending.buffer,
                              s.pending.argRegion, s.pending.argCount, s.pending.visibleCount);
            s.pending = {};
        }

        nri::Buffer* sourceArgs = ctx.Resolve(args);
        nri::Buffer* sourceVis  = ctx.Resolve(visibleIndices);
        if (!s.buffer || !sourceArgs || !sourceVis || s.bytes < s.argRegion + s.visibleBytes)
        {
            ARC_ERROR("[nri-graph] GpuScene: the visibility readback node could not resolve its buffers");
            return;
        }
        ctx.core.CmdCopyBuffer(ctx.cmd, *s.buffer, 0, *sourceArgs, 0, s.argBytes);
        ctx.core.CmdCopyBuffer(ctx.cmd, *s.buffer, s.argRegion, *sourceVis, 0, s.visibleBytes);
        s.recordedSeq = s.declaredSeq;
        // SNAPSHOTTED, not re-read later: by the time this copy is published,
        // `s` describes the NEXT declaration (possibly a different size, or a
        // grown buffer). The buffer stays alive until this frame's fence at the
        // earliest, and a growth parks its destroy behind that -- so the next
        // record on this slot can always still read it.
        s.pending = VisibilityRing::Pending{
            s.buffer, s.argRegion,
            static_cast<std::uint32_t>(s.argBytes / sizeof(DrawIndexedArgs)),
            static_cast<std::uint32_t>(s.visibleBytes / sizeof(std::uint32_t)),
            s.declaredFence, s.declaredSeq,
        };
    }

    bool GpuSceneArmVisibilityReadback(GpuScene* device) noexcept
    {
        return device && device->EnableVisibilityReadback();
    }

    std::optional<std::uint32_t> GpuSceneVisibleRows(const GpuScene* device) noexcept
    {
        const GpuVisibilityReadback* latest = device ? device->LatestVisibility() : nullptr;
        if (!latest)
            return std::nullopt;   // unarmed, or nothing has completed yet
        return latest->VisibleRows();
    }
}
