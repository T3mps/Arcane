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
    }

    std::unique_ptr<GpuScene> GpuScene::Create(NriDevice& device)
    {
        std::unique_ptr<GpuScene> s(new GpuScene());
        s->m_device = &device;
        if (!s->CreateInstances(kInitialRows))
            return nullptr;
        for (std::uint32_t slot = 0; slot < kSwapchainFramesInFlight; ++slot)
            if (!s->CreateSlotBuffers(slot, kInitialRows, kScratchRows))
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
            if (m_visibleView[s]) core.DestroyDescriptor(m_visibleView[s]);
            if (m_visible[s])     core.DestroyBuffer(m_visible[s]);
            if (m_args[s])        core.DestroyBuffer(m_args[s]);
        }
        if (m_debugReadback) core.DestroyBuffer(m_debugReadback);
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

    bool GpuScene::CreateSlotBuffers(std::uint32_t slot, std::uint32_t rows, std::uint32_t argCount)
    {
        if (!CreateBuffer(*m_device, nri::MemoryLocation::DEVICE, std::uint64_t(argCount) * sizeof(DrawIndexedArgs), 0,
                          nri::BufferUsageBits::ARGUMENT_BUFFER, "gpuscene args", m_args[slot]))
            return false;
        m_argCapacity[slot] = argCount;
        if (!CreateBuffer(*m_device, nri::MemoryLocation::DEVICE, std::uint64_t(rows) * sizeof(std::uint32_t), sizeof(std::uint32_t),
                          nri::BufferUsageBits::SHADER_RESOURCE, "gpuscene visible", m_visible[slot]))
            return false;
        if (!CreateStructuredView(*m_device, m_visible[slot], sizeof(std::uint32_t), m_visibleView[slot]))
            return false;
        m_visibleCapacity[slot] = rows;
        return true;
    }

    void GpuScene::Park(std::uint64_t fence, std::function<void()> destroy)
    {
        m_parked.emplace_back(fence, std::move(destroy));
    }

    bool GpuScene::EnsureSlotCapacity(std::uint32_t slot, std::uint32_t rows, std::uint32_t argCount, std::uint64_t fence)
    {
        if (rows <= m_visibleCapacity[slot] && argCount <= m_argCapacity[slot])
            return true;

        std::uint32_t newRows = std::max(m_visibleCapacity[slot], 1u), newArgs = std::max(m_argCapacity[slot], 1u);
        while (newRows < rows) newRows *= 2;
        while (newArgs < argCount) newArgs *= 2;

        // Build the replacements FIRST, so a refusal leaves the slot exactly as
        // it was (still drawable, still the size the last frame used).
        nri::Buffer* oldArgs = m_args[slot]; nri::Buffer* oldVis = m_visible[slot]; nri::Descriptor* oldView = m_visibleView[slot];
        const std::uint32_t oldArgCap = m_argCapacity[slot], oldVisCap = m_visibleCapacity[slot];
        m_args[slot] = nullptr; m_visible[slot] = nullptr; m_visibleView[slot] = nullptr;
        if (!CreateSlotBuffers(slot, newRows, newArgs))
        {
            const nri::CoreInterface& core = m_device->Core();
            if (m_visibleView[slot]) core.DestroyDescriptor(m_visibleView[slot]);
            if (m_visible[slot])     core.DestroyBuffer(m_visible[slot]);
            if (m_args[slot])        core.DestroyBuffer(m_args[slot]);
            m_args[slot] = oldArgs; m_visible[slot] = oldVis; m_visibleView[slot] = oldView;
            m_argCapacity[slot] = oldArgCap; m_visibleCapacity[slot] = oldVisCap;
            return false;
        }

        const nri::CoreInterface* core = &m_device->Core();
        Park(fence, [core, oldArgs, oldVis, oldView]
        {
            if (oldView) core->DestroyDescriptor(oldView);
            if (oldVis)  core->DestroyBuffer(oldVis);
            if (oldArgs) core->DestroyBuffer(oldArgs);
        });
        return true;
    }

    bool GpuScene::Reserve(const GpuSceneFrame* frame, std::size_t adHocCount, std::uint32_t frameSlot,
                           std::uint64_t fence)
    {
        if (frameSlot >= kSwapchainFramesInFlight)
        {
            ARC_ERROR("[nri-graph] GpuScene::Reserve: frame slot {} is out of range", frameSlot);
            return false;
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
                return false;
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
        if (frame && frame->HasDraws())
        {
            const std::uint32_t rows = std::max(frame->rowCount, static_cast<std::uint32_t>(frame->visibleIndices.size()));
            if (!EnsureSlotCapacity(frameSlot, rows, static_cast<std::uint32_t>(frame->args.size()), fence))
                return false;
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

    bool GpuScene::Apply(const GpuSceneFrame* frame, std::span<const GpuInstance> adHoc, std::uint32_t frameSlot,
                         RenderGraphNodeContext& ctx)
    {
        if (frameSlot >= kSwapchainFramesInFlight || !m_instances)
            return false;
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

        // 2. The staged rows (dirty this frame), then the scratch rows for this slot.
        if (frame && !CopyRows(ctx, frame->stage.rows, frame->stage.values, 0, /*contiguous*/ false))
            return false;
        if (!adHoc.empty())
        {
            std::span<const GpuInstance> rows = adHoc;
            if (rows.size() > kScratchRows)
                rows = rows.subspan(0, kScratchRows);   // Reserve warned, once
            if (!CopyRows(ctx, {}, rows, ScratchFirstRow(frameSlot), /*contiguous*/ true))
                return false;
        }

        // 3. This slot's indirect args and visible indices, whole arrays.
        if (frame && frame->HasDraws())
        {
            if (frame->args.size() > m_argCapacity[frameSlot] || frame->visibleIndices.size() > m_visibleCapacity[frameSlot])
            {
                ARC_ERROR("[nri-graph] GpuScene: the slot {} buffers ({} args / {} rows) are smaller than the frame "
                          "({} / {}) -- Reserve did not run for this frame",
                          frameSlot, m_argCapacity[frameSlot], m_visibleCapacity[frameSlot],
                          frame->args.size(), frame->visibleIndices.size());
                return false;
            }
            const std::uint64_t argBytes = frame->args.size() * sizeof(DrawIndexedArgs);
            const std::uint64_t visBytes = frame->visibleIndices.size() * sizeof(std::uint32_t);
            const NriUploadRing::Alloc a = ctx.ring.Allocate(argBytes, kRingAlign);
            const NriUploadRing::Alloc v = visBytes ? ctx.ring.Allocate(visBytes, kRingAlign) : NriUploadRing::Alloc{};
            if (!a.buffer || !a.cpu || (visBytes && (!v.buffer || !v.cpu)))
            {
                ARC_ERROR("[nri-graph] GpuScene: the upload ring refused the args/visible arrays ({} + {} bytes)", argBytes, visBytes);
                return false;
            }
            std::memcpy(a.cpu, frame->args.data(), argBytes);
            core.CmdCopyBuffer(ctx.cmd, *m_args[frameSlot], 0, *a.buffer, a.offset, argBytes);
            if (visBytes)
            {
                std::memcpy(v.cpu, frame->visibleIndices.data(), visBytes);
                core.CmdCopyBuffer(ctx.cmd, *m_visible[frameSlot], 0, *v.buffer, v.offset, visBytes);
            }
        }

        // 4. Acknowledge the mirror this frame wrote (spec s5.2): the next Sync
        //    against a DIFFERENT mirror generation (a swapped registry) rebuilds.
        if (frame)
            m_syncedGeneration = frame->stage.generation;
        return true;
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
        nri::Buffer* args[kSwapchainFramesInFlight]; nri::Buffer* vis[kSwapchainFramesInFlight]; nri::Descriptor* visView[kSwapchainFramesInFlight];
        for (std::uint32_t s = 0; s < kSwapchainFramesInFlight; ++s) { args[s] = m_args[s]; vis[s] = m_visible[s]; visView[s] = m_visibleView[s]; }
        graves.Bury(fence, [core, inst, instView, readback, args, vis, visView]
        {
            if (instView) core->DestroyDescriptor(instView);
            if (inst)     core->DestroyBuffer(inst);
            if (readback) core->DestroyBuffer(readback);
            for (std::uint32_t s = 0; s < kSwapchainFramesInFlight; ++s)
            {
                if (visView[s]) core->DestroyDescriptor(visView[s]);
                if (vis[s])     core->DestroyBuffer(vis[s]);
                if (args[s])    core->DestroyBuffer(args[s]);
            }
        });
        m_instances = nullptr; m_instancesView = nullptr; m_debugReadback = nullptr; m_debugReadbackBytes = 0;
        m_pendingGrowCopy = {};
        for (std::uint32_t s = 0; s < kSwapchainFramesInFlight; ++s) { m_args[s] = nullptr; m_visible[s] = nullptr; m_visibleView[s] = nullptr; }

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
}
