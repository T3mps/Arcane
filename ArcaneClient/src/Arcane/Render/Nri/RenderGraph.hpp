#pragma once

// RenderGraph -- Phase 2's declarative frame graph, declaration side (Task
// 3). Design informed by Filament's frame graph (filament/src/fg,
// Apache-2.0): the eager-setup/deferred-execute split and resource handles
// over raw pointers come from FrameGraph.h/PassNode/ResourceNode; this
// version is deliberately smaller (no subresources, no culling, no real
// aliasing yet -- see the plan's Deadlock chassis criteria).
//
// Scope (plan: docs/plans/2026-08-13-nri-phase2-framegraph-2d-cutover.md,
// Tasks 3+4): declaration AND compile -- handles, resource descs, node
// declaration, the builder (Task 3); derived barriers, transient lifetimes
// and transient pool-slot assignment (Task 4). No nri device calls in this
// TU beyond desc/enum types (nri::Format, nri::AccessLayoutStage,
// nri::AccessBits, nri::Layout, nri::StageBits, nri::BufferUsageBits are
// pure data, never dereferenced as devices/queues/etc. here). Compile() is
// PURE -- it derives everything from the declarations and nothing else, so
// Task 6's executor can consume its RgCompiled verbatim. Execute() (the
// executor that records real commands) is Task 6 --
// RenderGraphNodeContext::Resolve/ColorView are declared below for their
// std::function signature but defined in Task 6's TU, not this one.
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
#include <optional>
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
    // index == kInvalid never refers to a declared resource. Checked
    // verbatim (before any decode), so it stays unambiguous regardless of
    // the encoding below.
    inline constexpr std::uint32_t kInvalid = 0xFFFFFFFFu;

    // ---------------------------------------------------------------------
    // Value handles -- a single uint32_t per the frozen contract, but NOT a
    // raw resource-vector index: `index` packs a generation into the same
    // field (top 8 bits generation, low 24 bits slot -- entt/Bevy-style
    // versioned handles). RenderGraph::EncodeHandle()/DecodeAndValidateSlot()
    // (private) are the ONLY seam that may interpret this value -- nothing
    // else, including Task 4/6 code extending this class, should treat
    // `.index` as a subscript into m_textures/m_buffers.
    //
    // RenderGraph::Reset() bumps its generation counter (Generation()), so a
    // handle minted before Reset() carries the OLD generation and fails to
    // decode against the NEW one -- caught by IsHandleValid() (queryable,
    // non-fatal) and by an ARC_ASSERT in every accessor that takes a handle
    // (Read/Write/NameOf/IsTransient/WasWritten). This catches the common,
    // steady-state case a plain bounds check cannot: a graph rebuilt every
    // frame with the SAME OR A LARGER shape, where a stale handle's raw
    // index would otherwise still be in bounds and silently alias a
    // different (fresh) resource in the same slot.
    //
    // Generation wraps mod 256 (8 bits): a handle held across exactly 256
    // (or a multiple of 256) Reset() calls is indistinguishable from a
    // current one. Accepted, documented limit on the debug safety net --
    // the same tradeoff fixed-width versioned handles always make; nothing
    // in this phase holds a graph handle across more than a handful of
    // frames.
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

    // =====================================================================
    // Compile output (Task 4). Pure data: RenderGraph::Compile() derives all
    // of it from the declarations alone -- no nri device calls, no GPU
    // allocation, no mutation of the graph -- and Task 6's executor replays
    // it verbatim.
    //
    // INDEX SPACE -- READ THIS BEFORE SUBSCRIPTING ANYTHING IN HERE. Two
    // different index spaces live in this block and they are NOT the same:
    //
    //   * `resourceIndex` (RgBarrier::resourceIndex, RgTransient::
    //     resourceIndex) is a RESOURCE SLOT -- see below.
    //   * RgCompiled::transientLifetimes and ::transientPoolSlot are indexed
    //     by TRANSIENT INDEX, i.e. a position in RgCompiled::transients --
    //     NOT by a resource slot. Go slot-ward via
    //     transients[i].resourceIndex; there is no reverse mapping and
    //     subscripting the lifetime/pool vectors with a resource slot is
    //     silently wrong whenever the graph holds an imported resource.
    //
    // Every `resourceIndex` is a DECODED, plain 0-based SLOT into the
    // graph's texture vector or its buffer vector, selected by the
    // neighbouring `isTexture` flag. It is NOT an RgTexture/RgBuffer
    // `.index` value:
    // those pack a generation into their top 8 bits (see the RgTexture/
    // RgBuffer comment above) and are never valid subscripts. Compile()
    // decodes every handle it consumes -- the accesses recorded by
    // RecordAccess(), AND NodeDesc::colorAttachments/depthAttachment, which
    // the SetColorAttachments/SetDepthAttachment setters deliberately store
    // unvalidated -- through RenderGraph::DecodeAndValidateSlot(), the one
    // handle seam, and stores only decoded slots in here. So: index
    // RgCompiled with these values directly; never re-decode them, and
    // never feed an RgTexture/RgBuffer handle to one of these vectors.
    // =====================================================================

    // One derived state transition, emitted for exactly one resource.
    //
    // For TEXTURES `before`/`after` are complete nri::AccessLayoutStage
    // triples and all three fields matter. For BUFFERS the `layout` field of
    // both is ALWAYS nri::Layout::UNDEFINED and carries no meaning:
    // nri::BufferBarrierDesc has no layout member (its before/after are
    // nri::AccessStage, access+stages only), so Task 6 drops it when it
    // translates. Sharing one struct across both kinds is the frozen
    // contract's shape; forcing UNDEFINED on the buffer side keeps a
    // meaningless field from ever reading as meaningful.
    struct RgBarrier
    {
        std::uint32_t          resourceIndex = 0;   // DECODED slot -- see INDEX SPACE above
        bool                   isTexture = true;
        nri::AccessLayoutStage before{};
        nri::AccessLayoutStage after{};
    };

    struct RgCompiledNode
    {
        std::uint32_t          nodeIndex = 0;
        std::vector<RgBarrier> preBarriers;   // batched: ONE nri CmdBarrier group per node
    };

    // One transient resource, in the order Compile() enumerates them: every
    // transient TEXTURE in slot order first, then every transient BUFFER in
    // slot order. RgCompiled::transientLifetimes and ::transientPoolSlot are
    // index-parallel to RgCompiled::transients -- this struct is what makes
    // "index-parallel to transients" a defined, self-describing statement
    // instead of an ordering Task 6 would have to re-derive (and could
    // re-derive differently).
    struct RgTransient
    {
        std::uint32_t resourceIndex = 0;   // DECODED slot -- see INDEX SPACE above
        bool          isTexture = true;
    };

    // A transient no node ever reads or writes has no lifetime and gets no
    // pool slot: its Lifetime is {kRgNoNode, kRgNoNode} and its
    // transientPoolSlot entry is kRgNoPoolSlot. Task 6 must not realize it
    // (there is no pool slot to realize it into).
    inline constexpr std::uint32_t kRgNoNode     = 0xFFFFFFFFu;
    inline constexpr std::uint32_t kRgNoPoolSlot = 0xFFFFFFFFu;

    struct RgCompiled
    {
        // One entry per declared node, in declaration order:
        // nodes.size() == RenderGraph::NodeCount() and nodes[i].nodeIndex == i
        // ALWAYS -- a node with nothing to transition carries an EMPTY
        // preBarriers rather than being omitted, so Task 6's executor can
        // walk this vector as its node list and never has to cross-reference
        // it against the graph's own node vector.
        std::vector<RgCompiledNode> nodes;

        // Transitions that run after the LAST node, restoring every imported
        // TEXTURE to the exit state its ImportTexture() call promised (e.g.
        // nri::Layout::PRESENT for a swapchain backbuffer). Imported BUFFERS
        // never appear here: ImportBuffer() takes no entry/exit state, so the
        // graph has nothing to restore them to.
        std::vector<RgBarrier> exitBarriers;

        // Inclusive node-index range [first, last] over which a transient is
        // live -- first/last are the lowest/highest node index that reads or
        // writes it.
        struct Lifetime { std::uint32_t first = kRgNoNode, last = kRgNoNode; };

        std::vector<RgTransient>   transients;
        std::vector<Lifetime>      transientLifetimes;   // index-parallel to transients
        std::vector<std::uint32_t> transientPoolSlot;    // index-parallel to transients
        std::uint32_t              poolSlotCount = 0;    // distinct pool slots Task 6 must realize
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

        // Derives, from the declarations above and NOTHING else, everything
        // Task 6's executor needs: the batched per-node barriers, the
        // imported-texture exit barriers, transient lifetimes, and the
        // transient pool-slot assignment. PURE -- no nri device calls, no
        // GPU allocation, no mutation of this graph; compiling the same
        // declarations twice yields the same result. See the INDEX SPACE
        // comment above RgBarrier for how to read the indices it produces.
        //
        // Returns std::nullopt -- and, when `outError` is non-null, fills it
        // with a message that NAMES the offending node and/or resource --
        // when the declarations cannot yield a correct barrier chain:
        //   1. a Raster node that declared no color or depth attachment
        //      (NodeHasRequiredAttachments() false: recorded non-fatally at
        //      declaration, refused HERE -- this is the "later" of Task 3's
        //      assert-later model);
        //   2. an attachment handle that fails to decode (invalid, stale
        //      across a Reset(), or out of range) -- the setters store
        //      attachment handles unvalidated, so Compile is their only
        //      validation point;
        //   3. a Raster node attachment the node never declared a Read()/
        //      Write() for -- without a declared access the graph derives no
        //      transition, and the attachment would be bound in whatever
        //      layout it happened to be left in;
        //   4. a Read() of a TRANSIENT at a node earlier than the first node
        //      that Write()s it (which subsumes "never written at all") --
        //      its contents are undefined. Node-granular: a node may read a
        //      transient it also writes itself, regardless of the order the
        //      two accesses were declared in, because one node resolves to
        //      exactly one state;
        //   5. a single node declaring two DIFFERENT states for the same
        //      resource (e.g. ShaderRead and ShaderWriteCs) -- the node's
        //      one batched pre-barrier group cannot transition a resource
        //      twice, and the two usages are a genuine read/write hazard;
        //   6. an access whose recorded slot is out of range -- only
        //      reachable in a build where the ARC_ASSERT in RecordAccess()
        //      is compiled out (see that function).
        [[nodiscard]] std::optional<RgCompiled> Compile(std::string* outError = nullptr) const;

        // Execute() lands in Task 6. Reset() clears every declared node and
        // resource -- this graph is ready to declare the next frame's build
        // -- and bumps Generation() (see the RgTexture/RgBuffer comment
        // above for what that buys a stale handle).
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

        // True iff `texture`/`buffer` decodes to a slot that both exists in
        // the CURRENT generation and is in range -- the non-fatal, queryable
        // proxy for "would this handle ARC_ASSERT if used right now". Exists
        // specifically so the stale-handle-across-Reset() invariant (see the
        // RgTexture/RgBuffer comment above) has a real, executed negative
        // test rather than only an untestable fatal assert.
        [[nodiscard]] bool IsHandleValid(RgTexture texture) const noexcept;
        [[nodiscard]] bool IsHandleValid(RgBuffer buffer) const noexcept;

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
        // rejected, at declaration time (assert-later model). resourceIndex
        // here is the DECODED, plain 0-based slot into m_textures/m_buffers
        // (RecordAccess() decodes the caller's encoded handle once, via
        // DecodeAndValidateSlot(), before storing this) -- Task 4 reading
        // m_accesses never has to deal with the handle encoding.
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
        void      RecordAccess(std::size_t nodeIndex, std::uint32_t encodedHandle,
                                bool isTexture, bool isWrite, RgUsage usage);

        // ---------------------------------------------------------------
        // The handle-encoding seam (see the RgTexture/RgBuffer comment
        // above). EncodeHandle() is the only place that PACKS a (slot,
        // generation) pair; DecodeAndValidateSlot() is the only place that
        // UNPACKS and validates one. Every method that mints a handle
        // (Create*/Import*Internal) or consumes one (RecordAccess, NameOf,
        // IsTransient, WasWritten, IsHandleValid) goes through exactly one
        // of these two -- never through `.index` directly. Keep it that
        // way: a future task adding a handle-consuming method (e.g. Task
        // 4/6's Resolve()) MUST route through DecodeAndValidateSlot() too.
        // ---------------------------------------------------------------
        static constexpr std::uint32_t kIndexBits      = 24;
        static constexpr std::uint32_t kGenerationBits = 8;
        static constexpr std::uint32_t kIndexMask      = (1u << kIndexBits) - 1u;      // 0x00FFFFFF -- max live slots per generation
        static constexpr std::uint32_t kGenerationMask = (1u << kGenerationBits) - 1u; // 0xFF -- generation wraps mod 256
        static constexpr std::size_t   kNoSlot         = static_cast<std::size_t>(-1); // decode failure: invalid, stale, or out of range

        [[nodiscard]] static std::uint32_t EncodeHandle(std::uint32_t slot, std::uint32_t generation) noexcept;
        [[nodiscard]] std::size_t DecodeAndValidateSlot(std::uint32_t encoded, std::size_t resourceCount) const noexcept;

        static constexpr std::size_t kNoCurrentNode = static_cast<std::size_t>(-1);

        std::vector<TextureResource> m_textures;
        std::vector<BufferResource>  m_buffers;
        std::vector<NodeDesc>        m_nodes;
        std::vector<Access>          m_accesses;
        std::size_t                  m_currentNodeIndex = kNoCurrentNode;
        std::uint32_t                m_generation = 0;
    };
}
