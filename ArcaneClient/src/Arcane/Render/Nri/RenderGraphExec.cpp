// RenderGraph, EXECUTION half (Phase 2, Task 6). The declaration and
// compile halves live in RenderGraph.cpp and are PURE -- see RenderGraph.hpp's
// header for why the class is split across two TUs and why it must stay that
// way.
//
// What this TU owns: every GPU resource the graph holds (the transient pool,
// the cached attachment views, one command allocator + command buffer per
// frame slot, and the graph's own submission fence), the translation of
// RgCompiled into recorded NRI commands, and present sequencing.
//
// THE ONE CmdBarrier CALL SITE. Every barrier on the graph path is emitted by
// EmitBarriers() below, from an RgBarrier list Compile() derived. Nothing in
// this file synthesizes, reorders, drops or coalesces a barrier, and no other
// file on the graph path may call CmdBarrier at all (spec ratification 1; the
// remaining hand-written ones live in NriSmoke.cpp, which Task 13 deletes).
// The `before` triples Compile() hands over already account for pool-slot
// handover between transients, so replaying them verbatim is not laziness --
// re-deriving anything here would silently diverge from the derivation Task
// 4's tests pin.
//
// Include order: NRI headers first, ALWAYS (see NriCommon.hpp). NriDevice.hpp
// pulls in Extensions/NRIDeviceCreation.h, which declares nri::Message::ERROR;
// <windows.h> reaches this TU through Arcane/Base/Log.hpp -> spdlog and
// #defines ERROR via wingdi.h, corrupting every later textual "ERROR" in the
// TU if it lands first.
#include "RenderGraph.hpp"

#include "NriCommon.hpp"
#include "NriDevice.hpp"
#include "NriSwapChain.hpp"

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/GpuBreadcrumbs.hpp>
#include <Arcane/Render/GpuInstrumentation.hpp>
#include <Arcane/Render/IGpuCrashBackend.hpp>
#include <Arcane/Render/NvrhiMessageCallback.hpp>
#include <Arcane/Render/Swapchain.hpp>   // kSwapchainFramesInFlight

#undef ERROR

#include <string>
#include <string_view>
#include <utility>

namespace Arcane
{
    namespace
    {
        // Task 1's tagged error seam. "nri-graph" rather than "nri": these
        // are the GRAPH's own refusals (a caller contract broken, a compile
        // that does not match the declarations), not an NRI call's result --
        // those go through ARC_NRI_CHECK, which tags "[nri]" and routes a
        // typed DEVICE_LOST into the device-removed chain (NriCommon.cpp).
        // Both land in the same RenderErrorCount() latch the 0/0 gate reads.
        void GraphError(const std::string& text)
        {
            NvrhiMessageCallback::Instance().NoteError("nri-graph", text.c_str());
        }

        // ---------------------------------------------------------------
        // NodeScope -- one graph node, both marker channels.
        // ---------------------------------------------------------------
        // The NRI-side twin of Render/GpuInstrumentation.hpp's GpuPassScope,
        // deliberately the same SHAPE (F-2c-bis is binding here too: markers-
        // only DRED with no annotation markers yields an EMPTY breadcrumb
        // list, strictly worse than no DRED at all) with each of its two
        // channels re-plumbed onto NRI:
        //
        //   nvrhi beginMarker/endMarker  ->  CmdBeginAnnotation/CmdEndAnnotation
        //   backend->WriteMarker(list)   ->  backend->WriteMarkerNative(native)
        //                                    with the native list from
        //                                    GetCommandBufferNativeObject
        //
        // It is not a GpuPassScope subclass or a template over the two: the
        // classes share no member and no branch once the two calls differ.
        //
        // The crash backend is the SAME process-wide one the NVRHI passes
        // write into (installed by the device layer over the nvrhi device),
        // and its marker buffer lives on the same native device NRI wrapped
        // -- so both producers' markers replay out of one buffer into one
        // report. Every step degrades independently: no backend, an unarmed
        // backend, or a backend that hands back no native command list each
        // disable exactly their own channel and nothing else.
        class NodeScope
        {
        public:
            NodeScope(const nri::CoreInterface& core, nri::CommandBuffer& cmd, const std::string& label) noexcept
                : m_core(&core), m_cmd(&cmd)
            {
                // The annotation goes out even with no crash backend
                // installed: it is what a PIX/RenderDoc capture and D3D12
                // DRED's markers-only tier read, and DRED enablement is
                // process-global and independent of whether a backend object
                // exists.
                m_core->CmdBeginAnnotation(*m_cmd, label.c_str(), nri::BGRA_UNUSED);
                m_annotated = true;

                // Latched, not re-read in the destructor: a teardown that
                // cleared the slot mid-scope must not leave a BeginScope
                // without its EndScope, nor hand the end marker to a
                // different backend than the begin marker. Same rule as
                // GpuPassScope's.
                m_backend = ActiveGpuCrashBackend();
                if (!m_backend)
                    return;

                m_token  = m_backend->Breadcrumbs().BeginScope(label);
                m_scoped = true;

                // Null on the NONE backend by construction (ImplNONE returns
                // nullptr) and on any backend that has no native list to
                // give -- both are ordinary, not errors.
                m_native = m_core->GetCommandBufferNativeObject(m_cmd);
                if (m_native)
                    (void)m_backend->WriteMarkerNative(m_native, m_token, true);
            }

            ~NodeScope()
            {
                // Exact mirror of the constructor's order so the scopes nest.
                if (m_scoped && m_backend)
                {
                    if (m_native)
                        (void)m_backend->WriteMarkerNative(m_native, m_token, false);
                    m_backend->Breadcrumbs().EndScope(m_token);
                }
                if (m_annotated)
                    m_core->CmdEndAnnotation(*m_cmd);
            }

            NodeScope(const NodeScope&)            = delete;
            NodeScope& operator=(const NodeScope&) = delete;

        private:
            const nri::CoreInterface* m_core      = nullptr;
            nri::CommandBuffer*       m_cmd       = nullptr;
            IGpuCrashBackend*         m_backend   = nullptr;
            void*                     m_native    = nullptr;
            std::uint32_t             m_token     = 0;
            bool                      m_annotated = false;
            bool                      m_scoped    = false;
        };

        // The usage bits ONE declared access implies for the texture it
        // touches. This is the other half of Task 4's usage -> (access,
        // layout, stage) table: NRI's validation layer checks every barrier's
        // access AND layout against the texture's usage mask
        // (Source/Validation/CommandBufferVal.hpp, IsAccessMaskSupported /
        // IsTextureLayoutSupported), so a pool texture realized without the
        // bit some tenant's barrier needs has EVERY barrier on it rejected.
        //
        // Copy usages (CopySrc/CopyDst/ReadbackHost) and Present contribute
        // NOTHING on purpose: NRI constrains neither COPY_SOURCE/
        // COPY_DESTINATION nor Layout::PRESENT against the usage mask, and
        // both backends add the transfer bits unconditionally at creation
        // (VK: DeviceVK.hpp always ORs VK_IMAGE_USAGE_TRANSFER_SRC|DST). A
        // copy-only transient therefore legitimately realizes with usage
        // NONE.
        nri::TextureUsageBits TextureUsageFor(RgUsage usage) noexcept
        {
            switch (usage)
            {
            case RgUsage::ColorWrite:    return nri::TextureUsageBits::COLOR_ATTACHMENT;
            case RgUsage::DepthWrite:    return nri::TextureUsageBits::DEPTH_STENCIL_ATTACHMENT;
            case RgUsage::ShaderRead:    return nri::TextureUsageBits::SHADER_RESOURCE;
            case RgUsage::ShaderWriteCs: return nri::TextureUsageBits::SHADER_RESOURCE_STORAGE;
            case RgUsage::CopySrc:
            case RgUsage::CopyDst:
            case RgUsage::ReadbackHost:
            case RgUsage::Present:       break;
            }
            return nri::TextureUsageBits::NONE;
        }

        bool SameTextureDesc(const nri::TextureDesc& a, const nri::TextureDesc& b) noexcept
        {
            return a.type == b.type && a.usage == b.usage && a.format == b.format
                && a.width == b.width && a.height == b.height && a.depth == b.depth
                && a.mipNum == b.mipNum && a.layerNum == b.layerNum && a.sampleNum == b.sampleNum;
        }

        bool SameBufferDesc(const nri::BufferDesc& a, const nri::BufferDesc& b) noexcept
        {
            return a.size == b.size && a.structureStride == b.structureStride && a.usage == b.usage;
        }
    }

    // ==================================================================
    // RenderGraphNodeContext -- resolution, from within a node's exec fn.
    // ==================================================================

    nri::Texture* RenderGraphNodeContext::Resolve(RgTexture texture) const
    {
        if (!graph)
            return nullptr;
        // Through the ONE handle seam, exactly as the header's private
        // section requires of every handle-consuming method -- so a handle
        // minted before a Reset() fails to decode here rather than indexing
        // this frame's table with last frame's slot.
        const std::size_t slot = graph->DecodeAndValidateSlot(texture.index, graph->m_textures.size());
        if (slot == RenderGraph::kNoSlot || slot >= graph->m_resolvedTextures.size())
            return nullptr;
        return graph->m_resolvedTextures[slot];
    }

    nri::Buffer* RenderGraphNodeContext::Resolve(RgBuffer buffer) const
    {
        if (!graph)
            return nullptr;
        const std::size_t slot = graph->DecodeAndValidateSlot(buffer.index, graph->m_buffers.size());
        if (slot == RenderGraph::kNoSlot || slot >= graph->m_resolvedBuffers.size())
            return nullptr;
        return graph->m_resolvedBuffers[slot];
    }

    nri::Descriptor* RenderGraphNodeContext::ColorView(RgTexture texture) const
    {
        nri::Texture* resolved = Resolve(texture);
        if (!resolved || !graph)
            return nullptr;
        return graph->ViewForTexture(resolved, /*depth=*/false);
    }

    // ==================================================================
    // Lifetime
    // ==================================================================

    RenderGraph::~RenderGraph()
    {
        // Buried, not destroyed: the last submission may still be reading
        // them. ~NriDevice drains the graveyard behind a DeviceWaitIdle,
        // which is the one place that can honor Graveyard's "the caller has
        // already made the GPU idle" contract on the way out -- and the
        // reason RgExecuteDesc::device documents that the device must
        // outlive the graph.
        ReleaseGpuResourcesInternal(/*all=*/true);
    }

    void RenderGraph::ReleaseGpuResources()
    {
        // The public, frame-driver-facing half: pool + views only. The
        // command slots and the fence are execution machinery and stay --
        // a graph released this way is immediately usable again, it just
        // realizes its pool from scratch on the next Execute().
        ReleaseGpuResourcesInternal(/*all=*/false);
    }

    void RenderGraph::ReleaseGpuResourcesInternal(bool all)
    {
        if (!m_device)
        {
            // Nothing was ever realized (no Execute), so there is nothing to
            // bury -- and no device to bury it into.
            return;
        }

        Graveyard&                graves = m_device->Graves();
        const nri::CoreInterface* core   = &m_device->Core();

        // Every burial in this call uses the SAME fence value -- the last
        // value this graph signalled. m_submitValue only ever grows (one per
        // successful submit) and every burial path in this file keys off its
        // CURRENT value, so Graveyard's nondecreasing invariant holds across
        // any interleaving of release, re-realization and destruction. A
        // graph that never submitted buries at 0, which is correct: nothing
        // it created was ever seen by the GPU.
        const std::uint64_t fence = m_submitValue;

        for (const CachedView& cached : m_views)
        {
            if (cached.view)
                graves.Bury(fence, [core, view = cached.view] { core->DestroyDescriptor(view); });
        }
        m_views.clear();

        for (const PoolResource& slot : m_pool)
        {
            if (slot.texture)
                graves.Bury(fence, [core, texture = slot.texture] { core->DestroyTexture(texture); });
            if (slot.buffer)
                graves.Bury(fence, [core, buffer = slot.buffer] { core->DestroyBuffer(buffer); });
        }
        m_pool.clear();

        m_resolvedTextures.clear();
        m_resolvedBuffers.clear();
        m_texturePoolSlot.clear();
        m_bufferPoolSlot.clear();

        // The pool is gone, so every carry state it held went with it -- the
        // next Execute() realizes fresh resources that genuinely start
        // undefined, and must not be handed a state from a buried one.
        m_poolCarrySeeded.clear();
        m_poolFirstBefore.clear();

        if (!all)
            return;

        // Command buffer before its allocator (the buffer is allocated OUT of
        // it), same order NriSmoke.cpp's teardown uses.
        for (const GpuFrameSlot& slot : m_frames)
        {
            if (slot.cmd)
                graves.Bury(fence, [core, cmd = slot.cmd] { core->DestroyCommandBuffer(cmd); });
            if (slot.allocator)
                graves.Bury(fence, [core, allocator = slot.allocator] { core->DestroyCommandAllocator(allocator); });
        }
        m_frames.clear();

        if (m_fence)
        {
            graves.Bury(fence, [core, f = m_fence] { core->DestroyFence(f); });
            m_fence = nullptr;
        }
    }

    // ==================================================================
    // Realization
    // ==================================================================

    bool RenderGraph::EnsureExecutionResources()
    {
        if (!m_frames.empty() && m_fence)
            return true;

        const nri::CoreInterface& core = m_device->Core();

        if (!m_fence)
        {
            // Starts at 0 and is signalled with m_submitValue + 1 per
            // successful submit, so GetFenceValue() reads "submissions this
            // graph has COMPLETED" -- which is exactly what the graveyard
            // needs to reap against, and the only completion signal a
            // headless (swapchain-less) graph has at all.
            if (!ARC_NRI_CHECK(core.CreateFence(m_device->Device(), 0, m_fence)) || !m_fence)
            {
                m_fence = nullptr;
                return false;
            }
            core.SetDebugName(m_fence, "RenderGraph submission fence");
        }

        if (m_frames.empty())
        {
            m_frames.resize(kSwapchainFramesInFlight);
            for (GpuFrameSlot& slot : m_frames)
            {
                // Short-circuit order matters: a failed CreateCommandAllocator
                // leaves slot.allocator null, and CreateCommandBuffer takes it
                // by reference (NriSmoke.cpp makes the same note).
                if (!ARC_NRI_CHECK(core.CreateCommandAllocator(*m_device->GraphicsQueue(), slot.allocator))
                    || !slot.allocator
                    || !ARC_NRI_CHECK(core.CreateCommandBuffer(*slot.allocator, slot.cmd))
                    || !slot.cmd)
                {
                    // ALL-OR-NOTHING: bury whatever was created and leave
                    // m_frames EMPTY. A half-filled vector would satisfy this
                    // function's "already realized" early-out above on the
                    // next call, and Execute() would then reset a null
                    // allocator -- turning a create failure into a crash one
                    // frame later.
                    Graveyard&                graves    = m_device->Graves();
                    const nri::CoreInterface* graveCore = &core;
                    for (const GpuFrameSlot& partial : m_frames)
                    {
                        if (partial.cmd)
                            graves.Bury(m_submitValue, [graveCore, c = partial.cmd]
                                        { graveCore->DestroyCommandBuffer(c); });
                        if (partial.allocator)
                            graves.Bury(m_submitValue, [graveCore, a = partial.allocator]
                                        { graveCore->DestroyCommandAllocator(a); });
                    }
                    m_frames.clear();

                    GraphError("RenderGraph::Execute: per-frame command buffers could not be created");
                    return false;
                }
            }
        }
        return true;
    }

    bool RenderGraph::RealizePool(const RgCompiled& compiled)
    {
        const nri::CoreInterface& core = m_device->Core();

        // ------------------------------------------------------------
        // Step 1: what the compile ASKS for, per pool slot.
        //
        // Textures need a usage mask, and RgTextureDesc has none: the graph
        // models usage per DECLARED ACCESS, not per resource. So each slot's
        // mask is the UNION over every tenant's every access (plus the
        // attachment roles below) -- because all those tenants share ONE
        // physical texture, and NRI's validation layer checks each barrier's
        // access and layout against that one texture's mask. Miss a bit and
        // every barrier on the slot is rejected, including the handover
        // barrier whose `before` carries the PREVIOUS tenant's access.
        //
        // Buffers need no union: Compile()'s descsMatch compares usage
        // exactly, so every tenant of a buffer slot declared the same usage
        // and the representative's is the slot's.
        // ------------------------------------------------------------
        struct Desired
        {
            bool             isTexture = true;
            bool             seen      = false;
            nri::TextureDesc texture{};
            nri::BufferDesc  buffer{};
            const std::string* name = nullptr;
        };
        std::vector<Desired> desired(compiled.poolSlotCount);

        if (compiled.transientPoolSlot.size() != compiled.transients.size())
        {
            GraphError("RenderGraph::Execute: the compiled graph's transient vectors disagree in size -- "
                       "it was not produced by a Compile() of these declarations");
            return false;
        }

        for (std::size_t t = 0; t < compiled.transients.size(); ++t)
        {
            const std::uint32_t poolSlot = compiled.transientPoolSlot[t];
            if (poolSlot == kRgNoPoolSlot)
                continue;   // no node touches it -- nothing to realize (RenderGraph.hpp, RgTransient)
            if (poolSlot >= compiled.poolSlotCount)
            {
                GraphError("RenderGraph::Execute: a transient's pool slot is out of range");
                return false;
            }

            const RgTransient& transient = compiled.transients[t];
            const std::size_t  count     = transient.isTexture ? m_textures.size() : m_buffers.size();
            if (transient.resourceIndex >= count)
            {
                GraphError("RenderGraph::Execute: a transient's resource index is out of range -- the compiled "
                           "graph does not describe these declarations");
                return false;
            }

            Desired& slot = desired[poolSlot];
            if (!slot.seen)
            {
                slot.seen      = true;
                slot.isTexture = transient.isTexture;
                if (transient.isTexture)
                {
                    const TextureResource& resource = m_textures[transient.resourceIndex];
                    // nri::Dim_t is uint16_t. A declared extent that does not
                    // round-trip through it would silently become a DIFFERENT
                    // (possibly zero) texture that creates successfully --
                    // refuse instead of realizing the wrong thing.
                    if (resource.desc.width  > 0xFFFFu || resource.desc.height > 0xFFFFu
                        || resource.desc.width == 0 || resource.desc.height == 0)
                    {
                        GraphError("RenderGraph::Execute: transient texture '" + resource.name
                                    + "' has an extent NRI cannot express (nri::Dim_t is 16-bit, and 0 is "
                                      "not a valid dimension)");
                        return false;
                    }
                    slot.name                = &resource.name;
                    slot.texture.type        = nri::TextureType::TEXTURE_2D;
                    slot.texture.format      = resource.desc.format;
                    slot.texture.width       = static_cast<nri::Dim_t>(resource.desc.width);
                    slot.texture.height      = static_cast<nri::Dim_t>(resource.desc.height);
                    slot.texture.depth       = 1;
                    slot.texture.mipNum      = 1;
                    slot.texture.layerNum    = 1;
                    slot.texture.sampleNum   = 1;
                    slot.texture.usage       = nri::TextureUsageBits::NONE;
                }
                else
                {
                    const BufferResource& resource = m_buffers[transient.resourceIndex];
                    slot.name          = &resource.name;
                    slot.buffer.size   = resource.size;
                    slot.buffer.usage  = resource.usage;
                }
            }
            else if (slot.isTexture != transient.isTexture)
            {
                // Compile() never assigns a texture and a buffer to one slot
                // (its slot ids are one numbering across both kinds); refuse
                // rather than realize whichever kind was seen first.
                GraphError("RenderGraph::Execute: a pool slot is claimed by both a texture and a buffer");
                return false;
            }

            if (!transient.isTexture)
                continue;

            for (const Access& access : m_accesses)
            {
                if (!access.isTexture || access.resourceIndex != transient.resourceIndex)
                    continue;
                slot.texture.usage = slot.texture.usage | TextureUsageFor(access.usage);
            }
        }

        // Attachment roles, ORed in on top of the declared accesses. Compile()
        // requires an attachment to carry SOME declared access but not that
        // the access matches its attachment role (a known, deferred Task-4
        // gap), and creating a COLOR_ATTACHMENT view on a texture without
        // that usage bit is a hard failure rather than a wrong-looking pixel.
        // Cheap insurance, and it never widens a mask the declarations
        // already justify.
        const auto addAttachmentUsage = [&](RgTexture attachment, nri::TextureUsageBits bits)
        {
            const std::size_t slot = DecodeAndValidateSlot(attachment.index, m_textures.size());
            if (slot == kNoSlot || slot >= m_texturePoolSlot.size())
                return;
            const std::uint32_t poolSlot = m_texturePoolSlot[slot];
            if (poolSlot == kRgNoPoolSlot || poolSlot >= desired.size() || !desired[poolSlot].isTexture)
                return;
            desired[poolSlot].texture.usage = desired[poolSlot].texture.usage | bits;
        };
        for (const NodeDesc& node : m_nodes)
        {
            if (node.kind != NodeKind::Raster)
                continue;
            for (const RgTexture& color : node.colorAttachments)
                addAttachmentUsage(color, nri::TextureUsageBits::COLOR_ATTACHMENT);
            if (node.depthAttachment.index != kInvalid)
                addAttachmentUsage(node.depthAttachment, nri::TextureUsageBits::DEPTH_STENCIL_ATTACHMENT);
        }

        // ------------------------------------------------------------
        // Step 2: realize what is not already realized. A slot whose
        // realized desc still matches is KEPT untouched -- that is what
        // makes a second Execute() of the same compiled graph create nothing
        // (DebugTransientCreateCount()).
        // ------------------------------------------------------------
        Graveyard&                graves    = m_device->Graves();
        const nri::CoreInterface* graveCore = &core;

        // Buries a pool entry AND every cached view that names its texture:
        // a view outliving the resource it views is a dangling descriptor,
        // and the graveyard is the only place that can free either safely.
        const auto buryPoolResource = [&](PoolResource& entry)
        {
            if (entry.texture)
            {
                for (std::size_t v = 0; v < m_views.size();)
                {
                    if (m_views[v].texture == entry.texture)
                    {
                        if (m_views[v].view)
                        {
                            graves.Bury(m_submitValue, [graveCore, view = m_views[v].view]
                                        { graveCore->DestroyDescriptor(view); });
                        }
                        m_views.erase(m_views.begin() + static_cast<std::ptrdiff_t>(v));
                        continue;
                    }
                    ++v;
                }
                graves.Bury(m_submitValue, [graveCore, t = entry.texture] { graveCore->DestroyTexture(t); });
            }
            if (entry.buffer)
                graves.Bury(m_submitValue, [graveCore, b = entry.buffer] { graveCore->DestroyBuffer(b); });
            entry = PoolResource{};
        };

        for (std::size_t i = compiled.poolSlotCount; i < m_pool.size(); ++i)
            buryPoolResource(m_pool[i]);
        m_pool.resize(compiled.poolSlotCount);

        for (std::uint32_t p = 0; p < compiled.poolSlotCount; ++p)
        {
            const Desired&  want    = desired[p];
            PoolResource&   slot    = m_pool[p];
            const bool      realized = slot.texture != nullptr || slot.buffer != nullptr;

            if (!want.seen)
            {
                // A pool slot Compile() counted but assigned nothing to
                // cannot happen today (poolSlotCount IS the assignment
                // count); leaving it unrealized is the safe reading either
                // way, since nothing can reference it.
                continue;
            }

            if (realized && slot.isTexture == want.isTexture
                && (want.isTexture ? SameTextureDesc(slot.textureDesc, want.texture)
                                    : SameBufferDesc(slot.bufferDesc, want.buffer)))
            {
                continue;   // reuse -- the whole point of DebugTransientCreateCount()
            }

            if (realized)
            {
                // Shape changed (a resize, or a differently-declared frame
                // without a Reset()): bury the old one before replacing it.
                buryPoolResource(slot);
            }

            slot.isTexture = want.isTexture;
            if (want.isTexture)
            {
                // CreateCommittedTexture rather than CreateTexture +
                // GetTextureMemoryDesc + AllocateMemory + BindTextureMemory:
                // one dedicated allocation per pool slot, which is what
                // Phase 2's "no real aliasing yet" scope calls for. Slot
                // SHARING (two transients with disjoint lifetimes on one
                // resource) is what buys the memory back here, and it needs
                // no memory-object machinery at all.
                if (!ARC_NRI_CHECK(core.CreateCommittedTexture(m_device->Device(), nri::MemoryLocation::DEVICE,
                                                                0.0f, want.texture, slot.texture))
                    || !slot.texture)
                {
                    slot = PoolResource{};
                    GraphError("RenderGraph::Execute: transient texture '"
                                + (want.name ? *want.name : std::string("?")) + "' could not be created");
                    return false;
                }
                slot.textureDesc = want.texture;
                if (want.name)
                    core.SetDebugName(slot.texture, want.name->c_str());
            }
            else
            {
                if (!ARC_NRI_CHECK(core.CreateCommittedBuffer(m_device->Device(), nri::MemoryLocation::DEVICE,
                                                               0.0f, want.buffer, slot.buffer))
                    || !slot.buffer)
                {
                    slot = PoolResource{};
                    GraphError("RenderGraph::Execute: transient buffer '"
                                + (want.name ? *want.name : std::string("?")) + "' could not be created");
                    return false;
                }
                slot.bufferDesc = want.buffer;
                if (want.name)
                    core.SetDebugName(slot.buffer, want.name->c_str());
            }
            ++m_transientCreateCount;
        }

        return true;
    }

    nri::Descriptor* RenderGraph::ViewForTexture(nri::Texture* texture, bool depth) const noexcept
    {
        if (!texture)
            return nullptr;
        for (const CachedView& cached : m_views)
        {
            if (cached.texture == texture && cached.depth == depth)
                return cached.view;
        }
        return nullptr;
    }

    bool RenderGraph::RealizeAttachmentViews()
    {
        const nri::CoreInterface& core = m_device->Core();

        // Created up front, for exactly the textures the declarations attach
        // -- so RenderGraphNodeContext::ColorView is a lookup and never a
        // creation point, and so a view failure refuses the frame before
        // anything is recorded rather than half way through it.
        const auto ensure = [&](RgTexture attachment, bool depth) -> bool
        {
            const std::size_t slot = DecodeAndValidateSlot(attachment.index, m_textures.size());
            if (slot == kNoSlot)
                return true;   // Compile() already refused genuinely bad attachment handles
            nri::Texture* texture = slot < m_resolvedTextures.size() ? m_resolvedTextures[slot] : nullptr;
            if (!texture)
            {
                GraphError("RenderGraph::Execute: attachment '" + m_textures[slot].name
                            + "' resolved to no texture");
                return false;
            }
            if (ViewForTexture(texture, depth))
                return true;

            nri::TextureViewDesc viewDesc = {};
            viewDesc.texture = texture;
            viewDesc.type    = depth ? nri::TextureView::DEPTH_STENCIL_ATTACHMENT
                                      : nri::TextureView::COLOR_ATTACHMENT;
            // A transient's format is the one it was realized with; an
            // imported texture's is whatever it actually is, read back from
            // NRI rather than assumed (an imported backbuffer's channel
            // order is resolved by NRI, not by us -- NriSwapChain::Format()).
            const std::uint32_t poolSlot = slot < m_texturePoolSlot.size() ? m_texturePoolSlot[slot]
                                                                            : kRgNoPoolSlot;
            viewDesc.format = (poolSlot != kRgNoPoolSlot && poolSlot < m_pool.size())
                            ? m_pool[poolSlot].textureDesc.format
                            : core.GetTextureDesc(*texture).format;

            nri::Descriptor* view = nullptr;
            if (!ARC_NRI_CHECK(core.CreateTextureView(viewDesc, view)) || !view)
            {
                GraphError("RenderGraph::Execute: attachment view for '" + m_textures[slot].name
                            + "' could not be created");
                return false;
            }
            m_views.push_back(CachedView{ texture, depth, view });
            return true;
        };

        for (const NodeDesc& node : m_nodes)
        {
            if (node.kind != NodeKind::Raster)
                continue;
            for (const RgTexture& color : node.colorAttachments)
            {
                if (!ensure(color, /*depth=*/false))
                    return false;
            }
            if (node.depthAttachment.index != kInvalid)
            {
                if (!ensure(node.depthAttachment, /*depth=*/true))
                    return false;
            }
        }
        return true;
    }

    nri::Texture* RenderGraph::TextureForSlot(std::size_t slot) const noexcept
    {
        return slot < m_resolvedTextures.size() ? m_resolvedTextures[slot] : nullptr;
    }

    nri::Buffer* RenderGraph::BufferForSlot(std::size_t slot) const noexcept
    {
        return slot < m_resolvedBuffers.size() ? m_resolvedBuffers[slot] : nullptr;
    }

    std::uint32_t RenderGraph::PoolSlotForBarrier(const RgBarrier& barrier) const noexcept
    {
        if (barrier.isTexture)
        {
            return barrier.resourceIndex < m_texturePoolSlot.size()
                 ? m_texturePoolSlot[barrier.resourceIndex] : kRgNoPoolSlot;
        }
        return barrier.resourceIndex < m_bufferPoolSlot.size()
             ? m_bufferPoolSlot[barrier.resourceIndex] : kRgNoPoolSlot;
    }

    std::optional<nri::AccessLayoutStage> RenderGraph::DebugFirstBarrierBefore(std::uint32_t slot) const
    {
        if (slot >= m_poolCarrySeeded.size() || m_poolCarrySeeded[slot] == 0
            || slot >= m_poolFirstBefore.size())
        {
            return std::nullopt;
        }
        return m_poolFirstBefore[slot];
    }

    // ==================================================================
    // Barriers -- the ONE CmdBarrier call site (see the file header).
    // ==================================================================

    void RenderGraph::EmitBarriers(nri::CommandBuffer& cmd, std::span<const RgBarrier> barriers)
    {
        if (barriers.empty())
            return;   // never issue an empty group

        m_scratchTextureBarriers.clear();
        m_scratchBufferBarriers.clear();

        for (const RgBarrier& barrier : barriers)
        {
            // CROSS-FRAME POOL HANDOVER -- the executor's one amendment to a
            // verbatim replay (RgCompiled's contract block says so, and why).
            // The pool survives Reset(), so this physical resource may still
            // be carrying the previous frame's writes; Compile(), being pure
            // and per-frame, always hands the slot's first use a
            // `before = {NONE, UNDEFINED, ALL}`, which performs no source
            // availability operation at all. Patch access + stages from what
            // the last frame actually left here, leave the layout UNDEFINED
            // (discard, never inherit contents) -- byte-for-byte the amendment
            // Compile() makes for a within-frame tenant handover.
            const std::uint32_t poolSlot = PoolSlotForBarrier(barrier);
            nri::AccessLayoutStage before = barrier.before;
            if (poolSlot != kRgNoPoolSlot && poolSlot < m_pool.size()
                && poolSlot < m_poolCarrySeeded.size() && m_poolCarrySeeded[poolSlot] == 0)
            {
                m_poolCarrySeeded[poolSlot] = 1;
                // Guarded on the state Compile() actually emits for a first
                // use rather than applied blind: anything else means this is
                // NOT the slot's first-use barrier and must not be touched.
                if (m_pool[poolSlot].hasCarry
                    && before.access == nri::AccessBits::NONE
                    && before.layout == nri::Layout::UNDEFINED)
                {
                    before.access = m_pool[poolSlot].carry.access;
                    before.stages = m_pool[poolSlot].carry.stages;
                }
                if (poolSlot < m_poolFirstBefore.size())
                    m_poolFirstBefore[poolSlot] = before;
            }

            if (barrier.isTexture)
            {
                nri::Texture* texture = TextureForSlot(barrier.resourceIndex);
                // Execute() bounds-checks and null-checks every barrier
                // before recording starts, so reaching this is a bug in that
                // check rather than bad input.
                ARC_ASSERT(texture != nullptr, "RenderGraph::EmitBarriers: unresolved texture barrier");
                if (!texture)
                    continue;

                nri::TextureBarrierDesc desc = {};
                desc.texture = texture;
                desc.before  = before;
                desc.after   = barrier.after;
                // mipNum/layerNum left at REMAINING (0): the graph transitions
                // whole resources, and REMAINING stays correct for an imported
                // texture that happens to carry more than one mip or layer.
                m_scratchTextureBarriers.push_back(desc);
            }
            else
            {
                nri::Buffer* buffer = BufferForSlot(barrier.resourceIndex);
                ARC_ASSERT(buffer != nullptr, "RenderGraph::EmitBarriers: unresolved buffer barrier");
                if (!buffer)
                    continue;

                // nri::BufferBarrierDesc's before/after are AccessStage:
                // access + stages, no layout. RgBarrier's layout is always
                // UNDEFINED for a buffer and is DROPPED here, exactly as
                // RgBarrier's contract says.
                nri::BufferBarrierDesc desc = {};
                desc.buffer = buffer;
                desc.before = { before.access, before.stages };
                desc.after  = { barrier.after.access, barrier.after.stages };
                m_scratchBufferBarriers.push_back(desc);
            }

            // The slot now holds whatever this barrier moved it to. The LAST
            // barrier of the frame therefore leaves the carry state the next
            // frame patches from -- and that is exact, because Compile()
            // emits a barrier for every state change and the physical
            // resource cannot change state any other way.
            if (poolSlot != kRgNoPoolSlot && poolSlot < m_pool.size())
            {
                m_pool[poolSlot].carry    = barrier.after;
                m_pool[poolSlot].hasCarry = true;
            }
        }

        nri::BarrierDesc group = {};
        group.buffers    = m_scratchBufferBarriers.empty() ? nullptr : m_scratchBufferBarriers.data();
        group.bufferNum  = static_cast<std::uint32_t>(m_scratchBufferBarriers.size());
        group.textures   = m_scratchTextureBarriers.empty() ? nullptr : m_scratchTextureBarriers.data();
        group.textureNum = static_cast<std::uint32_t>(m_scratchTextureBarriers.size());
        if (group.bufferNum == 0 && group.textureNum == 0)
            return;

        m_device->Core().CmdBarrier(cmd, group);
    }

    // ==================================================================
    // Execute
    // ==================================================================

    bool RenderGraph::Execute(const RgExecuteDesc& desc, const RgCompiled& compiled)
    {
        if (m_device == nullptr)
        {
            m_device = &desc.device;
        }
        else if (m_device != &desc.device)
        {
            // Every resource this graph holds belongs to the first device;
            // adopting a second one would bury this device's textures through
            // that device's function table.
            GraphError("RenderGraph::Execute: a second NriDevice was passed to a graph that already "
                       "realized resources on another -- one graph belongs to one device");
            return false;
        }

        const nri::CoreInterface& core = m_device->Core();

        // ------------------------------------------------------------
        // The compiled graph must describe THESE declarations. The cheap
        // caller bug is Reset() -> re-declare -> Execute() with last frame's
        // RgCompiled, which would index this frame's resources with last
        // frame's slots; RgCompiled carries no generation stamp, so this is
        // structural.
        // ------------------------------------------------------------
        if (compiled.nodes.size() != m_nodes.size())
        {
            GraphError("RenderGraph::Execute: the compiled graph has " + std::to_string(compiled.nodes.size())
                        + " node(s) but this graph declares " + std::to_string(m_nodes.size())
                        + " -- Compile() again after re-declaring");
            return false;
        }

        if (!EnsureExecutionResources())
            return false;

        if (desc.frameSlot >= m_frames.size())
        {
            GraphError("RenderGraph::Execute: frameSlot " + std::to_string(desc.frameSlot)
                        + " is out of range (kSwapchainFramesInFlight = "
                        + std::to_string(m_frames.size()) + ")");
            return false;
        }

        // Whatever this graph's own timeline has retired is safe to free. On
        // the NONE backend GetFenceValue is hard-wired to 0, so this reaps
        // only value-0 burials -- headless tests feed the graveyard their own
        // values by hand.
        m_device->Graves().Reap(core.GetFenceValue(*m_fence));

        // Transient slot mapping for this frame, built before anything reads
        // it (RealizePool's attachment-role pass does).
        m_texturePoolSlot.assign(m_textures.size(), kRgNoPoolSlot);
        m_bufferPoolSlot.assign(m_buffers.size(), kRgNoPoolSlot);
        for (std::size_t t = 0; t < compiled.transients.size() && t < compiled.transientPoolSlot.size(); ++t)
        {
            const RgTransient& transient = compiled.transients[t];
            if (transient.isTexture && transient.resourceIndex < m_texturePoolSlot.size())
                m_texturePoolSlot[transient.resourceIndex] = compiled.transientPoolSlot[t];
            else if (!transient.isTexture && transient.resourceIndex < m_bufferPoolSlot.size())
                m_bufferPoolSlot[transient.resourceIndex] = compiled.transientPoolSlot[t];
        }

        if (!RealizePool(compiled))
            return false;

        // Cross-frame handover bookkeeping for THIS frame: no slot has had
        // its first barrier emitted yet. PoolResource::carry itself is NOT
        // cleared -- it is precisely what survives from the last Execute().
        m_poolCarrySeeded.assign(m_pool.size(), 0);
        m_poolFirstBefore.assign(m_pool.size(), nri::AccessLayoutStage{});

        // ------------------------------------------------------------
        // Acquire -- as LATE as possible. Everything fallible above this
        // line fails without an outstanding acquire; a failure BELOW it
        // leaves one for ~NriSwapChain's QueueWaitIdle to clean up, which is
        // untidy but strictly better than presenting a frame whose release
        // fence nothing signalled (that parks the present engine forever --
        // NriSmoke.cpp's bail-outs make the same trade).
        // ------------------------------------------------------------
        bool wantsSwapChain = false;
        for (const TextureResource& resource : m_textures)
            wantsSwapChain = wantsSwapChain || resource.swapChain;

        nri::Texture* backbuffer = nullptr;
        if (wantsSwapChain)
        {
            if (!desc.swapChain)
            {
                GraphError("RenderGraph::Execute: a node imported the swapchain backbuffer but "
                           "RgExecuteDesc::swapChain is null");
                return false;
            }
            backbuffer = desc.swapChain->AcquireNextTexture();
            if (!backbuffer)
            {
                // Routine, already logged at the right severity by
                // NriSwapChain (a zero-sized surface, or an OUT_OF_DATE
                // awaiting the frame driver's Resize()). Deliberately NOT a
                // GraphError: skipping a frame is not a latch-worthy event.
                return false;
            }
        }

        // ------------------------------------------------------------
        // Resolution table for this frame.
        // ------------------------------------------------------------
        m_resolvedTextures.assign(m_textures.size(), nullptr);
        m_resolvedBuffers.assign(m_buffers.size(), nullptr);
        for (std::size_t i = 0; i < m_textures.size(); ++i)
        {
            const TextureResource& resource = m_textures[i];
            if (resource.swapChain)
                m_resolvedTextures[i] = backbuffer;
            else if (resource.kind == ResourceKind::Imported)
                m_resolvedTextures[i] = resource.imported;
            else if (m_texturePoolSlot[i] != kRgNoPoolSlot && m_texturePoolSlot[i] < m_pool.size())
                m_resolvedTextures[i] = m_pool[m_texturePoolSlot[i]].texture;
        }
        for (std::size_t i = 0; i < m_buffers.size(); ++i)
        {
            const BufferResource& resource = m_buffers[i];
            if (resource.kind == ResourceKind::Imported)
                m_resolvedBuffers[i] = resource.imported;
            else if (m_bufferPoolSlot[i] != kRgNoPoolSlot && m_bufferPoolSlot[i] < m_pool.size())
                m_resolvedBuffers[i] = m_pool[m_bufferPoolSlot[i]].buffer;
        }

        if (!RealizeAttachmentViews())
            return false;

        // Every barrier's resource must exist BEFORE recording opens: a
        // barrier that named an out-of-range slot, or a resource nothing
        // realized, would otherwise be silently skipped mid-record and leave
        // the resource in the wrong state for the node that follows it.
        const auto barriersResolve = [&](std::span<const RgBarrier> barriers, const char* which) -> bool
        {
            for (const RgBarrier& barrier : barriers)
            {
                const bool ok = barrier.isTexture
                              ? (barrier.resourceIndex < m_resolvedTextures.size()
                                 && m_resolvedTextures[barrier.resourceIndex] != nullptr)
                              : (barrier.resourceIndex < m_resolvedBuffers.size()
                                 && m_resolvedBuffers[barrier.resourceIndex] != nullptr);
                if (!ok)
                {
                    GraphError(std::string("RenderGraph::Execute: a ") + which + " barrier names a "
                                + (barrier.isTexture ? "texture" : "buffer")
                                + " this frame did not realize -- the compiled graph does not describe "
                                  "these declarations");
                    return false;
                }
            }
            return true;
        };
        for (const RgCompiledNode& node : compiled.nodes)
        {
            if (!barriersResolve(node.preBarriers, "pre"))
                return false;
        }
        if (!barriersResolve(compiled.exitBarriers, "exit"))
            return false;

        // ------------------------------------------------------------
        // Record. ONE command buffer for every node (the frozen shape).
        // ------------------------------------------------------------
        // Named `frame`, not `slot`: the lambdas below take a texture SLOT,
        // and two different slot spaces sharing one name in one scope is
        // exactly the kind of confusion RgCompiled's INDEX SPACE comment
        // exists to prevent.
        GpuFrameSlot& frame = m_frames[desc.frameSlot];
        core.ResetCommandAllocator(*frame.allocator);

        nri::CommandBuffer& cmd = *frame.cmd;
        if (!ARC_NRI_CHECK(core.BeginCommandBuffer(cmd, nullptr)))
            return false;

        RenderGraphNodeContext context{ cmd, core, desc.ring, desc.pipelines, this };

        // The cached view for an attachment handle -- RealizeAttachmentViews
        // above already created every one of them, so a null here means the
        // handle did not decode (which Compile() has already refused).
        const auto attachmentView = [&](RgTexture attachment, bool depth) -> nri::Descriptor*
        {
            const std::size_t slot = DecodeAndValidateSlot(attachment.index, m_textures.size());
            if (slot == kNoSlot)
                return nullptr;
            return ViewForTexture(TextureForSlot(slot), depth);
        };

        // The pass's render area: the first colour attachment's extent, or
        // the depth attachment's for a depth-only pass. A transient's is the
        // desc it was realized with; an imported texture's is read back from
        // NRI rather than assumed.
        const auto attachmentExtent = [&](const NodeDesc& node, std::uint32_t& width,
                                          std::uint32_t& height) -> bool
        {
            const RgTexture reference = node.colorAttachments.empty() ? node.depthAttachment
                                                                      : node.colorAttachments.front();
            const std::size_t slot = DecodeAndValidateSlot(reference.index, m_textures.size());
            if (slot == kNoSlot)
                return false;

            const std::uint32_t poolSlot = m_texturePoolSlot[slot];
            if (poolSlot != kRgNoPoolSlot && poolSlot < m_pool.size() && m_pool[poolSlot].texture)
            {
                width  = m_pool[poolSlot].textureDesc.width;
                height = m_pool[poolSlot].textureDesc.height;
                return true;
            }

            nri::Texture* texture = TextureForSlot(slot);
            if (!texture)
                return false;
            const nri::TextureDesc& textureDesc = core.GetTextureDesc(*texture);
            width  = textureDesc.width;
            height = textureDesc.height;
            return true;
        };

        for (std::size_t i = 0; i < compiled.nodes.size(); ++i)
        {
            const RgCompiledNode& compiledNode = compiled.nodes[i];
            const NodeDesc&       node         = m_nodes[i];

            // Opened BEFORE the node's barriers so a capture attributes the
            // transitions to the pass that needed them.
            NodeScope scope(core, cmd, node.passLabel);

            EmitBarriers(cmd, compiledNode.preBarriers);

            bool rendering = false;
            if (node.kind == NodeKind::Raster)
            {
                m_scratchColorAttachments.clear();
                for (const RgTexture& color : node.colorAttachments)
                {
                    nri::AttachmentDesc attachment = {};
                    attachment.descriptor = attachmentView(color, /*depth=*/false);
                    m_scratchColorAttachments.push_back(attachment);
                }

                nri::RenderingDesc renderingDesc = {};
                renderingDesc.colors   = m_scratchColorAttachments.empty() ? nullptr
                                                                            : m_scratchColorAttachments.data();
                renderingDesc.colorNum = static_cast<std::uint32_t>(m_scratchColorAttachments.size());
                if (node.depthAttachment.index != kInvalid)
                    renderingDesc.depth.descriptor = attachmentView(node.depthAttachment, /*depth=*/true);

                core.CmdBeginRendering(cmd, renderingDesc);
                rendering = true;

                // "Initial state (mandatory)" per NRI.h -- a pipeline is not
                // allowed to inherit either of these, so the graph sets them
                // to the full attachment once per pass. A node that wants
                // something else overrides them from its exec fn.
                std::uint32_t width = 0, height = 0;
                if (attachmentExtent(node, width, height) && width != 0 && height != 0)
                {
                    const nri::Viewport viewport = { 0.0f, 0.0f, static_cast<float>(width),
                                                     static_cast<float>(height), 0.0f, 1.0f };
                    core.CmdSetViewports(cmd, &viewport, 1);
                    const nri::Rect scissor = { 0, 0, static_cast<nri::Dim_t>(width),
                                                static_cast<nri::Dim_t>(height) };
                    core.CmdSetScissors(cmd, &scissor, 1);
                }
            }

            if (node.exec)
                node.exec(context);

            if (rendering)
                core.CmdEndRendering(cmd);
        }

        EmitBarriers(cmd, compiled.exitBarriers);

        if (!ARC_NRI_CHECK(core.EndCommandBuffer(cmd)))
            return false;

        // ------------------------------------------------------------
        // Submit. The graph's own fence is ALWAYS signalled (it is the
        // graveyard's clock and a headless graph's only completion signal);
        // the swapchain's acquire/release fences join it only for a frame
        // that acquired. NriSwapChain owns both but submits no real work
        // itself, so wiring them into a submission is the caller's job --
        // and the caller is now the graph.
        // ------------------------------------------------------------
        nri::FenceSubmitDesc waitFence = {};
        waitFence.fence  = backbuffer ? desc.swapChain->CurrentAcquireFence() : nullptr;
        // StageBits::ALL (NRI's lazy default, 0) rather than NriSmoke's
        // COLOR_ATTACHMENT: a graph does not know which stage first touches
        // the backbuffer -- the first node might copy into it rather than
        // render to it -- and a too-narrow wait stage is a race, not a
        // slowdown.
        waitFence.stages = nri::StageBits::ALL;

        nri::FenceSubmitDesc signalFences[2] = {};
        signalFences[0].fence = m_fence;
        signalFences[0].value = m_submitValue + 1;
        std::uint32_t signalNum = 1;
        if (backbuffer)
        {
            // A SWAPCHAIN_SEMAPHORE fence carries no value (NriSmoke.cpp
            // leaves it 0 for the same reason).
            signalFences[1].fence = desc.swapChain->CurrentReleaseFence();
            signalNum = 2;
        }

        nri::CommandBuffer* commandBuffers[] = { &cmd };

        nri::QueueSubmitDesc submit = {};
        submit.waitFences       = waitFence.fence ? &waitFence : nullptr;
        submit.waitFenceNum     = waitFence.fence ? 1u : 0u;
        submit.commandBuffers   = commandBuffers;
        submit.commandBufferNum = 1;
        submit.signalFences     = signalFences;
        submit.signalFenceNum   = signalNum;

        if (!ARC_NRI_CHECK(core.QueueSubmit(*m_device->GraphicsQueue(), submit)))
        {
            // No Present: a failed submit signalled no release fence, so
            // presenting would park the present engine on a fence nothing
            // will ever signal. ARC_NRI_CHECK has already routed a typed
            // DEVICE_LOST into the device-removed chain.
            return false;
        }
        ++m_submitValue;

        if (backbuffer)
            desc.swapChain->Present();

        return true;
    }
}
