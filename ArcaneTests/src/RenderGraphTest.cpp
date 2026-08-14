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
// Include order matters here for the same reason it does in
// NriSubstrateTest.cpp: NriDevice.hpp pulls in Extensions/NRIDeviceCreation.h,
// which declares nri::Message with an enumerator literally named ERROR, and
// <windows.h> (reachable through Arcane/Render/Device.hpp -> spdlog) #defines
// ERROR via wingdi.h. Keep the NRI includes first.
#include <NRI.h>
#include <Extensions/NRIDeviceCreation.h>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/Device.hpp>            // RenderErrorCount / ResetRenderErrorCount
#include <Arcane/Render/GpuBreadcrumbs.hpp>    // the CPU-side ring the marker-policy case reads
#include <Arcane/Render/GpuInstrumentation.hpp>// SetActiveGpuCrashBackend / ClearActiveGpuCrashBackendIfCurrent
#include <Arcane/Render/IGpuCrashBackend.hpp>  // IGpuCrashBackend, for the marker-policy spy
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/Nri/NriPipelineCache.hpp>
#include <Arcane/Render/Nri/NriUploadRing.hpp>
#include <Arcane/Render/Nri/RenderGraph.hpp>
#include <Arcane/Render/Swapchain.hpp>         // kSwapchainFramesInFlight

#include <cstdint>
#include <optional>
#include <span>
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

TEST_CASE("rendergraph compile: (e) a pool-slot handover seeds the next tenant's before from the previous tenant's final access/stages", "[nri]")
{
    // Fix round 1. Two transients that share a pool slot are ONE physical
    // texture at execution time, so the second tenant's first barrier must
    // make the first tenant's writes available -- `before.access = NONE`
    // would leave a write-after-write hazard across the reused object. The
    // layout half stays UNDEFINED (a contents-discarding transition), which
    // is what keeps the first-use rule true per LOGICAL transient.
    //
    // Node 2 also pins a batched group's CONTENTS: two entries, identical
    // `after`, DIFFERENT `before` -- `late` seeded from the handover, `side`
    // (a different desc, so its own pool slot with no previous tenant) still
    // seeded from kUnknownState. The handover is the only thing that can
    // explain the difference.
    Arcane::RenderGraph graph;
    Arcane::RgTexture early, late, side;

    graph.AddNode("declare", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            early = builder.CreateTexture("early", MakeColorDesc());
            late  = builder.CreateTexture("late", MakeColorDesc());
            side  = builder.CreateTexture("side", MakeColorDesc(32, 32));
        },
        [](Arcane::RenderGraphNodeContext&) {});

    AddColorNode(graph, "uses-early", early);   // node 1: early -> kColorState, lifetime [1,1]

    graph.AddNode("handover", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            builder.Write(late, Arcane::RgUsage::ShaderWriteCs);
            builder.Write(side, Arcane::RgUsage::ShaderWriteCs);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    const Arcane::RgCompiled compiled = CompileOk(graph);

    // early and late share a slot; side (different desc) does not.
    REQUIRE(compiled.transients.size() == 3);
    REQUIRE(compiled.transientPoolSlot[0] == compiled.transientPoolSlot[1]);
    REQUIRE(compiled.transientPoolSlot[2] != compiled.transientPoolSlot[0]);
    CHECK(compiled.poolSlotCount == 2);

    // The FIRST tenant of a slot is untouched by the handover rule.
    REQUIRE(compiled.nodes[1].preBarriers.size() == 1);
    CHECK(compiled.nodes[1].preBarriers[0].resourceIndex == 0u);
    CheckState(compiled.nodes[1].preBarriers[0].before, kUnknownState);
    CheckState(compiled.nodes[1].preBarriers[0].after, kColorState);

    // The batched group: both entries, in declaration order.
    REQUIRE(compiled.nodes[2].preBarriers.size() == 2);
    const nri::AccessLayoutStage storageState{
        nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::Layout::SHADER_RESOURCE_STORAGE,
        nri::StageBits::COMPUTE_SHADER };

    const Arcane::RgBarrier& handover = compiled.nodes[2].preBarriers[0];
    CHECK(handover.resourceIndex == 1u);   // `late`
    CHECK(handover.isTexture);
    // The previous tenant's access + stages; contents discarded via UNDEFINED.
    CheckState(handover.before, kColorState.access, nri::Layout::UNDEFINED, kColorState.stages);
    CheckState(handover.after, storageState);

    const Arcane::RgBarrier& fresh = compiled.nodes[2].preBarriers[1];
    CHECK(fresh.resourceIndex == 2u);      // `side`
    CHECK(fresh.isTexture);
    CheckState(fresh.before, kUnknownState);   // no previous tenant -- unchanged behaviour
    CheckState(fresh.after, storageState);
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

// =====================================================================
// Task 5 -- NriUploadRing: per-frame-slot upload arenas.
//
// RingLayout is the pure bump-allocator math NriUploadRing wraps -- no NRI
// device, fully headless. NriUploadRing itself creates a REAL persistent-
// mapped nri::Buffer per slot at Init(), which fails outright on the NONE
// backend (ImplNONE's MapBuffer returns null unconditionally -- see
// NriUploadRing.hpp's file header), so it -- and BeginFrame()/Allocate()/
// HighWater(), which only ever operate on slots Init() populated -- is a
// [gpu] desk-verify item and gets no test here. Every case below drives
// Arcane::RingLayout directly: exactly the math NriUploadRing forwards a
// per-slot instance of into.
//
// NOT tested here, and why: a non-power-of-two `align` trips RingLayout::
// Allocate's ARC_ASSERT, which is FATAL in a debug build (same "assert is
// fatal, cannot be exercised by a surviving test case" situation as
// Graveyard's reentrancy contract, documented in NriSubstrateTest.cpp). The
// self-review pass for this behaviour was a code-reading exercise, not a
// test.
// =====================================================================

TEST_CASE("uploadring layout: alignment rounds the cursor up, not the size", "[nri]")
{
    Arcane::RingLayout layout;
    layout.Init(64);

    const auto first = layout.Allocate(1, 1);
    REQUIRE(first.ok);
    CHECK(first.offset == 0);
    CHECK(layout.Cursor() == 1);

    // cursor is 1; a 16-byte-aligned request must round UP to 16, not just
    // add its size to the unaligned cursor.
    const auto second = layout.Allocate(3, 16);
    REQUIRE(second.ok);
    CHECK(second.offset == 16);
    CHECK(layout.Cursor() == 19);
}

TEST_CASE("uploadring layout: an allocation that exactly fits the remaining capacity succeeds", "[nri]")
{
    Arcane::RingLayout layout;
    layout.Init(16);

    const auto alloc = layout.Allocate(16, 1);
    REQUIRE(alloc.ok);
    CHECK(alloc.offset == 0);
    CHECK(layout.Cursor() == 16);

    // Capacity is now exactly exhausted -- even a 1-byte request overflows.
    const auto overflow = layout.Allocate(1, 1);
    CHECK_FALSE(overflow.ok);
}

TEST_CASE("uploadring layout: overflow returns failure and leaves the cursor UNCHANGED -- never wraps", "[nri]")
{
    Arcane::RingLayout layout;
    layout.Init(8);

    const auto first = layout.Allocate(4, 1);
    REQUIRE(first.ok);
    REQUIRE(layout.Cursor() == 4);

    // 5 more bytes would need 9 total against an 8-byte capacity.
    const auto overflow = layout.Allocate(5, 1);
    CHECK_FALSE(overflow.ok);
    CHECK(overflow.offset == 0);
    // The failed call must not have moved the cursor at all -- a caller
    // that ignores the failure and allocates again lands where a
    // successful smaller request would have, never past the end.
    CHECK(layout.Cursor() == 4);
    CHECK(layout.OverflowCount() == 1);

    // A request that actually fits still succeeds afterward -- one failure
    // does not poison the layout.
    const auto fits = layout.Allocate(4, 1);
    REQUIRE(fits.ok);
    CHECK(fits.offset == 4);
    CHECK(layout.Cursor() == 8);
}

TEST_CASE("uploadring layout: a single allocation larger than the whole slot overflows immediately", "[nri]")
{
    // Self-review edge case: slotBytes smaller than one alloc.
    Arcane::RingLayout layout;
    layout.Init(16);

    const auto alloc = layout.Allocate(17, 1);
    CHECK_FALSE(alloc.ok);
    CHECK(layout.Cursor() == 0);
    CHECK(layout.OverflowCount() == 1);
}

TEST_CASE("uploadring layout: a zero-size allocation succeeds at the current cursor and claims nothing", "[nri]")
{
    // Self-review edge case: zero-size alloc.
    Arcane::RingLayout layout;
    layout.Init(16);

    REQUIRE(layout.Allocate(4, 1).ok);
    REQUIRE(layout.Cursor() == 4);

    const auto zero = layout.Allocate(0, 1);
    REQUIRE(zero.ok);
    CHECK(zero.offset == 4);
    CHECK(layout.Cursor() == 4);   // unchanged -- claimed nothing

    // Even exactly at full capacity, a zero-size request still succeeds.
    REQUIRE(layout.Allocate(12, 1).ok);
    REQUIRE(layout.Cursor() == 16);
    const auto zeroAtEnd = layout.Allocate(0, 1);
    CHECK(zeroAtEnd.ok);
    CHECK(zeroAtEnd.offset == 16);
}

TEST_CASE("uploadring layout: Reset (BeginFrame's half) zeroes the cursor but leaves highWater/overflowCount alone", "[nri]")
{
    Arcane::RingLayout layout;
    layout.Init(16);

    REQUIRE(layout.Allocate(10, 1).ok);
    CHECK_FALSE(layout.Allocate(10, 1).ok);   // overflows: 10 + 10 > 16
    REQUIRE(layout.HighWater() == 10);
    REQUIRE(layout.OverflowCount() == 1);

    layout.Reset();

    CHECK(layout.Cursor() == 0);
    CHECK(layout.HighWater() == 10);       // survives -- lifetime peak, logged at shutdown
    CHECK(layout.OverflowCount() == 1);    // survives too

    // The slot is fully usable again after Reset -- this is the whole point
    // of a per-frame ring.
    const auto afterReset = layout.Allocate(16, 1);
    REQUIRE(afterReset.ok);
    CHECK(afterReset.offset == 0);
}

TEST_CASE("uploadring layout: two independent slots never see each other's allocations", "[nri]")
{
    // NriUploadRing owns kSwapchainFramesInFlight of these; nothing in
    // RingLayout itself is shared across instances -- this proves it.
    Arcane::RingLayout slotA, slotB;
    slotA.Init(16);
    slotB.Init(16);

    const auto a1 = slotA.Allocate(10, 1);
    REQUIRE(a1.ok);
    CHECK(a1.offset == 0);

    // slotB starts fresh regardless of what slotA already claimed.
    const auto b1 = slotB.Allocate(10, 1);
    REQUIRE(b1.ok);
    CHECK(b1.offset == 0);

    CHECK(slotA.Cursor() == 10);
    CHECK(slotB.Cursor() == 10);

    // Overflowing slotA must not affect slotB's capacity or state at all.
    const auto aOverflow = slotA.Allocate(10, 1);
    CHECK_FALSE(aOverflow.ok);
    CHECK(slotB.Cursor() == 10);
    CHECK(slotB.OverflowCount() == 0);
}

TEST_CASE("uploadring layout: high-water tracks the peak cursor across multiple allocations, not just the latest", "[nri]")
{
    Arcane::RingLayout layout;
    layout.Init(100);

    CHECK(layout.HighWater() == 0);

    REQUIRE(layout.Allocate(50, 1).ok);
    CHECK(layout.HighWater() == 50);

    REQUIRE(layout.Allocate(20, 1).ok);
    CHECK(layout.HighWater() == 70);

    layout.Reset();
    CHECK(layout.HighWater() == 70);   // still the peak, even though cursor is back to 0

    // A smaller frame after Reset must not LOWER the recorded high water.
    REQUIRE(layout.Allocate(10, 1).ok);
    CHECK(layout.HighWater() == 70);

    // A frame that exceeds the previous peak DOES raise it.
    REQUIRE(layout.Allocate(85, 1).ok);   // cursor: 10 + 85 = 95 > 70
    CHECK(layout.Cursor() == 95);
    CHECK(layout.HighWater() == 95);
}

// =========================================================================
// Task 6: the EXECUTOR. NONE-backend integration -- still "[nri]", still
// inside the ~[gpu] dev gate.
//
// What a NONE device buys and what it does not. ImplNONE.cpp answers every
// CoreInterface entry these cases drive (CreateCommittedTexture/Buffer,
// CreateTextureView, CreateCommandAllocator/Buffer, CreateFence,
// Begin/EndCommandBuffer, CmdBarrier, CmdBeginRendering, QueueSubmit,
// SetDebugName, the annotations) with a dummy non-null object and SUCCESS,
// so what runs here is the executor's REAL control flow -- ordering,
// refusals, pool reuse, burial -- against a device that records nothing.
// Pixels, actual barriers and present are [gpu] desk items, per the plan.
//
// Three NONE footguns these cases are built around:
//   * MapBuffer returns null unconditionally, so NriUploadRing::Init() cannot
//     succeed -- the ring below is deliberately default-constructed and never
//     Init()'d. Legal because the executor only FORWARDS it to node exec fns
//     and never allocates from it (RgExecuteDesc::ring), and these exec fns
//     do not allocate either.
//   * GetFenceValue is hard-wired to 0, so the graph's own Reap (top of
//     Execute) can only ever release value-0 burials. Every case that wants a
//     burial released feeds the graveyard the value BY HAND, exactly as
//     NriSubstrateTest.cpp's Graveyard case does.
//   * Every CreateTexture hands back the SAME dummy pointer, so nothing here
//     may assert that two distinct pool slots hold distinct nri::Texture*.
//
// The device is declared BEFORE the graph in every case on purpose: the
// graph buries its resources in the device's graveyard on the way out, and
// reverse declaration order is what makes the graph die first
// (RgExecuteDesc::device's "must outlive the graph" contract, made
// structural).
// =========================================================================

namespace
{
    // A 3-node write -> read -> copy graph, the brief's shape:
    //
    //   node 0 "write"  Raster : Write(color, ColorWrite) + colour attachment
    //   node 1 "read"   Compute: Read(color, ShaderRead), Write(scratch, ShaderWriteCs)
    //   node 2 "copy"   Copy   : Read(scratch, CopySrc), Write(dst, CopyDst)
    //
    // All three transients share one desc, and their lifetimes are
    // color [0,1], scratch [1,2], dst [2,2] -- so Compile()'s greedy pool
    // assignment gives color and scratch separate slots (they overlap at node
    // 1) and hands dst color's slot (disjoint, identical desc). Two pool
    // slots for three transients: the executor therefore has to realize a
    // SHARED slot and replay a handover barrier, not just three independent
    // textures.
    struct ThreeNodeGraph
    {
        Arcane::RgTexture color, scratch, dst;
        int               execCount[3] = { 0, 0, 0 };
        nri::Texture*     resolvedColor = nullptr;
        nri::Descriptor*  resolvedColorView = nullptr;

        void Declare(Arcane::RenderGraph& graph)
        {
            graph.AddNode("write", Arcane::RenderGraph::NodeKind::Raster,
                [&](Arcane::RenderGraphBuilder& builder)
                {
                    color = builder.CreateTexture("color", MakeColorDesc());
                    builder.Write(color, Arcane::RgUsage::ColorWrite);
                    const Arcane::RgTexture attachments[] = { color };
                    graph.SetColorAttachments(attachments);
                },
                [this](Arcane::RenderGraphNodeContext& context)
                {
                    ++execCount[0];
                    resolvedColor     = context.Resolve(color);
                    resolvedColorView = context.ColorView(color);
                });

            graph.AddNode("read", Arcane::RenderGraph::NodeKind::Compute,
                [&](Arcane::RenderGraphBuilder& builder)
                {
                    scratch = builder.CreateTexture("scratch", MakeColorDesc());
                    builder.Read(color, Arcane::RgUsage::ShaderRead);
                    builder.Write(scratch, Arcane::RgUsage::ShaderWriteCs);
                },
                [this](Arcane::RenderGraphNodeContext&) { ++execCount[1]; });

            graph.AddNode("copy", Arcane::RenderGraph::NodeKind::Copy,
                [&](Arcane::RenderGraphBuilder& builder)
                {
                    dst = builder.CreateTexture("dst", MakeColorDesc());
                    builder.Read(scratch, Arcane::RgUsage::CopySrc);
                    builder.Write(dst, Arcane::RgUsage::CopyDst);
                },
                [this](Arcane::RenderGraphNodeContext&) { ++execCount[2]; });
        }
    };
}

TEST_CASE("rendergraph exec: a 3-node write-read-copy graph records and submits on a NONE device with no latch growth", "[nri]")
{
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    Arcane::NriUploadRing    ring;        // never Init()'d -- see the block comment above
    Arcane::NriPipelineCache pipelines;   // Task 7's placeholder stub
    Arcane::RenderGraph      graph;

    ThreeNodeGraph shape;
    shape.Declare(graph);

    const Arcane::RgCompiled compiled = CompileOk(graph);
    REQUIRE(compiled.nodes.size() == 3);
    REQUIRE(compiled.transients.size() == 3);
    // The shared-slot shape this case exists to exercise; if Compile()'s
    // packing ever changes, the rest of these numbers move with it.
    REQUIRE(compiled.poolSlotCount == 2);

    const Arcane::RgExecuteDesc desc{ *device, /*swapChain=*/nullptr, ring, pipelines, /*frameSlot=*/0 };
    REQUIRE(graph.Execute(desc, compiled));

    // Every node's exec fn ran exactly once, in one command buffer.
    CHECK(shape.execCount[0] == 1);
    CHECK(shape.execCount[1] == 1);
    CHECK(shape.execCount[2] == 1);

    // Exactly the compile's pool-slot count was realized -- not one per
    // transient.
    CHECK(graph.DebugTransientCount() == compiled.poolSlotCount);
    CHECK(graph.DebugTransientCreateCount() == compiled.poolSlotCount);

    // One submission, so the graph's fence timeline (and therefore every
    // burial's key) advanced by exactly one.
    CHECK(graph.DebugSubmitCount() == 1);

    // Nothing was buried yet: a successful Execute realizes, it does not
    // release.
    CHECK(device->Graves().Pending() == 0);

    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("rendergraph exec: node exec fns resolve their declared handles to real resources", "[nri]")
{
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    Arcane::NriUploadRing    ring;
    Arcane::NriPipelineCache pipelines;
    Arcane::RenderGraph      graph;

    ThreeNodeGraph shape;
    shape.Declare(graph);

    const Arcane::RgCompiled    compiled = CompileOk(graph);
    const Arcane::RgExecuteDesc desc{ *device, nullptr, ring, pipelines, 0 };
    REQUIRE(graph.Execute(desc, compiled));

    // Resolve() went through the handle seam and landed on the pool texture
    // the executor realized; ColorView() found the attachment view Execute()
    // created up front (both are dummy-but-non-null on NONE, which is exactly
    // the distinction being made -- null would mean "did not resolve").
    CHECK(shape.resolvedColor != nullptr);
    CHECK(shape.resolvedColorView != nullptr);

    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("rendergraph exec: a second Execute of the same compiled graph creates zero new transients", "[nri]")
{
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    Arcane::NriUploadRing    ring;
    Arcane::NriPipelineCache pipelines;
    Arcane::RenderGraph      graph;

    ThreeNodeGraph shape;
    shape.Declare(graph);

    const Arcane::RgCompiled compiled = CompileOk(graph);

    const Arcane::RgExecuteDesc first{ *device, nullptr, ring, pipelines, 0 };
    REQUIRE(graph.Execute(first, compiled));
    const std::uint64_t created = graph.DebugTransientCreateCount();
    REQUIRE(created == compiled.poolSlotCount);

    // A different frame slot, same compiled graph -- the steady-state frame
    // shape. Nothing about the pool may change.
    const Arcane::RgExecuteDesc second{ *device, nullptr, ring, pipelines, 1 };
    REQUIRE(graph.Execute(second, compiled));

    // The lifetime CREATION counter is what proves reuse: a
    // destroy-and-recreate would leave DebugTransientCount() identical and
    // this one bumped.
    CHECK(graph.DebugTransientCreateCount() == created);
    CHECK(graph.DebugTransientCount() == compiled.poolSlotCount);
    CHECK(graph.DebugSubmitCount() == 2);

    // Reuse means nothing was released either.
    CHECK(device->Graves().Pending() == 0);

    CHECK(shape.execCount[0] == 2);
    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("rendergraph exec: Reset alone releases nothing; ReleaseGpuResources buries the pool and its views", "[nri]")
{
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    Arcane::NriUploadRing    ring;
    Arcane::NriPipelineCache pipelines;
    Arcane::RenderGraph      graph;

    ThreeNodeGraph shape;
    shape.Declare(graph);

    const Arcane::RgCompiled    compiled = CompileOk(graph);
    const Arcane::RgExecuteDesc desc{ *device, nullptr, ring, pipelines, 0 };
    REQUIRE(graph.Execute(desc, compiled));
    REQUIRE(device->Graves().Pending() == 0);

    // Reset clears DECLARATIONS ONLY (fix round 1). The pool has to survive
    // it: Reset is the only way to clear declarations, so a per-frame driver
    // calls it every frame, and burying here would make pool reuse
    // unreachable in the one loop shape that exists.
    graph.Reset();
    CHECK(graph.DebugTransientCount() == compiled.poolSlotCount);
    CHECK(device->Graves().Pending() == 0);

    // The explicit release is what hands the memory back.
    graph.ReleaseGpuResources();

    // Two pool slots plus the one colour-attachment view node 0 declared.
    // Buried, NOT destroyed: the submission that used them may still be in
    // flight, which is the whole reason the graveyard exists.
    CHECK(graph.DebugTransientCount() == 0);
    CHECK(device->Graves().Pending() == compiled.poolSlotCount + 1);

    // Reaping below the burial value releases nothing...
    device->Graves().Reap(graph.DebugSubmitCount() - 1);
    CHECK(device->Graves().Pending() == compiled.poolSlotCount + 1);

    // ...and reaping AT it releases everything. NONE's GetFenceValue is
    // hard-wired to 0, so the value is fed by hand here -- the graph's own
    // Reap could never get there on this backend.
    device->Graves().Reap(graph.DebugSubmitCount());
    CHECK(device->Graves().Pending() == 0);

    // Released is not broken: the next frame simply realizes again.
    ThreeNodeGraph rebuilt;
    rebuilt.Declare(graph);
    const Arcane::RgCompiled again = CompileOk(graph);
    REQUIRE(graph.Execute(desc, again));
    CHECK(graph.DebugTransientCount() == again.poolSlotCount);
    CHECK(graph.DebugTransientCreateCount() == compiled.poolSlotCount + again.poolSlotCount);

    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("rendergraph exec: the per-frame Reset-redeclare-compile-execute loop creates zero new transients", "[nri]")
{
    // THE loop shape Task 7's frame driver runs. Before fix round 1, Reset()
    // buried the pool, so this -- the only way a real driver can clear
    // declarations -- re-created every render target every frame and buried
    // the old ones, reaped kSwapchainFramesInFlight frames later.
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    Arcane::NriUploadRing    ring;
    Arcane::NriPipelineCache pipelines;
    Arcane::RenderGraph      graph;

    std::uint32_t poolSlots = 0;
    for (std::uint32_t frame = 0; frame < 4; ++frame)
    {
        graph.Reset();

        ThreeNodeGraph shape;
        shape.Declare(graph);

        const Arcane::RgCompiled    compiled = CompileOk(graph);
        const Arcane::RgExecuteDesc desc{ *device, nullptr, ring, pipelines,
                                          frame % Arcane::kSwapchainFramesInFlight };
        REQUIRE(graph.Execute(desc, compiled));

        if (frame == 0)
            poolSlots = compiled.poolSlotCount;
        REQUIRE(compiled.poolSlotCount == poolSlots);

        CHECK(shape.execCount[0] == 1);
        CHECK(graph.DebugTransientCount() == poolSlots);

        // The whole point: creations happened on frame 0 and never again,
        // and nothing was ever buried.
        CHECK(graph.DebugTransientCreateCount() == poolSlots);
        CHECK(device->Graves().Pending() == 0);
    }

    CHECK(graph.DebugSubmitCount() == 4);
    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("rendergraph exec: the carried-over pool slot's first barrier picks up the previous frame's outgoing state", "[nri]")
{
    // The correctness consequence of letting the pool cross frames. Compile()
    // is pure and per-frame: the `before` it derives for a slot's first use is
    // always {NONE, UNDEFINED, ALL}, which performs no source availability
    // operation for the PREVIOUS frame's writes to the same physical texture
    // -- the identical write-after-write hazard RenderGraph.cpp's POOL
    // HANDOVER block closes within a frame. The executor patches it at the
    // frame boundary, where Compile() cannot see.
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    Arcane::NriUploadRing    ring;
    Arcane::NriPipelineCache pipelines;
    Arcane::RenderGraph      graph;
    const Arcane::RgExecuteDesc desc{ *device, nullptr, ring, pipelines, 0 };

    {
        ThreeNodeGraph shape;
        shape.Declare(graph);
        const Arcane::RgCompiled compiled = CompileOk(graph);
        REQUIRE(graph.Execute(desc, compiled));
    }

    // Frame 1 realized pool slot 0 fresh, so its first barrier is Compile()'s
    // unpatched first-use state -- a genuinely undefined new texture.
    const auto firstFrame = graph.DebugFirstBarrierBefore(0);
    REQUIRE(firstFrame.has_value());
    CheckState(*firstFrame, kUnknownState);

    // Slot 0's tenants are `color` (ColorWrite -> ShaderRead) and then `dst`
    // (CopyDst, which shares the slot), so frame 1 leaves the physical
    // texture in the CopyDst state.
    graph.Reset();
    {
        ThreeNodeGraph shape;
        shape.Declare(graph);
        const Arcane::RgCompiled compiled = CompileOk(graph);
        // Compile() still derives the unpatched first-use state -- it has no
        // way to know a previous frame existed. That is what makes the
        // executor's amendment necessary rather than redundant.
        REQUIRE(compiled.nodes[0].preBarriers.size() == 1);
        CheckState(compiled.nodes[0].preBarriers[0].before, kUnknownState);

        REQUIRE(graph.Execute(desc, compiled));
        REQUIRE(graph.DebugTransientCreateCount() == compiled.poolSlotCount);   // reused, not re-created
    }

    // What was actually RECORDED: the previous frame's outgoing access and
    // stages, with the layout still UNDEFINED (discard, never inherit
    // contents) -- byte-for-byte the within-frame handover's shape.
    const auto secondFrame = graph.DebugFirstBarrierBefore(0);
    REQUIRE(secondFrame.has_value());
    CheckState(*secondFrame, nri::AccessBits::COPY_DESTINATION, nri::Layout::UNDEFINED, nri::StageBits::COPY);

    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("rendergraph exec: a desc change after Reset re-creates and buries exactly the changed slots", "[nri]")
{
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    Arcane::NriUploadRing    ring;
    Arcane::NriPipelineCache pipelines;
    Arcane::RenderGraph      graph;
    const Arcane::RgExecuteDesc desc{ *device, nullptr, ring, pipelines, 0 };

    std::uint32_t firstPoolSlots = 0;
    {
        ThreeNodeGraph shape;
        shape.Declare(graph);
        const Arcane::RgCompiled compiled = CompileOk(graph);
        REQUIRE(graph.Execute(desc, compiled));
        firstPoolSlots = compiled.poolSlotCount;
        REQUIRE(graph.DebugTransientCreateCount() == firstPoolSlots);
    }

    graph.Reset();

    // Same node shape, DIFFERENT extent -- what a window resize looks like to
    // the graph. Every slot's realized desc now mismatches, so every slot is
    // buried and re-created; the reuse path deliberately does not paper over
    // a shape change.
    Arcane::RgTexture color, scratch, dst;
    graph.AddNode("write", Arcane::RenderGraph::NodeKind::Raster,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            color = builder.CreateTexture("color", MakeColorDesc(128, 128));
            builder.Write(color, Arcane::RgUsage::ColorWrite);
            const Arcane::RgTexture attachments[] = { color };
            graph.SetColorAttachments(attachments);
        },
        [](Arcane::RenderGraphNodeContext&) {});
    graph.AddNode("read", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            scratch = builder.CreateTexture("scratch", MakeColorDesc(128, 128));
            builder.Read(color, Arcane::RgUsage::ShaderRead);
            builder.Write(scratch, Arcane::RgUsage::ShaderWriteCs);
        },
        [](Arcane::RenderGraphNodeContext&) {});
    graph.AddNode("copy", Arcane::RenderGraph::NodeKind::Copy,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            dst = builder.CreateTexture("dst", MakeColorDesc(128, 128));
            builder.Read(scratch, Arcane::RgUsage::CopySrc);
            builder.Write(dst, Arcane::RgUsage::CopyDst);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    const Arcane::RgCompiled resized = CompileOk(graph);
    REQUIRE(resized.poolSlotCount == firstPoolSlots);
    REQUIRE(graph.Execute(desc, resized));

    CHECK(graph.DebugTransientCount() == resized.poolSlotCount);
    CHECK(graph.DebugTransientCreateCount() == firstPoolSlots + resized.poolSlotCount);

    // Buried: the old pool textures, plus the old colour-attachment view
    // (swept with the texture it named -- a view outliving its resource is a
    // dangling descriptor).
    CHECK(device->Graves().Pending() == firstPoolSlots + 1);
    device->Graves().Reap(graph.DebugSubmitCount());
    CHECK(device->Graves().Pending() == 0);

    // A re-realized slot starts genuinely undefined, so no carry state from
    // the buried texture may leak into it.
    const auto firstBarrier = graph.DebugFirstBarrierBefore(0);
    REQUIRE(firstBarrier.has_value());
    CheckState(*firstBarrier, kUnknownState);

    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("rendergraph exec: a graph with no nodes still submits and advances the fence", "[nri]")
{
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    Arcane::NriUploadRing    ring;
    Arcane::NriPipelineCache pipelines;
    Arcane::RenderGraph      graph;

    // Nothing declared at all. The empty submission is deliberate: the
    // graph's fence is the graveyard's clock, and a frame that skipped the
    // submit would leave burials from that frame keyed to a value nothing
    // ever signals.
    const Arcane::RgCompiled compiled = CompileOk(graph);
    CHECK(compiled.nodes.empty());
    CHECK(compiled.exitBarriers.empty());

    const Arcane::RgExecuteDesc desc{ *device, nullptr, ring, pipelines, 0 };
    REQUIRE(graph.Execute(desc, compiled));

    CHECK(graph.DebugTransientCount() == 0);
    CHECK(graph.DebugSubmitCount() == 1);
    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("rendergraph exec: a swapchain-importing node refuses a null swapChain", "[nri]")
{
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    Arcane::NriUploadRing    ring;
    Arcane::NriPipelineCache pipelines;
    Arcane::RenderGraph      graph;

    Arcane::RgTexture backbuffer;
    graph.AddNode("clear", Arcane::RenderGraph::NodeKind::Raster,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            backbuffer = builder.ImportSwapChainTexture("backbuffer");
            builder.Write(backbuffer, Arcane::RgUsage::ColorWrite);
            const Arcane::RgTexture attachments[] = { backbuffer };
            graph.SetColorAttachments(attachments);
        },
        [](Arcane::RenderGraphNodeContext&) { FAIL("a refused Execute must record nothing"); });

    const Arcane::RgCompiled compiled = CompileOk(graph);
    // ImportSwapChainTexture pins the exit state, so the graph always leaves
    // the backbuffer present-ready without the caller choosing anything.
    REQUIRE(compiled.exitBarriers.size() == 1);
    CheckState(compiled.exitBarriers[0].after, kPresentState);

    // No swapchain to acquire from: refused, loudly, before any recording.
    const Arcane::RgExecuteDesc desc{ *device, /*swapChain=*/nullptr, ring, pipelines, 0 };
    CHECK_FALSE(graph.Execute(desc, compiled));
    CHECK(graph.DebugSubmitCount() == 0);

    // The refusal reached the shared 0/0 gate latch through the "nri-graph"
    // tagged seam -- restore it so no other case inherits the +1.
    CHECK(Arcane::RenderErrorCount() == before + 1);
    Arcane::ResetRenderErrorCount();
}

TEST_CASE("rendergraph exec: an RgCompiled from before a Reset is refused", "[nri]")
{
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    Arcane::NriUploadRing    ring;
    Arcane::NriPipelineCache pipelines;
    Arcane::RenderGraph      graph;

    ThreeNodeGraph shape;
    shape.Declare(graph);
    const Arcane::RgCompiled stale = CompileOk(graph);

    graph.Reset();

    // One node now, three in `stale`. Executing the stale compile would walk
    // three nodes against one declaration and index this frame's resources
    // with last frame's slots.
    Arcane::RgTexture tex;
    graph.AddNode("only", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            tex = builder.CreateTexture("tex", MakeColorDesc());
            builder.Write(tex, Arcane::RgUsage::ShaderWriteCs);
        },
        [](Arcane::RenderGraphNodeContext&) { FAIL("a refused Execute must record nothing"); });

    const Arcane::RgExecuteDesc desc{ *device, nullptr, ring, pipelines, 0 };
    CHECK_FALSE(graph.Execute(desc, stale));
    CHECK(graph.DebugSubmitCount() == 0);

    CHECK(Arcane::RenderErrorCount() == before + 1);
    Arcane::ResetRenderErrorCount();
}

TEST_CASE("rendergraph exec: an out-of-range frame slot is refused", "[nri]")
{
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    Arcane::NriUploadRing    ring;
    Arcane::NriPipelineCache pipelines;
    Arcane::RenderGraph      graph;

    ThreeNodeGraph shape;
    shape.Declare(graph);
    const Arcane::RgCompiled compiled = CompileOk(graph);

    const Arcane::RgExecuteDesc desc{ *device, nullptr, ring, pipelines,
                                      Arcane::kSwapchainFramesInFlight };
    CHECK_FALSE(graph.Execute(desc, compiled));
    CHECK(graph.DebugSubmitCount() == 0);
    CHECK(shape.execCount[0] == 0);

    CHECK(Arcane::RenderErrorCount() == before + 1);
    Arcane::ResetRenderErrorCount();
}

TEST_CASE("rendergraph exec: an imported attachment's view is rebuilt every Execute, never carried", "[nri]")
{
    // Fix round 2. Views are found by texture POINTER, and the graph is never
    // told when an imported texture's OWNER destroys it -- NriSwapChain::
    // Resize() destroys and recreates every backbuffer entirely outside the
    // graph. NRI may then hand a recreated texture the address a destroyed one
    // just vacated, so no pointer-keyed cache can tell "same texture" from
    // "different texture, recycled address", and a stale nri::Descriptor still
    // names the OLD native image. The graph therefore does not cache imported
    // views across frames at all.
    //
    // Deliberately driven through plain ImportTexture(), not
    // ImportSwapChainTexture(): the hazard belongs to EVERY imported texture,
    // which is why a resize-epoch signal on NriSwapChain was rejected as the
    // fix -- it would have left this path exposed.
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    Arcane::NriUploadRing    ring;
    Arcane::NriPipelineCache pipelines;
    Arcane::RenderGraph      graph;
    const Arcane::RgExecuteDesc desc{ *device, nullptr, ring, pipelines, 0 };

    // Two distinct stand-in addresses for "the backbuffer before the resize"
    // and "the backbuffer after it". Nothing ever dereferences them: on NONE
    // every Cmd* is a no-op and GetTextureDesc ignores its argument
    // (ImplNONE.cpp), and the graph itself only stores and compares the
    // pointer. Real textures are useless here -- NONE hands every
    // CreateCommittedTexture the SAME dummy pointer, so two of them would be
    // indistinguishable, which is exactly the property under test.
    auto* const beforeResize = reinterpret_cast<nri::Texture*>(0x1000);
    auto* const afterResize  = reinterpret_cast<nri::Texture*>(0x2000);

    constexpr nri::AccessLayoutStage kEntry{
        nri::AccessBits::NONE, nri::Layout::UNDEFINED, nri::StageBits::ALL };

    int               execCount = 0;
    nri::Descriptor*  boundView = nullptr;
    // Lives in the CASE's scope, not the lambda's: the exec fn runs during
    // Execute(), long after declare() returned, so a handle owned by declare()
    // would be a dangling reference by the time the node reads it.
    Arcane::RgTexture backbuffer;

    const auto declare = [&](nri::Texture* texture)
    {
        graph.AddNode("clear", Arcane::RenderGraph::NodeKind::Raster,
            [&](Arcane::RenderGraphBuilder& builder)
            {
                backbuffer = builder.ImportTexture("backbuffer", texture, kEntry, kPresentState,
                                                    /*persistent=*/false);
                builder.Write(backbuffer, Arcane::RgUsage::ColorWrite);
                const Arcane::RgTexture attachments[] = { backbuffer };
                graph.SetColorAttachments(attachments);
            },
            [&](Arcane::RenderGraphNodeContext& context)
            {
                ++execCount;
                boundView = context.ColorView(backbuffer);
            });
    };

    // Frame 1 -- the pre-resize backbuffer.
    declare(beforeResize);
    REQUIRE(graph.Execute(desc, CompileOk(graph)));
    CHECK(execCount == 1);
    CHECK(boundView != nullptr);
    // Imported views are turned over at the START of the next Execute, so
    // frame 1 has buried nothing yet. No transients at all in this graph, so
    // the pool cannot be confused with the views.
    CHECK(graph.DebugTransientCount() == 0);
    CHECK(device->Graves().Pending() == 0);

    // Frame 2 -- the swapchain resized, so the import is a different texture.
    graph.Reset();
    declare(afterResize);
    REQUIRE(graph.Execute(desc, CompileOk(graph)));
    CHECK(execCount == 2);
    CHECK(boundView != nullptr);

    // THE ASSERTION: frame 1's view is in the graveyard, so nothing can serve
    // it for the recreated texture. (On NONE every descriptor is the same
    // dummy pointer, so the burial -- not a pointer compare -- is what proves
    // the old one is gone; a graph that carried it would bury nothing.)
    CHECK(device->Graves().Pending() == 1);

    // Frame 3 -- the SAME pointer as frame 2, to pin that the turnover is
    // unconditional rather than a pointer-change optimisation. It has to be:
    // "the pointer did not change" is precisely the case a recycled address
    // fakes.
    graph.Reset();
    declare(afterResize);
    REQUIRE(graph.Execute(desc, CompileOk(graph)));
    CHECK(execCount == 3);
    CHECK(device->Graves().Pending() == 2);

    device->Graves().Reap(graph.DebugSubmitCount());
    CHECK(device->Graves().Pending() == 0);

    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("rendergraph exec: ReleaseGpuResources + Drain destroys imported views SYNCHRONOUSLY", "[nri]")
{
    // Fix round 1, finding 1 -- the mechanism NriGraphContext::Resize() and
    // ~NriGraphContext both depend on, pinned here because neither of THEM can
    // be exercised headlessly (both need a real window and swapchain).
    //
    // THE HAZARD. RenderGraph turns imported views over per Execute, but it
    // does so by BURYING them -- so between frames they sit PENDING in the
    // device graveyard, still naming the backbuffers they were created over.
    // The graveyard's ordinary drain site is ~NriDevice, which on the vehicle
    // runs AFTER the swapchain is destroyed. A pending view reaped then is a
    // DestroyDescriptor over a freed VkImage: a validation error, and therefore
    // a nonzero exit for a vehicle whose contract is "the latch did not grow".
    //
    // THE FIX both call sites make is to idle, ReleaseGpuResources() and DRAIN
    // before letting the backbuffers go. What that needs from the graph is that
    // the pair is a COMPLETE and IMMEDIATE release -- nothing left pending
    // afterwards. That is what this case pins. If a future edit stopped
    // ReleaseGpuResourcesInternal from calling ReleaseImportedViews, the
    // dangle would come back silently and this is what would catch it.
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    Arcane::NriUploadRing    ring;
    Arcane::NriPipelineCache pipelines;
    Arcane::RenderGraph      graph;
    const Arcane::RgExecuteDesc desc{ *device, nullptr, ring, pipelines, 0 };

    // Stands in for an acquired backbuffer -- never dereferenced (every NONE
    // Cmd* is a no-op and the graph only stores/compares the pointer).
    auto* const backbufferTexture = reinterpret_cast<nri::Texture*>(0x1000);
    constexpr nri::AccessLayoutStage kEntry{
        nri::AccessBits::NONE, nri::Layout::UNDEFINED, nri::StageBits::ALL };

    Arcane::RgTexture backbuffer;
    graph.AddNode("clear", Arcane::RenderGraph::NodeKind::Raster,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            backbuffer = builder.ImportTexture("backbuffer", backbufferTexture,
                                               kEntry, kPresentState, /*persistent=*/false);
            builder.Write(backbuffer, Arcane::RgUsage::ColorWrite);
            graph.SetColorAttachments(std::span<const Arcane::RgTexture>(&backbuffer, 1));
        },
        [](Arcane::RenderGraphNodeContext&) {});

    REQUIRE(graph.Execute(desc, CompileOk(graph)));

    // Steady state between frames: the view is LIVE (created this Execute) and
    // nothing is pending yet. This is the exact instant a resize arrives in.
    CHECK(device->Graves().Pending() == 0);

    // The release buries it -- and, on its own, that is all it does. Left here,
    // the reap would happen inside ~NriDevice, after the swapchain is gone.
    graph.ReleaseGpuResources();
    CHECK(device->Graves().Pending() == 1);

    // The drain is what makes it actually run, while the texture still exists.
    // Pending() == 0 is the whole property: NOTHING survives to be reaped after
    // the backbuffers are freed.
    device->Graves().Drain();
    CHECK(device->Graves().Pending() == 0);

    // ...and the graph is immediately usable again -- ReleaseGpuResources is
    // pool + views only, so the command slots and the fence survived and the
    // next frame simply re-realizes. That is what lets Resize() call it on
    // every window drag.
    graph.Reset();
    Arcane::RgTexture rebuilt;
    graph.AddNode("clear", Arcane::RenderGraph::NodeKind::Raster,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            // A DIFFERENT address: the post-resize backbuffer.
            rebuilt = builder.ImportTexture("backbuffer",
                                            reinterpret_cast<nri::Texture*>(0x2000),
                                            kEntry, kPresentState, /*persistent=*/false);
            builder.Write(rebuilt, Arcane::RgUsage::ColorWrite);
            graph.SetColorAttachments(std::span<const Arcane::RgTexture>(&rebuilt, 1));
        },
        [](Arcane::RenderGraphNodeContext&) {});
    REQUIRE(graph.Execute(desc, CompileOk(graph)));

    graph.ReleaseGpuResources();
    device->Graves().Drain();
    CHECK(device->Graves().Pending() == 0);

    CHECK(Arcane::RenderErrorCount() == before);
}

// ======================================================================
// The GPU-marker policy on the graph path (D1 shakedown, finding B)
// ======================================================================
// The vehicle holds TWO devices through Phase 2 -- the engine's NVRHI device,
// which the process-wide crash backend was built over, and the graph's NRI
// device -- and a native GPU marker is a write from THIS command buffer into
// THAT backend's marker buffer. Across a device boundary that is a spec
// violation: the first desk run fired 20 x
// VUID-vkCmdWriteBufferMarkerAMD-commonparent, and dx12 had the same bug
// silently (a GPU virtual address from another device's address space).
//
// NodeScope now gates the native marker on device IDENTITY. This case pins the
// gate from the closed side, which is the side that matters and the only one a
// NONE device can reach: GetDeviceNativeObject and GetCommandBufferNativeObject
// both answer null there, so the OPEN side (matching devices -> the marker goes
// out) stays a desk property.
namespace
{
    // Records what the graph path asked of a crash backend, and reports a
    // NativeDevice() that is deliberately not the graph's -- the vehicle's
    // two-device shape, headlessly.
    class MarkerSpyBackend final : public Arcane::IGpuCrashBackend
    {
    public:
        explicit MarkerSpyBackend(void* nativeDevice) noexcept : m_nativeDevice(nativeDevice) {}

        bool WriteMarker(nvrhi::ICommandList*, std::uint32_t, bool) override
        {
            ++nvrhiMarkers;
            return false;
        }
        bool WriteMarkerNative(void*, std::uint32_t, bool) override
        {
            ++nativeMarkers;
            return false;
        }
        void CollectFault(Arcane::Diag::Envelope&) override {}
        Arcane::GpuBreadcrumbs& Breadcrumbs() override { return m_breadcrumbs; }
        const char* Name() const override { return "spy"; }
        [[nodiscard]] void* NativeDevice() const override { return m_nativeDevice; }

        int nvrhiMarkers  = 0;
        int nativeMarkers = 0;

    private:
        void*                  m_nativeDevice = nullptr;
        Arcane::GpuBreadcrumbs m_breadcrumbs;
    };
}

TEST_CASE("rendergraph exec: a crash backend on ANOTHER device gets CPU breadcrumbs but no native marker", "[nri]")
{
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    // Any address that is not this graph's native device. (A NONE device's is
    // null, so the gate is closed here for BOTH reasons -- which is the honest
    // headless approximation of the vehicle: never open by accident.)
    int              foreignDevice = 0;
    MarkerSpyBackend spy(&foreignDevice);
    Arcane::SetActiveGpuCrashBackend(&spy);

    {
        Arcane::NriUploadRing    ring;
        Arcane::NriPipelineCache pipelines;
        Arcane::RenderGraph      graph;

        ThreeNodeGraph shape;
        shape.Declare(graph);

        const Arcane::RgCompiled    compiled = CompileOk(graph);
        const Arcane::RgExecuteDesc desc{ *device, /*swapChain=*/nullptr, ring, pipelines, /*frameSlot=*/0 };
        REQUIRE(graph.Execute(desc, compiled));
    }

    // THE PROPERTY: not one native marker crossed the device boundary.
    CHECK(spy.nativeMarkers == 0);
    // ...and the graph never reaches for the nvrhi overload at all (it holds no
    // nvrhi::ICommandList -- that is the whole reason WriteMarkerNative exists).
    CHECK(spy.nvrhiMarkers == 0);

    // The half that MUST survive the gate: the CPU breadcrumb ring still opened
    // one scope per node. Tokens are monotonic from 0 and never reused, so the
    // next one issued is exactly the number of scopes opened so far.
    CHECK(spy.Breadcrumbs().BeginScope("probe") == 3);

    REQUIRE(Arcane::ClearActiveGpuCrashBackendIfCurrent(&spy));
    CHECK(Arcane::RenderErrorCount() == before);
}

// ======================================================================
// The --nri-graph vehicle's frame SHAPE (Phase 2, Task 7)
// ======================================================================
// NriGraphContext itself needs a window, a device and a swapchain, so it is
// desk-only. Its DECLARATIONS are not: Compile() is pure, so the exact node/
// usage shape NriGraphContext::BuildFrame declares can be restated here and
// its derived barrier chain pinned headlessly. That is what makes "the clear
// frame presents, and a capture frame copies before it presents" a checked
// property rather than a desk observation -- and it is the chain Task 8 will
// insert its batch node into.
TEST_CASE("nri graph vehicle: the clear + capture frame derives colour -> copy -> present", "[nri]")
{
    Arcane::RenderGraph graph;
    Arcane::RgTexture backbuffer{};
    Arcane::RgBuffer  capture{};

    // Node 0 -- exactly BuildFrame's clear node: import the swapchain, declare
    // it as the colour attachment, write it. No draws; the clear itself is a
    // CmdClearAttachments inside the exec fn (the clear-seam decision -- see
    // NriGraphContext::BuildFrame), which is invisible to Compile by design.
    graph.AddNode("clear", Arcane::RenderGraph::NodeKind::Raster,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            backbuffer = builder.ImportSwapChainTexture("backbuffer");
            builder.Write(backbuffer, Arcane::RgUsage::ColorWrite);
            graph.SetColorAttachments(std::span<const Arcane::RgTexture>(&backbuffer, 1));
        },
        [](Arcane::RenderGraphNodeContext&) {});

    // Node 1 -- exactly BuildFrame's capture node. The staging buffer is
    // IMPORTED (a transient would be DEVICE-local and unmappable) and declared
    // ReadbackHost; the backbuffer is read as CopySrc.
    graph.AddNode("capture", Arcane::RenderGraph::NodeKind::Copy,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            capture = builder.ImportBuffer("capture", nullptr, 4096);
            builder.Read(backbuffer, Arcane::RgUsage::CopySrc);
            builder.Write(capture, Arcane::RgUsage::ReadbackHost);
        },
        [](Arcane::RenderGraphNodeContext&) {});

    const Arcane::RgCompiled compiled = CompileOk(graph);
    REQUIRE(compiled.nodes.size() == 2);

    // Node 0: the freshly acquired backbuffer's contents are not ours, so it
    // enters discarding and becomes a colour attachment.
    REQUIRE(compiled.nodes[0].preBarriers.size() == 1);
    CheckState(compiled.nodes[0].preBarriers[0].before, kUnknownState);
    CheckState(compiled.nodes[0].preBarriers[0].after,
               nri::AccessBits::COLOR_ATTACHMENT, nri::Layout::COLOR_ATTACHMENT,
               nri::StageBits::COLOR_ATTACHMENT);

    // Node 1: the backbuffer becomes a copy source. The imported staging
    // buffer also transitions (COPY_DESTINATION), and a buffer barrier's
    // layout is meaningless by contract -- hence UNDEFINED on both sides.
    REQUIRE(compiled.nodes[1].preBarriers.size() == 2);
    const Arcane::RgBarrier& textureBarrier = compiled.nodes[1].preBarriers[0].isTexture
                                                ? compiled.nodes[1].preBarriers[0]
                                                : compiled.nodes[1].preBarriers[1];
    const Arcane::RgBarrier& bufferBarrier  = compiled.nodes[1].preBarriers[0].isTexture
                                                ? compiled.nodes[1].preBarriers[1]
                                                : compiled.nodes[1].preBarriers[0];
    CHECK(textureBarrier.isTexture);
    CheckState(textureBarrier.after,
               nri::AccessBits::COPY_SOURCE, nri::Layout::COPY_SOURCE, nri::StageBits::COPY);
    CHECK_FALSE(bufferBarrier.isTexture);
    CheckState(bufferBarrier.after,
               nri::AccessBits::COPY_DESTINATION, nri::Layout::UNDEFINED, nri::StageBits::COPY);

    // ...and the graph -- not the caller -- is what leaves the backbuffer
    // present-ready. Exactly one exit barrier: the imported staging BUFFER
    // gets none (ImportBuffer takes no exit state to restore it to).
    REQUIRE(compiled.exitBarriers.size() == 1);
    CHECK(compiled.exitBarriers[0].isTexture);
    CheckState(compiled.exitBarriers[0].after, kPresentState);

    // Nothing transient: both resources are imported, so the frame allocates
    // no pool slot at all.
    CHECK(compiled.transients.empty());
    CHECK(compiled.poolSlotCount == 0);
}

// ======================================================================
// NriPipelineCache (Phase 2, Task 7)
// ======================================================================
// The NONE backend is enough to prove everything that is actually the
// CACHE's behaviour. Its CreatePipelineLayout/CreateGraphicsPipeline hand
// back one shared dummy object per type (ThirdParty/NRI/Source/NONE/
// ImplNONE.cpp), so pointer identity proves nothing here -- the observable
// that matters is HOW OFTEN the cache reached for the device, which these
// cases count through the fill callback and through Graveyard::Pending().
namespace
{
    // A minimal, deterministic layout desc. Value-initialized (`= {}`) and
    // then assigned field by field -- which is not style but
    // NriPipelineCache::RegisterLayout's stated DEDUP CONTRACT: the desc is
    // compared byte-wise, so its padding has to be zeroed.
    nri::PipelineLayoutDesc MakeLayoutDesc(nri::StageBits stages = nri::StageBits::VERTEX_SHADER
                                                                | nri::StageBits::FRAGMENT_SHADER)
    {
        nri::PipelineLayoutDesc desc = {};
        desc.shaderStages = stages;
        return desc;
    }

    Arcane::NriPipelineCache::GraphicsKey MakeGraphicsKey(std::uint32_t layoutId)
    {
        Arcane::NriPipelineCache::GraphicsKey key;
        key.shaderPairId    = 0x1234;
        key.layoutId        = layoutId;
        key.colorFormats[0] = nri::Format::RGBA8_UNORM;
        key.colorCount      = 1;
        key.blend           = Arcane::NriPipelineCache::GraphicsKey::Blend::AlphaOver;
        return key;
    }
}

TEST_CASE("nri pipeline cache: RegisterLayout dedups identical descs and separates different ones", "[nri]")
{
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    Arcane::NriPipelineCache cache;
    cache.Bind(*device);
    REQUIRE(cache.IsBound());

    const std::uint32_t a = cache.RegisterLayout(MakeLayoutDesc());
    REQUIRE(a != Arcane::NriPipelineCache::kInvalidLayout);
    CHECK(cache.LayoutCount() == 1);

    // An identical desc gets the SAME id, and creates nothing.
    const std::uint32_t again = cache.RegisterLayout(MakeLayoutDesc());
    CHECK(again == a);
    CHECK(cache.LayoutCount() == 1);

    // One field's worth of difference is a different layout -- exactly the
    // case a dedup that compared only pointers or counts would miss.
    const std::uint32_t b = cache.RegisterLayout(MakeLayoutDesc(nri::StageBits::COMPUTE_SHADER));
    CHECK(b != a);
    CHECK(cache.LayoutCount() == 2);

    CHECK(cache.Layout(a) != nullptr);
    CHECK(cache.Layout(b) != nullptr);
    // Ids this cache never issued resolve to null rather than to a neighbour.
    CHECK(cache.Layout(Arcane::NriPipelineCache::kInvalidLayout) == nullptr);
    CHECK(cache.Layout(9999) == nullptr);

    cache.Clear(device->Graves(), 0);
    device->Graves().Drain();
    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("nri pipeline cache: GetGraphics creates once per key and serves the rest", "[nri]")
{
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    Arcane::NriPipelineCache cache;
    cache.Bind(*device);
    const std::uint32_t layout = cache.RegisterLayout(MakeLayoutDesc());
    REQUIRE(layout != Arcane::NriPipelineCache::kInvalidLayout);

    int fills = 0;
    // What a real caller supplies: shaders + fixed-function state. It must NOT
    // touch the key-derived block -- and this one deliberately does, because
    // the cache re-stamps that block after the callback returns precisely so a
    // callback cannot desynchronise the cache from what it created (or leave
    // outputMerger.colors pointing into a dead stack frame).
    const auto fill = [&](nri::GraphicsPipelineDesc& desc)
    {
        ++fills;
        desc.rasterization.fillMode = nri::FillMode::SOLID;
        desc.rasterization.cullMode = nri::CullMode::NONE;
        desc.outputMerger.colors    = nullptr;
        desc.outputMerger.colorNum  = 0;
        desc.inputAssembly.topology = nri::Topology::POINT_LIST;
    };

    const Arcane::NriPipelineCache::GraphicsKey key = MakeGraphicsKey(layout);

    nri::Pipeline* first = cache.GetGraphics(key, fill);
    REQUIRE(first != nullptr);
    CHECK(fills == 1);
    CHECK(cache.PipelineCount() == 1);

    // A HIT: same key, no second creation, no second fill.
    nri::Pipeline* second = cache.GetGraphics(key, fill);
    CHECK(second == first);
    CHECK(fills == 1);
    CHECK(cache.PipelineCount() == 1);

    // A MISS on the attachment format alone -- the whole reason formats are in
    // the key (a pipeline bakes them, and binding it under a differently
    // formatted attachment is undefined on both backends).
    Arcane::NriPipelineCache::GraphicsKey other = key;
    other.colorFormats[0] = nri::Format::BGRA8_UNORM;
    REQUIRE(cache.GetGraphics(other, fill) != nullptr);
    CHECK(fills == 2);
    CHECK(cache.PipelineCount() == 2);

    // ...and on the packed state that is not a format.
    Arcane::NriPipelineCache::GraphicsKey additive = key;
    additive.blend = Arcane::NriPipelineCache::GraphicsKey::Blend::Additive;
    REQUIRE(cache.GetGraphics(additive, fill) != nullptr);
    CHECK(fills == 3);
    CHECK(cache.PipelineCount() == 3);

    cache.Clear(device->Graves(), 0);
    device->Graves().Drain();
    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("nri pipeline cache: Clear buries everything at one fence and reissues no stale id", "[nri]")
{
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    Arcane::NriPipelineCache cache;
    cache.Bind(*device);
    const std::uint32_t layout = cache.RegisterLayout(MakeLayoutDesc());
    int fills = 0;
    REQUIRE(cache.GetGraphics(MakeGraphicsKey(layout), [&](nri::GraphicsPipelineDesc&) { ++fills; })
            != nullptr);
    CHECK(fills == 1);

    // ONE burial per object, all at the SAME fence value -- which trivially
    // satisfies Graveyard's nondecreasing rule, and is why the owner passes
    // the graph's own last submitted value here rather than 0 (NriSmoke's
    // fence-0 teardown would violate it on a device the graph has used).
    CHECK(device->Graves().Pending() == 0);
    cache.Clear(device->Graves(), 7);
    CHECK(device->Graves().Pending() == 2);   // one pipeline + one layout
    CHECK(cache.LayoutCount() == 0);
    CHECK(cache.PipelineCount() == 0);

    // An id a caller held across the Clear() must not resolve to whatever
    // lands in the same slot next: the id counter keeps climbing.
    const std::uint32_t reissued = cache.RegisterLayout(MakeLayoutDesc());
    CHECK(reissued != layout);
    CHECK(cache.Layout(layout) == nullptr);
    CHECK(cache.Layout(reissued) != nullptr);

    // The pipeline is genuinely gone, so the same shape is a MISS again.
    REQUIRE(cache.GetGraphics(MakeGraphicsKey(reissued), [&](nri::GraphicsPipelineDesc&) { ++fills; })
            != nullptr);
    CHECK(fills == 2);

    cache.Clear(device->Graves(), 8);
    device->Graves().Drain();
    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("nri pipeline cache: caller-contract breaches are refused and latched, not served", "[nri]")
{
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    int fills = 0;
    const auto fill = [&](nri::GraphicsPipelineDesc&) { ++fills; };

    // 1. An unbound cache -- nothing can be created against no device.
    {
        Arcane::NriPipelineCache unbound;
        CHECK(unbound.RegisterLayout(MakeLayoutDesc()) == Arcane::NriPipelineCache::kInvalidLayout);
        CHECK(unbound.GetGraphics(MakeGraphicsKey(0), fill) == nullptr);
    }

    Arcane::NriPipelineCache cache;
    cache.Bind(*device);
    const std::uint32_t layout = cache.RegisterLayout(MakeLayoutDesc());
    REQUIRE(layout != Arcane::NriPipelineCache::kInvalidLayout);

    // 2. A layout id this cache never issued.
    CHECK(cache.GetGraphics(MakeGraphicsKey(layout + 500), fill) == nullptr);

    // 3. More colour attachments than the key can carry.
    Arcane::NriPipelineCache::GraphicsKey tooMany = MakeGraphicsKey(layout);
    tooMany.colorCount = Arcane::NriPipelineCache::kMaxColorAttachments + 1;
    CHECK(cache.GetGraphics(tooMany, fill) == nullptr);

    // 4. A graphics pipeline that would write nothing at all.
    Arcane::NriPipelineCache::GraphicsKey writesNothing = MakeGraphicsKey(layout);
    writesNothing.colorCount  = 0;
    writesNothing.depthFormat = nri::Format::UNKNOWN;
    CHECK(cache.GetGraphics(writesNothing, fill) == nullptr);

    // None of them reached the fill callback, and none was cached as a
    // poisoned entry a later call would keep serving.
    CHECK(fills == 0);
    CHECK(cache.PipelineCount() == 0);

    // Every refusal went through the tagged "nri-graph" seam, so a --nri-graph
    // desk run exits nonzero on any of them. This case therefore asserts the
    // OPPOSITE of its neighbours: the latch must have grown.
    CHECK(Arcane::RenderErrorCount() > before);

    cache.Clear(device->Graves(), 0);
    device->Graves().Drain();
    // Restore the latch so the rest of this process still sees a clean
    // baseline -- the same courtesy GpuCrashReportTest.cpp extends.
    Arcane::ResetRenderErrorCount();
}
