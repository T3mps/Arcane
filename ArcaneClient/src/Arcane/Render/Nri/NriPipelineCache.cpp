// NriPipelineCache -- see the header for what it caches, why the attachment
// formats are part of a pipeline's identity, the fill callback's contract, and
// why Clear() rather than the destructor is the release path.
//
// Same include-order rule as every file in this directory (NriCommon.hpp): NRI
// headers first, because Extensions/NRIDeviceCreation.h declares
// nri::Message::ERROR and <windows.h> (via Arcane/Base/Log.hpp -> spdlog)
// #defines ERROR via wingdi.h.
#include <NRI.h>

#include "NriPipelineCache.hpp"

#include "NriCommon.hpp"

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>

#undef ERROR

#include <cstring>
#include <string>

namespace Arcane
{
    namespace
    {
        // The same tagged seam RenderGraphExec.cpp reports through, for the
        // same reason: these are the CACHE's own refusals (a caller contract
        // broken), not an NRI call's result -- those go through ARC_NRI_CHECK.
        // Both land in the RenderErrorCount() latch the 0/0 gate reads, which
        // is what makes a graph desk run's exit code meaningful.
        void CacheError(const std::string& text)
        {
            RenderErrorLatch::Instance().NoteError("nri-graph", text.c_str());
        }

        // Byte-wise equality over two POD arrays -- see RegisterLayout's
        // DEDUP CONTRACT in the header for why this is exact for descs built
        // the way this tree builds them, and harmless (a miss, never a wrong
        // hit) when it is not.
        template <typename T>
        bool SameArray(const std::vector<T>& mine, const T* theirs, std::uint32_t count) noexcept
        {
            if (mine.size() != count)
                return false;
            if (count == 0)
                return true;
            if (theirs == nullptr)
                return false;
            return std::memcmp(mine.data(), theirs, sizeof(T) * count) == 0;
        }

        // The blend state the cache stamps into every colour attachment for a
        // given key. One place, so two pipelines that key the same blend
        // cannot end up with different blend state.
        void ApplyBlend(nri::ColorAttachmentDesc& color, NriPipelineCache::GraphicsKey::Blend blend) noexcept
        {
            using Blend = NriPipelineCache::GraphicsKey::Blend;

            color.colorWriteMask = nri::ColorWriteBits::RGBA;
            color.blendEnabled   = (blend != Blend::Opaque);
            color.colorBlend     = {};
            color.alphaBlend     = {};
            if (!color.blendEnabled)
                return;

            color.colorBlend.op = nri::BlendOp::ADD;
            color.alphaBlend.op = nri::BlendOp::ADD;
            switch (blend)
            {
            case Blend::AlphaOver:
                color.colorBlend.srcFactor = nri::BlendFactor::SRC_ALPHA;
                color.colorBlend.dstFactor = nri::BlendFactor::ONE_MINUS_SRC_ALPHA;
                // Alpha accumulates as "over" too, so a rendered-into
                // intermediate composites correctly downstream (the classic
                // straight-alpha result the batcher's own state produces).
                color.alphaBlend.srcFactor = nri::BlendFactor::ONE;
                color.alphaBlend.dstFactor = nri::BlendFactor::ONE_MINUS_SRC_ALPHA;
                break;
            case Blend::PremultipliedOver:
                color.colorBlend.srcFactor = nri::BlendFactor::ONE;
                color.colorBlend.dstFactor = nri::BlendFactor::ONE_MINUS_SRC_ALPHA;
                color.alphaBlend.srcFactor = nri::BlendFactor::ONE;
                color.alphaBlend.dstFactor = nri::BlendFactor::ONE_MINUS_SRC_ALPHA;
                break;
            case Blend::Additive:
                color.colorBlend.srcFactor = nri::BlendFactor::ONE;
                color.colorBlend.dstFactor = nri::BlendFactor::ONE;
                color.alphaBlend.srcFactor = nri::BlendFactor::ONE;
                color.alphaBlend.dstFactor = nri::BlendFactor::ONE;
                break;
            case Blend::Opaque:
                break;   // unreachable: handled by the early return above
            }
        }
    }

    // ==================================================================
    // LayoutEntry
    // ==================================================================

    void NriPipelineCache::Flatten(LayoutEntry& entry, const nri::PipelineLayoutDesc& desc)
    {
        entry.rootRegisterSpace = desc.rootRegisterSpace;
        entry.shaderStages      = desc.shaderStages;
        entry.flags             = desc.flags;

        entry.rootConstants.assign(desc.rootConstants,
                                    desc.rootConstants + (desc.rootConstants ? desc.rootConstantNum : 0));
        entry.rootDescriptors.assign(desc.rootDescriptors,
                                      desc.rootDescriptors + (desc.rootDescriptors ? desc.rootDescriptorNum : 0));
        entry.rootSamplers.assign(desc.rootSamplers,
                                   desc.rootSamplers + (desc.rootSamplers ? desc.rootSamplerNum : 0));

        const std::uint32_t setNum = desc.descriptorSets ? desc.descriptorSetNum : 0;
        entry.sets.assign(desc.descriptorSets, desc.descriptorSets + setNum);
        entry.setRanges.clear();
        entry.setRanges.reserve(setNum);
        for (std::uint32_t s = 0; s < setNum; ++s)
        {
            const nri::DescriptorSetDesc& set = desc.descriptorSets[s];
            const std::uint32_t rangeNum = set.ranges ? set.rangeNum : 0;
            entry.setRanges.emplace_back(set.ranges, set.ranges + rangeNum);
        }
    }

    bool NriPipelineCache::LayoutEntry::Matches(const nri::PipelineLayoutDesc& desc) const noexcept
    {
        if (rootRegisterSpace != desc.rootRegisterSpace
            || shaderStages != desc.shaderStages
            || flags != desc.flags)
            return false;

        if (!SameArray(rootConstants, desc.rootConstants, desc.rootConstants ? desc.rootConstantNum : 0)
            || !SameArray(rootDescriptors, desc.rootDescriptors,
                          desc.rootDescriptors ? desc.rootDescriptorNum : 0)
            || !SameArray(rootSamplers, desc.rootSamplers, desc.rootSamplers ? desc.rootSamplerNum : 0))
            return false;

        const std::uint32_t setNum = desc.descriptorSets ? desc.descriptorSetNum : 0;
        if (sets.size() != setNum)
            return false;
        for (std::uint32_t s = 0; s < setNum; ++s)
        {
            const nri::DescriptorSetDesc& theirs = desc.descriptorSets[s];
            // The set struct itself carries a POINTER to its ranges, so it
            // cannot be compared byte-wise -- compare the fields that are
            // identity and the range ARRAY separately.
            if (sets[s].registerSpace != theirs.registerSpace || sets[s].flags != theirs.flags)
                return false;
            if (!SameArray(setRanges[s], theirs.ranges, theirs.ranges ? theirs.rangeNum : 0))
                return false;
        }
        return true;
    }

    // ==================================================================
    // NriPipelineCache
    // ==================================================================

    NriPipelineCache::~NriPipelineCache()
    {
        if (m_layouts.empty() && m_pipelines.empty())
            return;

        if (!m_device)
        {
            // Cannot happen through Bind() (nothing is created unbound), but
            // stating it beats a null deref if it ever does.
            ARC_WARN("[nri-graph] ~NriPipelineCache: {} pipeline(s) + {} layout(s) leaked -- the "
                     "cache has no device to destroy them through",
                     m_pipelines.size(), m_layouts.size());
            return;
        }

        // See the header: burying at fence 0 here would violate the
        // graveyard's nondecreasing rule on a device the graph has already
        // buried against at higher values. Destroy directly, behind an idle.
        ARC_WARN("[nri-graph] ~NriPipelineCache: {} pipeline(s) + {} layout(s) still live -- the "
                 "owner never called Clear(); destroying them directly behind a DeviceWaitIdle",
                 m_pipelines.size(), m_layouts.size());

        const nri::CoreInterface& core = m_device->Core();
        if (core.DeviceWaitIdle)
            (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&m_device->Device()));

        for (PipelineEntry& entry : m_pipelines)
            if (entry.pipeline) core.DestroyPipeline(entry.pipeline);
        for (LayoutEntry& entry : m_layouts)
            if (entry.layout) core.DestroyPipelineLayout(entry.layout);
        m_pipelines.clear();
        m_layouts.clear();
    }

    void NriPipelineCache::Bind(NriDevice& device)
    {
        if (m_device == &device)
            return;
        if (m_device != nullptr)
        {
            CacheError("NriPipelineCache::Bind: this cache already belongs to another NriDevice -- "
                       "one cache belongs to one device (its objects are created and destroyed "
                       "through that device's function table)");
            return;
        }
        m_device = &device;
    }

    std::uint32_t NriPipelineCache::RegisterLayout(const nri::PipelineLayoutDesc& desc)
    {
        if (!m_device)
        {
            CacheError("NriPipelineCache::RegisterLayout: the cache is not bound to a device -- "
                       "call Bind() first");
            return kInvalidLayout;
        }

        for (std::size_t i = 0; i < m_layouts.size(); ++i)
        {
            if (m_layouts[i].Matches(desc))
                return m_layoutBase + static_cast<std::uint32_t>(i);
        }

        LayoutEntry entry;
        Flatten(entry, desc);
        if (!ARC_NRI_CHECK(m_device->Core().CreatePipelineLayout(m_device->Device(), desc, entry.layout))
            || !entry.layout)
        {
            CacheError("NriPipelineCache::RegisterLayout: CreatePipelineLayout failed");
            return kInvalidLayout;
        }

        const std::uint32_t id = m_layoutBase + static_cast<std::uint32_t>(m_layouts.size());
        m_layouts.push_back(std::move(entry));
        return id;
    }

    nri::PipelineLayout* NriPipelineCache::Layout(std::uint32_t id) const
    {
        if (id == kInvalidLayout || id < m_layoutBase)
            return nullptr;
        const std::uint32_t slot = id - m_layoutBase;
        if (slot >= m_layouts.size())
            return nullptr;
        return m_layouts[slot].layout;
    }

    nri::Pipeline* NriPipelineCache::GetGraphics(const GraphicsKey& key,
                                                 const std::function<void(nri::GraphicsPipelineDesc&)>& fill)
    {
        if (!m_device)
        {
            CacheError("NriPipelineCache::GetGraphics: the cache is not bound to a device -- "
                       "call Bind() first");
            return nullptr;
        }

        // Linear scan, deliberately: a 2D frame's PSO set is a handful of
        // entries, and a flat vector of trivially-comparable keys beats a hash
        // map at that size in both lookup cost and reasoning cost. Revisit if
        // a later phase's node count makes it measurable.
        for (const PipelineEntry& entry : m_pipelines)
        {
            if (entry.key == key)
                return entry.pipeline;
        }

        if (key.colorCount > kMaxColorAttachments)
        {
            CacheError("NriPipelineCache::GetGraphics: colorCount " + std::to_string(key.colorCount)
                        + " exceeds kMaxColorAttachments (" + std::to_string(kMaxColorAttachments) + ")");
            return nullptr;
        }
        if (key.colorCount == 0 && key.depthFormat == nri::Format::UNKNOWN)
        {
            CacheError("NriPipelineCache::GetGraphics: the key names neither a colour attachment nor "
                       "a depth format -- a graphics pipeline must write something");
            return nullptr;
        }

        nri::PipelineLayout* layout = Layout(key.layoutId);
        if (!layout)
        {
            CacheError("NriPipelineCache::GetGraphics: layoutId " + std::to_string(key.layoutId)
                        + " was never registered with this cache");
            return nullptr;
        }

        // Key-derived state, set BEFORE fill so a callback can read it, and
        // re-stamped AFTER fill so a callback cannot break it (header: the
        // fill contract). The colour array lives in THIS frame, which outlives
        // CreateGraphicsPipeline below -- that is precisely the lifetime a
        // callback-supplied array would not have.
        std::array<nri::ColorAttachmentDesc, kMaxColorAttachments> colors{};
        const auto stampKeyState = [&](nri::GraphicsPipelineDesc& desc)
        {
            for (std::uint8_t c = 0; c < key.colorCount; ++c)
            {
                colors[c].format = key.colorFormats[c];
                ApplyBlend(colors[c], key.blend);
            }
            desc.pipelineLayout            = layout;
            desc.inputAssembly.topology    = key.topology;
            desc.outputMerger.colors       = key.colorCount != 0 ? colors.data() : nullptr;
            desc.outputMerger.colorNum     = key.colorCount;
            desc.outputMerger.depthStencilFormat = key.depthFormat;
        };

        nri::GraphicsPipelineDesc desc = {};
        stampKeyState(desc);
        if (fill)
            fill(desc);
        stampKeyState(desc);

        nri::Pipeline* pipeline = nullptr;
        if (!ARC_NRI_CHECK(m_device->Core().CreateGraphicsPipeline(m_device->Device(), desc, pipeline))
            || !pipeline)
        {
            // NOT cached: a transient creation failure (a shader blob that was
            // not ready yet) must not poison the key for the rest of the run.
            CacheError("NriPipelineCache::GetGraphics: CreateGraphicsPipeline failed for shader pair "
                        + std::to_string(key.shaderPairId));
            return nullptr;
        }

        m_pipelines.push_back(PipelineEntry{ key, pipeline });
        return pipeline;
    }

    void NriPipelineCache::Clear(Graveyard& graveyard, std::uint64_t fence)
    {
        if (m_layouts.empty() && m_pipelines.empty())
            return;
        if (!m_device)
            return;   // nothing can be live without a device (see RegisterLayout/GetGraphics)

        const nri::CoreInterface* core = &m_device->Core();

        // Pipelines before layouts: a pipeline references the layout it was
        // created with, and the graveyard runs thunks in burial order.
        for (PipelineEntry& entry : m_pipelines)
        {
            if (entry.pipeline)
                graveyard.Bury(fence, [core, p = entry.pipeline] { core->DestroyPipeline(p); });
        }
        for (LayoutEntry& entry : m_layouts)
        {
            if (entry.layout)
                graveyard.Bury(fence, [core, l = entry.layout] { core->DestroyPipelineLayout(l); });
        }

        // The id counter climbs past everything just released, so an id a
        // caller cached before this call resolves to null afterwards rather
        // than to whatever landed in the same slot next.
        m_layoutBase += static_cast<std::uint32_t>(m_layouts.size());
        m_pipelines.clear();
        m_layouts.clear();
    }
}
