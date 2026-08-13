#pragma once

// RenderGraph -- Phase 2's declarative frame graph, declaration side (Task
// 3). Design informed by Filament's frame graph (filament/src/fg,
// Apache-2.0): the eager-setup/deferred-execute split and resource handles
// over raw pointers come from FrameGraph.h/PassNode/ResourceNode; this
// version is deliberately smaller (no subresources, no culling, no real
// aliasing yet -- see the plan's Deadlock chassis criteria).
//
// Scope (plan: docs/plans/2026-08-13-nri-phase2-framegraph-2d-cutover.md,
// Task 3): declaration ONLY -- handles, resource descs, node declaration,
// the builder. No nri device calls in this TU beyond desc/enum types
// (nri::Format, nri::AccessLayoutStage, nri::BufferUsageBits are pure data,
// never dereferenced as devices/queues/etc. here). Compile() (barrier
// derivation, transient lifetimes) is Task 4; Execute() (the executor that
// records real commands) is Task 6 -- RenderGraphNodeContext::Resolve/
// ColorView are declared below for their std::function signature but
// defined in Task 6's TU, not this one.
//
// Execution order = declaration order. Phase 2 does not reorder, cull, or
// parallelize nodes: AddNode() appends to a flat vector and Task 6's
// executor walks it front to back. Revisit only if a later phase's
// profiling data demands it.
//
// Include order: NRI's Extensions/NRIDeviceCreation.h (deliberately NOT
// included by this TU) is what causes the nri::Message::ERROR / wingdi.h
// ERROR macro clash documented in NriCommon.hpp. This file only needs
// NRIDescs.h-level plain data (Format, AccessLayoutStage, BufferUsageBits,
// the opaque Texture/Buffer/CommandBuffer/Descriptor handles, CoreInterface)
// -- <NRI.h> alone provides all of it. Keep it that way; do not add an
// Extensions include here without re-reading that comment.
#include <NRI.h>

#include <Arcane/Base/Api.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace Arcane
{
    // Task 5 / Task 7: neither exists yet. RenderGraphNodeContext only ever
    // needs a reference to these, which a forward declaration satisfies as
    // long as nothing in this TU calls through one -- nothing does.
    class NriUploadRing;
    class NriPipelineCache;

    class RenderGraph;

    // Sentinel for both handle structs below: an RgTexture/RgBuffer with
    // index == kInvalid never refers to a declared resource.
    inline constexpr std::uint32_t kInvalid = 0xFFFFFFFFu;

    // ---------------------------------------------------------------------
    // Value handles -- deliberately just an index, no generation field.
    // RenderGraph::Reset() clears the backing vectors and bumps an internal
    // generation counter (Generation()), so a handle from a previous frame
    // that outlives Reset() is caught by an out-of-bounds ARC_ASSERT the
    // next time it is used (Read/Write/the introspection accessors below)
    // -- not by comparing generations, since the handle itself carries
    // none to compare.
    // ---------------------------------------------------------------------
    struct RgTexture { std::uint32_t index = kInvalid; };
    struct RgBuffer  { std::uint32_t index = kInvalid; };

    // Subset of nri::TextureDesc the graph owns. Mip/layer/sample counts and
    // sharing mode are not here -- add them when a task actually needs them
    // (YAGNI), not speculatively now.
    struct RgTextureDesc
    {
        nri::Format   format = nri::Format::UNKNOWN;
        std::uint32_t width = 0, height = 0;
        bool          depthStencil = false;   // chooses attachment vs shader usage bits (Task 4)
    };

    // Per-declaration usage: what THIS Read/Write means, not a resource-wide
    // flag (the same resource can be ColorWrite in one node and ShaderRead
    // in the next). Task 4 maps each value to an (access, layout, stage)
    // triple for barrier derivation.
    enum class RgUsage : std::uint8_t
    {
        ColorWrite, DepthWrite, ShaderRead, ShaderWriteCs, // (UAV, compute)
        CopySrc, CopyDst, Present, ReadbackHost
    };

    // Handed to a node's Setup function. A thin wrapper over its owning
    // RenderGraph -- every method here forwards into the graph's own
    // storage. SetColorAttachments/SetDepthAttachment live on RenderGraph
    // itself (below), not here: reach them from within Setup by capturing
    // the RenderGraph by reference (see AddNode()'s comment).
    class ARCANE_API RenderGraphBuilder
    {
    public:
        RgTexture CreateTexture(const char* name, const RgTextureDesc& desc);               // transient
        RgTexture ImportTexture(const char* name, nri::Texture* texture,
                                 nri::AccessLayoutStage entry, nri::AccessLayoutStage exit,
                                 bool persistent);                                           // swapchain, history, canvas
        RgBuffer  CreateBuffer(const char* name, std::uint64_t size, nri::BufferUsageBits usage); // transient
        RgBuffer  ImportBuffer(const char* name, nri::Buffer* buffer, std::uint64_t size);

        void Read (RgTexture texture, RgUsage usage);
        void Read (RgBuffer buffer, RgUsage usage);
        void Write(RgTexture texture, RgUsage usage);
        void Write(RgBuffer buffer, RgUsage usage);

    private:
        friend class RenderGraph;
        RenderGraphBuilder(RenderGraph& graph, std::size_t nodeIndex) noexcept
            : m_graph(graph), m_nodeIndex(nodeIndex) {}

        RenderGraph& m_graph;
        std::size_t  m_nodeIndex;
    };

    // Handed to a node's Exec function -- Task 6's executor invokes it;
    // nothing in this TU constructs or calls through one. Resolve/ColorView
    // bodies belong to Task 6's executor TU: declared here (so this header
    // is a complete, includable contract for every later task) but
    // deliberately not defined by RenderGraph.cpp.
    struct RenderGraphNodeContext
    {
        nri::CommandBuffer&       cmd;
        const nri::CoreInterface& core;

        [[nodiscard]] nri::Texture*    Resolve(RgTexture texture) const;
        [[nodiscard]] nri::Descriptor* ColorView(RgTexture texture) const;  // cached attachment view
        [[nodiscard]] nri::Buffer*     Resolve(RgBuffer buffer) const;

        NriUploadRing&    ring;       // Task 5
        NriPipelineCache& pipelines;  // Task 7
    };

    class ARCANE_API RenderGraph
    {
    public:
        using Setup = std::function<void(RenderGraphBuilder&)>;
        using Exec  = std::function<void(RenderGraphNodeContext&)>;
        enum class NodeKind : std::uint8_t { Raster, Compute, Copy };

        RenderGraph() = default;
        ~RenderGraph() = default;

        // Declares one node in EXECUTION order (see the file-header comment
        // -- no reordering in Phase 2). Runs `setup` synchronously, right
        // here, before returning -- Filament's addPass() does the same
        // (eager setup, deferred execute). m_currentNodeIndex is this
        // call's node for the duration of `setup`, so SetColorAttachments/
        // SetDepthAttachment -- reached from within `setup` by capturing
        // this RenderGraph by reference, since they are not on
        // RenderGraphBuilder -- target the right node. `exec` is stored for
        // Task 6's executor and is never invoked here.
        //
        // Raster nodes must end this call with at least one color or depth
        // attachment declared. That is checked here, but non-fatally
        // (ARC_ENSURE) and recorded rather than refused: declaration always
        // succeeds (see NodeHasRequiredAttachments()) -- matching the
        // assert-later model RenderGraphBuilder::Read() uses for a
        // never-written transient (RenderGraph.cpp).
        void AddNode(const char* name, NodeKind kind, Setup setup, Exec exec);

        // Apply to the node the most recent AddNode() call declared. Valid
        // to call only while that node's `setup` is running (typically via
        // a `setup` that captures this RenderGraph by reference) --
        // ARC_ASSERTs otherwise.
        void SetColorAttachments(std::span<const RgTexture> attachments);
        void SetDepthAttachment(RgTexture attachment);

        // Compile()/Execute() land in Tasks 4/6. Reset() clears every
        // declared node and resource -- this graph is ready to declare the
        // next frame's build -- and bumps Generation() (see the RgTexture/
        // RgBuffer comment above for what that buys a stale handle).
        void Reset();

        // -------------------------------------------------------------
        // Declaration-side introspection. Not part of the cross-task
        // contract frozen above -- exercised directly by this task's [nri]
        // tests, and available for Task 4's Compile() to read (same class,
        // extended in place per that task's brief).
        // -------------------------------------------------------------
        [[nodiscard]] std::uint32_t Generation() const noexcept { return m_generation; }

        [[nodiscard]] std::size_t NodeCount() const noexcept { return m_nodes.size(); }
        [[nodiscard]] const char* NodeName(std::size_t nodeIndex) const;
        [[nodiscard]] bool        NodeHasRequiredAttachments(std::size_t nodeIndex) const;

        [[nodiscard]] std::size_t TextureCount() const noexcept { return m_textures.size(); }
        [[nodiscard]] std::size_t BufferCount() const noexcept { return m_buffers.size(); }
        [[nodiscard]] const char* NameOf(RgTexture texture) const;
        [[nodiscard]] const char* NameOf(RgBuffer buffer) const;
        [[nodiscard]] bool        IsTransient(RgTexture texture) const;
        [[nodiscard]] bool        IsTransient(RgBuffer buffer) const;
        [[nodiscard]] bool        WasWritten(RgTexture texture) const;
        [[nodiscard]] bool        WasWritten(RgBuffer buffer) const;

    private:
        friend class RenderGraphBuilder;

        enum class ResourceKind : std::uint8_t { Transient, Imported };

        struct TextureResource
        {
            std::string            name;
            RgTextureDesc           desc;
            ResourceKind            kind = ResourceKind::Transient;
            nri::Texture*           imported = nullptr;
            nri::AccessLayoutStage  importEntry{};
            nri::AccessLayoutStage  importExit{};
            bool                    persistent = false;
            // Imported resources always arrive with a defined incoming
            // state (the caller-supplied `entry`), so this starts true; a
            // transient starts false until its first declared Write().
            // Task 4's Compile() refuses a Read() while this is still false
            // at that point -- assert-later: recorded here, refused there.
            // See RenderGraph::RecordAccess() in the .cpp.
            bool                    everWritten = false;
        };

        struct BufferResource
        {
            std::string          name;
            std::uint64_t         size = 0;
            nri::BufferUsageBits  usage{};
            ResourceKind          kind = ResourceKind::Transient;
            nri::Buffer*          imported = nullptr;
            bool                  everWritten = false;   // see TextureResource::everWritten
        };

        struct NodeDesc
        {
            std::string             name;
            NodeKind                 kind = NodeKind::Raster;
            Exec                     exec;
            std::vector<RgTexture>   colorAttachments;
            RgTexture                depthAttachment{};
            bool                     hasRequiredAttachments = true;  // see NodeHasRequiredAttachments()
        };

        // One declared Read()/Write() -- the raw material Task 4's Compile()
        // walks to derive barriers and transient lifetimes. Recorded, never
        // rejected, at declaration time (assert-later model).
        struct Access
        {
            std::size_t   nodeIndex;
            std::uint32_t resourceIndex;
            bool          isTexture;
            bool          isWrite;
            RgUsage       usage;
        };

        RgTexture CreateTextureInternal(const char* name, const RgTextureDesc& desc);
        RgTexture ImportTextureInternal(const char* name, nri::Texture* texture,
                                         nri::AccessLayoutStage entry, nri::AccessLayoutStage exit,
                                         bool persistent);
        RgBuffer  CreateBufferInternal(const char* name, std::uint64_t size, nri::BufferUsageBits usage);
        RgBuffer  ImportBufferInternal(const char* name, nri::Buffer* buffer, std::uint64_t size);
        void      RecordAccess(std::size_t nodeIndex, std::uint32_t resourceIndex,
                                bool isTexture, bool isWrite, RgUsage usage);

        static constexpr std::size_t kNoCurrentNode = static_cast<std::size_t>(-1);

        std::vector<TextureResource> m_textures;
        std::vector<BufferResource>  m_buffers;
        std::vector<NodeDesc>        m_nodes;
        std::vector<Access>          m_accesses;
        std::size_t                  m_currentNodeIndex = kNoCurrentNode;
        std::uint32_t                m_generation = 0;
    };
}
