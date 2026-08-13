// RenderGraph (Phase 2, Tasks 3+4): headless [nri] coverage of the pure
// declaration invariants (Task 3) and of Compile()'s derived barriers,
// transient lifetimes and pool-slot assignment (Task 4) -- no nri device, no
// Execute() (Task 6). See RenderGraph.hpp's file header for the design
// reference (Filament's frame graph, Apache-2.0) and the eager-setup/
// deferred-execute model these cases rely on: a node's Setup callback runs
// synchronously inside AddNode(), so a Setup that captures the RenderGraph
// by reference can call SetColorAttachments/SetDepthAttachment (which target
// "the node AddNode() is currently declaring") and every handle
// CreateTexture/CreateBuffer/ImportTexture/ImportBuffer returns is
// immediately usable by the time AddNode() returns.
#include <NRI.h>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/Nri/RenderGraph.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace
{
    Arcane::RgTextureDesc MakeColorDesc(std::uint32_t width = 64, std::uint32_t height = 64)
    {
        Arcane::RgTextureDesc desc;
        desc.format = nri::Format::RGBA8_UNORM;
        desc.width = width;
        desc.height = height;
        return desc;
    }

    Arcane::RgTextureDesc MakeDepthDesc(std::uint32_t width = 64, std::uint32_t height = 64)
    {
        Arcane::RgTextureDesc desc;
        desc.format = nri::Format::D32_SFLOAT;
        desc.width = width;
        desc.height = height;
        desc.depthStencil = true;
        return desc;
    }

    // ------------------------------------------------------------------
    // The mapping table under test, restated independently of the
    // implementation (RenderGraph.cpp's StateFor) so a change to either side
    // has to be made deliberately in both. Bit values are NRI v180's
    // (ThirdParty/NRI/Include/NRIDescs.h).
    // ------------------------------------------------------------------
    constexpr nri::AccessLayoutStage kUnknownState{ nri::AccessBits::NONE, nri::Layout::UNDEFINED, nri::StageBits::ALL };
    constexpr nri::StageBits         kShaderReadStages =
        nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER | nri::StageBits::COMPUTE_SHADER;

    void CheckState(const nri::AccessLayoutStage& actual,
                    nri::AccessBits access, nri::Layout layout, nri::StageBits stages)
    {
        // Compared through the underlying integers so a failure prints the
        // actual bits -- Catch2 cannot stringify NRI's enum classes.
        CHECK(static_cast<std::uint32_t>(actual.access) == static_cast<std::uint32_t>(access));
        CHECK(static_cast<std::uint32_t>(actual.layout) == static_cast<std::uint32_t>(layout));
        CHECK(static_cast<std::uint32_t>(actual.stages) == static_cast<std::uint32_t>(stages));
    }

    void CheckState(const nri::AccessLayoutStage& actual, const nri::AccessLayoutStage& expected)
    {
        CheckState(actual, expected.access, expected.layout, expected.stages);
    }

    // Compile-must-succeed helper: surfaces the refusal message in the
    // failure output when a case that should compile does not.
    Arcane::RgCompiled CompileOk(const Arcane::RenderGraph& graph)
    {
        std::string error;
        std::optional<Arcane::RgCompiled> compiled = graph.Compile(&error);
        INFO("Compile() refused: " << error);
        REQUIRE(compiled.has_value());
        return *compiled;
    }

    std::size_t TotalBarriers(const Arcane::RgCompiled& compiled)
    {
        std::size_t total = 0;
        for (const Arcane::RgCompiledNode& node : compiled.nodes)
            total += node.preBarriers.size();
        return total;
    }

    // Drives one RgUsage through Compile and hands back the state it derived.
    // Node 0 seeds a defined state with `seed` (a different usage, so node 1
    // always crosses a state edge and therefore always emits exactly one
    // barrier to read the `after` off). Both accesses are Write(): isWrite
    // feeds only the read-before-write refusal, never the state mapping, so
    // writing throughout keeps the helper's shape uniform across read-ish and
    // write-ish usages alike.
    nri::AccessLayoutStage DerivedTextureState(Arcane::RgUsage seed, Arcane::RgUsage usage)
    {
        Arcane::RenderGraph graph;
        Arcane::RgTexture tex;

        graph.AddNode("seed", Arcane::RenderGraph::NodeKind::Copy,
            [&](Arcane::RenderGraphBuilder& builder)
            {
                tex = builder.CreateTexture("tex", MakeColorDesc());
                builder.Write(tex, seed);
            },
            [](Arcane::RenderGraphNodeContext&) {});

        graph.AddNode("under-test", Arcane::RenderGraph::NodeKind::Compute,
            [&](Arcane::RenderGraphBuilder& builder) { builder.Write(tex, usage); },
            [](Arcane::RenderGraphNodeContext&) {});

        const Arcane::RgCompiled compiled = CompileOk(graph);
        REQUIRE(compiled.nodes.size() == 2);
        REQUIRE(compiled.nodes[1].preBarriers.size() == 1);
        return compiled.nodes[1].preBarriers[0].after;
    }

    nri::AccessLayoutStage DerivedBufferState(Arcane::RgUsage seed, Arcane::RgUsage usage)
    {
        Arcane::RenderGraph graph;
        Arcane::RgBuffer buf;

        graph.AddNode("seed", Arcane::RenderGraph::NodeKind::Copy,
            [&](Arcane::RenderGraphBuilder& builder)
            {
                buf = builder.CreateBuffer("buf", 256, nri::BufferUsageBits::SHADER_RESOURCE);
                builder.Write(buf, seed);
            },
            [](Arcane::RenderGraphNodeContext&) {});

        graph.AddNode("under-test", Arcane::RenderGraph::NodeKind::Compute,
            [&](Arcane::RenderGraphBuilder& builder) { builder.Write(buf, usage); },
            [](Arcane::RenderGraphNodeContext&) {});

        const Arcane::RgCompiled compiled = CompileOk(graph);
        REQUIRE(compiled.nodes.size() == 2);
        REQUIRE(compiled.nodes[1].preBarriers.size() == 1);
        return compiled.nodes[1].preBarriers[0].after;
    }

    // A Raster node that writes `tex` as a colour attachment and declares it
    // as its attachment -- the shape most of the barrier cases below need.
    void AddColorNode(Arcane::RenderGraph& graph, const char* name, Arcane::RgTexture tex)
    {
        graph.AddNode(name, Arcane::RenderGraph::NodeKind::Raster,
            [&graph, tex](Arcane::RenderGraphBuilder& builder)
            {
                builder.Write(tex, Arcane::RgUsage::ColorWrite);
                const Arcane::RgTexture attachments[] = { tex };
                graph.SetColorAttachments(attachments);
            },
            [](Arcane::RenderGraphNodeContext&) {});
    }

    void AddShaderReadNode(Arcane::RenderGraph& graph, const char* name, Arcane::RgTexture tex)
    {
        graph.AddNode(name, Arcane::RenderGraph::NodeKind::Compute,
            [tex](Arcane::RenderGraphBuilder& builder) { builder.Read(tex, Arcane::RgUsage::ShaderRead); },
            [](Arcane::RenderGraphNodeContext&) {});
    }

    // The state ColorWrite maps to -- the entry state an imported "canvas"
    // that is already a colour attachment arrives in.
    constexpr nri::AccessLayoutStage kColorState{
        nri::AccessBits::COLOR_ATTACHMENT, nri::Layout::COLOR_ATTACHMENT, nri::StageBits::COLOR_ATTACHMENT };
    constexpr nri::AccessLayoutStage kPresentState{
        nri::AccessBits::NONE, nri::Layout::PRESENT, nri::StageBits::NONE };
}

TEST_CASE("rendergraph: CreateTexture/CreateBuffer handles are unique and their names are retrievable", "[nri]")
{
    Arcane::RenderGraph graph;
    Arcane::RgTexture texA, texB;
    Arcane::RgBuffer  bufA;

    graph.AddNode("setup", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            const Arcane::RgTextureDesc desc = MakeColorDesc();
            texA = builder.CreateTexture("texA", desc);
            texB = builder.CreateTexture("texB", desc);
            bufA = builder.CreateBuffer("bufA", 256, nri::BufferUsageBits::SHADER_RESOURCE);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    CHECK(graph.TextureCount() == 2);
    CHECK(graph.BufferCount() == 1);

    // Unique: two CreateTexture calls never alias the same slot.
    CHECK(texA.index != texB.index);

    // Name-retrievable: the name passed to Create*/Import* round-trips.
    CHECK(std::string(graph.NameOf(texA)) == "texA");
    CHECK(std::string(graph.NameOf(texB)) == "texB");
    CHECK(std::string(graph.NameOf(bufA)) == "bufA");
}

TEST_CASE("rendergraph: CreateTexture is transient, ImportTexture is not", "[nri]")
{
    Arcane::RenderGraph graph;
    Arcane::RgTexture transientTex, importedTex;

    graph.AddNode("setup", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            transientTex = builder.CreateTexture("transient", MakeColorDesc());

            // nullptr is fine here: this task's declaration side never
            // dereferences an imported native pointer, only classifies and
            // stores it for Task 6's executor.
            nri::AccessLayoutStage entry{};
            nri::AccessLayoutStage exit{};
            importedTex = builder.ImportTexture("imported", nullptr, entry, exit, /*persistent=*/true);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    CHECK(graph.IsTransient(transientTex));
    CHECK_FALSE(graph.IsTransient(importedTex));
}

TEST_CASE("rendergraph: CreateBuffer is transient, ImportBuffer is not", "[nri]")
{
    Arcane::RenderGraph graph;
    Arcane::RgBuffer transientBuf, importedBuf;

    graph.AddNode("setup", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            transientBuf = builder.CreateBuffer("transient", 128, nri::BufferUsageBits::SHADER_RESOURCE);
            importedBuf  = builder.ImportBuffer("imported", nullptr, 128);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    CHECK(graph.IsTransient(transientBuf));
    CHECK_FALSE(graph.IsTransient(importedBuf));
}

TEST_CASE("rendergraph: importing a resource marks it already-written (a defined incoming state)", "[nri]")
{
    Arcane::RenderGraph graph;
    Arcane::RgTexture importedTex;
    Arcane::RgBuffer  importedBuf;

    graph.AddNode("setup", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            nri::AccessLayoutStage entry{};
            nri::AccessLayoutStage exit{};
            importedTex = builder.ImportTexture("swapchain", nullptr, entry, exit, /*persistent=*/false);
            importedBuf = builder.ImportBuffer("readback", nullptr, 64);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    CHECK(graph.WasWritten(importedTex));
    CHECK(graph.WasWritten(importedBuf));
}

TEST_CASE("rendergraph: a Read of a never-written transient texture is accepted at declaration -- Task 4's Compile() refuses it later", "[nri]")
{
    Arcane::RenderGraph graph;
    Arcane::RgTexture tex;

    // Declaring the read must not throw, assert-fail, or otherwise abort --
    // the assert-later model: the precondition is recorded (WasWritten()
    // still false below), not refused, at declaration time.
    graph.AddNode("setup", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            tex = builder.CreateTexture("never-written", MakeColorDesc());
            builder.Read(tex, Arcane::RgUsage::ShaderRead);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    CHECK(graph.NodeCount() == 1);
    CHECK_FALSE(graph.WasWritten(tex));
}

TEST_CASE("rendergraph: a Read of a never-written transient buffer is accepted at declaration -- Task 4's Compile() refuses it later", "[nri]")
{
    Arcane::RenderGraph graph;
    Arcane::RgBuffer buf;

    graph.AddNode("setup", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            buf = builder.CreateBuffer("never-written", 64, nri::BufferUsageBits::SHADER_RESOURCE);
            builder.Read(buf, Arcane::RgUsage::ShaderRead);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    CHECK(graph.NodeCount() == 1);
    CHECK_FALSE(graph.WasWritten(buf));
}

TEST_CASE("rendergraph: a Write records the transient as written; a subsequent Read does not un-write it", "[nri]")
{
    Arcane::RenderGraph graph;
    Arcane::RgTexture tex;

    graph.AddNode("setup", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            tex = builder.CreateTexture("written", MakeColorDesc());
            builder.Write(tex, Arcane::RgUsage::ShaderWriteCs);
        },
        [](Arcane::RenderGraphNodeContext&) {});
    CHECK(graph.WasWritten(tex));

    graph.AddNode("reader", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder) { builder.Read(tex, Arcane::RgUsage::ShaderRead); },
        [](Arcane::RenderGraphNodeContext&) {});
    CHECK(graph.WasWritten(tex));
}

TEST_CASE("rendergraph: a Raster node with a color attachment satisfies the attachment requirement", "[nri]")
{
    Arcane::RenderGraph graph;

    graph.AddNode("raster-color", Arcane::RenderGraph::NodeKind::Raster,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            const Arcane::RgTexture color = builder.CreateTexture("color", MakeColorDesc());
            builder.Write(color, Arcane::RgUsage::ColorWrite);
            const Arcane::RgTexture attachments[] = { color };
            graph.SetColorAttachments(attachments);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    REQUIRE(graph.NodeCount() == 1);
    CHECK(graph.NodeHasRequiredAttachments(0));
}

TEST_CASE("rendergraph: a Raster node with only a depth attachment also satisfies the attachment requirement", "[nri]")
{
    Arcane::RenderGraph graph;

    graph.AddNode("raster-depth-only", Arcane::RenderGraph::NodeKind::Raster,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            Arcane::RgTextureDesc depthDesc = MakeColorDesc();
            depthDesc.depthStencil = true;
            const Arcane::RgTexture depth = builder.CreateTexture("depth", depthDesc);
            builder.Write(depth, Arcane::RgUsage::DepthWrite);
            graph.SetDepthAttachment(depth);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    REQUIRE(graph.NodeCount() == 1);
    CHECK(graph.NodeHasRequiredAttachments(0));
}

TEST_CASE("rendergraph: a Raster node declared with no attachments fails the attachment requirement (recorded, non-fatal)", "[nri]")
{
    Arcane::RenderGraph graph;

    // No SetColorAttachments/SetDepthAttachment call in this Setup --
    // declaration must still succeed (non-fatal ARC_ENSURE), just with the
    // miss recorded for the caller/Task 4 to see.
    graph.AddNode("raster-missing-attachment", Arcane::RenderGraph::NodeKind::Raster,
        [](Arcane::RenderGraphBuilder&) {},
        [](Arcane::RenderGraphNodeContext&) {});

    REQUIRE(graph.NodeCount() == 1);
    CHECK_FALSE(graph.NodeHasRequiredAttachments(0));
}

TEST_CASE("rendergraph: Compute/Copy nodes have no attachment requirement", "[nri]")
{
    Arcane::RenderGraph graph;

    graph.AddNode("compute", Arcane::RenderGraph::NodeKind::Compute,
        [](Arcane::RenderGraphBuilder&) {}, [](Arcane::RenderGraphNodeContext&) {});
    graph.AddNode("copy", Arcane::RenderGraph::NodeKind::Copy,
        [](Arcane::RenderGraphBuilder&) {}, [](Arcane::RenderGraphNodeContext&) {});

    CHECK(graph.NodeHasRequiredAttachments(0));
    CHECK(graph.NodeHasRequiredAttachments(1));
}

TEST_CASE("rendergraph: nodes preserve declaration order -- execution order = declaration order, no reordering in Phase 2", "[nri]")
{
    Arcane::RenderGraph graph;

    graph.AddNode("first",  Arcane::RenderGraph::NodeKind::Compute, [](Arcane::RenderGraphBuilder&) {}, [](Arcane::RenderGraphNodeContext&) {});
    graph.AddNode("second", Arcane::RenderGraph::NodeKind::Compute, [](Arcane::RenderGraphBuilder&) {}, [](Arcane::RenderGraphNodeContext&) {});
    graph.AddNode("third",  Arcane::RenderGraph::NodeKind::Compute, [](Arcane::RenderGraphBuilder&) {}, [](Arcane::RenderGraphNodeContext&) {});

    REQUIRE(graph.NodeCount() == 3);
    CHECK(std::string(graph.NodeName(0)) == "first");
    CHECK(std::string(graph.NodeName(1)) == "second");
    CHECK(std::string(graph.NodeName(2)) == "third");
}

TEST_CASE("rendergraph: Reset clears every declared node and resource and bumps the generation", "[nri]")
{
    Arcane::RenderGraph graph;
    const std::uint32_t generationBefore = graph.Generation();

    graph.AddNode("node", Arcane::RenderGraph::NodeKind::Compute,
        [](Arcane::RenderGraphBuilder& builder) { builder.CreateTexture("tex", MakeColorDesc()); },
        [](Arcane::RenderGraphNodeContext&) {});
    REQUIRE(graph.NodeCount() == 1);
    REQUIRE(graph.TextureCount() == 1);

    graph.Reset();

    CHECK(graph.NodeCount() == 0);
    CHECK(graph.TextureCount() == 0);
    CHECK(graph.BufferCount() == 0);
    CHECK(graph.Generation() == generationBefore + 1);
}

TEST_CASE("rendergraph: a default-constructed (kInvalid) handle is never valid", "[nri]")
{
    Arcane::RenderGraph graph;

    // Fresh graph, no resources declared at all -- kInvalid must read as
    // invalid regardless, since it is checked verbatim before any decode.
    CHECK_FALSE(graph.IsHandleValid(Arcane::RgTexture{}));
    CHECK_FALSE(graph.IsHandleValid(Arcane::RgBuffer{}));
}

TEST_CASE("rendergraph: a handle minted before Reset() is caught as stale after it, even when the new graph re-declares the same slot -- the common, steady-state case", "[nri]")
{
    // This is the fix-round-1 case: a plain bounds check alone only catches
    // a stale handle when the post-Reset() graph is SMALLER than before.
    // The generation packed into the handle's encoded index must catch the
    // common per-frame-rebuild case too, where the new graph re-declares
    // the SAME (or a larger) number of resources and the stale handle's
    // slot bits are still perfectly in bounds.
    Arcane::RenderGraph graph;
    Arcane::RgTexture staleTex;

    graph.AddNode("frame-1", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder) { staleTex = builder.CreateTexture("tex", MakeColorDesc()); },
        [](Arcane::RenderGraphNodeContext&) {});
    REQUIRE(graph.IsHandleValid(staleTex));

    graph.Reset();

    Arcane::RgTexture freshTex;
    graph.AddNode("frame-2", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder) { freshTex = builder.CreateTexture("tex", MakeColorDesc()); },
        [](Arcane::RenderGraphNodeContext&) {});

    // Same declaration shape as frame 1 -- freshTex occupies the identical
    // slot staleTex used to. A bounds-only check would have let staleTex
    // silently alias it; the generation encoding must not.
    CHECK_FALSE(graph.IsHandleValid(staleTex));
    CHECK(graph.IsHandleValid(freshTex));
    CHECK(std::string(graph.NameOf(freshTex)) == "tex");
}

TEST_CASE("rendergraph: a handle minted before Reset() also stays stale when the new graph re-declares a LARGER shape", "[nri]")
{
    Arcane::RenderGraph graph;
    Arcane::RgTexture staleTex;

    graph.AddNode("frame-1", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder) { staleTex = builder.CreateTexture("tex", MakeColorDesc()); },
        [](Arcane::RenderGraphNodeContext&) {});

    graph.Reset();

    graph.AddNode("frame-2", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            builder.CreateTexture("a", MakeColorDesc());
            builder.CreateTexture("b", MakeColorDesc());
            builder.CreateTexture("c", MakeColorDesc());
        },
        [](Arcane::RenderGraphNodeContext&) {});

    REQUIRE(graph.TextureCount() == 3);
    CHECK_FALSE(graph.IsHandleValid(staleTex));
}

// =====================================================================
// Task 4 -- Compile(): derived barriers, transient lifetimes, pool slots.
//
// Every case below is headless and device-free: Compile() is pure, so the
// whole barrier-derivation surface is unit-testable without a GPU. The
// usage->state rows come first (they are what every other case is written
// against), then the barrier-edge rules, then lifetimes/pool slots, then
// the compile refusals.
// =====================================================================

TEST_CASE("rendergraph compile: an empty graph compiles to an empty result", "[nri]")
{
    Arcane::RenderGraph graph;
    const Arcane::RgCompiled compiled = CompileOk(graph);

    CHECK(compiled.nodes.empty());
    CHECK(compiled.exitBarriers.empty());
    CHECK(compiled.transients.empty());
    CHECK(compiled.transientLifetimes.empty());
    CHECK(compiled.transientPoolSlot.empty());
    CHECK(compiled.poolSlotCount == 0);
}

// ---------------------------------------------------------------------
// Rule: usage -> (access, layout, stage) mapping table. One case per row.
// ---------------------------------------------------------------------

TEST_CASE("rendergraph compile: ColorWrite maps to COLOR_ATTACHMENT access/layout/stage", "[nri]")
{
    CheckState(DerivedTextureState(Arcane::RgUsage::CopyDst, Arcane::RgUsage::ColorWrite),
               nri::AccessBits::COLOR_ATTACHMENT, nri::Layout::COLOR_ATTACHMENT, nri::StageBits::COLOR_ATTACHMENT);
}

TEST_CASE("rendergraph compile: DepthWrite maps to DEPTH_STENCIL_ATTACHMENT access/layout/stage", "[nri]")
{
    CheckState(DerivedTextureState(Arcane::RgUsage::CopyDst, Arcane::RgUsage::DepthWrite),
               nri::AccessBits::DEPTH_STENCIL_ATTACHMENT, nri::Layout::DEPTH_STENCIL_ATTACHMENT,
               nri::StageBits::DEPTH_STENCIL_ATTACHMENT);
}

TEST_CASE("rendergraph compile: ShaderRead maps to SHADER_RESOURCE across every shader stage this phase uses", "[nri]")
{
    CheckState(DerivedTextureState(Arcane::RgUsage::CopyDst, Arcane::RgUsage::ShaderRead),
               nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, kShaderReadStages);
}

TEST_CASE("rendergraph compile: ShaderWriteCs maps to SHADER_RESOURCE_STORAGE + COMPUTE_SHADER", "[nri]")
{
    CheckState(DerivedTextureState(Arcane::RgUsage::CopyDst, Arcane::RgUsage::ShaderWriteCs),
               nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::Layout::SHADER_RESOURCE_STORAGE,
               nri::StageBits::COMPUTE_SHADER);
}

TEST_CASE("rendergraph compile: CopySrc maps to COPY_SOURCE + COPY stage", "[nri]")
{
    CheckState(DerivedTextureState(Arcane::RgUsage::CopyDst, Arcane::RgUsage::CopySrc),
               nri::AccessBits::COPY_SOURCE, nri::Layout::COPY_SOURCE, nri::StageBits::COPY);
}

TEST_CASE("rendergraph compile: CopyDst maps to COPY_DESTINATION + COPY stage", "[nri]")
{
    CheckState(DerivedTextureState(Arcane::RgUsage::ColorWrite, Arcane::RgUsage::CopyDst),
               nri::AccessBits::COPY_DESTINATION, nri::Layout::COPY_DESTINATION, nri::StageBits::COPY);
}

TEST_CASE("rendergraph compile: Present maps to NONE access, PRESENT layout, NONE stage", "[nri]")
{
    // NRI's own instruction for Layout::PRESENT ("NONE (use after.stages =
    // StageBits::NONE)") -- and exactly what Phase 1's NriSmoke.cpp writes
    // by hand for its present transition.
    CheckState(DerivedTextureState(Arcane::RgUsage::ColorWrite, Arcane::RgUsage::Present),
               nri::AccessBits::NONE, nri::Layout::PRESENT, nri::StageBits::NONE);
}

TEST_CASE("rendergraph compile: ReadbackHost maps to COPY_DESTINATION on a buffer, with no layout", "[nri]")
{
    CheckState(DerivedBufferState(Arcane::RgUsage::ShaderWriteCs, Arcane::RgUsage::ReadbackHost),
               nri::AccessBits::COPY_DESTINATION, nri::Layout::UNDEFINED, nri::StageBits::COPY);
}

TEST_CASE("rendergraph compile: a buffer's derived state never carries a layout", "[nri]")
{
    // The same usage that gives a texture Layout::SHADER_RESOURCE must give
    // a buffer Layout::UNDEFINED -- nri::BufferBarrierDesc has no layout.
    CheckState(DerivedBufferState(Arcane::RgUsage::CopyDst, Arcane::RgUsage::ShaderRead),
               nri::AccessBits::SHADER_RESOURCE, nri::Layout::UNDEFINED, kShaderReadStages);
}

// ---------------------------------------------------------------------
// Rule: write->read and read->write edges each produce exactly ONE
// barrier; an imported resource's entry state seeds the chain; a
// transient's chain starts from {NONE, UNDEFINED} with no special case.
// ---------------------------------------------------------------------

TEST_CASE("rendergraph compile: (a) write -> shader read -> write over an imported canvas produces exactly 2 barriers", "[nri]")
{
    Arcane::RenderGraph graph;
    Arcane::RgTexture canvas;

    graph.AddNode("import", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            // Entry == the ColorWrite state, so node A's write is already the
            // current state and contributes NO barrier: the two barriers this
            // case counts are precisely the write->read and read->write EDGES.
            canvas = builder.ImportTexture("canvas", nullptr, kColorState, kColorState, /*persistent=*/true);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    AddColorNode(graph, "A-write-color", canvas);
    AddShaderReadNode(graph, "B-shader-read", canvas);
    AddColorNode(graph, "C-write-color", canvas);

    const Arcane::RgCompiled compiled = CompileOk(graph);
    REQUIRE(compiled.nodes.size() == 4);   // node 0 is the import-only node

    CHECK(TotalBarriers(compiled) == 2);
    CHECK(compiled.nodes[0].preBarriers.empty());
    CHECK(compiled.nodes[1].preBarriers.empty());   // A: already in the entry state

    REQUIRE(compiled.nodes[2].preBarriers.size() == 1);   // B: write -> read
    const Arcane::RgBarrier& toRead = compiled.nodes[2].preBarriers[0];
    CHECK(toRead.isTexture);
    CheckState(toRead.before, kColorState);
    CheckState(toRead.after, nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, kShaderReadStages);

    REQUIRE(compiled.nodes[3].preBarriers.size() == 1);   // C: read -> write
    const Arcane::RgBarrier& toWrite = compiled.nodes[3].preBarriers[0];
    CheckState(toWrite.before, nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, kShaderReadStages);
    CheckState(toWrite.after, kColorState);

    // Exit == entry == the final state, so there is nothing to restore.
    CHECK(compiled.exitBarriers.empty());
}

TEST_CASE("rendergraph compile: the same chain over a TRANSIENT adds a first-use barrier from {NONE, UNDEFINED}", "[nri]")
{
    // The "transient first-use needs no from-UNDEFINED special case beyond
    // before = {UNKNOWN, UNDEFINED}" rule: first use is an ordinary barrier
    // whose `before` happens to be the unknown state.
    Arcane::RenderGraph graph;
    Arcane::RgTexture tex;

    graph.AddNode("create", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder) { tex = builder.CreateTexture("canvas", MakeColorDesc()); },
        [](Arcane::RenderGraphNodeContext&) {});

    AddColorNode(graph, "A-write-color", tex);
    AddShaderReadNode(graph, "B-shader-read", tex);
    AddColorNode(graph, "C-write-color", tex);

    const Arcane::RgCompiled compiled = CompileOk(graph);
    REQUIRE(compiled.nodes.size() == 4);

    CHECK(TotalBarriers(compiled) == 3);   // first use + the same 2 edges

    REQUIRE(compiled.nodes[1].preBarriers.size() == 1);
    CheckState(compiled.nodes[1].preBarriers[0].before, kUnknownState);
    CheckState(compiled.nodes[1].preBarriers[0].after, kColorState);

    // A transient is never restored on exit -- it does not outlive the frame.
    CHECK(compiled.exitBarriers.empty());
}

// ---------------------------------------------------------------------
// Rule: consecutive same-state declarations produce NO barrier.
// ---------------------------------------------------------------------

TEST_CASE("rendergraph compile: (b) two back-to-back reads of the same state produce no barrier at all", "[nri]")
{
    Arcane::RenderGraph graph;
    Arcane::RgTexture tex;

    const nri::AccessLayoutStage shaderReadState{
        nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, kShaderReadStages };

    graph.AddNode("import", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            tex = builder.ImportTexture("already-readable", nullptr, shaderReadState, shaderReadState, false);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    AddShaderReadNode(graph, "reader-1", tex);
    AddShaderReadNode(graph, "reader-2", tex);

    const Arcane::RgCompiled compiled = CompileOk(graph);
    CHECK(TotalBarriers(compiled) == 0);
    CHECK(compiled.exitBarriers.empty());
}

TEST_CASE("rendergraph compile: a second reader after a write->read edge adds nothing", "[nri]")
{
    Arcane::RenderGraph graph;
    Arcane::RgTexture tex;

    graph.AddNode("writer", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            tex = builder.CreateTexture("tex", MakeColorDesc());
            builder.Write(tex, Arcane::RgUsage::ShaderWriteCs);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    AddShaderReadNode(graph, "reader-1", tex);
    AddShaderReadNode(graph, "reader-2", tex);

    const Arcane::RgCompiled compiled = CompileOk(graph);
    REQUIRE(compiled.nodes.size() == 3);
    CHECK(compiled.nodes[0].preBarriers.size() == 1);   // unknown -> storage
    CHECK(compiled.nodes[1].preBarriers.size() == 1);   // storage -> shader resource
    CHECK(compiled.nodes[2].preBarriers.empty());       // the no-op edge
}

TEST_CASE("rendergraph compile: two consecutive colour writes of the same target produce one barrier, not two", "[nri]")
{
    Arcane::RenderGraph graph;
    Arcane::RgTexture tex;

    graph.AddNode("create", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder) { tex = builder.CreateTexture("tex", MakeColorDesc()); },
        [](Arcane::RenderGraphNodeContext&) {});

    AddColorNode(graph, "pass-1", tex);
    AddColorNode(graph, "pass-2", tex);

    const Arcane::RgCompiled compiled = CompileOk(graph);
    CHECK(TotalBarriers(compiled) == 1);
    CHECK(compiled.nodes[2].preBarriers.empty());
}

// ---------------------------------------------------------------------
// Rule: imported entry seeds the chain, imported exit appends to
// exitBarriers.
// ---------------------------------------------------------------------

TEST_CASE("rendergraph compile: (c) an imported swapchain seeds from its entry state and exits to PRESENT", "[nri]")
{
    Arcane::RenderGraph graph;
    Arcane::RgTexture backbuffer;

    graph.AddNode("import", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            // An acquired backbuffer's contents are undefined -- the entry
            // state the swapchain hands the graph.
            backbuffer = builder.ImportTexture("backbuffer", nullptr, kUnknownState, kPresentState, false);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    AddColorNode(graph, "draw", backbuffer);

    const Arcane::RgCompiled compiled = CompileOk(graph);
    REQUIRE(compiled.nodes.size() == 2);

    REQUIRE(compiled.nodes[1].preBarriers.size() == 1);
    CheckState(compiled.nodes[1].preBarriers[0].before, kUnknownState);
    CheckState(compiled.nodes[1].preBarriers[0].after, kColorState);

    REQUIRE(compiled.exitBarriers.size() == 1);
    CHECK(compiled.exitBarriers[0].isTexture);
    CheckState(compiled.exitBarriers[0].before, kColorState);
    CheckState(compiled.exitBarriers[0].after, kPresentState);
}

TEST_CASE("rendergraph compile: an imported texture no node touches still gets its entry -> exit barrier", "[nri]")
{
    Arcane::RenderGraph graph;

    graph.AddNode("import-only", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            builder.ImportTexture("untouched", nullptr, kColorState, kPresentState, false);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    const Arcane::RgCompiled compiled = CompileOk(graph);
    CHECK(TotalBarriers(compiled) == 0);

    // The exit state is a PROMISE the import made; nothing moved the resource
    // off its entry state, so keeping that promise still costs one barrier.
    REQUIRE(compiled.exitBarriers.size() == 1);
    CheckState(compiled.exitBarriers[0].before, kColorState);
    CheckState(compiled.exitBarriers[0].after, kPresentState);
}

TEST_CASE("rendergraph compile: an imported texture whose exit equals its final state gets no exit barrier", "[nri]")
{
    Arcane::RenderGraph graph;
    Arcane::RgTexture tex;

    graph.AddNode("import", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            tex = builder.ImportTexture("history", nullptr, kUnknownState, kColorState, /*persistent=*/true);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    AddColorNode(graph, "draw", tex);

    const Arcane::RgCompiled compiled = CompileOk(graph);
    CHECK(compiled.exitBarriers.empty());
}

TEST_CASE("rendergraph compile: imported buffers never produce exit barriers", "[nri]")
{
    // ImportBuffer() takes no entry/exit state, so there is nothing to
    // restore -- and the buffer's chain starts from the unknown state.
    Arcane::RenderGraph graph;
    Arcane::RgBuffer buf;

    graph.AddNode("readback", Arcane::RenderGraph::NodeKind::Copy,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            buf = builder.ImportBuffer("readback", nullptr, 4096);
            builder.Write(buf, Arcane::RgUsage::ReadbackHost);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    const Arcane::RgCompiled compiled = CompileOk(graph);
    REQUIRE(compiled.nodes.size() == 1);
    REQUIRE(compiled.nodes[0].preBarriers.size() == 1);
    CHECK_FALSE(compiled.nodes[0].preBarriers[0].isTexture);
    CheckState(compiled.nodes[0].preBarriers[0].before, kUnknownState);
    CHECK(compiled.exitBarriers.empty());
}

TEST_CASE("rendergraph compile: a buffer-only graph derives buffer barriers and no textures", "[nri]")
{
    Arcane::RenderGraph graph;
    Arcane::RgBuffer buf;

    graph.AddNode("fill", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            buf = builder.CreateBuffer("scratch", 1024, nri::BufferUsageBits::SHADER_RESOURCE_STORAGE);
            builder.Write(buf, Arcane::RgUsage::ShaderWriteCs);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    graph.AddNode("consume", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder) { builder.Read(buf, Arcane::RgUsage::ShaderRead); },
        [](Arcane::RenderGraphNodeContext&) {});

    const Arcane::RgCompiled compiled = CompileOk(graph);
    REQUIRE(TotalBarriers(compiled) == 2);
    for (const Arcane::RgCompiledNode& node : compiled.nodes)
        for (const Arcane::RgBarrier& barrier : node.preBarriers)
        {
            CHECK_FALSE(barrier.isTexture);
            CHECK(static_cast<std::uint32_t>(barrier.before.layout) == static_cast<std::uint32_t>(nri::Layout::UNDEFINED));
            CHECK(static_cast<std::uint32_t>(barrier.after.layout) == static_cast<std::uint32_t>(nri::Layout::UNDEFINED));
        }

    REQUIRE(compiled.transients.size() == 1);
    CHECK_FALSE(compiled.transients[0].isTexture);
}

TEST_CASE("rendergraph compile: a Raster node with only a depth attachment derives the depth-stencil state", "[nri]")
{
    Arcane::RenderGraph graph;

    graph.AddNode("depth-prepass", Arcane::RenderGraph::NodeKind::Raster,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            const Arcane::RgTexture depth = builder.CreateTexture("depth", MakeDepthDesc());
            builder.Write(depth, Arcane::RgUsage::DepthWrite);
            graph.SetDepthAttachment(depth);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    const Arcane::RgCompiled compiled = CompileOk(graph);
    REQUIRE(compiled.nodes.size() == 1);
    REQUIRE(compiled.nodes[0].preBarriers.size() == 1);
    CheckState(compiled.nodes[0].preBarriers[0].before, kUnknownState);
    CheckState(compiled.nodes[0].preBarriers[0].after,
               nri::AccessBits::DEPTH_STENCIL_ATTACHMENT, nri::Layout::DEPTH_STENCIL_ATTACHMENT,
               nri::StageBits::DEPTH_STENCIL_ATTACHMENT);
}

// ---------------------------------------------------------------------
// Transient lifetimes + pool-slot assignment.
// ---------------------------------------------------------------------

TEST_CASE("rendergraph compile: (d) transient lifetimes are the first and last node that touch the resource", "[nri]")
{
    Arcane::RenderGraph graph;
    Arcane::RgTexture straight;

    graph.AddNode("n0", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            straight = builder.CreateTexture("straight", MakeColorDesc());
            builder.Write(straight, Arcane::RgUsage::ShaderWriteCs);
        },
        [](Arcane::RenderGraphNodeContext&) {});
    AddShaderReadNode(graph, "n1", straight);
    AddShaderReadNode(graph, "n2", straight);

    const Arcane::RgCompiled compiled = CompileOk(graph);
    REQUIRE(compiled.transients.size() == 1);
    REQUIRE(compiled.transientLifetimes.size() == 1);
    CHECK(compiled.transientLifetimes[0].first == 0);
    CHECK(compiled.transientLifetimes[0].last == 2);
}

TEST_CASE("rendergraph compile: (d) a transient skipped by an intervening node still spans it", "[nri]")
{
    // Used at nodes 1 and 3 -- node 2 never touches it. The lifetime is the
    // INCLUSIVE span, so the resource is live across node 2 too and cannot be
    // recycled underneath it.
    Arcane::RenderGraph graph;
    Arcane::RgTexture tex;

    graph.AddNode("n0", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder) { tex = builder.CreateTexture("skipper", MakeColorDesc()); },
        [](Arcane::RenderGraphNodeContext&) {});
    graph.AddNode("n1", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder) { builder.Write(tex, Arcane::RgUsage::ShaderWriteCs); },
        [](Arcane::RenderGraphNodeContext&) {});
    graph.AddNode("n2", Arcane::RenderGraph::NodeKind::Compute,
        [](Arcane::RenderGraphBuilder&) {}, [](Arcane::RenderGraphNodeContext&) {});
    AddShaderReadNode(graph, "n3", tex);

    const Arcane::RgCompiled compiled = CompileOk(graph);
    REQUIRE(compiled.transientLifetimes.size() == 1);
    CHECK(compiled.transientLifetimes[0].first == 1);
    CHECK(compiled.transientLifetimes[0].last == 3);
}

TEST_CASE("rendergraph compile: a transient no node touches has no lifetime and no pool slot", "[nri]")
{
    Arcane::RenderGraph graph;

    graph.AddNode("n0", Arcane::RenderGraph::NodeKind::Compute,
        [](Arcane::RenderGraphBuilder& builder) { builder.CreateTexture("unused", MakeColorDesc()); },
        [](Arcane::RenderGraphNodeContext&) {});

    const Arcane::RgCompiled compiled = CompileOk(graph);
    REQUIRE(compiled.transients.size() == 1);
    CHECK(compiled.transientLifetimes[0].first == Arcane::kRgNoNode);
    CHECK(compiled.transientLifetimes[0].last == Arcane::kRgNoNode);
    CHECK(compiled.transientPoolSlot[0] == Arcane::kRgNoPoolSlot);
    CHECK(compiled.poolSlotCount == 0);
}

TEST_CASE("rendergraph compile: (e) two identical transients with disjoint lifetimes share one pool slot", "[nri]")
{
    Arcane::RenderGraph graph;
    Arcane::RgTexture early, late;

    graph.AddNode("declare", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            early = builder.CreateTexture("early", MakeColorDesc());
            late  = builder.CreateTexture("late", MakeColorDesc());
        },
        [](Arcane::RenderGraphNodeContext&) {});

    graph.AddNode("uses-early", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder) { builder.Write(early, Arcane::RgUsage::ShaderWriteCs); },
        [](Arcane::RenderGraphNodeContext&) {});
    graph.AddNode("uses-late", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder) { builder.Write(late, Arcane::RgUsage::ShaderWriteCs); },
        [](Arcane::RenderGraphNodeContext&) {});

    const Arcane::RgCompiled compiled = CompileOk(graph);
    REQUIRE(compiled.transientPoolSlot.size() == 2);
    CHECK(compiled.transientLifetimes[0].first == 1);
    CHECK(compiled.transientLifetimes[0].last == 1);
    CHECK(compiled.transientLifetimes[1].first == 2);
    CHECK(compiled.transientLifetimes[1].last == 2);
    CHECK(compiled.transientPoolSlot[0] == compiled.transientPoolSlot[1]);
    CHECK(compiled.poolSlotCount == 1);
}

TEST_CASE("rendergraph compile: (e) two identical transients with overlapping lifetimes get different pool slots", "[nri]")
{
    Arcane::RenderGraph graph;
    Arcane::RgTexture a, b;

    graph.AddNode("both", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            a = builder.CreateTexture("a", MakeColorDesc());
            b = builder.CreateTexture("b", MakeColorDesc());
            builder.Write(a, Arcane::RgUsage::ShaderWriteCs);
            builder.Write(b, Arcane::RgUsage::ShaderWriteCs);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    const Arcane::RgCompiled compiled = CompileOk(graph);
    REQUIRE(compiled.transientPoolSlot.size() == 2);
    CHECK(compiled.transientPoolSlot[0] != compiled.transientPoolSlot[1]);
    CHECK(compiled.poolSlotCount == 2);
}

TEST_CASE("rendergraph compile: (e) transients whose lifetimes meet at one node do NOT share a pool slot", "[nri]")
{
    // `early` and `late` are both live at node 1 -- the spans touch, so they
    // overlap and must not be recycled into one another.
    Arcane::RenderGraph graph;
    Arcane::RgTexture early, late;

    graph.AddNode("declare", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            early = builder.CreateTexture("early", MakeColorDesc());
            late  = builder.CreateTexture("late", MakeColorDesc());
        },
        [](Arcane::RenderGraphNodeContext&) {});
    graph.AddNode("hand-off", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            builder.Write(early, Arcane::RgUsage::ShaderWriteCs);
            builder.Write(late, Arcane::RgUsage::ShaderWriteCs);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    const Arcane::RgCompiled compiled = CompileOk(graph);
    CHECK(compiled.transientPoolSlot[0] != compiled.transientPoolSlot[1]);
    CHECK(compiled.poolSlotCount == 2);
}

TEST_CASE("rendergraph compile: (e) disjoint lifetimes with DIFFERENT descs do not share a pool slot", "[nri]")
{
    Arcane::RenderGraph graph;
    Arcane::RgTexture small, large;

    graph.AddNode("declare", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            small = builder.CreateTexture("small", MakeColorDesc(64, 64));
            large = builder.CreateTexture("large", MakeColorDesc(128, 128));
        },
        [](Arcane::RenderGraphNodeContext&) {});
    graph.AddNode("uses-small", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder) { builder.Write(small, Arcane::RgUsage::ShaderWriteCs); },
        [](Arcane::RenderGraphNodeContext&) {});
    graph.AddNode("uses-large", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder) { builder.Write(large, Arcane::RgUsage::ShaderWriteCs); },
        [](Arcane::RenderGraphNodeContext&) {});

    const Arcane::RgCompiled compiled = CompileOk(graph);
    CHECK(compiled.transientPoolSlot[0] != compiled.transientPoolSlot[1]);
    CHECK(compiled.poolSlotCount == 2);
}

TEST_CASE("rendergraph compile: a transient buffer never shares a pool slot with a transient texture", "[nri]")
{
    Arcane::RenderGraph graph;
    Arcane::RgTexture tex;
    Arcane::RgBuffer  buf;

    graph.AddNode("declare", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            tex = builder.CreateTexture("tex", MakeColorDesc());
            buf = builder.CreateBuffer("buf", 256, nri::BufferUsageBits::SHADER_RESOURCE_STORAGE);
        },
        [](Arcane::RenderGraphNodeContext&) {});
    graph.AddNode("uses-tex", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder) { builder.Write(tex, Arcane::RgUsage::ShaderWriteCs); },
        [](Arcane::RenderGraphNodeContext&) {});
    graph.AddNode("uses-buf", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder) { builder.Write(buf, Arcane::RgUsage::ShaderWriteCs); },
        [](Arcane::RenderGraphNodeContext&) {});

    const Arcane::RgCompiled compiled = CompileOk(graph);
    REQUIRE(compiled.transients.size() == 2);
    CHECK(compiled.transients[0].isTexture);    // textures enumerate first
    CHECK_FALSE(compiled.transients[1].isTexture);
    CHECK(compiled.transientPoolSlot[0] != compiled.transientPoolSlot[1]);
    CHECK(compiled.poolSlotCount == 2);
}

TEST_CASE("rendergraph compile: imported resources are not transients and get no lifetime entry", "[nri]")
{
    Arcane::RenderGraph graph;
    Arcane::RgTexture imported, transientTex;

    graph.AddNode("declare", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            imported     = builder.ImportTexture("imported", nullptr, kColorState, kColorState, true);
            transientTex = builder.CreateTexture("transient", MakeColorDesc());
            builder.Write(transientTex, Arcane::RgUsage::ShaderWriteCs);
            builder.Write(imported, Arcane::RgUsage::ColorWrite);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    const Arcane::RgCompiled compiled = CompileOk(graph);
    REQUIRE(compiled.transients.size() == 1);
    CHECK(compiled.transients[0].isTexture);
    CHECK(compiled.transientLifetimes.size() == 1);
    CHECK(compiled.transientPoolSlot.size() == 1);
}

// ---------------------------------------------------------------------
// Compile refusals -- every message names the offending resource and/or
// node, which is what makes them actionable at a call site.
// ---------------------------------------------------------------------

TEST_CASE("rendergraph compile: (f) reading a never-written transient texture is refused, naming the resource", "[nri]")
{
    Arcane::RenderGraph graph;

    graph.AddNode("reader", Arcane::RenderGraph::NodeKind::Compute,
        [](Arcane::RenderGraphBuilder& builder)
        {
            const Arcane::RgTexture tex = builder.CreateTexture("never-written-tex", MakeColorDesc());
            builder.Read(tex, Arcane::RgUsage::ShaderRead);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    std::string error;
    CHECK_FALSE(graph.Compile(&error).has_value());
    INFO("message was: " << error);
    CHECK(error.find("never-written-tex") != std::string::npos);
}

TEST_CASE("rendergraph compile: (f) reading a never-written transient buffer is refused, naming the resource", "[nri]")
{
    Arcane::RenderGraph graph;

    graph.AddNode("reader", Arcane::RenderGraph::NodeKind::Compute,
        [](Arcane::RenderGraphBuilder& builder)
        {
            const Arcane::RgBuffer buf = builder.CreateBuffer("never-written-buf", 64, nri::BufferUsageBits::SHADER_RESOURCE);
            builder.Read(buf, Arcane::RgUsage::ShaderRead);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    std::string error;
    CHECK_FALSE(graph.Compile(&error).has_value());
    INFO("message was: " << error);
    CHECK(error.find("never-written-buf") != std::string::npos);
}

TEST_CASE("rendergraph compile: (f) reading a transient BEFORE the node that writes it is refused", "[nri]")
{
    // Declaration order IS execution order in Phase 2, so a read at node 0 of
    // something first written at node 1 reads undefined content -- the same
    // defect as never writing it at all.
    Arcane::RenderGraph graph;
    Arcane::RgTexture tex;

    graph.AddNode("early-reader", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            tex = builder.CreateTexture("read-too-early", MakeColorDesc());
            builder.Read(tex, Arcane::RgUsage::ShaderRead);
        },
        [](Arcane::RenderGraphNodeContext&) {});
    graph.AddNode("late-writer", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder) { builder.Write(tex, Arcane::RgUsage::ShaderWriteCs); },
        [](Arcane::RenderGraphNodeContext&) {});

    std::string error;
    CHECK_FALSE(graph.Compile(&error).has_value());
    INFO("message was: " << error);
    CHECK(error.find("read-too-early") != std::string::npos);
}

TEST_CASE("rendergraph compile: reading an IMPORTED resource nothing writes is fine -- it arrives with a defined state", "[nri]")
{
    Arcane::RenderGraph graph;
    Arcane::RgTexture tex;
    Arcane::RgBuffer  buf;

    graph.AddNode("reader", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            tex = builder.ImportTexture("imported-tex", nullptr, kColorState, kColorState, true);
            buf = builder.ImportBuffer("imported-buf", nullptr, 64);
            builder.Read(tex, Arcane::RgUsage::ShaderRead);
            builder.Read(buf, Arcane::RgUsage::ShaderRead);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    std::string error;
    const bool compiled = graph.Compile(&error).has_value();
    INFO("Compile() refused: " << error);
    CHECK(compiled);
}

TEST_CASE("rendergraph compile: a node that reads a transient it also writes ITSELF compiles -- one node, one state", "[nri]")
{
    // In-place read-modify-write on a storage texture: the read is declared
    // BEFORE the write, but both land on the same node, and a node resolves to
    // exactly one state -- so the read-before-write rule is node-granular and
    // this is not a refusal.
    Arcane::RenderGraph graph;

    graph.AddNode("in-place", Arcane::RenderGraph::NodeKind::Compute,
        [](Arcane::RenderGraphBuilder& builder)
        {
            const Arcane::RgTexture tex = builder.CreateTexture("in-place", MakeColorDesc());
            builder.Read(tex, Arcane::RgUsage::ShaderWriteCs);
            builder.Write(tex, Arcane::RgUsage::ShaderWriteCs);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    const Arcane::RgCompiled compiled = CompileOk(graph);
    REQUIRE(compiled.nodes.size() == 1);
    CHECK(compiled.nodes[0].preBarriers.size() == 1);   // one transition, not two
}

TEST_CASE("rendergraph compile: a node reading and writing one resource in the same state emits a single barrier", "[nri]")
{
    Arcane::RenderGraph graph;
    Arcane::RgTexture tex;

    graph.AddNode("seed", Arcane::RenderGraph::NodeKind::Copy,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            tex = builder.CreateTexture("rmw", MakeColorDesc());
            builder.Write(tex, Arcane::RgUsage::CopyDst);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    graph.AddNode("read-modify-write", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            builder.Read(tex, Arcane::RgUsage::ShaderWriteCs);
            builder.Write(tex, Arcane::RgUsage::ShaderWriteCs);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    const Arcane::RgCompiled compiled = CompileOk(graph);
    REQUIRE(compiled.nodes.size() == 2);
    CHECK(compiled.nodes[1].preBarriers.size() == 1);
}

TEST_CASE("rendergraph compile: a node declaring two DIFFERENT states for one resource is refused, naming it", "[nri]")
{
    Arcane::RenderGraph graph;
    Arcane::RgTexture tex;

    graph.AddNode("seed", Arcane::RenderGraph::NodeKind::Copy,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            tex = builder.CreateTexture("two-states", MakeColorDesc());
            builder.Write(tex, Arcane::RgUsage::CopyDst);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    graph.AddNode("conflicted", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            builder.Read(tex, Arcane::RgUsage::ShaderRead);
            builder.Write(tex, Arcane::RgUsage::ShaderWriteCs);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    std::string error;
    CHECK_FALSE(graph.Compile(&error).has_value());
    INFO("message was: " << error);
    CHECK(error.find("two-states") != std::string::npos);
    CHECK(error.find("conflicted") != std::string::npos);
}

TEST_CASE("rendergraph compile: a Raster node with no attachment is refused, naming the node", "[nri]")
{
    // Recorded non-fatally at declaration (Task 3's NodeHasRequiredAttachments)
    // -- this is the "later" half of that assert-later model.
    Arcane::RenderGraph graph;

    graph.AddNode("attachmentless-raster", Arcane::RenderGraph::NodeKind::Raster,
        [](Arcane::RenderGraphBuilder&) {}, [](Arcane::RenderGraphNodeContext&) {});

    std::string error;
    CHECK_FALSE(graph.Compile(&error).has_value());
    INFO("message was: " << error);
    CHECK(error.find("attachmentless-raster") != std::string::npos);
}

TEST_CASE("rendergraph compile: a Raster attachment the node never declared an access for is refused", "[nri]")
{
    // Without a declared Read/Write the graph derives no transition, so the
    // attachment would be bound in whatever layout it was last left in.
    Arcane::RenderGraph graph;

    graph.AddNode("unwritten-attachment", Arcane::RenderGraph::NodeKind::Raster,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            const Arcane::RgTexture color = builder.CreateTexture("orphan-attachment", MakeColorDesc());
            const Arcane::RgTexture attachments[] = { color };
            graph.SetColorAttachments(attachments);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    std::string error;
    CHECK_FALSE(graph.Compile(&error).has_value());
    INFO("message was: " << error);
    CHECK(error.find("orphan-attachment") != std::string::npos);
    CHECK(error.find("unwritten-attachment") != std::string::npos);
}

TEST_CASE("rendergraph compile: an attachment handle left stale across Reset() is refused, naming the node", "[nri]")
{
    // SetColorAttachments deliberately stores handles UNVALIDATED (Task 3's
    // assert-later model) -- Compile decoding them through the one handle seam
    // is their only validation point.
    Arcane::RenderGraph graph;
    Arcane::RgTexture stale;

    graph.AddNode("frame-1", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            stale = builder.CreateTexture("stale", MakeColorDesc());
            builder.Write(stale, Arcane::RgUsage::ColorWrite);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    graph.Reset();

    graph.AddNode("frame-2-raster", Arcane::RenderGraph::NodeKind::Raster,
        [&](Arcane::RenderGraphBuilder&)
        {
            const Arcane::RgTexture attachments[] = { stale };
            graph.SetColorAttachments(attachments);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    std::string error;
    CHECK_FALSE(graph.Compile(&error).has_value());
    INFO("message was: " << error);
    CHECK(error.find("frame-2-raster") != std::string::npos);
}

// ---------------------------------------------------------------------
// Purity + the shape Task 6's executor relies on.
// ---------------------------------------------------------------------

TEST_CASE("rendergraph compile: Compile() is pure -- it mutates nothing and repeats exactly", "[nri]")
{
    Arcane::RenderGraph graph;
    Arcane::RgTexture tex;

    graph.AddNode("create", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder) { tex = builder.CreateTexture("tex", MakeColorDesc()); },
        [](Arcane::RenderGraphNodeContext&) {});
    AddColorNode(graph, "draw", tex);
    AddShaderReadNode(graph, "read", tex);

    const std::size_t   nodesBefore    = graph.NodeCount();
    const std::size_t   texturesBefore = graph.TextureCount();
    const std::uint32_t genBefore      = graph.Generation();

    const Arcane::RgCompiled first  = CompileOk(graph);
    const Arcane::RgCompiled second = CompileOk(graph);

    CHECK(graph.NodeCount() == nodesBefore);
    CHECK(graph.TextureCount() == texturesBefore);
    CHECK(graph.Generation() == genBefore);

    REQUIRE(first.nodes.size() == second.nodes.size());
    CHECK(TotalBarriers(first) == TotalBarriers(second));
    CHECK(first.poolSlotCount == second.poolSlotCount);
    for (std::size_t i = 0; i < first.nodes.size(); ++i)
    {
        REQUIRE(first.nodes[i].preBarriers.size() == second.nodes[i].preBarriers.size());
        for (std::size_t b = 0; b < first.nodes[i].preBarriers.size(); ++b)
        {
            CheckState(first.nodes[i].preBarriers[b].before, second.nodes[i].preBarriers[b].before);
            CheckState(first.nodes[i].preBarriers[b].after,  second.nodes[i].preBarriers[b].after);
        }
    }
}

TEST_CASE("rendergraph compile: every declared node gets a compiled entry, in declaration order", "[nri]")
{
    Arcane::RenderGraph graph;

    graph.AddNode("a", Arcane::RenderGraph::NodeKind::Compute, [](Arcane::RenderGraphBuilder&) {}, [](Arcane::RenderGraphNodeContext&) {});
    graph.AddNode("b", Arcane::RenderGraph::NodeKind::Compute, [](Arcane::RenderGraphBuilder&) {}, [](Arcane::RenderGraphNodeContext&) {});
    graph.AddNode("c", Arcane::RenderGraph::NodeKind::Copy,    [](Arcane::RenderGraphBuilder&) {}, [](Arcane::RenderGraphNodeContext&) {});

    const Arcane::RgCompiled compiled = CompileOk(graph);
    REQUIRE(compiled.nodes.size() == graph.NodeCount());
    for (std::uint32_t i = 0; i < compiled.nodes.size(); ++i)
    {
        CHECK(compiled.nodes[i].nodeIndex == i);
        CHECK(compiled.nodes[i].preBarriers.empty());
    }
}

TEST_CASE("rendergraph compile: barrier resourceIndex is a decoded slot, not an encoded handle", "[nri]")
{
    // Guards the INDEX SPACE contract Task 6 reads: this graph is on
    // generation 1, so an encoded handle's top byte is non-zero while the
    // decoded slot stays a small 0-based index.
    Arcane::RenderGraph graph;
    graph.Reset();   // generation 1

    Arcane::RgTexture tex;
    graph.AddNode("writer", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            tex = builder.CreateTexture("tex", MakeColorDesc());
            builder.Write(tex, Arcane::RgUsage::ShaderWriteCs);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    REQUIRE(graph.Generation() == 1);
    REQUIRE(tex.index != 0u);   // the encoded handle carries the generation

    const Arcane::RgCompiled compiled = CompileOk(graph);
    REQUIRE(compiled.nodes[0].preBarriers.size() == 1);
    CHECK(compiled.nodes[0].preBarriers[0].resourceIndex == 0u);   // decoded slot
    REQUIRE(compiled.transients.size() == 1);
    CHECK(compiled.transients[0].resourceIndex == 0u);
}
