#pragma once

// NRI substrate (Phase 2): the graph path's pipeline/pipeline-layout cache.
//
// Landed empty by Task 6 (so RgExecuteDesc's frozen `NriPipelineCache&` field
// named a real, constructible type) and FILLED IN HERE by Task 7, in place --
// no signature anywhere changed, because there was nothing to change.
//
// WHAT IT IS. Two dedup'd tables over one NriDevice:
//
//   * pipeline LAYOUTS, keyed by the nri::PipelineLayoutDesc itself
//     (RegisterLayout returns a small dense id; registering an identical desc
//     twice returns the SAME id and creates nothing);
//   * graphics PIPELINES, keyed by GraphicsKey -- {shader pair, layout id,
//     attachment formats, colour count, topology, blend} -- with the actual
//     nri::GraphicsPipelineDesc filled by a caller callback on a MISS only.
//
// WHY FORMATS ARE IN THE KEY (spec: "formats live in PSOs"). NRI has no
// render-pass object and no format-agnostic PSO: a graphics pipeline bakes its
// attachment formats at creation, and binding it inside a CmdBeginRendering
// whose attachments carry different formats is undefined on both backends. The
// swapchain's channel order is RESOLVED by NRI rather than pinned
// (NriSwapChain::Format()), and a graph node can render into a transient of an
// entirely different format from one frame to the next, so "which formats" is
// genuinely part of a pipeline's identity here -- not a detail a caller can be
// trusted to remember. Keying on it makes a format change a cache MISS (a new
// PSO) instead of a silent mismatch.
//
// THE FILL CALLBACK'S CONTRACT. Three rules; the first is enforced for you,
// and the two after it are the ones that bite.
//
// 1. EVERYTHING IN THE KEY BELONGS TO THE CACHE, structurally. After `fill`
//    returns, GetGraphics RE-STAMPS pipelineLayout, inputAssembly.topology and
//    the whole outputMerger colour/format block from the key -- so a callback
//    that tried to set them can neither desynchronise the cache from what it
//    actually created, nor leave outputMerger.colors pointing at an array that
//    died with its own stack frame.
//
// 2. ANYTHING YOU POINT THE DESC AT MUST OUTLIVE THE CALL.
//    GraphicsPipelineDesc's `shaders`, `vertexInput` and `multisample` are
//    POINTERS INTO CALLER MEMORY, and CreateGraphicsPipeline runs AFTER `fill`
//    has returned -- so the obvious spelling
//
//        [&](nri::GraphicsPipelineDesc& desc) {
//            nri::ShaderDesc stages[2] = { ... };   // DANGLES
//            desc.shaders = stages; desc.shaderNum = 2;
//        }
//
//    hands the cache a pointer into a dead frame. It is the identical hazard
//    rule 1 closes for outputMerger.colors, on the three fields the cache
//    cannot own on your behalf. Keep those arrays -- and the shader bytecode
//    they point at -- in a scope that encloses the GetGraphics call: the
//    caller's own frame, or a member.
//
// 3. EVERYTHING THE CALLBACK SETS MUST BE A PURE FUNCTION OF THE KEY. The
//    cache compares keys and nothing else, so any state that is NOT keyed --
//    `vertexInput`, `rasterization`, the depth/stencil TEST state,
//    `multisample`, `robustness` -- must be folded into `shaderPairId`, which
//    is opaque to the cache and exists precisely to be the caller's
//    discriminator. Otherwise two genuinely different pipelines collide on one
//    entry and the second silently gets the first's PSO.
//
// LIFETIME. The cache owns every nri::Pipeline and nri::PipelineLayout it
// creates and hands them back as borrowed pointers. Clear(graveyard, fence) is
// the sanctioned release -- project switch, shutdown, or a shader reload that
// invalidates every PSO -- and buries everything at ONE fence value, which
// trivially satisfies Graveyard's nondecreasing-burial rule for that call.
// Choosing that value is the caller's job, because only the caller knows which
// timeline the cache's users submitted on (NriGraphContext passes the graph's
// own last-submitted value, the same one RenderGraph::ReleaseGpuResources
// buries at). ~NriPipelineCache is a safety net, not the path: see its comment.
//
// Include-order rule (same as every file in this directory -- see
// NriCommon.hpp): NRI headers first, because Extensions/NRIDeviceCreation.h
// (reachable through NriDevice.hpp) declares nri::Message::ERROR and
// <windows.h> (via Arcane/Base/Log.hpp -> spdlog) #defines ERROR via wingdi.h.
#include <NRI.h>

#include <Arcane/Base/Api.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>   // NriDevice, Graveyard

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace Arcane
{
    class ARCANE_API NriPipelineCache
    {
    public:
        // Max colour attachments a single graphics pipeline can name here.
        // Four is the Deadlock chassis' GBuffer width and comfortably above
        // Phase 2's needs (every 2D node renders to one). A caller asking for
        // more is refused, loudly, rather than silently truncated.
        static constexpr std::uint32_t kMaxColorAttachments = 4;

        // RegisterLayout's failure value, and the value a default-constructed
        // GraphicsKey carries -- so a key whose layout was never registered
        // cannot accidentally name layout 0.
        static constexpr std::uint32_t kInvalidLayout = 0xFFFFFFFFu;

        struct GraphicsKey
        {
            // The caller's shader identity: a pair of artifact ids packed
            // into 64 bits, a material hash, whatever the caller uses -- the
            // cache never interprets it, it only compares it. It MUST change
            // whenever the bytecode `fill` would supply changes, or a stale
            // PSO is served after a shader reload.
            std::uint64_t shaderPairId = 0;

            // From RegisterLayout. kInvalidLayout is refused by GetGraphics.
            std::uint32_t layoutId = kInvalidLayout;

            std::array<nri::Format, kMaxColorAttachments> colorFormats{};
            nri::Format   depthFormat = nri::Format::UNKNOWN;   // UNKNOWN = no depth attachment
            std::uint8_t  colorCount  = 0;

            // Packed pipeline state that is NOT derivable from the formats and
            // must not be left to `fill` (see the header's fill contract).
            nri::Topology topology = nri::Topology::TRIANGLE_LIST;
            // Which blend the cache stamps into every colour attachment.
            // Deliberately an enum rather than a raw nri::BlendDesc pair: the
            // key has to be cheaply and TOTALLY comparable, and a struct of
            // enums with padding is neither.
            enum class Blend : std::uint8_t
            {
                Opaque,               // blendEnabled = false
                AlphaOver,            // src.a, 1-src.a  (straight alpha)
                PremultipliedOver,    // 1, 1-src.a
                Additive              // 1, 1
            };
            Blend blend = Blend::Opaque;

            [[nodiscard]] bool operator==(const GraphicsKey&) const noexcept = default;
        };

        NriPipelineCache() = default;

        // Owns live NRI objects tied to one device; copying would double-free
        // every one of them.
        NriPipelineCache(const NriPipelineCache&)            = delete;
        NriPipelineCache& operator=(const NriPipelineCache&) = delete;

        // SAFETY NET, NOT THE PATH. The sanctioned release is Clear(); a cache
        // that still holds objects here means the owner never called it. There
        // is no fence value to bury against at this point, and burying at 0
        // would VIOLATE Graveyard's nondecreasing rule on a device whose
        // graveyard the graph has already used at higher values, so this
        // destroys DIRECTLY behind a DeviceWaitIdle and says so at WARN.
        // Safe only because a cache is destroyed as part of its owner's
        // teardown, with the device still alive -- which the member
        // declaration order of every owner must guarantee.
        ~NriPipelineCache();

        // Binds this cache to the device every object it creates belongs to.
        // Idempotent for the same device; REFUSES a rebind to a different one
        // (logged + latched) rather than start mixing function tables. The
        // device must outlive this object -- same contract as every other
        // Nri/ wrapper.
        void Bind(NriDevice& device);
        [[nodiscard]] bool IsBound() const noexcept { return m_device != nullptr; }

        // Creates the layout, or returns the id of an identical one already
        // registered. kInvalidLayout on failure (unbound cache, or NRI refused
        // the desc) -- already logged and latched.
        //
        // DEDUP CONTRACT: `desc` and everything it points at are compared BYTE
        // WISE against previous registrations. That is exact for descs built
        // the way every desc in this tree is built -- value-initialized
        // (`nri::PipelineLayoutDesc desc = {}`) and then assigned field by
        // field, which zeroes the padding -- and it is why that idiom is a
        // requirement here rather than a style. A desc whose padding holds
        // stack garbage simply misses the cache and creates a second identical
        // layout: wasteful, never wrong.
        [[nodiscard]] std::uint32_t RegisterLayout(const nri::PipelineLayoutDesc& desc);

        // The layout `id` names, or null for kInvalidLayout / an id this cache
        // never issued.
        [[nodiscard]] nri::PipelineLayout* Layout(std::uint32_t id) const;

        // The pipeline for `key`, creating it on a miss. `fill` runs ONLY on a
        // miss and receives a desc whose key-derived fields are already set.
        // READ THE HEADER'S FILL CONTRACT before writing one -- it is three
        // rules, and two of them (pointer lifetime for shaders/vertexInput/
        // multisample, and folding non-keyed state into shaderPairId) are
        // caller obligations this class cannot check for you.
        // Null on failure (unbound cache, unregistered layoutId, colorCount
        // over kMaxColorAttachments, or NRI refused the pipeline) -- already
        // logged and latched; the failed key is NOT cached, so a later call
        // retries rather than serving null forever.
        [[nodiscard]] nri::Pipeline* GetGraphics(const GraphicsKey& key,
                                                 const std::function<void(nri::GraphicsPipelineDesc&)>& fill);

        // Buries every pipeline and every layout at `fence` and empties both
        // tables. Idempotent; a no-op on a cache that created nothing. Layout
        // ids issued before this call are NOT reissued after it -- the id
        // counter keeps climbing -- so a stale id can never silently resolve
        // to a different layout.
        void Clear(Graveyard& graveyard, std::uint64_t fence);

        // Introspection for tests and shutdown logs -- not part of any
        // cross-task contract.
        [[nodiscard]] std::size_t LayoutCount() const noexcept { return m_layouts.size(); }
        [[nodiscard]] std::size_t PipelineCount() const noexcept { return m_pipelines.size(); }

    private:
        // One registered layout: the created object plus the flattened,
        // owned copy of the desc that identifies it. The flattening is what
        // makes dedup possible at all -- nri::PipelineLayoutDesc is four
        // pointer+count pairs into CALLER memory that is typically a stack
        // temporary, so nothing about the desc survives the call unless it is
        // copied here.
        struct LayoutEntry
        {
            nri::PipelineLayout*                 layout = nullptr;
            std::uint32_t                        rootRegisterSpace = 0;
            nri::StageBits                       shaderStages{};
            nri::PipelineLayoutBits              flags{};
            std::vector<nri::RootConstantDesc>   rootConstants;
            std::vector<nri::RootDescriptorDesc> rootDescriptors;
            std::vector<nri::RootSamplerDesc>    rootSamplers;
            // Descriptor sets carry their own range arrays, so each set's
            // ranges are copied alongside it (index-parallel to `sets`).
            std::vector<nri::DescriptorSetDesc>               sets;
            std::vector<std::vector<nri::DescriptorRangeDesc>> setRanges;

            [[nodiscard]] bool Matches(const nri::PipelineLayoutDesc& desc) const noexcept;
        };

        struct PipelineEntry
        {
            GraphicsKey    key{};
            nri::Pipeline* pipeline = nullptr;
        };

        // Fills `entry`'s owned copy from `desc`. Static because it runs
        // before the entry has an id or an object.
        static void Flatten(LayoutEntry& entry, const nri::PipelineLayoutDesc& desc);

        NriDevice*                 m_device = nullptr;
        std::vector<LayoutEntry>   m_layouts;     // index == the id it was issued under, minus m_layoutBase
        std::vector<PipelineEntry> m_pipelines;   // linear scan: a 2D frame has a handful
        // Ids are issued from a counter that NEVER resets, so an id from
        // before a Clear() cannot resolve after it (Layout() range-checks
        // against this base). See Clear().
        std::uint32_t              m_layoutBase = 0;
    };
}
