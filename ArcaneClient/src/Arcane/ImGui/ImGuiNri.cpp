// ImGuiNri -- see the header for the §7 "PORT OURS" ruling, the ImTextureID
// convention it preserves, the root-constant finding, and why the font atlas
// goes through the helper while the vertex/index streams go through the ring.
//
// Same include-order rule as every NRI-touching file in this tree
// (NriCommon.hpp): NRI headers first, because Extensions/NRIDeviceCreation.h
// declares nri::Message::ERROR and <windows.h> (via Arcane/Base/Log.hpp ->
// spdlog) #defines ERROR via wingdi.h.
#include <NRI.h>
#include <Extensions/NRIHelper.h>

#include <Arcane/ImGui/ImGuiNri.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/Nri/NriCommon.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/Nri/NriUploadRing.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>
#include <Arcane/Render/ShaderConventions.hpp>   // kVsEntry / kPsEntry
#include <Arcane/Render/FramePacing.hpp>           // kSwapchainFramesInFlight

#include <imgui.h>

#undef ERROR

#include <algorithm>
#include <cstring>
#include <string>

namespace Arcane
{
    namespace
    {
        // The tagged seam the graph path reports its OWN refusals through --
        // not an NRI call's result (those go through ARC_NRI_CHECK). Both land
        // in the RenderErrorCount() latch, which is what makes a --nri-graph
        // run's exit code meaningful.
        void GraphError(const std::string& text)
        {
            RenderErrorLatch::Instance().NoteError("nri-graph", text.c_str());
        }

        // 16-byte root-constant block matching imgui.hlsl's ImGuiConstants
        // (scale + translate). The layout is the shader's, not this file's.
        struct ImGuiRootConstants
        {
            float scale[2];
            float translate[2];
        };
        static_assert(sizeof(ImGuiRootConstants) == 16,
                      "imgui root constants are 16 bytes (scale + translate)");

        // ImDrawVert is float2 pos, float2 uv, ImU32 col = 20 bytes; the
        // vertex input below depends on it exactly. ImDrawIdx is 16-bit ->
        // IndexType::UINT16. These are also NRIImgui's documented
        // requirements (§7.4), so a future switch to it would not silently
        // change meaning.
        static_assert(sizeof(ImDrawVert) == 20,
                      "ImDrawVert layout assumed by the imgui vertex input");
        static_assert(sizeof(ImDrawIdx) == 2,
                      "ImDrawIdx assumed 16-bit (IndexType::UINT16)");

        // THE RING'S ALIGNMENT IS A POWER OF TWO, and ImDrawVert's 20-byte
        // stride is not -- so this is NOT `sizeof(ImDrawVert)` the way
        // Batch2DNode's is `sizeof(Batch2DVertex)` (32). RingLayout::Allocate
        // ARC_ASSERTs the power-of-two property in debug and only rounds
        // correctly by luck otherwise, so the alignment has to be chosen
        // rather than copied.
        //
        // 16 is safe and cheap: neither backend requires a vertex-buffer bind
        // offset to be a multiple of the STRIDE (D3D12's
        // D3D12_VERTEX_BUFFER_VIEW and vkCmdBindVertexBuffers both index from
        // the bound offset), and the only real requirement is that each
        // attribute lands on its own natural boundary -- which holds because
        // 16 and the 20-byte stride are both multiples of 4, ImDrawVert's
        // widest member alignment. The index side keeps sizeof(ImDrawIdx),
        // which IS a power of two and IS required: both backends want an
        // index-buffer offset that is a multiple of the index size.
        constexpr std::uint64_t kVertexAlign = 16;
        static_assert((kVertexAlign & (kVertexAlign - 1)) == 0,
                      "RingLayout::Allocate requires a power-of-two alignment");
        static_assert(kVertexAlign % alignof(ImDrawVert) == 0 && sizeof(ImDrawVert) % 4 == 0,
                      "every ImDrawVert attribute must stay naturally aligned inside the ring");
        static_assert((sizeof(ImDrawIdx) & (sizeof(ImDrawIdx) - 1)) == 0,
                      "RingLayout::Allocate requires a power-of-two alignment");

        // Clamps an ImGui clip edge into the attachment and into nri::Rect's
        // int16_t/Dim_t fields. nri::Rect is narrow AND unsigned in its extent
        // half, so a negative left edge (an ImGui window dragged off the left
        // of the surface) would wrap into a colossal width.
        std::uint32_t ClampEdge(float value, std::uint32_t limit) noexcept
        {
            if (!(value > 0.0f))
                return 0;
            const float clamped = value < (float)limit ? value : (float)limit;
            return (std::uint32_t)clamped;
        }
    }

    void ImGuiNri::InstallBackendIdentity()
    {
        // Remembered, not merely installed -- see m_imguiContext. This is the
        // one moment this class can know which context it serves.
        m_imguiContext = ImGui::GetCurrentContext();

        // The backend flags, on the context the caller has already pinned.
        // HasTextures is the 1.92 protocol
        // NewFrameTexUpdates implements; HasVtxOffset is what lets one
        // concatenated vertex buffer serve every draw list
        // (DrawIndexedDesc::baseVertex). Both flags are idempotent and the name
        // is cosmetic, so re-running this on a context that already has them
        // costs nothing -- which is what makes AdoptContext safe to call
        // unconditionally.
        ImGuiIO& io = ImGui::GetIO();
        io.BackendRendererName = "imgui_impl_nri";
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    }

    void ImGuiNri::AdoptContext(void* imguiContext)
    {
        // THE PIN IS THE WHOLE POINT (see the declaration). Init installs on
        // whatever is CURRENT, which is right only when the caller's context is
        // the one it just created and left pinned; a SECOND context created
        // earlier -- the editor's game-UI context, whose graph vehicle is built
        // later, while the EDITOR context is current -- would never receive the
        // flags at all, and a draw list produced without
        // ImGuiBackendFlags_RendererHasTextures carries no Textures array for
        // NewFrameTexUpdates to upload from: the atlas would never reach the
        // GPU and every draw would sample nothing.
        if (!imguiContext)
            return;
        ImGuiContext* const target = static_cast<ImGuiContext*>(imguiContext);
        ImGuiContext* const prev   = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(target);
        InstallBackendIdentity();
        ImGui::SetCurrentContext(prev);
    }

    bool ImGuiNri::Init(NriDevice& device, NriPipelineCache& pipelines,
                        std::span<const std::uint8_t> vs, std::span<const std::uint8_t> ps)
    {
        m_device    = &device;
        m_pipelines = &pipelines;
        m_vs        = vs;
        m_ps        = ps;

        if (m_vs.empty() || m_ps.empty())
        {
            ARC_ERROR("[nri-graph] ImGuiNri: the imgui_vs/imgui_ps bins are missing -- the HUD "
                      "cannot be drawn");
            return false;
        }

        // Sets the backend flags on whatever context is CURRENT. HasTextures
        // is the 1.92 protocol NewFrameTexUpdates implements; HasVtxOffset is
        // what lets one concatenated vertex buffer serve every draw list
        // (DrawIndexedDesc::baseVertex).
        //
        // The host's ImGuiLayer has already created and pinned its context by
        // the time this runs on an ordinary boot, so this OVERWRITES the
        // backend name and re-ORs flags ImGuiLayer's own Init already set --
        // both harmless, the name is cosmetic and the flags are idempotent.
        // It fires again, on that same still-live context, every time the
        // graph vehicle is rebuilt (a project switch tears down and recreates
        // NriGraphContext, and with it this node), which is the other reason
        // the overwrite has to be harmless rather than merely convenient --
        // there is exactly one ImGui renderer backend in the process today,
        // not two. The guard is for a host that builds the vehicle before any
        // context exists: ImGui::GetIO() would assert on a null GImGui.
        if (ImGui::GetCurrentContext() == nullptr)
        {
            ARC_WARN("[nri-graph] ImGuiNri::Init ran with no current ImGui context -- the backend "
                     "flags were not installed. The HUD will not draw until a context exists. "
                     "AdoptContext() is the explicit fix.");
        }
        else
        {
            InstallBackendIdentity();
        }

        // The vertex input, in MEMBERS (fill contract, rule 2). Semantic names
        // are imgui.hlsl's VSInput; the vk.locations are dxc's declaration
        // order for that same struct.
        m_attributes[0].d3d.semanticName = "POSITION";
        m_attributes[0].vk.location      = 0;
        m_attributes[0].offset           = offsetof(ImDrawVert, pos);
        m_attributes[0].format           = nri::Format::RG32_SFLOAT;
        m_attributes[1].d3d.semanticName = "TEXCOORD";
        m_attributes[1].vk.location      = 1;
        m_attributes[1].offset           = offsetof(ImDrawVert, uv);
        m_attributes[1].format           = nri::Format::RG32_SFLOAT;
        m_attributes[2].d3d.semanticName = "COLOR";
        m_attributes[2].vk.location      = 2;
        m_attributes[2].offset           = offsetof(ImDrawVert, col);
        // RGBA8_UNORM, not SFLOAT: ImDrawVert::col is a packed ImU32 the
        // shader multiplies straight into the sampled texel.
        m_attributes[2].format           = nri::Format::RGBA8_UNORM;

        m_stream.bindingSlot = 0;
        m_stream.stepRate    = nri::VertexStreamStepRate::PER_VERTEX;
        m_stream.stride      = (std::uint16_t)sizeof(ImDrawVert);

        m_vertexInput.attributes   = m_attributes;
        m_vertexInput.attributeNum = (std::uint8_t)std::size(m_attributes);
        m_vertexInput.streams      = &m_stream;
        m_vertexInput.streamNum    = 1;

        return CreateSampler() && CreateLayout() && CreatePool();
    }

    bool ImGuiNri::CreateSampler()
    {
        const nri::CoreInterface& core = m_device->Core();

        // LINEAR + clamp -- ImGui's own default, and what keeps the font atlas
        // from bleeding at its edges.
        nri::SamplerDesc samplerDesc = {};
        samplerDesc.filters.min  = nri::Filter::LINEAR;
        samplerDesc.filters.mag  = nri::Filter::LINEAR;
        samplerDesc.filters.mip  = nri::Filter::LINEAR;
        samplerDesc.addressModes = { nri::AddressMode::CLAMP_TO_EDGE, nri::AddressMode::CLAMP_TO_EDGE,
                                     nri::AddressMode::CLAMP_TO_EDGE };
        samplerDesc.mipMax       = 16.0f;
        if (!ARC_NRI_CHECK(core.CreateSampler(m_device->Device(), samplerDesc, m_sampler)) || !m_sampler)
        {
            ARC_ERROR("[nri-graph] ImGuiNri: sampler creation failed");
            return false;
        }
        return true;
    }

    bool ImGuiNri::CreateLayout()
    {
        // imgui.hlsl's register map: b0 constants (a VK PUSH CONSTANT block --
        // see THE ROOT-CONSTANT FINDING in the header), t0 texture, s0
        // sampler.
        //
        // Root constants live in rootRegisterSpace (0 -> b0/space0 on D3D12; a
        // push-constant block, which has no space at all, on Vulkan) and the
        // texture/sampler set is registerSpace 0 to match the shader's
        // implicit space0. Those two CAN share space 0 because
        // rootDescriptorNum and rootSamplerNum are both ZERO -- NRI's
        // Source/Validation/DeviceVal.hpp only refuses the collision when one
        // of them is nonzero, which is also why the sampler here is a
        // descriptor-set entry rather than a root (static) sampler.
        //
        // VERTEX only for the root constants: imgui.hlsl reads g_scale/
        // g_translate in vs_main and nowhere else. On D3D12 visibility is a
        // hard root-signature property, and narrowing it is free because the
        // fragment stage genuinely never reads the block.
        nri::RootConstantDesc rootConstant = {};
        rootConstant.registerIndex = 0;
        rootConstant.size          = sizeof(ImGuiRootConstants);
        rootConstant.shaderStages  = nri::StageBits::VERTEX_SHADER;

        nri::DescriptorRangeDesc ranges[2] = {};
        ranges[0].baseRegisterIndex = 0;
        ranges[0].descriptorNum     = 1;
        ranges[0].descriptorType    = nri::DescriptorType::TEXTURE;
        ranges[0].shaderStages      = nri::StageBits::FRAGMENT_SHADER;
        ranges[1].baseRegisterIndex = 0;
        ranges[1].descriptorNum     = 1;
        ranges[1].descriptorType    = nri::DescriptorType::SAMPLER;
        ranges[1].shaderStages      = nri::StageBits::FRAGMENT_SHADER;

        nri::DescriptorSetDesc setDesc = {};
        setDesc.registerSpace = 0;
        setDesc.ranges        = ranges;
        setDesc.rangeNum      = 2;

        // Value-initialized then assigned field by field: NriPipelineCache's
        // DEDUP CONTRACT (the desc is compared byte-wise, so its padding has
        // to be zeroed).
        nri::PipelineLayoutDesc layoutDesc = {};
        layoutDesc.rootRegisterSpace = 0;
        layoutDesc.rootConstants     = &rootConstant;
        layoutDesc.rootConstantNum   = 1;
        layoutDesc.descriptorSets    = &setDesc;
        layoutDesc.descriptorSetNum  = 1;
        layoutDesc.shaderStages      = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;

        m_layoutId = m_pipelines->RegisterLayout(layoutDesc);
        if (!m_pipelines->Layout(m_layoutId))
        {
            ARC_ERROR("[nri-graph] ImGuiNri: pipeline layout registration failed");
            return false;
        }
        return true;
    }

    bool ImGuiNri::CreatePool()
    {
        const nri::CoreInterface& core = m_device->Core();

        // ONE SET PER TEXTURE, not per frame slot: a set here binds {that
        // texture's view, the shared sampler} and is written exactly once, at
        // the moment the texture is first seen. Nothing rewrites a set the GPU
        // might be reading -- the one path that reuses one (AcquireSet's
        // recycling of a retired set) waits kSwapchainFramesInFlight recorded
        // frames first.
        nri::DescriptorPoolDesc poolDesc = {};
        poolDesc.descriptorSetMaxNum = kMaxTextures;
        poolDesc.textureMaxNum       = kMaxTextures;
        poolDesc.samplerMaxNum       = kMaxTextures;
        if (!ARC_NRI_CHECK(core.CreateDescriptorPool(m_device->Device(), poolDesc, m_pool)) || !m_pool)
        {
            ARC_ERROR("[nri-graph] ImGuiNri: descriptor pool creation failed");
            return false;
        }

        // RESERVED, not merely sized: EnsureEntry returns an Entry* into this
        // vector and the caller uses it after the push, so a reallocation
        // would dangle it. The cap IS the reservation, so it never grows past
        // it.
        m_textures.reserve(kMaxTextures);
        return true;
    }

    nri::DescriptorSet* ImGuiNri::AcquireSet(const nri::CoreInterface& core)
    {
        // A retired set is reusable once the submission that last bound it has
        // retired. kSwapchainFramesInFlight recorded frames is exactly that
        // bound: frame N records only after frame N - kSwapchainFramesInFlight
        // completed (the pacing wait inside NriSwapChain::AcquireNextTexture),
        // which is the same argument Batch2DNode's constant arena and the pick
        // readback both rest on.
        for (std::size_t i = 0; i < m_retired.size(); ++i)
        {
            if (m_recordCount - m_retired[i].retiredAt < kSwapchainFramesInFlight)
                continue;
            nri::DescriptorSet* set = m_retired[i].set;
            m_retired.erase(m_retired.begin() + (std::ptrdiff_t)i);
            return set;
        }

        if (m_setsAllocated >= kMaxTextures)
        {
            if (!m_warnedPoolFull)
            {
                m_warnedPoolFull = true;
                GraphError("ImGuiNri: more than " + std::to_string(kMaxTextures)
                           + " concurrent ImGui textures -- raise ImGuiNri::kMaxTextures. Draws "
                             "using the extra textures are dropped.");
            }
            return nullptr;
        }

        nri::PipelineLayout* layout = m_pipelines->Layout(m_layoutId);
        nri::DescriptorSet*  set    = nullptr;
        if (!layout
            || !ARC_NRI_CHECK(core.AllocateDescriptorSets(*m_pool, *layout, 0, &set, 1, 0))
            || !set)
        {
            GraphError("ImGuiNri: descriptor set allocation failed");
            return nullptr;
        }
        ++m_setsAllocated;
        return set;
    }

    ImGuiNri::Entry* ImGuiNri::EnsureEntry(const nri::CoreInterface& core, nri::Texture* texture,
                                            ImTextureData* owner)
    {
        if (!texture)
            return nullptr;
        for (Entry& entry : m_textures)
            if (entry.texture == texture)
                return &entry;

        nri::TextureViewDesc viewDesc = {};
        viewDesc.texture  = texture;
        viewDesc.type     = nri::TextureView::TEXTURE;
        // The texture's ACTUAL format, read back from NRI -- never assumed.
        // A user texture (the ImTextureID convention's case b) is the host's
        // and we know nothing about it beyond the handle.
        viewDesc.format   = core.GetTextureDesc(*texture).format;
        viewDesc.mipNum   = 1;
        viewDesc.layerNum = 1;

        nri::Descriptor* view = nullptr;
        if (!ARC_NRI_CHECK(core.CreateTextureView(viewDesc, view)) || !view)
        {
            GraphError("ImGuiNri: the shader-resource view over an ImGui texture could not be created");
            return nullptr;
        }

        nri::DescriptorSet* set = AcquireSet(core);
        if (!set)
        {
            core.DestroyDescriptor(view);   // never bound, nothing can be reading it
            return nullptr;
        }

        // Written ONCE, here. Both ranges at the same time, and never again --
        // see CreatePool.
        const nri::Descriptor* boundView    = view;
        const nri::Descriptor* boundSampler = m_sampler;
        nri::UpdateDescriptorRangeDesc updates[2] = {};
        updates[0].descriptorSet = set;
        updates[0].rangeIndex    = 0;
        updates[0].descriptors   = &boundView;
        updates[0].descriptorNum = 1;
        updates[1].descriptorSet = set;
        updates[1].rangeIndex    = 1;
        updates[1].descriptors   = &boundSampler;
        updates[1].descriptorNum = 1;
        core.UpdateDescriptorRanges(updates, 2);

        m_textures.push_back(Entry{ owner, texture, view, set, /*owned=*/owner != nullptr });
        return &m_textures.back();
    }

    void ImGuiNri::ReleaseEntry(Entry& entry, Graveyard* graveyard, std::uint64_t fence)
    {
        // TWO DISPOSAL DISCIPLINES, one eviction. Which one applies is the
        // CALLER's knowledge, not this function's:
        //
        //   graveyard != null -- DEFERRED. An earlier submitted frame may still
        //     be reading these, so they are BURIED at `fence`. The lane is a
        //     PARAMETER (Task 8-pre) rather than something read off the device,
        //     so every deferred disposal this backend makes lands in the OWNING
        //     CONTEXT's one lane, in burial order. See NewFrameTexUpdates in
        //     the header for why the device's graveyard is the wrong answer
        //     once one device carries two contexts.
        //
        //   graveyard == null -- IMMEDIATE. The caller has already made the GPU
        //     idle, so nothing can be reading them and there is nothing to
        //     defer BEHIND. Used by InvalidateUserTextureNow, whose whole
        //     purpose is to get the view destroyed while the texture it views
        //     is still ALIVE -- an ordering no deferred burial can offer when
        //     the texture's owner is a DIFFERENT context with a different lane.
        const nri::CoreInterface* core = &m_device->Core();
        if (entry.view)
        {
            nri::Descriptor* view = entry.view;
            if (graveyard)
                graveyard->Bury(fence, [core, view] { core->DestroyDescriptor(view); });
            else
                core->DestroyDescriptor(view);
        }
        if (entry.owned && entry.texture)
        {
            nri::Texture* texture = entry.texture;
            if (graveyard)
                graveyard->Bury(fence, [core, texture] { core->DestroyTexture(texture); });
            else
                core->DestroyTexture(texture);
        }
        // RETIRED EITHER WAY, and deliberately: retirement is about REUSE, not
        // lifetime. NRI cannot free a single set, so the only thing that can be
        // done with one is rewrite it -- and AcquireSet's age gate is what makes
        // that safe. An idle device would make immediate reuse safe too, but the
        // gate costs nothing and keeping ONE recycling rule is worth more than
        // recovering one descriptor set a frame earlier. The set names a
        // destroyed view until it is rewritten, which is exactly the state
        // DestroyTexture has always left one in: nothing can bind it, because
        // the entry that pointed at it is gone.
        if (entry.set)
            m_retired.push_back(RetiredSet{ entry.set, m_recordCount });

        entry.view    = nullptr;
        entry.texture = nullptr;
        entry.set     = nullptr;
        entry.owner   = nullptr;
        entry.owned   = false;
    }

    void ImGuiNri::DestroyTexture(ImTextureData* tex, Graveyard& graveyard, std::uint64_t fence)
    {
        // Evict the cache entry BEFORE the texture is buried -- an ABA rule,
        // and load-bearing: NRI does not ref-count, so
        // once the burial runs NRI is free to hand the vacated address to a
        // replacement, and a pointer-keyed cache would report a HIT on a
        // descriptor naming freed memory.
        for (std::size_t i = 0; i < m_textures.size(); ++i)
        {
            if (m_textures[i].owner != tex)
                continue;
            ReleaseEntry(m_textures[i], &graveyard, fence);
            m_textures.erase(m_textures.begin() + (std::ptrdiff_t)i);
            break;
        }

        // Tolerated on a texture we never serviced (an unserviced WantCreate
        // has no entry to evict) -- exactly what Release()'s platform-list
        // walk relies on.
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
    }

    bool ImGuiNri::HasEntryFor(nri::Texture* texture) const noexcept
    {
        if (!texture)
            return false;
        for (const Entry& entry : m_textures)
            if (entry.texture == texture)
                return true;
        return false;
    }

    bool ImGuiNri::EnsureUserTexture(nri::Texture* texture)
    {
        if (!m_device || !texture)
            return false;
        // owner = null IS what makes it a user texture: this backend creates
        // the view + set and owns neither the texture nor its lifetime (Entry::
        // owned stays false, so nothing here will ever bury it).
        return EnsureEntry(m_device->Core(), texture, /*owner=*/nullptr) != nullptr;
    }

    bool ImGuiNri::EvictUserEntry(nri::Texture* texture, Graveyard* graveyard,
                                   std::uint64_t fence, const char* who)
    {
        if (!m_device || !texture)
            return false;

        for (std::size_t i = 0; i < m_textures.size(); ++i)
        {
            if (m_textures[i].texture != texture)
                continue;

            if (m_textures[i].owner != nullptr)
            {
                // An ImTextureData-owned entry. The 1.92 protocol owns its
                // lifetime and DestroyTexture is its path -- which also has to
                // bury the TEXTURE and stamp the ImTextureData's status, and
                // this hook deliberately does neither. Refuse rather than
                // half-dispose it.
                GraphError(std::string("ImGuiNri::") + who + ": that texture is owned by an "
                           "ImTextureData -- the 1.92 protocol destroys it (WantDestroy), not "
                           "this hook. Nothing was invalidated.");
                return false;
            }

            // EXACTLY DestroyTexture's disposal, minus the texture (it is the
            // caller's). ReleaseEntry decides deferred-vs-immediate off
            // `graveyard`; the descriptor set is RETIRED either way.
            ReleaseEntry(m_textures[i], graveyard, fence);
            m_textures.erase(m_textures.begin() + (std::ptrdiff_t)i);
            return true;
        }

        // No entry: nothing has drawn this texture yet. ROUTINE -- the caller
        // contract is to invalidate unconditionally BEFORE every destroy, and a
        // resize before the first frame legitimately finds nothing.
        return false;
    }

    bool ImGuiNri::InvalidateUserTexture(nri::Texture* texture, Graveyard& graveyard,
                                          std::uint64_t fence)
    {
        return EvictUserEntry(texture, &graveyard, fence, "InvalidateUserTexture");
    }

    bool ImGuiNri::InvalidateUserTextureNow(nri::Texture* texture)
    {
        if (!m_device || !texture)
            return false;

        // ===== WHY THIS IDLES, AND WHY IT IS THE RESIZE PATH'S VARIANT =====
        // The deferred hook buries the view in the CALLER's lane. When the
        // texture belongs to a DIFFERENT context -- which is the whole editor
        // case, a viewport output drawn by the chrome backend -- that lane is
        // not the one the texture is destroyed through, and there is no
        // ordering between two graveyards. Called after the owner's resize, the
        // burial would run one to two frames AFTER the texture was destroyed:
        // DestroyDescriptor over an already-destroyed resource, every resize,
        // for the one descriptor that inherently spans both contexts. That is
        // the exact inversion the graph's own teardown ordering exists to
        // prevent (NriGraphContext.cpp's offscreen burial block).
        //
        // Destroying the view HERE, behind an idle, restores the invariant
        // instead of documenting an exception -- but only if the caller runs
        // this BEFORE the owner destroys the texture, which is what
        // NriGraphContext::ResizeOffscreen's prescribed sequence does.
        //
        // The idle is OURS rather than a stated precondition: an unenforceable
        // "the caller has already idled" is exactly the kind of contract this
        // seam has been burned by.
        //
        // WHAT IT ACTUALLY COSTS, stated honestly because the desk-watch list
        // on ResizeOffscreen prescribes the worst case for it: this is a SECOND
        // full device idle on a resize, not a free ride on an existing one.
        // ResizeOffscreen issues exactly ONE of its own, and it does so AFTER
        // this call, so the pair is a doubling -- 1 -> 2 device idles per
        // resize. A resize is NOT a rare event during the drag-storm that same
        // list prescribes: a panel dragged by the mouse changes size every
        // frame, so that is two idles per frame for the duration of the drag,
        // and it is why "frame time / hitching" is on the watch list at all.
        //
        // It is still the right trade. There is no cheaper way to get the view
        // destroyed while its texture is alive when the two die through
        // different lanes, the storm is a desk motion rather than a shipping
        // one, and the alternative -- an unordered cross-graveyard disposal --
        // is a correctness defect rather than a cost. What the CALLER owes in
        // exchange is the size guard: these three lines belong under the "did
        // the size actually change" test, because ResizeOffscreen's own no-op
        // guard sits INSIDE it, i.e. after this idle has already happened.
        const nri::CoreInterface& core = m_device->Core();
        if (core.DeviceWaitIdle)
            (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&m_device->Device()));

        return EvictUserEntry(texture, /*graveyard=*/nullptr, /*fence=*/0,
                              "InvalidateUserTextureNow");
    }

    void ImGuiNri::UpdateTexture(ImTextureData* tex, Graveyard& graveyard, std::uint64_t fence)
    {
        const nri::CoreInterface& core = m_device->Core();

        if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates)
        {
            // THE HELPER IS WHOLE-SUBRESOURCE, so a WantUpdates here is a full
            // re-upload rather than a partial one. Region staging is a later
            // optimisation, and skipping it is deliberate
            // rather than an oversight.
            if (tex->BytesPerPixel != 4)
            {
                if (!m_warnedFormat)
                {
                    m_warnedFormat = true;
                    GraphError("ImGuiNri: an ImGui texture is not 4 bytes/pixel (RGBA32) -- this "
                               "backend uploads RGBA8_UNORM only. The texture is skipped.");
                }
                return;
            }

            nri::Texture* texture = nullptr;
            if (tex->Status == ImTextureStatus_WantUpdates)
            {
                for (const Entry& entry : m_textures)
                    if (entry.owner == tex)
                        texture = entry.texture;
                if (!texture)
                {
                    // An update for a texture we never created. Treat it as a
                    // create rather than dropping the frame's glyphs.
                    tex->SetStatus(ImTextureStatus_WantCreate);
                }
            }

            if (!texture)
            {
                // RGBA8_UNORM (NOT sRGB): ImGui colours are display-referred
                // and the target is the display-referred backbuffer.
                nri::TextureDesc textureDesc = {};
                textureDesc.type      = nri::TextureType::TEXTURE_2D;
                textureDesc.usage     = nri::TextureUsageBits::SHADER_RESOURCE;
                textureDesc.format    = nri::Format::RGBA8_UNORM;
                textureDesc.width     = (nri::Dim_t)tex->Width;
                textureDesc.height    = (nri::Dim_t)tex->Height;
                textureDesc.depth     = 1;
                textureDesc.mipNum    = 1;
                textureDesc.layerNum  = 1;
                textureDesc.sampleNum = 1;
                if (!ARC_NRI_CHECK(core.CreateCommittedTexture(m_device->Device(),
                                                                nri::MemoryLocation::DEVICE,
                                                                0.0f, textureDesc, texture))
                    || !texture)
                {
                    GraphError("ImGuiNri: an ImGui texture could not be created");
                    return;
                }
                core.SetDebugName(texture, "nri-graph imgui texture");
            }

            // Through NRI's OWN helper, exactly like Batch2DNode's white texel
            // and a registered material's textures: the ring cannot stage a
            // texture (it carries CONSTANT|VERTEX|INDEX buffers), and a
            // hand-rolled staging buffer would mean this file records a
            // barrier -- which the phase's rule forbids. UploadData submits
            // and waits internally, which is why this whole function is
            // DECLARATION-time work.
            nri::HelperInterface helper = {};
            if (!ARC_NRI_CHECK(nriGetInterface(m_device->Device(), NRI_INTERFACE(nri::HelperInterface),
                                                &helper)))
            {
                GraphError("ImGuiNri: HelperInterface unavailable -- the ImGui texture cannot be "
                           "uploaded");
                if (tex->Status == ImTextureStatus_WantCreate)
                    core.DestroyTexture(texture);   // never bound, never submitted
                return;
            }

            nri::TextureSubresourceUploadDesc subresource = {};
            subresource.slices     = tex->GetPixels();
            subresource.sliceNum   = 1;
            subresource.rowPitch   = (std::uint32_t)tex->GetPitch();
            subresource.slicePitch = (std::uint32_t)tex->GetPitch() * (std::uint32_t)tex->Height;

            nri::TextureUploadDesc upload = {};
            upload.subresources = &subresource;
            upload.texture      = texture;
            upload.after        = { nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE,
                                    nri::StageBits::FRAGMENT_SHADER };
            upload.planes       = nri::PlaneBits::ALL;
            if (!ARC_NRI_CHECK(helper.UploadData(*m_device->GraphicsQueue(), &upload, 1, nullptr, 0)))
            {
                GraphError("ImGuiNri: the ImGui texture upload failed");
                if (tex->Status == ImTextureStatus_WantCreate)
                    core.DestroyTexture(texture);
                return;
            }

            if (tex->Status == ImTextureStatus_WantCreate)
            {
                // The view + descriptor set are built here, at declaration
                // time, rather than lazily at record time -- there is nothing
                // to gain by deferring them and the failure is loud where it
                // can still be reported before a command buffer is open.
                if (!EnsureEntry(core, texture, tex))
                {
                    const nri::CoreInterface* c = &core;
                    graveyard.Bury(fence, [c, texture] { c->DestroyTexture(texture); });
                    return;   // already reported
                }
                // THE ImTextureID CONVENTION (§7.3): the raw backend
                // texture handle, cast through intptr_t.
                tex->SetTexID((ImTextureID)(intptr_t)texture);
            }
            tex->SetStatus(ImTextureStatus_OK);
        }

        // THE UnusedFrames GUARD: ImGui only means a WantDestroy once the
        // texture has gone unused for a frame.
        if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0)
            DestroyTexture(tex, graveyard, fence);
    }

    void ImGuiNri::NewFrameTexUpdates(ImDrawData* drawData, Graveyard& graveyard,
                                       std::uint64_t fence)
    {
        if (!m_device || !drawData || drawData->Textures == nullptr)
            return;
        for (ImTextureData* tex : *drawData->Textures)
            if (tex->Status != ImTextureStatus_OK)
                UpdateTexture(tex, graveyard, fence);
    }

    void ImGuiNri::RenderDrawData(ImDrawData* drawData, RenderGraphNodeContext& context,
                                  RgTexture target)
    {
        if (!m_device || !drawData || drawData->DisplaySize.x <= 0.0f ||
            drawData->DisplaySize.y <= 0.0f)
            return;

        // The clock the retired-set gate is measured in. Bumped even on a
        // frame that draws nothing below, because a frame that recorded is a
        // frame that passed the pacing wait.
        ++m_recordCount;

        if (drawData->TotalVtxCount <= 0)
            return;

        const nri::CoreInterface& core = context.core;

        nri::Texture* targetTexture = context.Resolve(target);
        if (!targetTexture)
        {
            GraphError("ImGuiNri: the node could not resolve its target");
            return;
        }
        const nri::TextureDesc& targetDesc = core.GetTextureDesc(*targetTexture);

        nri::PipelineLayout* layout = m_pipelines->Layout(m_layoutId);
        if (!layout)
        {
            GraphError("ImGuiNri: the pipeline layout is gone -- nothing recorded");
            return;
        }

        // ---------------------------------------------------------------
        // Vertex + index streams, straight into this frame's ring slot,
        // ALLOCATED HERE at RECORD time. Load-bearing: the vehicle calls
        // ring.BeginFrame(slot) AFTER the frame is declared, so anything
        // allocated during setup lands in the PREVIOUS frame's slot and is
        // overwritten while the GPU still reads it (Batch2DNode's matching
        // note; DeclareGraphFrame's RING FOOTGUN block).
        //
        // The draw lists are concatenated into one VB and one IB, which is
        // what ImGuiBackendFlags_RendererHasVtxOffset buys: every draw indexes
        // the same pair through baseVertex/baseIndex. Nothing retains a
        // pointer into ImGui's lists past this function.
        // ---------------------------------------------------------------
        const std::uint64_t vertexBytes = (std::uint64_t)drawData->TotalVtxCount * sizeof(ImDrawVert);
        const std::uint64_t indexBytes  = (std::uint64_t)drawData->TotalIdxCount * sizeof(ImDrawIdx);

        // kVertexAlign, NOT sizeof(ImDrawVert) -- see its definition above.
        const NriUploadRing::Alloc vertexAlloc = context.ring.Allocate(vertexBytes, kVertexAlign);
        const NriUploadRing::Alloc indexAlloc  = context.ring.Allocate(indexBytes, sizeof(ImDrawIdx));
        if (!vertexAlloc.cpu || !indexAlloc.cpu)
        {
            if (!m_warnedRing)
            {
                m_warnedRing = true;
                GraphError("ImGuiNri: the upload ring could not fit this frame's HUD geometry ("
                           + std::to_string(vertexBytes + indexBytes)
                           + " bytes) -- the HUD is dropped this frame. Raise "
                             "kUploadRingBytesPerFrame in NriGraphContext.cpp.");
            }
            return;
        }

        {
            auto* vtxDst = static_cast<std::uint8_t*>(vertexAlloc.cpu);
            auto* idxDst = static_cast<std::uint8_t*>(indexAlloc.cpu);
            for (const ImDrawList* list : drawData->CmdLists)
            {
                const std::size_t vbytes = (std::size_t)list->VtxBuffer.Size * sizeof(ImDrawVert);
                const std::size_t ibytes = (std::size_t)list->IdxBuffer.Size * sizeof(ImDrawIdx);
                std::memcpy(vtxDst, list->VtxBuffer.Data, vbytes);
                std::memcpy(idxDst, list->IdxBuffer.Data, ibytes);
                vtxDst += vbytes;
                idxDst += ibytes;
            }
        }

        // Root constants: the VS outputs pos*scale + translate directly, so
        // scale.y is NEGATIVE (ImGui's y-down -> clip y-up). DisplayPos is
        // (0,0) for single-viewport apps but handled for generality. The
        // transform is imgui.hlsl's contract, not a choice made here.
        const float L = drawData->DisplayPos.x;
        const float T = drawData->DisplayPos.y;
        ImGuiRootConstants push{};
        push.scale[0] =  2.0f / drawData->DisplaySize.x;
        push.scale[1] = -2.0f / drawData->DisplaySize.y;
        push.translate[0] = -1.0f - L * push.scale[0];
        push.translate[1] =  1.0f - T * push.scale[1];

        NriPipelineCache::GraphicsKey key = {};
        key.shaderPairId    = kShaderPairId;
        key.layoutId        = m_layoutId;
        // Read back from the resolved target rather than assumed: NRI resolves
        // a swapchain's channel order instead of letting anyone pin it
        // (NriSwapChain::Format), and a pipeline bakes its attachment formats
        // at creation.
        key.colorFormats[0] = targetDesc.format;
        key.colorCount      = 1;
        key.depthFormat     = nri::Format::UNKNOWN;
        key.topology        = nri::Topology::TRIANGLE_LIST;
        // SrcAlpha/InvSrcAlpha + One/InvSrcAlpha -- ImGui's own blend state.
        key.blend           = NriPipelineCache::GraphicsKey::Blend::AlphaOver;

        // `stages` lives in THIS frame, which encloses GetGraphics -- the fill
        // contract's rule 2, same as m_vertexInput being a member.
        nri::ShaderDesc stages[2] = {};
        stages[0].stage          = nri::StageBits::VERTEX_SHADER;
        stages[0].bytecode       = m_vs.data();
        stages[0].size           = m_vs.size();
        stages[0].entryPointName = kVsEntry;
        stages[1].stage          = nri::StageBits::FRAGMENT_SHADER;
        stages[1].bytecode       = m_ps.data();
        stages[1].size           = m_ps.size();
        stages[1].entryPointName = kPsEntry;

        nri::Pipeline* pipeline = m_pipelines->GetGraphics(key, [&](nri::GraphicsPipelineDesc& desc)
        {
            desc.vertexInput = &m_vertexInput;
            desc.shaders     = stages;
            desc.shaderNum   = 2;
            desc.rasterization.fillMode = nri::FillMode::SOLID;
            // ImGui emits both windings; culling would drop half the glyphs.
            desc.rasterization.cullMode = nri::CullMode::NONE;
        });
        if (!pipeline)
            return;   // already logged + latched by the cache

        core.CmdSetDescriptorPool(context.cmd, *m_pool);
        core.CmdSetPipelineLayout(context.cmd, nri::BindPoint::GRAPHICS, *layout);

        nri::SetRootConstantsDesc rootConstants = {};
        rootConstants.rootConstantIndex = 0;
        rootConstants.data              = &push;
        rootConstants.size              = sizeof(push);
        core.CmdSetRootConstants(context.cmd, rootConstants);

        core.CmdSetPipeline(context.cmd, *pipeline);

        nri::VertexBufferDesc vertexBuffer = {};
        vertexBuffer.buffer = vertexAlloc.buffer;
        vertexBuffer.offset = vertexAlloc.offset;
        vertexBuffer.stride = sizeof(ImDrawVert);
        core.CmdSetVertexBuffers(context.cmd, 0, &vertexBuffer, 1);
        core.CmdSetIndexBuffer(context.cmd, *indexAlloc.buffer, indexAlloc.offset,
                                nri::IndexType::UINT16);

        const ImVec2 clipOff   = drawData->DisplayPos;
        const ImVec2 clipScale = drawData->FramebufferScale;

        // Per ImDrawList, per ImDrawCmd, which is the walk ImGui's draw data
        // calls for. The executor already set a full-attachment VIEWPORT and
        // SCISSOR for this pass; the viewport is kept and the scissor is
        // re-set per command. CmdSetScissors is not a barrier, so a node is
        // allowed to issue it, and nothing is restored afterwards -- every
        // pass re-establishes its own state.
        std::uint32_t globalVtxOffset = 0;
        std::uint32_t globalIdxOffset = 0;
        nri::DescriptorSet* lastSet = nullptr;
        for (const ImDrawList* list : drawData->CmdLists)
        {
            for (int ci = 0; ci < list->CmdBuffer.Size; ++ci)
            {
                const ImDrawCmd* cmd = &list->CmdBuffer[ci];
                // v1 does not register user callbacks; assert none slipped in
                // (NRIImgui does not support them either -- §7.4 parity).
                IM_ASSERT(cmd->UserCallback == nullptr &&
                          "ImGuiNri: user draw callbacks unsupported");

                const float clipMinX = (cmd->ClipRect.x - clipOff.x) * clipScale.x;
                const float clipMinY = (cmd->ClipRect.y - clipOff.y) * clipScale.y;
                const float clipMaxX = (cmd->ClipRect.z - clipOff.x) * clipScale.x;
                const float clipMaxY = (cmd->ClipRect.w - clipOff.y) * clipScale.y;
                if (clipMaxX <= clipMinX || clipMaxY <= clipMinY)
                    continue;

                const std::uint32_t x0 = ClampEdge(clipMinX, targetDesc.width);
                const std::uint32_t y0 = ClampEdge(clipMinY, targetDesc.height);
                const std::uint32_t x1 = ClampEdge(clipMaxX, targetDesc.width);
                const std::uint32_t y1 = ClampEdge(clipMaxY, targetDesc.height);
                if (x1 <= x0 || y1 <= y0)
                    continue;   // entirely off-surface after clamping

                Entry* entry = EnsureEntry(core, (nri::Texture*)(intptr_t)cmd->GetTexID(), nullptr);
                if (!entry || !entry->set)
                    continue;   // already reported; a bound-nothing draw is worse than no draw

                if (entry->set != lastSet)
                {
                    lastSet = entry->set;
                    nri::SetDescriptorSetDesc setDesc = {};
                    setDesc.setIndex      = 0;
                    setDesc.descriptorSet = entry->set;
                    core.CmdSetDescriptorSet(context.cmd, setDesc);
                }

                const nri::Rect scissor = { (std::int16_t)x0, (std::int16_t)y0,
                                            (nri::Dim_t)(x1 - x0), (nri::Dim_t)(y1 - y0) };
                core.CmdSetScissors(context.cmd, &scissor, 1);

                nri::DrawIndexedDesc draw = {};
                draw.indexNum    = cmd->ElemCount;
                draw.instanceNum = 1;
                draw.baseIndex   = globalIdxOffset + cmd->IdxOffset;
                draw.baseVertex  = (std::int32_t)(globalVtxOffset + cmd->VtxOffset);
                core.CmdDrawIndexed(context.cmd, draw);
            }
            globalVtxOffset += (std::uint32_t)list->VtxBuffer.Size;
            globalIdxOffset += (std::uint32_t)list->IdxBuffer.Size;
        }
    }

    void ImGuiNri::Release(Graveyard& graveyard, std::uint64_t fence)
    {
        if (!m_device)
            return;

        // WALK THE PLATFORM TEXTURE LIST, not merely our own: that is what
        // catches an ImTextureData we never serviced -- a stuck
        // WantCreate that arrived after the last frame, or a WantDestroy with
        // UnusedFrames == 0 -- and leaves its status Destroyed so a later
        // re-Init is clean. The RefCount == 1 guard is the dx11 reference's
        // "only the platform list holds a ref", i.e. only destroy what this
        // backend owns.
        //
        // PINNED TO THE CONTEXT THIS BACKEND ADOPTED, not
        // to whatever happens to be current. The walk DISOWNS an ImTextureData
        // -- invalid TexID, plus a Destroyed request ImGui bounces back to
        // WantCreate while the CPU pixels are live -- so run against a FOREIGN
        // context it asks another backend to re-create an atlas that backend
        // still holds a live entry for. Unreachable while one process held one
        // backend; reachable the moment a host holds two (the editor's chrome
        // backend and its game backend, released back to back from a teardown
        // that pins neither). m_imguiContext carries the full account, the
        // one-backend-per-context invariant this rests on, and the caller
        // obligation the pin creates.
        if (ImGuiContext* const owned = static_cast<ImGuiContext*>(m_imguiContext))
        {
            ImGuiContext* const prev = ImGui::GetCurrentContext();
            ImGui::SetCurrentContext(owned);
            for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
                if (tex->RefCount == 1)
                    DestroyTexture(tex, graveyard, fence);
            ImGui::SetCurrentContext(prev);
        }
        else if (ImGui::GetCurrentContext() != nullptr)
        {
            // Init ran with no context at all (already WARNed there). Fall back
            // to the pre-Task-9 behaviour rather than skipping the walk: a
            // process with no backend identity installed also has no second
            // context to contaminate.
            for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
                if (tex->RefCount == 1)
                    DestroyTexture(tex, graveyard, fence);
        }

        // Anything the walk above did not reach (a USER texture's view + set,
        // which no ImTextureData owns).
        for (Entry& entry : m_textures)
            ReleaseEntry(entry, &graveyard, fence);
        m_textures.clear();
        m_retired.clear();

        const nri::CoreInterface* core = &m_device->Core();
        if (m_pool)
        {
            graveyard.Bury(fence, [core, p = m_pool] { core->DestroyDescriptorPool(p); });
            m_pool = nullptr;
            m_setsAllocated = 0;
        }
        if (m_sampler)
        {
            graveyard.Bury(fence, [core, d = m_sampler] { core->DestroyDescriptor(d); });
            m_sampler = nullptr;
        }
    }

    ImGuiNri::~ImGuiNri()
    {
        if (!m_device || (!m_sampler && !m_pool && m_textures.empty()))
            return;

        ARC_WARN("[nri-graph] ImGuiNri destroyed with live NRI objects -- either Init() failed part "
                 "way (an ERROR above says which step) or its owner never called Release(). "
                 "Destroying directly behind a DeviceWaitIdle.");
        const nri::CoreInterface& core = m_device->Core();
        (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&m_device->Device()));
        for (const Entry& entry : m_textures)
        {
            if (entry.view)             core.DestroyDescriptor(entry.view);
            if (entry.owned && entry.texture) core.DestroyTexture(entry.texture);
        }
        m_textures.clear();
        m_retired.clear();
        if (m_pool)    core.DestroyDescriptorPool(m_pool);
        if (m_sampler) core.DestroyDescriptor(m_sampler);
        m_pool = nullptr;
        m_sampler = nullptr;
        m_setsAllocated = 0;
    }
}
