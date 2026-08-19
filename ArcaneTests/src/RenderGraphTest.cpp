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
// <windows.h> (reachable through Arcane/Base/Log.hpp -> spdlog, which nearly
// every engine header below pulls in) #defines ERROR via wingdi.h. Keep the
// NRI includes first.
#include <NRI.h>
#include <Extensions/NRIDeviceCreation.h>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/ImGui/ImGuiNri.hpp>           // the user-texture invalidation hook (Task 8-pre)
#include <Arcane/Render/RenderErrorLatch.hpp>  // RenderErrorCount / ResetRenderErrorCount
#include <Arcane/Render/GpuBreadcrumbs.hpp>    // the CPU-side ring the marker-policy case reads
#include <Arcane/Render/GpuInstrumentation.hpp>// SetActiveGpuCrashBackend / ClearActiveGpuCrashBackendIfCurrent
#include <Arcane/Render/IGpuCrashBackend.hpp>  // IGpuCrashBackend, for the marker-policy spy
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>   // DeclareGraphFrame -- the vehicle's frame SHAPE
#include <Arcane/Render/Nri/NriPipelineCache.hpp>
#include <Arcane/Render/Nri/nodes/Batch2DNode.hpp>   // SpriteMaterialLayout / the arena region math
#include <Arcane/Render/Nri/nodes/FullscreenNodes.hpp>  // FullscreenMaterialLayout / PostChainNode
#include <Arcane/Render/Nri/nodes/PickOutlineNodes.hpp> // OutlineJfaStepCount / PickNode / OutlineNode
#include <Arcane/Render/Nri/NriUploadRing.hpp>
#include <Arcane/Render/Nri/RenderGraph.hpp>
#include <Arcane/Render/Batcher2D.hpp>         // a device-less batcher drains the spans a node counts
#include <Arcane/Render/PostChainCache.hpp>    // PostChainDesc -- the frame's post-chain wiring
#include <Arcane/Material/MaterialSource.hpp>  // kSceneInput
#include <Arcane/Render/FramePacing.hpp>         // kSwapchainFramesInFlight

// AFTER the NRI + engine headers, deliberately: this file's include-order note
// above pins NRI first, and imgui.h is an ordinary header with no ERROR clash.
// Needed only by the ImGuiNri hook case, which creates and destroys its OWN
// ImGui context so the backend's platform-texture walk sees nothing but it.
#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <vector>

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

    // ------------------------------------------------------------------
    // THE D3D12 ENHANCED-BARRIER INVARIANT (D2 fix). NRI's D3D12 backend
    // derives LayoutBefore and AccessBefore independently -- GetBarrierLayout
    // is handed the access mask, but its ONLY access-sensitive branch is
    // Layout::INPUT_ATTACHMENT, which RenderGraph.cpp's StateFor never
    // produces -- and then sets D3D12_TEXTURE_BARRIER_FLAG_DISCARD from the
    // layout alone (ThirdParty/NRI/Source/D3D12/CommandBufferD3D12.hpp
    // :1054-1056, :170-175 and :1078-1079). A TEXTURE `before` that pairs
    // Layout::UNDEFINED with a non-NONE access therefore becomes a
    // D3D12_TEXTURE_BARRIER with LayoutBefore = UNDEFINED, AccessBefore !=
    // NO_ACCESS and FLAG_DISCARD -- rejected by enhanced-barrier validation,
    // which invalidates the command list, so EndCommandBuffer ->
    // ID3D12GraphicsCommandList::Close() fails. Vulkan accepts that pairing
    // happily, which is exactly why the shape shipped green on the VK half and
    // killed every dx12 `--nri-graph` run. NRI states the same contract in its
    // own public enum: Layout::UNDEFINED's "Compatible AccessBits" column is
    // EMPTY (Include/NRIDescs.h:544-547).
    //
    // BUFFERS are exempt: RgBarrier's layout is meaningless for them by
    // contract (always UNDEFINED) and Task 6's translation drops it.
    // ------------------------------------------------------------------
    void CheckBeforeIsD3D12Legal(const nri::AccessLayoutStage& before, bool isTexture = true)
    {
        if (!isTexture)
            return;
        CHECK((static_cast<std::uint32_t>(before.layout)
                   != static_cast<std::uint32_t>(nri::Layout::UNDEFINED)
               || static_cast<std::uint32_t>(before.access)
                   == static_cast<std::uint32_t>(nri::AccessBits::NONE)));
    }

    // The same invariant over EVERY barrier one compile produced. CompileOk()
    // below runs this on every compile the suite makes, which is the only
    // form that actually earns the name "regression net": a handover-producing
    // case added years from now inherits the coverage without its author
    // having to know this exists. A Task-4-era version of THIS would have
    // caught the D2 blocker before it shipped; a version applied case-by-case
    // would only have covered the cases someone remembered to annotate.
    void CheckAllBarriersD3D12Legal(const Arcane::RgCompiled& compiled)
    {
        for (const Arcane::RgCompiledNode& node : compiled.nodes)
        {
            for (const Arcane::RgBarrier& barrier : node.preBarriers)
            {
                INFO("node " << node.nodeIndex << ", barrier on resource " << barrier.resourceIndex);
                CheckBeforeIsD3D12Legal(barrier.before, barrier.isTexture);
            }
        }
        for (const Arcane::RgBarrier& barrier : compiled.exitBarriers)
        {
            INFO("exit barrier on resource " << barrier.resourceIndex);
            CheckBeforeIsD3D12Legal(barrier.before, barrier.isTexture);
        }
    }

    // Compile-must-succeed helper: surfaces the refusal message in the
    // failure output when a case that should compile does not.
    //
    // It also asserts the D3D12 enhanced-barrier invariant over everything the
    // compile derived. Putting it HERE rather than in the handover tests is
    // the point -- see CheckAllBarriersD3D12Legal above. Every
    // Layout::UNDEFINED-with-access expectation elsewhere in this file is a
    // BUFFER barrier, which the helper exempts by contract, so this is
    // universal rather than merely widespread.
    Arcane::RgCompiled CompileOk(const Arcane::RenderGraph& graph)
    {
        std::string error;
        std::optional<Arcane::RgCompiled> compiled = graph.Compile(&error);
        INFO("Compile() refused: " << error);
        REQUIRE(compiled.has_value());
        CheckAllBarriersD3D12Legal(*compiled);
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
    // StageBits::NONE)") -- and exactly what Phase 1's (since-retired)
    // triangle smoke wrote by hand for its present transition.
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

TEST_CASE("rendergraph compile: EVERY RgUsage derives a state distinct from the unknown one", "[nri]")
{
    // The unstated property TWO rules rest on, walked over every enumerator so
    // that adding a ninth RgUsage without a StateFor case fails HERE rather
    // than silently at runtime. `state = kUnknownState` is StateFor's
    // initialiser, so a missing case falls through to it, and then:
    //
    //  * a transient's FIRST USE emits no barrier at all, because Compile's
    //    edge test is `!SameState(current, want)` and `current` for a
    //    first use IS kUnknownState; and
    //  * the executor's cross-frame handover patch, which is guarded on
    //    `before.access == NONE && before.layout == UNDEFINED`, can no longer
    //    tell a slot's unpatched first-use barrier from a real one.
    //
    // Both failures are silent -- a dropped barrier and a mis-patched one, not
    // a refusal. The switch is over a plain enum with no MAX_NUM sentinel, so
    // -Wswitch has nothing to bite on and this walk is the coverage.
    //
    // The probe is the barrier's mere EXISTENCE: node 0 writes a fresh
    // transient with the usage under test, so its one pre-barrier is the
    // kUnknownState -> StateFor(usage) edge. If the two were equal there would
    // be no barrier to read, which is exactly the failure being pinned.
    const Arcane::RgUsage usages[] = {
        Arcane::RgUsage::ColorWrite, Arcane::RgUsage::DepthWrite,
        Arcane::RgUsage::ShaderRead, Arcane::RgUsage::ShaderWriteCs,
        Arcane::RgUsage::CopySrc,    Arcane::RgUsage::CopyDst,
        Arcane::RgUsage::Present,    Arcane::RgUsage::ReadbackHost,
    };
    // Guards the list itself against drifting out of sync with the enum: the
    // enumerators are contiguous and unvalued (RenderGraph.hpp), so the count
    // is the only thing that can silently change under this test.
    STATIC_REQUIRE(std::size(usages) == static_cast<std::size_t>(Arcane::RgUsage::ReadbackHost) + 1u);

    for (const Arcane::RgUsage usage : usages)
    {
        INFO("RgUsage enumerator " << static_cast<std::uint32_t>(usage));

        Arcane::RenderGraph graph;
        Arcane::RgTexture tex;
        graph.AddNode("use", Arcane::RenderGraph::NodeKind::Copy,
            [&](Arcane::RenderGraphBuilder& builder)
            {
                tex = builder.CreateTexture("tex", MakeColorDesc());
                builder.Write(tex, usage);
            },
            [](Arcane::RenderGraphNodeContext&) {});

        const Arcane::RgCompiled compiled = CompileOk(graph);
        REQUIRE(compiled.nodes.size() == 1);
        REQUIRE(compiled.nodes[0].preBarriers.size() == 1);
        CheckState(compiled.nodes[0].preBarriers[0].before, kUnknownState);
        CHECK_FALSE((static_cast<std::uint32_t>(compiled.nodes[0].preBarriers[0].after.access)
                         == static_cast<std::uint32_t>(kUnknownState.access)
                     && static_cast<std::uint32_t>(compiled.nodes[0].preBarriers[0].after.layout)
                         == static_cast<std::uint32_t>(kUnknownState.layout)
                     && static_cast<std::uint32_t>(compiled.nodes[0].preBarriers[0].after.stages)
                         == static_cast<std::uint32_t>(kUnknownState.stages)));
    }
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
    // LAYOUT carries too (D2 dx12 fix): the handover is an ordinary
    // state-to-state transition, because a D3D12 enhanced barrier pairing
    // LayoutBefore = UNDEFINED with a non-NONE AccessBefore is illegal and
    // fails Close() -- see RenderGraph.cpp's POOL HANDOVER block.
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
    // The previous tenant's state IN FULL -- access, layout and stages.
    CheckState(handover.before, kColorState);
    CheckState(handover.after, storageState);

    const Arcane::RgBarrier& fresh = compiled.nodes[2].preBarriers[1];
    CHECK(fresh.resourceIndex == 2u);      // `side`
    CHECK(fresh.isTexture);
    CheckState(fresh.before, kUnknownState);   // no previous tenant -- unchanged behaviour
    CheckState(fresh.after, storageState);
}

TEST_CASE("rendergraph compile: a pool handover ALWAYS emits a barrier, even when both tenants "
          "want the SAME state", "[nri]")
{
    // THE REGRESSION NET FOR THE D2 FIX. Carrying the previous tenant's LAYOUT
    // (rather than forcing UNDEFINED) means a handover's `before` can now
    // equal its `after` -- and the consecutive-same-state elision would then
    // swallow the barrier entirely. It must not: two tenants of one slot are
    // two LOGICAL resources on ONE physical object, so the previous tenant's
    // writes still need the source availability operation only a barrier
    // performs. Eliding it is the write-after-write hazard the pool handover
    // exists to close, arriving through a different door.
    //
    // `early` and `late` share a slot and BOTH use ColorWrite, so the
    // handover's before and after are the identical kColorState triple. While
    // the layout was forced to UNDEFINED this case could not exist (nothing
    // ever WANTS UNDEFINED, so before != after held by accident).
    Arcane::RenderGraph graph;
    Arcane::RgTexture early, late;

    graph.AddNode("declare", Arcane::RenderGraph::NodeKind::Compute,
        [&](Arcane::RenderGraphBuilder& builder)
        {
            early = builder.CreateTexture("early", MakeColorDesc());
            late  = builder.CreateTexture("late", MakeColorDesc());
        },
        [](Arcane::RenderGraphNodeContext&) {});

    AddColorNode(graph, "uses-early", early);   // node 1, lifetime [1,1]
    AddColorNode(graph, "uses-late", late);     // node 2, lifetime [2,2] -- same slot

    const Arcane::RgCompiled compiled = CompileOk(graph);
    REQUIRE(compiled.transients.size() == 2);
    REQUIRE(compiled.transientPoolSlot[0] == compiled.transientPoolSlot[1]);
    REQUIRE(compiled.poolSlotCount == 1);

    // THE PROPERTY: the barrier EXISTS...
    REQUIRE(compiled.nodes[2].preBarriers.size() == 1);
    const Arcane::RgBarrier& handover = compiled.nodes[2].preBarriers[0];
    CHECK(handover.resourceIndex == 1u);   // `late`
    // ...and it is a same-state one, which is what makes the assertion above
    // load-bearing rather than incidental.
    CheckState(handover.before, kColorState);
    CheckState(handover.after, kColorState);

    // And a same-state edge on ONE logical resource still elides, so the
    // exception is exactly as narrow as it claims to be: node 1's barrier is
    // `early`'s only one, and `early` is never barriered again.
    REQUIRE(compiled.nodes[1].preBarriers.size() == 1);
    CheckState(compiled.nodes[1].preBarriers[0].before, kUnknownState);
    CHECK(TotalBarriers(compiled) == 2);
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
// graph buries its resources in its LANE on the way out and destroys them
// through the device's function table, and reverse declaration order is what
// makes the graph die first (RgExecuteDesc::device's and ::graves' shared
// "must outlive the graph" contract, made structural).
//
// WHICH LANE THESE CASES PASS, and why it is the device's (NRI Phase 3,
// Task 8-pre): RgExecuteDesc::graves is a per-CONTEXT Graveyard, and a case
// that drives ONE graph has exactly one context's worth of burials -- so
// `device->Graves()` is a perfectly good lane for it, and passing it keeps
// every assertion in this file reading the same object it always did. It is a
// plain Graveyard that happens to be conveniently owned; nothing about a lane
// requires it to belong to a device. The cases that actually exercise TWO
// lanes declare their own Graveyards and never touch the device's -- see the
// TWO CONTEXTS, ONE DEVICE family further down.
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

    const Arcane::RgExecuteDesc desc{ *device, device->Graves(), /*swapChain=*/nullptr, ring, pipelines, /*frameSlot=*/0 };
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
    const Arcane::RgExecuteDesc desc{ *device, device->Graves(), nullptr, ring, pipelines, 0 };
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

    const Arcane::RgExecuteDesc first{ *device, device->Graves(), nullptr, ring, pipelines, 0 };
    REQUIRE(graph.Execute(first, compiled));
    const std::uint64_t created = graph.DebugTransientCreateCount();
    REQUIRE(created == compiled.poolSlotCount);

    // A different frame slot, same compiled graph -- the steady-state frame
    // shape. Nothing about the pool may change.
    const Arcane::RgExecuteDesc second{ *device, device->Graves(), nullptr, ring, pipelines, 1 };
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
    const Arcane::RgExecuteDesc desc{ *device, device->Graves(), nullptr, ring, pipelines, 0 };
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
        const Arcane::RgExecuteDesc desc{ *device, device->Graves(), nullptr, ring, pipelines,
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

TEST_CASE("rendergraph exec: the POOL EPOCH moves on a shrink and on a desc change, and not on "
          "steady-state reuse", "[nri]")
{
    // THE MECHANISM a node caching views over pool textures depends on
    // (RenderGraph::PoolEpoch). RealizePool buries pool textures from INSIDE
    // Execute() -- after every declaration, before every exec fn -- so an
    // owner has no ordering hook and a node has no other observable: NRI may
    // hand the recreated texture the address the destroyed one vacated, so a
    // pointer-keyed cache reports a HIT and binds freed memory.
    //
    // This was unreachable while the frame's shape was constant. It is
    // reachable now because a post chain changes how many pool slots a frame
    // needs -- it appears when its compile lands, goes away, and re-wires.
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    Arcane::NriUploadRing    ring;
    Arcane::NriPipelineCache pipelines;
    Arcane::RenderGraph      graph;
    const Arcane::RgExecuteDesc desc{ *device, device->Graves(), nullptr, ring, pipelines, 0 };

    // ONE node with N colour attachments, deliberately: it makes the two
    // shapes differ ONLY in slot count. Splitting them across nodes would also
    // change the surviving transient's USAGE bits (a dropped reader takes
    // SHADER_RESOURCE with it), which is a desc change -- and then the shrink
    // case below would silently be testing recreation instead of a pure
    // shrink, which is the case a create counter cannot see.
    const auto declare = [](Arcane::RenderGraph& g, std::uint32_t count, std::uint32_t extent)
    {
        g.AddNode("a", Arcane::RenderGraph::NodeKind::Raster,
            [&g, count, extent](Arcane::RenderGraphBuilder& builder)
            {
                std::vector<Arcane::RgTexture> targets;
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    const std::string name = "t" + std::to_string(i);
                    targets.push_back(builder.CreateTexture(name.c_str(),
                                                             MakeColorDesc(extent, extent)));
                    builder.Write(targets.back(), Arcane::RgUsage::ColorWrite);
                }
                g.SetColorAttachments(targets);
            },
            [](Arcane::RenderGraphNodeContext&) {});
    };

    declare(graph, /*count=*/2, /*extent=*/64);
    REQUIRE(graph.Execute(desc, CompileOk(graph)));
    REQUIRE(graph.DebugTransientCount() == 2);
    const std::uint64_t afterFirst = graph.PoolEpoch();

    // STEADY STATE: the same shape re-executed buries nothing, so a node's
    // cached views stay valid and it must NOT be told to rebuild them. An
    // epoch that moved every frame would be as useless as one that never did.
    graph.Reset();
    declare(graph, 2, 64);
    REQUIRE(graph.Execute(desc, CompileOk(graph)));
    CHECK(graph.PoolEpoch() == afterFirst);
    CHECK(graph.DebugTransientCreateCount() == 2);

    // A PURE SHRINK: slot 1 is past the new slot count, so RealizePool buries
    // it -- and DebugTransientCreateCount cannot see that, because nothing was
    // created. That is precisely why the epoch is its own counter.
    graph.Reset();
    declare(graph, 1, 64);
    REQUIRE(graph.Execute(desc, CompileOk(graph)));
    CHECK(graph.DebugTransientCount() == 1);
    const std::uint64_t afterShrink = graph.PoolEpoch();
    CHECK(afterShrink > afterFirst);
    CHECK(graph.DebugTransientCreateCount() == 2);   // nothing new was made

    // A DESC CHANGE: slot 0's extent no longer matches, so it is buried and
    // recreated -- the case where the replacement can legitimately land on the
    // freed address, which is exactly what makes a pointer comparison useless.
    graph.Reset();
    declare(graph, 1, 32);
    REQUIRE(graph.Execute(desc, CompileOk(graph)));
    const std::uint64_t afterRecreate = graph.PoolEpoch();
    CHECK(afterRecreate > afterShrink);
    CHECK(graph.DebugTransientCreateCount() == 3);

    // ...and the explicit release moves it too, so a node that checks the
    // epoch after one cannot conclude its views are still current.
    graph.ReleaseGpuResources();
    CHECK(graph.PoolEpoch() > afterRecreate);

    device->Graves().Reap(graph.DebugSubmitCount());
    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("rendergraph exec: a retired pool texture is buried AFTER every view naming it -- "
          "including a holder that SKIPPED the reshaping frame", "[nri]")
{
    // THE ORDERING HALF of the pool-epoch contract (whole-branch review, I1).
    // The epoch case above pins that a holder is TOLD; this one pins that the
    // telling happens in time to matter.
    //
    // A Graveyard replays its due prefix in BURIAL ORDER (Graveyard.hpp), so
    // "a view must never be destroyed after the texture it views" is really "a
    // view must never be BURIED after it". Through Phase 2 RealizePool buried
    // the texture inline, from the middle of Execute() -- ahead of every node
    // view buried by the exec fns that follow (same value, same lane), and
    // frames ahead of a node that was not in the reshaping frame at all. That
    // second case is the routine one now: the outline chain is declared only
    // while something wants an outline, so toggling the selection off shrinks
    // the pool on a frame OutlineNode does not record.
    //
    // WHAT PROVES THE ORDER HERE: a Graveyard is append-only and Pending()
    // counts what is queued, so the SEQUENCE in which Pending() grows IS the
    // burial order. The case therefore asserts the deltas at three points and
    // nothing about the thunks themselves (NONE's Destroy* record nothing, and
    // every CreateTexture hands back the same dummy pointer, so neither the
    // device nor a pointer comparison can be the witness).
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    Arcane::NriUploadRing    ring;
    Arcane::NriPipelineCache pipelines;
    Arcane::RenderGraph      graph;
    const Arcane::RgExecuteDesc desc{ *device, device->Graves(), nullptr, ring, pipelines, 0 };

    // The epoch case's shape helper, verbatim: ONE node with N colour
    // attachments, so the two shapes differ ONLY in pool slot count.
    const auto declare = [](Arcane::RenderGraph& g, std::uint32_t count, std::uint32_t extent)
    {
        g.AddNode("a", Arcane::RenderGraph::NodeKind::Raster,
            [&g, count, extent](Arcane::RenderGraphBuilder& builder)
            {
                std::vector<Arcane::RgTexture> targets;
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    const std::string name = "t" + std::to_string(i);
                    targets.push_back(builder.CreateTexture(name.c_str(),
                                                             MakeColorDesc(extent, extent)));
                    builder.Write(targets.back(), Arcane::RgUsage::ColorWrite);
                }
                g.SetColorAttachments(targets);
            },
            [](Arcane::RenderGraphNodeContext&) {});
    };

    // A STAND-IN for the three real nodes that cache descriptors over pool
    // textures (PostChainNode, TonemapNode, OutlineNode). It does exactly what
    // their SyncPoolEpoch does and nothing else: compare the graph's epoch,
    // and on a move bury every cached view at DebugSubmitCount(). The real
    // nodes cannot be built headlessly (they need a live device, a pipeline
    // cache and shader bytecode), so this case pins the MECHANISM and its
    // ORDER; that NriGraphContext::BuildFrame drives the real three at
    // declaration time is inspection-verified, like the Dist guards.
    struct ViewHolder
    {
        std::uint64_t epoch  = 0;
        int           cached = 0;
        int           freed  = 0;

        void Sync(Arcane::RenderGraph& g)
        {
            Arcane::Graveyard* graves = g.Graves();
            if (!graves || g.PoolEpoch() == epoch)
                return;
            epoch = g.PoolEpoch();
            for (int i = 0; i < cached; ++i)
                graves->Bury(g.DebugSubmitCount(), [this] { ++freed; });
            cached = 0;
        }
    };
    ViewHolder holder;

    // ---- frame 1: two pool slots; the holder builds views over them --------
    declare(graph, /*count=*/2, /*extent=*/64);
    REQUIRE(graph.Execute(desc, CompileOk(graph)));
    REQUIRE(graph.DebugTransientCount() == 2);
    REQUIRE(device->Graves().Pending() == 0);
    REQUIRE(graph.DebugRetiredPoolCount() == 0);
    holder.Sync(graph);        // adopts the current epoch (nothing was buried)
    holder.cached = 2;         // ...and now holds a descriptor over each slot

    // ---- frame 2: THE RESHAPE, on a frame the holder is not in -------------
    graph.Reset();
    declare(graph, /*count=*/1, /*extent=*/64);
    REQUIRE(graph.Execute(desc, CompileOk(graph)));
    CHECK(graph.DebugTransientCount() == 1);

    // The retired texture is STAGED, not buried: nothing about it has reached
    // the graveyard yet, and it will not until an Execute that every holder
    // has had its declaration-time chance to precede. This single assertion is
    // what the pre-fix code fails -- there the texture went into the graveyard
    // right here, ahead of the two views below.
    CHECK(graph.DebugRetiredPoolCount() == 1);
    CHECK(holder.freed == 0);
    const std::size_t afterReshape = device->Graves().Pending();

    // ---- frame 3: declaration first, then Execute --------------------------
    graph.Reset();
    // What NriGraphContext::BuildFrame does for EVERY node it owns, in frame
    // or not. The holder skipped the reshaping frame entirely; this is the
    // only point at which it can still bury first.
    holder.Sync(graph);
    CHECK(device->Graves().Pending() == afterReshape + 2);   // the two views went in HERE...

    declare(graph, /*count=*/1, /*extent=*/64);
    REQUIRE(graph.Execute(desc, CompileOk(graph)));
    CHECK(graph.DebugRetiredPoolCount() == 0);
    CHECK(device->Graves().Pending() == afterReshape + 3);   // ...and the texture AFTER them

    // The steady state is unaffected: a frame that reshapes nothing stages
    // nothing and buries nothing.
    graph.Reset();
    declare(graph, 1, 64);
    REQUIRE(graph.Execute(desc, CompileOk(graph)));
    CHECK(graph.DebugRetiredPoolCount() == 0);
    CHECK(device->Graves().Pending() == afterReshape + 3);

    // And the whole queue drains cleanly, in that order.
    device->Graves().Reap(graph.DebugSubmitCount());
    CHECK(device->Graves().Pending() == 0);
    CHECK(holder.freed == 2);

    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("rendergraph exec: a release with a staged retirement buries it too, rather than "
          "stranding it", "[nri]")
{
    // The staging area's other exit (whole-branch review, I1). The flush lives
    // at the top of the next Execute() -- and that Execute may never come: a
    // ReleaseGpuResources (the editor's project switch / resize path) or
    // ~RenderGraph is the end of the line. ReleaseGpuResourcesInternal
    // therefore flushes too, after its own view burials and before the pool's,
    // so the order it produces is the same views-then-resources one.
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    Arcane::NriUploadRing    ring;
    Arcane::NriPipelineCache pipelines;
    Arcane::RenderGraph      graph;
    const Arcane::RgExecuteDesc desc{ *device, device->Graves(), nullptr, ring, pipelines, 0 };

    const auto declare = [](Arcane::RenderGraph& g, std::uint32_t count)
    {
        g.AddNode("a", Arcane::RenderGraph::NodeKind::Raster,
            [&g, count](Arcane::RenderGraphBuilder& builder)
            {
                std::vector<Arcane::RgTexture> targets;
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    const std::string name = "t" + std::to_string(i);
                    targets.push_back(builder.CreateTexture(name.c_str(), MakeColorDesc(64, 64)));
                    builder.Write(targets.back(), Arcane::RgUsage::ColorWrite);
                }
                g.SetColorAttachments(targets);
            },
            [](Arcane::RenderGraphNodeContext&) {});
    };

    declare(graph, 2);
    REQUIRE(graph.Execute(desc, CompileOk(graph)));
    graph.Reset();
    declare(graph, 1);
    REQUIRE(graph.Execute(desc, CompileOk(graph)));
    REQUIRE(graph.DebugRetiredPoolCount() == 1);

    const std::size_t staged = device->Graves().Pending();
    graph.ReleaseGpuResources();
    CHECK(graph.DebugRetiredPoolCount() == 0);
    // The staged texture PLUS the surviving slot: nothing was stranded, and a
    // graveyard destroyed with pending burials is fatal in Debug anyway.
    CHECK(device->Graves().Pending() > staged);
    CHECK(graph.DebugTransientCount() == 0);

    device->Graves().Reap(graph.DebugSubmitCount());
    CHECK(device->Graves().Pending() == 0);
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
    const Arcane::RgExecuteDesc desc{ *device, device->Graves(), nullptr, ring, pipelines, 0 };

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

    // What was actually RECORDED: the previous frame's outgoing state IN FULL
    // -- access, LAYOUT and stages -- byte-for-byte the within-frame
    // handover's shape. The layout carries rather than staying UNDEFINED
    // because {UNDEFINED layout, non-NONE access} is an illegal D3D12
    // enhanced barrier that fails Close() (the D2 blocker); see
    // RenderGraph.cpp's POOL HANDOVER block. has_value() is half the
    // assertion: a handover that emitted NO barrier would fail right here.
    const auto secondFrame = graph.DebugFirstBarrierBefore(0);
    REQUIRE(secondFrame.has_value());
    CheckState(*secondFrame, nri::AccessBits::COPY_DESTINATION, nri::Layout::COPY_DESTINATION,
               nri::StageBits::COPY);
    CheckBeforeIsD3D12Legal(*secondFrame);

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
    const Arcane::RgExecuteDesc desc{ *device, device->Graves(), nullptr, ring, pipelines, 0 };

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

    // THE TWO HALVES RETIRE ON DIFFERENT SCHEDULES, and the split is the
    // ordering fix (whole-branch review, I1 -- see the "buried AFTER every view
    // naming it" case). The VIEWS go into the graveyard immediately, here: one
    // of them, because on the NONE backend every CreateTexture hands back the
    // same dummy pointer, so all three transients share one cached attachment
    // view and the first slot's sweep takes it. The TEXTURES are staged instead
    // and buried at the top of the NEXT Execute -- which is what puts them
    // behind every node view, including a node that skipped this frame.
    CHECK(device->Graves().Pending() == 1);
    CHECK(graph.DebugRetiredPoolCount() == firstPoolSlots);

    // A re-realized slot starts genuinely undefined, so no carry state from
    // the retired texture may leak into it.
    const auto firstBarrier = graph.DebugFirstBarrierBefore(0);
    REQUIRE(firstBarrier.has_value());
    CheckState(*firstBarrier, kUnknownState);

    // The staged textures are discharged by the next Execute -- or, as here,
    // by an explicit release, which flushes the staging area too rather than
    // stranding it.
    graph.ReleaseGpuResources();
    CHECK(graph.DebugRetiredPoolCount() == 0);
    device->Graves().Reap(graph.DebugSubmitCount());
    CHECK(device->Graves().Pending() == 0);

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

    const Arcane::RgExecuteDesc desc{ *device, device->Graves(), nullptr, ring, pipelines, 0 };
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
    const Arcane::RgExecuteDesc desc{ *device, device->Graves(), /*swapChain=*/nullptr, ring, pipelines, 0 };
    CHECK_FALSE(graph.Execute(desc, compiled));
    CHECK(graph.DebugSubmitCount() == 0);

    // The refusal reached the shared 0/0 gate latch through the "nri-graph"
    // tagged seam -- restore it so no other case inherits the +1.
    CHECK(Arcane::RenderErrorCount() == before + 1);
    Arcane::ResetRenderErrorCount();
}

TEST_CASE("rendergraph exec: an offscreen frame executes with NO swapchain -- no acquire, no "
          "present, and the graph's own fence still advances", "[nri]")
{
    // The executor half of NRI Phase 3, Task 7. RgExecuteDesc::swapChain is
    // documented as "null = headless/offscreen" and the refusal case above
    // proves the NEGATIVE (a swapchain IMPORT with no swapchain is refused);
    // this proves the POSITIVE, which is what NriGraphContext::
    // RenderFrameOffscreen actually runs: a frame whose final target is an
    // ordinary imported texture records, submits and signals with no acquire
    // and no present anywhere in the call.
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    // A REAL (NONE-backend) texture, because unlike the declaration-only cases
    // this one runs the executor: it resolves the import, reads its desc and
    // creates a COLOR_ATTACHMENT view over it.
    const nri::CoreInterface& core = device->Core();
    nri::TextureDesc outputDesc = {};
    outputDesc.type      = nri::TextureType::TEXTURE_2D;
    outputDesc.usage     = nri::TextureUsageBits::COLOR_ATTACHMENT | nri::TextureUsageBits::SHADER_RESOURCE;
    outputDesc.format    = nri::Format::BGRA8_UNORM;
    outputDesc.width     = 320;
    outputDesc.height    = 200;
    outputDesc.depth     = 1;
    outputDesc.mipNum    = 1;
    outputDesc.layerNum  = 1;
    outputDesc.sampleNum = 1;
    nri::Texture* output = nullptr;
    REQUIRE(core.CreateCommittedTexture(device->Device(), nri::MemoryLocation::DEVICE, 0.0f,
                                         outputDesc, output) == nri::Result::SUCCESS);
    REQUIRE(output != nullptr);

    {
        Arcane::NriUploadRing    ring;
        Arcane::NriPipelineCache pipelines;
        Arcane::RenderGraph      graph;
        const Arcane::RgExecuteDesc desc{ *device, device->Graves(), /*swapChain=*/nullptr, ring, pipelines, 0 };

        Arcane::RgFrameShape shape;
        shape.canvasWidth     = 320;
        shape.canvasHeight    = 200;
        shape.offscreenOutput = output;

        Arcane::DeclareGraphFrame(graph, shape, nullptr);
        const Arcane::RgCompiled compiled = CompileOk(graph);
        REQUIRE(graph.Execute(desc, compiled));

        // The graph's own fence is what a swapchain-less frame has instead of
        // a present, and it advanced -- so the graveyard has a clock and the
        // pacing wait NriGraphContext::RenderFrameOffscreen performs has
        // something to wait on.
        CHECK(graph.DebugSubmitCount() == 1);
        CHECK(graph.DebugTransientCount() == 1);   // the canvas; the output is imported

        // A SECOND frame reuses the pool and creates nothing -- the steady
        // state an editor viewport lives in.
        graph.Reset();
        Arcane::DeclareGraphFrame(graph, shape, nullptr);
        REQUIRE(graph.Execute(desc, CompileOk(graph)));
        CHECK(graph.DebugSubmitCount() == 2);
        CHECK(graph.DebugTransientCreateCount() == 1);

        // ------------------------------------------------------------
        // THE RESIZE SEQUENCE ResizeOffscreen performs, in order: idle,
        // invalidate the nodes' cached views (no nodes here -- that half is
        // FullscreenNodes' own coverage), release the graph's GPU resources,
        // DRAIN, and only then destroy + recreate the output.
        //
        // What is pinned here is the ORDER and the EPOCH: the release bumps the
        // POOL EPOCH (the only signal a node caching views over pool textures
        // gets), and the drain empties the graveyard -- running every view
        // burial the release queued -- while the output texture is still alive,
        // which is the whole reason the drain sits between the release and the
        // destroy rather than after both. (On the NONE backend every
        // CreateTexture hands back the SAME dummy pointer, so this cannot also
        // assert that the imported view is a DISTINCT one from the canvas's --
        // see this section's header comment.)
        // ------------------------------------------------------------
        const std::uint64_t epochBeforeResize = graph.PoolEpoch();
        graph.ReleaseGpuResources();
        CHECK(graph.PoolEpoch() > epochBeforeResize);
        CHECK(device->Graves().Pending() > 0);
        device->Graves().Drain();
        CHECK(device->Graves().Pending() == 0);

        core.DestroyTexture(output);
        output = nullptr;

        nri::TextureDesc resized = outputDesc;
        resized.width  = 640;
        resized.height = 400;
        REQUIRE(core.CreateCommittedTexture(device->Device(), nri::MemoryLocation::DEVICE, 0.0f,
                                             resized, output) == nri::Result::SUCCESS);

        // ...and the next frame at the new extent runs clean against the fresh
        // texture, realizing a new canvas slot (the old one's desc changed).
        graph.Reset();
        shape.canvasWidth     = 640;
        shape.canvasHeight    = 400;
        shape.offscreenOutput = output;
        Arcane::DeclareGraphFrame(graph, shape, nullptr);
        REQUIRE(graph.Execute(desc, CompileOk(graph)));
        CHECK(graph.DebugSubmitCount() == 3);

        graph.ReleaseGpuResources();
        device->Graves().Drain();
    }

    core.DestroyTexture(output);
    CHECK(Arcane::RenderErrorCount() == before);
}

// =========================================================================
// TWO CONTEXTS, ONE DEVICE -- the per-context Graveyard LANE (NRI Phase 3,
// Task 8-pre). The topology Task 8 wires up: an editor holding a host-window
// context (chrome -> present) and an offscreen context (the viewport) over ONE
// NriDevice, because Vulkan-Hpp's default dispatcher binds one VkDevice per
// process and DXGI allows one flip-model swapchain per HWND.
//
// WHAT WAS WRONG WITH ONE GRAVEYARD. Burials are keyed to the burying GRAPH's
// own submission fence, and two contexts have two RenderGraphs and therefore
// two INDEPENDENT fence timelines whose values mean nothing to each other. In
// one shared graveyard that is (i) a Debug nondecreasing assert the moment the
// counters interleave out of order and (ii) -- with no assert at all -- each
// Execute reaping the OTHER graph's thunks against ITS completed value, i.e.
// destroying resources a submission is still reading.
//
// These cases drive TWO RenderGraphs on ONE NONE device with TWO lanes, which
// is exactly the shape two NriGraphContexts have (the contexts themselves need
// a window, a swapchain and real shaders, so they are desk items; the lane
// property is not). Everything below deliberately leaves device->Graves()
// ALONE except as a stand-in for "some other owner's pending burials", which
// is the state a real second context would find it in.
// =========================================================================

TEST_CASE("rendergraph exec: two graphs on one device bury into their OWN lanes, and each "
          "Execute reaps only its own", "[nri]")
{
    // THE CASE THE OLD SHAPE CORRUPTED. Two graphs, interleaved burials, and
    // the assertion is that neither lane ever contains the other's work and
    // neither graph's reap can run the other's thunks.
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    // Stands in for a THIRD owner (or simply for whatever the device's
    // graveyard holds): nothing below may touch it. Reaped by hand at the end
    // so ~Graveyard is not left holding it.
    bool deviceLaneRan = false;
    device->Graves().Bury(4096, [&deviceLaneRan] { deviceLaneRan = true; });

    // Declared BEFORE the graphs, so each graph is destroyed before its lane --
    // RgExecuteDesc::graves' "must outlive the graph" contract, made structural
    // exactly as NriGraphContext's member order makes it.
    Arcane::Graveyard hostLane;
    Arcane::Graveyard viewportLane;

    {
        Arcane::NriUploadRing    ring;
        Arcane::NriPipelineCache pipelines;
        Arcane::RenderGraph      hostGraph;
        Arcane::RenderGraph      viewportGraph;

        const Arcane::RgExecuteDesc hostDesc{ *device, hostLane, nullptr, ring, pipelines, 0 };
        const Arcane::RgExecuteDesc viewportDesc{ *device, viewportLane, nullptr, ring, pipelines, 0 };

        // Both graphs run the same 3-node shape. Two pool slots each, so a
        // ReleaseGpuResources on either buries a known, nonzero amount.
        ThreeNodeGraph hostShape, viewportShape;
        hostShape.Declare(hostGraph);
        viewportShape.Declare(viewportGraph);

        REQUIRE(hostGraph.Execute(hostDesc, CompileOk(hostGraph)));
        REQUIRE(viewportGraph.Execute(viewportDesc, CompileOk(viewportGraph)));
        CHECK(hostGraph.DebugSubmitCount() == 1);
        CHECK(viewportGraph.DebugSubmitCount() == 1);

        // Each graph latched ITS lane, and says so.
        CHECK(hostGraph.Graves() == &hostLane);
        CHECK(viewportGraph.Graves() == &viewportLane);

        // Drive the HOST's timeline several submissions ahead, which is the
        // realistic asymmetry: chrome renders every frame, a viewport panel may
        // not. This is what used to make the shared graveyard's nondecreasing
        // invariant unsatisfiable for the viewport.
        for (int frame = 0; frame < 4; ++frame)
        {
            hostGraph.Reset();
            hostShape.Declare(hostGraph);
            REQUIRE(hostGraph.Execute(hostDesc, CompileOk(hostGraph)));
        }
        CHECK(hostGraph.DebugSubmitCount() == 5);
        CHECK(viewportGraph.DebugSubmitCount() == 1);

        // ---- INTERLEAVED BURIALS, out of order across the two timelines ----
        // The viewport buries at 1 AFTER the host has buried at 5. In one
        // shared graveyard that is Bury's nondecreasing assert, verbatim.
        hostGraph.ReleaseGpuResources();
        const std::size_t hostPending = hostLane.Pending();
        CHECK(hostPending > 0);
        CHECK(viewportLane.Pending() == 0);   // nothing of the viewport's landed there

        viewportGraph.ReleaseGpuResources();
        const std::size_t viewportPending = viewportLane.Pending();
        CHECK(viewportPending > 0);
        // ...and the host's lane did not grow: the two are disjoint.
        CHECK(hostLane.Pending() == hostPending);

        // ---- EACH REAPS ONLY ITS OWN ----
        // The viewport's next Execute reaps with ITS fence (NONE hard-wires
        // GetFenceValue to 0, so nothing is due) and must not touch the host's
        // lane, which is pending at 5.
        viewportGraph.Reset();
        viewportShape.Declare(viewportGraph);
        REQUIRE(viewportGraph.Execute(viewportDesc, CompileOk(viewportGraph)));
        CHECK(hostLane.Pending() == hostPending);

        // By hand, since NONE cannot advance a fence: reaping the HOST's lane
        // at the host's completed value clears the host's burials and leaves
        // every one of the viewport's alone. Under the old shape this same
        // number (5) would have swept the viewport's fence-1 thunks with it --
        // the use-after-free no assert catches.
        hostLane.Reap(hostGraph.DebugSubmitCount());
        CHECK(hostLane.Pending() == 0);
        CHECK(viewportLane.Pending() == viewportPending);

        // The shared device graveyard was never a party to any of it.
        CHECK(device->Graves().Pending() == 1);
        CHECK_FALSE(deviceLaneRan);

        // Both graphs die here, burying their command slots and fences into
        // their own lanes...
    }
    // ...which is what these drains run. (Order matters only within a lane.)
    hostLane.Drain();
    viewportLane.Drain();
    CHECK(hostLane.Pending() == 0);
    CHECK(viewportLane.Pending() == 0);

    CHECK(device->Graves().Pending() == 1);
    device->Graves().Drain();
    CHECK(deviceLaneRan);

    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("rendergraph exec: a second Graveyard lane is refused on a graph that already buried "
          "into another", "[nri]")
{
    // The lane's half of the "one graph belongs to one device" refusal. Every
    // burial already pending sits in the FIRST lane, keyed to this graph's
    // fence timeline; splitting the rest across a second would leave two
    // graveyards holding halves of ONE ordered sequence, and the ordering IS
    // the contract (a view must be destroyed before the texture it views).
    // Nothing can enforce that across two graveyards, so it is refused rather
    // than silently adopted -- exactly like a second device.
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    Arcane::Graveyard firstLane;
    Arcane::Graveyard secondLane;
    {
        Arcane::NriUploadRing    ring;
        Arcane::NriPipelineCache pipelines;
        Arcane::RenderGraph      graph;

        ThreeNodeGraph shape;
        shape.Declare(graph);
        const Arcane::RgCompiled compiled = CompileOk(graph);

        const Arcane::RgExecuteDesc first{ *device, firstLane, nullptr, ring, pipelines, 0 };
        REQUIRE(graph.Execute(first, compiled));

        const Arcane::RgExecuteDesc second{ *device, secondLane, nullptr, ring, pipelines, 0 };
        graph.Reset();
        shape.Declare(graph);
        CHECK_FALSE(graph.Execute(second, CompileOk(graph)));
        // Refused BEFORE recording: the submit count did not move.
        CHECK(graph.DebugSubmitCount() == 1);
        CHECK(secondLane.Pending() == 0);
        CHECK(graph.Graves() == &firstLane);
    }
    firstLane.Drain();
    CHECK(secondLane.Pending() == 0);

    CHECK(Arcane::RenderErrorCount() == before + 1);
    Arcane::ResetRenderErrorCount();
}

TEST_CASE("rendergraph exec: a graph whose Execute was never ENTERED buries nothing -- not on "
          "release, not on destruction", "[nri]")
{
    // Header window (b1) (NriGraphContext.hpp, TWO CONTEXTS, TWO LANES): a
    // failed InitOffscreen, or a context created and dropped without a frame.
    // RenderGraph never latched a device OR a lane, and
    // ReleaseGpuResourcesInternal returns early while either is null -- so it
    // buries nothing ANYWHERE: there are no command buffers, allocators or
    // fence to bury, because Execute is what creates them.
    //
    // WHAT CHANGED AT TASK 8-pre. This used to be the one window Task 7's
    // LOCAL-graveyard branch in ~NriGraphContext could close, and the case
    // existed to keep that branch honest. That branch is GONE -- the lane
    // subsumes it, since a context's lane starts EMPTY and 0 is nondecreasing
    // against nothing. The case is kept and re-aimed at the structural fact it
    // always really pinned: a later change that allocated command buffers, an
    // allocator or the fence AHEAD of Execute() would start burying at fence 0
    // from a graph with no lane latched at all. That must be red here rather
    // than a null-deref in the editor.
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    // Seeded high, exactly as a live sibling context's lane would be -- the
    // value a stray fence-0 burial would have to be nondecreasing against.
    bool reaped = false;
    Arcane::Graveyard siblingLane;
    siblingLane.Bury(4096, [&reaped] { reaped = true; });
    REQUIRE(siblingLane.Pending() == 1);

    {
        Arcane::RenderGraph graph;

        // A full frame's worth of DECLARATIONS -- and no Execute() at all.
        // An OFFSCREEN frame specifically, since that is the mode whose
        // teardown depends on this. (The stand-in output pointer is spelled out
        // here rather than shared with the offscreen frame-shape family below:
        // that helper is declared with those cases, several hundred lines on.)
        Arcane::RgFrameShape shape;
        shape.canvasWidth     = 320;
        shape.canvasHeight    = 200;
        shape.offscreenOutput = reinterpret_cast<nri::Texture*>(0x0FF5C1EE);
        Arcane::DeclareGraphFrame(graph, shape, nullptr);
        CHECK(graph.NodeCount() == 2);

        // Nothing was realized, so there is nothing to release -- and no lane
        // was latched to release it into.
        CHECK(graph.DebugSubmitCount() == 0);
        CHECK(graph.DebugTransientCount() == 0);
        CHECK(graph.Graves() == nullptr);

        graph.ReleaseGpuResources();
        CHECK(siblingLane.Pending() == 1);           // still only the seed
        CHECK(device->Graves().Pending() == 0);      // and nothing leaked to the device's
    }
    // ...and ~RenderGraph added nothing either: there are no command buffers,
    // no allocators and no submission fence to bury, because Execute() is what
    // creates them.
    CHECK(siblingLane.Pending() == 1);
    CHECK(device->Graves().Pending() == 0);
    CHECK_FALSE(reaped);

    siblingLane.Drain();
    CHECK(reaped);
    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("rendergraph exec: a graph whose Execute ENTERED and FAILED buries its tail in its own "
          "lane, at fence 0, past a sibling lane already at 4096", "[nri]")
{
    // HEADER WINDOW (b2) -- the sharpest edge, and the one the lane exists for.
    //
    // RenderGraph latches its device UNCONDITIONALLY at Execute's ENTRY, before
    // anything fallible, while m_submitValue only advances after a successful
    // QueueSubmit. So a first frame that reaches Execute and fails inside it (a
    // first-frame device loss on the viewport is the realistic Task-8 shape)
    // leaves a graph that HAS realized command buffers, an allocator and a
    // fence and WILL bury them -- at fence 0. Before the lane those went into
    // the shared device graveyard, behind whatever the sibling context had
    // already driven to N: a Debug nondecreasing assert, and in Release a reap
    // against a foreign fence.
    //
    // The failure driven here is an out-of-range frameSlot, which is refused
    // AFTER EnsureExecutionResources has created everything -- the same
    // "entered, realized, never submitted" state a mid-Execute device loss
    // leaves behind, and the only one a NONE device can be made to produce
    // (every NONE Create* succeeds, so the all-or-nothing cleanup inside
    // EnsureExecutionResources itself cannot be driven headlessly).
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    bool siblingRan = false;
    Arcane::Graveyard siblingLane;
    siblingLane.Bury(4096, [&siblingRan] { siblingRan = true; });

    Arcane::Graveyard viewportLane;
    {
        Arcane::NriUploadRing    ring;
        Arcane::NriPipelineCache pipelines;
        Arcane::RenderGraph      graph;

        ThreeNodeGraph shape;
        shape.Declare(graph);

        const Arcane::RgExecuteDesc desc{ *device, viewportLane, nullptr, ring, pipelines,
                                          Arcane::kSwapchainFramesInFlight };
        CHECK_FALSE(graph.Execute(desc, CompileOk(graph)));

        // ENTERED: the lane and the device are latched. NEVER SUCCEEDED: the
        // submit value is still 0, so every burial below keys at 0.
        CHECK(graph.Graves() == &viewportLane);
        CHECK(graph.DebugSubmitCount() == 0);
        CHECK(shape.execCount[0] == 0);

        // Destroys the graph, which buries the realized command buffers, their
        // allocators and the submission fence at 0 -- INTO ITS OWN LANE.
    }

    // THE ASSERTION: a real, nonzero tail, all of it in the viewport's lane,
    // none of it in the sibling's (which is pending at 4096) and none of it in
    // the device's. Under the old shape those same burials went into the shared
    // graveyard at 0 behind the 4096 entry.
    CHECK(viewportLane.Pending() > 0);
    CHECK(siblingLane.Pending() == 1);
    CHECK(device->Graves().Pending() == 0);

    viewportLane.Drain();
    CHECK(viewportLane.Pending() == 0);
    CHECK_FALSE(siblingRan);   // draining one lane cannot run another's thunks

    siblingLane.Drain();
    CHECK(siblingRan);

    // The refused frameSlot is the one latch bump this case makes.
    CHECK(Arcane::RenderErrorCount() == before + 1);
    Arcane::ResetRenderErrorCount();
}

TEST_CASE("imgui-nri: both invalidation variants evict the pointer-keyed entry and RETIRE its "
          "set -- deferred buries the view in the lane, Now destroys it in the call", "[nri]")
{
    // THE ABA CLOSURE (NRI Phase 3, Task 8-pre; NriGraphContext.hpp item (2)).
    //
    // THE HAZARD. ImGuiNri caches per texture by RAW POINTER, because that is
    // what an ImTextureID is on this backend (§7.3). NRI does not ref-count, so
    // when the editor's ResizeOffscreen destroys the viewport output and creates
    // its replacement, NRI is free to hand the replacement the address the
    // destroyed one just vacated -- and a bit-identical ImTextureID then reports
    // a cache HIT on an nri::Descriptor + descriptor set that still name the
    // DEAD resource. A pointer comparison cannot tell you it happened; that is
    // precisely why the header calls for "an explicit invalidation hook, not a
    // heuristic".
    //
    // WHAT IS PINNABLE ON NONE, and it is the whole hook: this case never needs
    // a real texture. Every NONE Create* hands back the same dummy handle and
    // GetTextureDesc ignores its argument (ImplNONE.cpp), so a STAND-IN address
    // exercises the cache exactly as a real one would -- and reusing that one
    // address for "before" and "after" IS the ABA case, not an approximation of
    // it. What NONE cannot show is a real sampler reading real texels; that is
    // desk work, and it is not what this hook is.
    //
    // RenderDrawData is what creates a user entry in production and cannot run
    // here (it allocates from the upload ring first, and NONE's MapBuffer
    // refuses), so the pair is created through EnsureUserTexture -- the same
    // EnsureEntry call, hoisted.
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    // OUR OWN context, saved/restored: Release()'s platform-texture walk
    // destroys every ImTextureData with RefCount == 1, and under Catch2's
    // random ordering it must not be some other case's.
    ImGuiContext* const previous = ImGui::GetCurrentContext();
    ImGuiContext* const context  = ImGui::CreateContext();
    REQUIRE(context != nullptr);

    // BEFORE the backend, so the backend is destroyed first -- the lane must
    // outlive everything that buries into it, the same ordering
    // NriGraphContext's member declarations establish.
    Arcane::Graveyard lane;
    {
        Arcane::NriPipelineCache pipelines;
        pipelines.Bind(*device);

        // Never dereferenced: the bytecode reaches NRI only through
        // GetGraphics, which no headless path here calls. Non-empty is all
        // Init checks.
        const std::uint8_t vsBytes[4] = { 1, 2, 3, 4 };
        const std::uint8_t psBytes[4] = { 5, 6, 7, 8 };

        Arcane::ImGuiNri backend;
        REQUIRE(backend.Init(*device, pipelines, vsBytes, psBytes));

        // "The offscreen output", as an ImTextureID would carry it.
        auto* const output = reinterpret_cast<nri::Texture*>(0x0FF5C1EE);

        // ---- invalidating something never drawn is ROUTINE, not an error ----
        // The caller contract is UNCONDITIONAL invalidation, so a resize before
        // the first frame legitimately lands here. No burial, no latch.
        CHECK_FALSE(backend.HasEntryFor(output));
        CHECK_FALSE(backend.InvalidateUserTexture(output, lane, 0));
        CHECK(lane.Pending() == 0);

        // ---- the entry a draw would have created -------------------------
        REQUIRE(backend.EnsureUserTexture(output));
        CHECK(backend.HasEntryFor(output));
        CHECK(backend.LiveTextureCount() == 1);
        CHECK(backend.RetiredSetCount() == 0);
        // Nothing was buried by CREATING one.
        CHECK(lane.Pending() == 0);

        // ---- the hook ----------------------------------------------------
        // Fence 7 stands in for the chrome graph's DebugSubmitCount() at the
        // moment the viewport resized.
        CHECK(backend.InvalidateUserTexture(output, lane, 7));

        // THE ASSERTION THE WHOLE HOOK EXISTS FOR: the entry is GONE, so the
        // next ImTextureID naming this exact address is a cache MISS rather
        // than a hit on a descriptor over a destroyed texture.
        CHECK_FALSE(backend.HasEntryFor(output));
        CHECK(backend.LiveTextureCount() == 0);

        // ...and it was DISPOSED, not destroyed. The view went into the LANE
        // (an in-flight frame may still be sampling it) --
        CHECK(lane.Pending() == 1);
        // -- and the descriptor set was RETIRED for age-gated recycling, which
        // is the only thing that can be done with one: NRI cannot free a single
        // set, and rewriting it now would rewrite it under a reading GPU.
        CHECK(backend.RetiredSetCount() == 1);

        // The burial is FENCED, not immediate: a reap short of 7 leaves it.
        lane.Reap(6);
        CHECK(lane.Pending() == 1);
        lane.Reap(7);
        CHECK(lane.Pending() == 0);

        // ---- THE ABA CASE ITSELF -----------------------------------------
        // The "replacement" at the SAME address. Post-hook this is a MISS and a
        // fresh view + set are built; pre-hook it was a HIT on the dead one.
        REQUIRE(backend.EnsureUserTexture(output));
        CHECK(backend.HasEntryFor(output));
        CHECK(backend.LiveTextureCount() == 1);
        // The retired set is NOT recycled into it -- no frame has been recorded
        // since the retirement, so the gate (kSwapchainFramesInFlight recorded
        // frames) has not opened and a fresh one was allocated instead.
        CHECK(backend.RetiredSetCount() == 1);

        // ---- THE IMMEDIATE VARIANT (fix round 1) --------------------------
        // THE CROSS-CONTEXT ORDERING PROBLEM the deferred hook cannot solve:
        // the viewport output's TEXTURE dies through the VIEWPORT context's
        // lane -- synchronously, inside ResizeOffscreen, which drains that lane
        // itself -- while a deferred invalidate buries the CHROME backend's
        // view in the CHROME lane, one to two frames out. Two graveyards, no
        // ordering between them, and in practice ordered the wrong way round:
        // DestroyDescriptor after DestroyTexture, every resize.
        //
        // InvalidateUserTextureNow destroys the view INSIDE the call (behind
        // its own DeviceWaitIdle), so a caller that runs it BEFORE the owner's
        // resize gets view-then-texture with no lane to reason about. THE
        // OBSERVABLE, and the thing that would go red if the disposal silently
        // went back to a burial: the entry is gone AND the lane is untouched.
        //
        // Driven on the entry the ABA step above just rebuilt, with the lane
        // provably empty going in.
        REQUIRE(backend.HasEntryFor(output));
        REQUIRE(lane.Pending() == 0);

        CHECK(backend.InvalidateUserTextureNow(output));
        CHECK_FALSE(backend.HasEntryFor(output));
        CHECK(backend.LiveTextureCount() == 0);
        // NOTHING DEFERRED. Not "reaped early" -- never buried: there is no
        // fence in this call at all, which is precisely what lets it run while
        // the texture is still alive and still be safe.
        CHECK(lane.Pending() == 0);
        // The SET still goes through retirement, because retirement is about
        // REUSE rather than lifetime and NRI cannot free one either way. Second
        // eviction, so second retired set.
        CHECK(backend.RetiredSetCount() == 2);

        // A miss stays routine on this variant too, and still buries nothing.
        CHECK_FALSE(backend.InvalidateUserTextureNow(output));
        CHECK(lane.Pending() == 0);

        // ---- teardown, through the same lane ------------------------------
        // The user TEXTURE is never buried by any of this: it is the caller's,
        // which is what makes it a user texture. Only the view, the pool and
        // the sampler are ours.
        backend.Release(lane, 8);
        CHECK(backend.LiveTextureCount() == 0);
        CHECK(lane.Pending() > 0);
        lane.Drain();
        CHECK(lane.Pending() == 0);

        pipelines.Clear(lane, 8);
        lane.Drain();
    }
    CHECK(lane.Pending() == 0);

    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous);

    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("imgui-nri: Release stamps the ADOPTED context's atlas, never whichever one is current",
          "[nri]")
{
    // NRI Phase 3, Task 9 (fix round 1). Release walks
    // ImGui::GetPlatformIO().Textures and, for every RefCount==1 ImTextureData,
    // invalidates its TexID and asks for Destroyed -- the right thing for THE
    // backend of that context (it catches an ImTextureData that was never
    // serviced: a stuck WantCreate, or a WantDestroy that arrived after the
    // last frame).
    //
    // IT USED TO WALK WHATEVER CONTEXT WAS CURRENT. With one backend per
    // process that was right by luck. The editor now holds two -- a chrome
    // backend over the editor context and a game backend over the plugin's --
    // and BOTH are Released from teardowns that pin neither.
    //
    // WHAT THE DAMAGE ACTUALLY IS, because it is not "the texture reads as
    // Destroyed": ImTextureData::SetStatus (imgui.h) BOUNCES a Destroyed
    // request back to WantCreate whenever the CPU-side Pixels are still live,
    // which for another context's live atlas they are. SetTexID has no such
    // guard. So a foreign walk leaves the victim as WANTCREATE WITH AN INVALID
    // TexID -- i.e. ImGui asks the owning backend to create a SECOND texture
    // for an ImTextureData that backend still holds a live cache Entry for,
    // and the id its draw commands were carrying is gone. That is why TexID is
    // the observable below and Status is not: Status is where ImGui's own
    // guard hides the stamp, and a test that asserted on it would go green
    // over a live bug.
    //
    // WHAT MAKES IT PINNABLE ON NONE: the walk and its stamp are pure ImGui
    // bookkeeping. Two contexts, each with its own atlas, each driven through
    // one NewFrame/EndFrame so UpdateTexturesEndFrame publishes that atlas into
    // its own PlatformIO.Textures at RefCount 1 -- exactly the condition the
    // walk tests -- and each given a stand-in TexID, which is what a backend
    // that HAD serviced it would have left behind.
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    ImGuiContext* const previous = ImGui::GetCurrentContext();

    // Publishes this context's atlas into its own platform texture list, and
    // hands it back. RendererHasTextures is what keeps ImGui from locking the
    // atlas into the legacy path; no texture is ever created for it here.
    const auto publishAtlas = [](ImGuiContext* ctx) -> ImTextureData*
    {
        ImGui::SetCurrentContext(ctx);
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1.0f, 1.0f);
        io.DeltaTime   = 1.0f / 60.0f;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        ImGui::NewFrame();
        ImGui::EndFrame();   // UpdateTexturesEndFrame builds PlatformIO.Textures
        REQUIRE(ImGui::GetPlatformIO().Textures.Size >= 1);
        return ImGui::GetPlatformIO().Textures[0];
    };

    ImGuiContext* const adopted = ImGui::CreateContext();
    ImGuiContext* const foreign = ImGui::CreateContext();
    REQUIRE(adopted != nullptr);
    REQUIRE(foreign != nullptr);

    ImTextureData* const adoptedTex = publishAtlas(adopted);
    ImTextureData* const foreignTex = publishAtlas(foreign);
    REQUIRE(adoptedTex != nullptr);
    REQUIRE(foreignTex != nullptr);
    REQUIRE(adoptedTex != foreignTex);

    // The id a backend that HAD serviced these would have left behind. Both,
    // so the two halves of the assertion below are symmetric and the pass
    // cannot come from one of them never having had an id at all.
    const ImTextureID kServiced = (ImTextureID)0xABCDu;
    adoptedTex->SetTexID(kServiced);
    foreignTex->SetTexID(kServiced);
    REQUIRE(adoptedTex->TexID == kServiced);
    REQUIRE(foreignTex->TexID == kServiced);

    Arcane::Graveyard lane;
    {
        Arcane::NriPipelineCache pipelines;
        pipelines.Bind(*device);
        const std::uint8_t vsBytes[4] = { 1, 2, 3, 4 };
        const std::uint8_t psBytes[4] = { 5, 6, 7, 8 };

        Arcane::ImGuiNri backend;
        // ADOPTED BY INIT, which records whatever is current -- the shape both
        // hosts' primary context takes. (AdoptContext is the explicit form, for
        // a context that was NOT current at Init; both land in the same member.)
        ImGui::SetCurrentContext(adopted);
        REQUIRE(backend.Init(*device, pipelines, vsBytes, psBytes));

        // THE SETUP THE BUG NEEDED: a DIFFERENT context is current when the
        // backend is torn down. In the editor that is simply whatever the last
        // frame or the shutdown path left pinned -- nobody pins for a destructor.
        ImGui::SetCurrentContext(foreign);
        backend.Release(lane, 1);

        // THE ASSERTION THE FIX EXISTS FOR, both halves. The adopted context's
        // texture was disowned (that is this backend's job, on the way out);
        // the FOREIGN context's -- another live backend's atlas -- was not
        // touched at all.
        CHECK(adoptedTex->TexID == ImTextureID_Invalid);
        CHECK(foreignTex->TexID == kServiced);
        // ...and the pin is restored, so a teardown cannot silently re-point
        // whatever ran before it.
        CHECK(ImGui::GetCurrentContext() == foreign);

        lane.Drain();
        pipelines.Clear(lane, 1);
        lane.Drain();
    }
    CHECK(lane.Pending() == 0);

    ImGui::DestroyContext(adopted);
    ImGui::DestroyContext(foreign);
    ImGui::SetCurrentContext(previous);

    CHECK(Arcane::RenderErrorCount() == before);
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

    const Arcane::RgExecuteDesc desc{ *device, device->Graves(), nullptr, ring, pipelines, 0 };
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

    const Arcane::RgExecuteDesc desc{ *device, device->Graves(), nullptr, ring, pipelines,
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
    const Arcane::RgExecuteDesc desc{ *device, device->Graves(), nullptr, ring, pipelines, 0 };

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
    const Arcane::RgExecuteDesc desc{ *device, device->Graves(), nullptr, ring, pipelines, 0 };

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

    // RAII safety net for the process-wide active-crash-backend slot. Installs
    // in the ctor; the dtor clears the slot ONLY IF IT STILL HOLDS THIS
    // GUARD'S BACKEND, mirroring ClearActiveGpuCrashBackendIfCurrent's own
    // stale-owner guard (GpuInstrumentation.hpp). Without this, a case that
    // sets the backend and then hits a REQUIRE failure between the set and its
    // manual clear unwinds past the clear entirely -- the slot is left
    // pointing at this case's dead stack, and the NEXT graph-executing case
    // (order is time-seeded) dereferences it and use-after-frees instead of
    // reporting its own, unrelated failure. A case may still clear explicitly
    // on its happy path (as an assertion, not just cleanup); the guard's dtor
    // is then a harmless no-op CAS miss.
    class ScopedGpuCrashBackend
    {
    public:
        explicit ScopedGpuCrashBackend(Arcane::IGpuCrashBackend* backend) noexcept
            : m_backend(backend)
        {
            Arcane::SetActiveGpuCrashBackend(m_backend);
        }
        ~ScopedGpuCrashBackend() { (void)Arcane::ClearActiveGpuCrashBackendIfCurrent(m_backend); }

        ScopedGpuCrashBackend(const ScopedGpuCrashBackend&)            = delete;
        ScopedGpuCrashBackend& operator=(const ScopedGpuCrashBackend&) = delete;

    private:
        Arcane::IGpuCrashBackend* m_backend;
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
    int                     foreignDevice = 0;
    MarkerSpyBackend        spy(&foreignDevice);
    ScopedGpuCrashBackend   backendGuard(&spy);

    {
        Arcane::NriUploadRing    ring;
        Arcane::NriPipelineCache pipelines;
        Arcane::RenderGraph      graph;

        ThreeNodeGraph shape;
        shape.Declare(graph);

        const Arcane::RgCompiled    compiled = CompileOk(graph);
        const Arcane::RgExecuteDesc desc{ *device, device->Graves(), /*swapChain=*/nullptr, ring, pipelines, /*frameSlot=*/0 };
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
// ======================================================================
// THE VEHICLE'S FRAME SHAPE (Phase 2, Tasks 7-8).
//
// These cases DRIVE Arcane::DeclareGraphFrame -- the very function
// NriGraphContext::BuildFrame calls -- with a null context, so every
// declaration, resource and attachment is the real one and only the exec fns
// are inert. Task 7's version of this case TRANSCRIBED BuildFrame's
// declarations instead, which meant a change to the frame could not make it
// red; this can, and Task 8's frame change did.
// ======================================================================

TEST_CASE("nri graph frame: batch -> tonemap derives canvas colour -> shader-read -> present", "[nri]")
{
    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth  = 320;
    shape.canvasHeight = 200;

    const Arcane::RgFrameHandles handles = Arcane::DeclareGraphFrame(graph, shape, nullptr);

    REQUIRE(graph.NodeCount() == 2);
    CHECK(std::string(graph.NodeName(0)) == "batch2d");
    CHECK(std::string(graph.NodeName(1)) == "tonemap");

    // The canvas is a TRANSIENT the graph owns; the backbuffer is the imported
    // swapchain texture. That split is the whole point of the two nodes.
    REQUIRE(graph.IsHandleValid(handles.canvas));
    REQUIRE(graph.IsHandleValid(handles.backbuffer));
    CHECK(graph.IsTransient(handles.canvas));
    CHECK_FALSE(graph.IsTransient(handles.backbuffer));
    CHECK(std::string(graph.NameOf(handles.canvas)) == "canvas");
    CHECK(std::string(graph.NameOf(handles.backbuffer)) == "backbuffer");

    const Arcane::RgCompiled compiled = CompileOk(graph);
    REQUIRE(compiled.nodes.size() == 2);

    // Node 0 (batch2d): the canvas transient's pool slot starts undefined and
    // becomes a colour attachment. On the FIRST frame that `before` is
    // Compile()'s discarding {NONE, UNDEFINED, ALL}.
    REQUIRE(compiled.nodes[0].preBarriers.size() == 1);
    CHECK(compiled.nodes[0].preBarriers[0].isTexture);
    CheckState(compiled.nodes[0].preBarriers[0].before, kUnknownState);
    CheckState(compiled.nodes[0].preBarriers[0].after, kColorState);

    // Node 1 (tonemap): TWO transitions, and neither is hand-written anywhere
    // -- the canvas COLOR_ATTACHMENT -> SHADER_RESOURCE (so the fullscreen
    // triangle can sample it) and the freshly acquired backbuffer's discarding
    // entry -> COLOR_ATTACHMENT.
    REQUIRE(compiled.nodes[1].preBarriers.size() == 2);
    bool sawCanvasRead = false;
    bool sawBackbufferWrite = false;
    for (const Arcane::RgBarrier& barrier : compiled.nodes[1].preBarriers)
    {
        REQUIRE(barrier.isTexture);
        if (static_cast<std::uint32_t>(barrier.after.layout)
            == static_cast<std::uint32_t>(nri::Layout::SHADER_RESOURCE))
        {
            sawCanvasRead = true;
            CheckState(barrier.before, kColorState);
            CheckState(barrier.after, nri::AccessBits::SHADER_RESOURCE,
                       nri::Layout::SHADER_RESOURCE, kShaderReadStages);
        }
        else
        {
            sawBackbufferWrite = true;
            CheckState(barrier.before, kUnknownState);
            CheckState(barrier.after, kColorState);
        }
    }
    CHECK(sawCanvasRead);
    CHECK(sawBackbufferWrite);

    // ...and the graph -- not the caller -- is what leaves the backbuffer
    // present-ready. The canvas is a transient and gets no exit barrier.
    REQUIRE(compiled.exitBarriers.size() == 1);
    CHECK(compiled.exitBarriers[0].isTexture);
    CheckState(compiled.exitBarriers[0].after, kPresentState);

    // Exactly one transient (the canvas), in exactly one pool slot.
    REQUIRE(compiled.transients.size() == 1);
    CHECK(compiled.poolSlotCount == 1);
}

TEST_CASE("nri graph frame: a capture frame copies the backbuffer BEFORE it presents", "[nri]")
{
    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth   = 320;
    shape.canvasHeight  = 200;
    shape.capture       = true;
    // ImportBuffer stores the pointer without dereferencing it, so a headless
    // drive can declare the real capture node with no staging buffer at all.
    shape.captureBuffer = nullptr;
    shape.captureBytes  = 4096;

    const Arcane::RgFrameHandles handles = Arcane::DeclareGraphFrame(graph, shape, nullptr);

    REQUIRE(graph.NodeCount() == 3);
    CHECK(std::string(graph.NodeName(2)) == "capture");
    REQUIRE(graph.IsHandleValid(handles.capture));
    CHECK_FALSE(graph.IsTransient(handles.capture));

    const Arcane::RgCompiled compiled = CompileOk(graph);
    REQUIRE(compiled.nodes.size() == 3);

    // Node 2: the backbuffer becomes a copy SOURCE while it is still the
    // graph's -- the copy is recorded before the present, not after it. The
    // imported staging buffer transitions to COPY_DESTINATION, and a buffer
    // barrier's layout is meaningless by contract (hence UNDEFINED).
    REQUIRE(compiled.nodes[2].preBarriers.size() == 2);
    const Arcane::RgBarrier& textureBarrier = compiled.nodes[2].preBarriers[0].isTexture
                                                ? compiled.nodes[2].preBarriers[0]
                                                : compiled.nodes[2].preBarriers[1];
    const Arcane::RgBarrier& bufferBarrier  = compiled.nodes[2].preBarriers[0].isTexture
                                                ? compiled.nodes[2].preBarriers[1]
                                                : compiled.nodes[2].preBarriers[0];
    CHECK(textureBarrier.isTexture);
    CheckState(textureBarrier.before, kColorState);
    CheckState(textureBarrier.after,
               nri::AccessBits::COPY_SOURCE, nri::Layout::COPY_SOURCE, nri::StageBits::COPY);
    CHECK_FALSE(bufferBarrier.isTexture);
    CheckState(bufferBarrier.after,
               nri::AccessBits::COPY_DESTINATION, nri::Layout::UNDEFINED, nri::StageBits::COPY);

    // Still exactly one exit barrier: the imported staging BUFFER gets none
    // (ImportBuffer takes no exit state to restore it to), and the canvas is a
    // transient.
    REQUIRE(compiled.exitBarriers.size() == 1);
    CHECK(compiled.exitBarriers[0].isTexture);
    CheckState(compiled.exitBarriers[0].after, kPresentState);
}

// ======================================================================
// THE OFFSCREEN FRAME (NRI Phase 3, Task 7).
//
// Same drive as every case above -- Arcane::DeclareGraphFrame with a null
// context -- because the ONE thing that differs between an offscreen frame and
// a presenting one is a declaration: the tonemap imports the vehicle's
// persistent output texture instead of calling ImportSwapChainTexture. Every
// other node, read, write and attachment is byte-for-byte the present frame's,
// which is exactly the property the census case below pins.
//
// RgFrameShape::offscreenOutput is a bare nri::Texture* and ImportTexture
// stores it WITHOUT dereferencing (RenderGraph::ImportTextureInternal), so a
// headless drive can declare the real node with a stand-in pointer -- the same
// licence the capture cases take with shape.captureBuffer.
// ======================================================================
namespace
{
    // A stand-in for the vehicle's persistent output. Never dereferenced: the
    // declaration side only records the pointer, and Compile() reads
    // importEntry/importExit and nothing else.
    nri::Texture* const kFakeOffscreenOutput = reinterpret_cast<nri::Texture*>(0x0FF5C1EE);

    // The state the offscreen output is RESTORED to on the way out -- what
    // makes it samplable by the editor's ImGui pass the moment the frame's
    // submission retires. Restated here independently of AddTonemapNode's own
    // constant, exactly as kPresentState restates the swapchain's.
    constexpr nri::AccessLayoutStage kShaderResourceState{
        nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, kShaderReadStages };

    // True if ANY barrier the compile produced -- pre or exit -- names
    // nri::Layout::PRESENT on either side. An offscreen frame must produce
    // none: nothing it declares is a swapchain texture, so nothing may be left
    // present-ready.
    bool MentionsPresentLayout(const Arcane::RgCompiled& compiled)
    {
        const auto names = [](const Arcane::RgBarrier& barrier)
        {
            return static_cast<std::uint32_t>(barrier.before.layout)
                       == static_cast<std::uint32_t>(nri::Layout::PRESENT)
                || static_cast<std::uint32_t>(barrier.after.layout)
                       == static_cast<std::uint32_t>(nri::Layout::PRESENT);
        };
        for (const Arcane::RgCompiledNode& node : compiled.nodes)
            for (const Arcane::RgBarrier& barrier : node.preBarriers)
                if (names(barrier))
                    return true;
        for (const Arcane::RgBarrier& barrier : compiled.exitBarriers)
            if (names(barrier))
                return true;
        return false;
    }
}

TEST_CASE("nri graph frame: an OFFSCREEN frame tonemaps into the imported output and never presents",
          "[nri]")
{
    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth     = 320;
    shape.canvasHeight    = 200;
    shape.offscreenOutput = kFakeOffscreenOutput;

    const Arcane::RgFrameHandles handles = Arcane::DeclareGraphFrame(graph, shape, nullptr);

    // Node for node the presenting frame's -- only what the tonemap writes
    // changed.
    REQUIRE(graph.NodeCount() == 2);
    CHECK(std::string(graph.NodeName(0)) == "batch2d");
    CHECK(std::string(graph.NodeName(1)) == "tonemap");

    // The output is IMPORTED (the vehicle owns it and it survives every frame),
    // not a transient the graph would recycle through its pool -- and it is
    // named for what it is, so a barrier dump reads honestly.
    REQUIRE(graph.IsHandleValid(handles.backbuffer));
    CHECK_FALSE(graph.IsTransient(handles.backbuffer));
    CHECK(std::string(graph.NameOf(handles.backbuffer)) == "offscreen");

    const Arcane::RgCompiled compiled = CompileOk(graph);
    REQUIRE(compiled.nodes.size() == 2);

    // Node 1 (tonemap): the SAME two transitions the presenting frame derives
    // -- the canvas COLOR_ATTACHMENT -> SHADER_RESOURCE, and the output's
    // contents-discarding entry -> COLOR_ATTACHMENT. The discarding entry is
    // what makes frame 1 (a freshly created texture, genuinely undefined) and
    // frame N (left in SHADER_RESOURCE by the previous frame's exit) one case
    // rather than two: the tonemap's opaque fullscreen triangle writes every
    // pixel, so nothing is lost by discarding, and {NONE, UNDEFINED, ALL} is
    // the one entry that is D3D12-enhanced-barrier legal unconditionally.
    REQUIRE(compiled.nodes[1].preBarriers.size() == 2);
    bool sawCanvasRead = false;
    bool sawOutputWrite = false;
    for (const Arcane::RgBarrier& barrier : compiled.nodes[1].preBarriers)
    {
        REQUIRE(barrier.isTexture);
        if (static_cast<std::uint32_t>(barrier.after.layout)
            == static_cast<std::uint32_t>(nri::Layout::SHADER_RESOURCE))
        {
            sawCanvasRead = true;
            CheckState(barrier.before, kColorState);
        }
        else
        {
            sawOutputWrite = true;
            CheckState(barrier.before, kUnknownState);
            CheckState(barrier.after, kColorState);
        }
    }
    CHECK(sawCanvasRead);
    CHECK(sawOutputWrite);

    // THE WHOLE POINT: the frame ends by restoring the output to
    // SHADER_RESOURCE, so whoever samples it next (the editor's ImGui pass)
    // finds it in a state a sampler can read -- and NOTHING anywhere in the
    // frame mentions PRESENT, because there is no swapchain in it.
    REQUIRE(compiled.exitBarriers.size() == 1);
    CHECK(compiled.exitBarriers[0].isTexture);
    CheckState(compiled.exitBarriers[0].before, kColorState);
    CheckState(compiled.exitBarriers[0].after, kShaderResourceState);
    CHECK_FALSE(MentionsPresentLayout(compiled));

    // Exactly one transient (the canvas) in one pool slot -- the output is
    // imported and is not a pool tenant.
    REQUIRE(compiled.transients.size() == 1);
    CHECK(compiled.poolSlotCount == 1);
}

TEST_CASE("nri graph frame: the offscreen frame's node census is the presenting frame's, node for node",
          "[nri]")
{
    // The property that makes Tasks 8-13 cheap: offscreen is not a second
    // frame shape, it is the same shape with a different final target. Driven
    // at the BUSIEST shape -- post chain + pick/outline + HUD + capture -- so a
    // future node added under a `shape.offscreenOutput` branch (rather than
    // unconditionally) makes this red.
    const auto declare = [](Arcane::RenderGraph& graph, const Arcane::PostChainDesc& post,
                            nri::Texture* output)
    {
        Arcane::RgFrameShape shape;
        shape.canvasWidth     = 320;
        shape.canvasHeight    = 200;
        shape.stage           = Arcane::GoldenStage::Full;
        shape.post            = &post;
        shape.pickOutline     = true;
        shape.imgui           = true;
        shape.capture         = true;
        shape.captureBuffer   = nullptr;
        shape.captureBytes    = 4096;
        shape.offscreenOutput = output;
        return Arcane::DeclareGraphFrame(graph, shape, nullptr);
    };

    // A two-pass LINEAR chain, built inline rather than through this file's
    // MakeLinearChain helper -- that one is declared with the post-chain family
    // further down, and this case reads better beside its offscreen siblings
    // than it would three hundred lines away from them.
    Arcane::PostChainDesc chain;
    chain.chainInputSlots = 1;
    {
        Arcane::PostChainPassDesc base;
        base.inputs.push_back(Arcane::kSceneInput);
        chain.passes.push_back(std::move(base));
        Arcane::PostChainPassDesc second;
        second.inputs.push_back(0u);
        chain.passes.push_back(std::move(second));
    }

    Arcane::RenderGraph presenting;
    declare(presenting, chain, /*output=*/nullptr);
    const Arcane::RgCompiled presentingCompiled = CompileOk(presenting);

    Arcane::RenderGraph offscreen;
    declare(offscreen, chain, kFakeOffscreenOutput);
    const Arcane::RgCompiled offscreenCompiled = CompileOk(offscreen);

    REQUIRE(offscreen.NodeCount() == presenting.NodeCount());
    for (std::size_t i = 0; i < presenting.NodeCount(); ++i)
    {
        INFO("node " << i);
        CHECK(std::string(offscreen.NodeName(i)) == std::string(presenting.NodeName(i)));
        REQUIRE(offscreenCompiled.nodes[i].preBarriers.size()
                == presentingCompiled.nodes[i].preBarriers.size());
    }

    // Same pool demand too: the final target is imported in BOTH shapes (a
    // swapchain backbuffer is an import as well), so neither becomes a
    // transient and the allocator sees the identical set.
    CHECK(offscreenCompiled.transients.size() == presentingCompiled.transients.size());
    CHECK(offscreenCompiled.poolSlotCount == presentingCompiled.poolSlotCount);

    // ...and the ONE difference is the exit state, which is the whole feature.
    REQUIRE(presentingCompiled.exitBarriers.size() == 1);
    CheckState(presentingCompiled.exitBarriers[0].after, kPresentState);
    REQUIRE(offscreenCompiled.exitBarriers.size() == 1);
    CheckState(offscreenCompiled.exitBarriers[0].after, kShaderResourceState);
    CHECK_FALSE(MentionsPresentLayout(offscreenCompiled));
}

TEST_CASE("nri graph frame: an offscreen capture frame copies the output BEFORE restoring it to "
          "SHADER_RESOURCE", "[nri]")
{
    // Task 13's harness reads its goldens back off the offscreen target, so the
    // capture node has to compose with the import the same way it composes with
    // the swapchain: copy while the frame still owns the texture, then let the
    // graph restore the exit state.
    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth     = 320;
    shape.canvasHeight    = 200;
    shape.capture         = true;
    shape.captureBuffer   = nullptr;
    shape.captureBytes    = 4096;
    shape.offscreenOutput = kFakeOffscreenOutput;

    const Arcane::RgFrameHandles handles = Arcane::DeclareGraphFrame(graph, shape, nullptr);

    REQUIRE(graph.NodeCount() == 3);
    CHECK(std::string(graph.NodeName(2)) == "capture");
    REQUIRE(graph.IsHandleValid(handles.capture));

    const Arcane::RgCompiled compiled = CompileOk(graph);
    REQUIRE(compiled.nodes.size() == 3);
    REQUIRE(compiled.nodes[2].preBarriers.size() == 2);

    const Arcane::RgBarrier& textureBarrier = compiled.nodes[2].preBarriers[0].isTexture
                                                ? compiled.nodes[2].preBarriers[0]
                                                : compiled.nodes[2].preBarriers[1];
    CheckState(textureBarrier.before, kColorState);
    CheckState(textureBarrier.after,
               nri::AccessBits::COPY_SOURCE, nri::Layout::COPY_SOURCE, nri::StageBits::COPY);

    // The exit barrier now runs from COPY_SOURCE, not COLOR_ATTACHMENT -- the
    // graph restores whatever the frame's LAST access left, which is the
    // property that keeps the sampled texture correct however the frame grows.
    REQUIRE(compiled.exitBarriers.size() == 1);
    CheckState(compiled.exitBarriers[0].before,
               nri::AccessBits::COPY_SOURCE, nri::Layout::COPY_SOURCE, nri::StageBits::COPY);
    CheckState(compiled.exitBarriers[0].after, kShaderResourceState);
    CHECK_FALSE(MentionsPresentLayout(compiled));
}

TEST_CASE("nri graph frame: a zero-extent canvas COMPILES but is a latched refusal at Execute",
          "[nri]")
{
    // Fix round 1, finding 1 -- the two halves of why NriGraphContext::
    // RenderFrame skips a zero-sized surface BEFORE it declares anything.
    // RenderFrame itself cannot be exercised headlessly (it needs a real window
    // and swapchain), so what is pinned here is the mechanism it guards
    // against; if the guard were removed, a minimised window would exit the
    // run nonzero instead of skipping a frame.
    const std::uint64_t before = Arcane::RenderErrorCount();

    // HALF ONE: the declaration side cannot catch it. A 0x0 canvas is a
    // perfectly well-formed set of declarations -- Compile is pure and derives
    // barriers, not extents -- so nothing refuses the frame here.
    {
        Arcane::RenderGraph graph;
        Arcane::RgFrameShape shape;
        shape.canvasWidth  = 0;
        shape.canvasHeight = 0;
        Arcane::DeclareGraphFrame(graph, shape, nullptr);

        const Arcane::RgCompiled compiled = CompileOk(graph);
        CHECK(compiled.transients.size() == 1);
        CHECK(compiled.poolSlotCount == 1);
    }
    CHECK(Arcane::RenderErrorCount() == before);

    // HALF TWO: Execute REFUSES it, and the refusal is LATCHED -- which on the
    // vehicle means FrameOutcome::Failed and a nonzero exit, not a routine
    // skip. It also happens during pool realization, i.e. BEFORE the acquire
    // that owns the real zero-sized-surface skip, so that skip can never be
    // reached with a zero-extent transient declared.
    {
        auto device = Arcane::NriDevice::CreateNoneForTests();
        REQUIRE(device != nullptr);

        Arcane::NriUploadRing    ring;
        Arcane::NriPipelineCache pipelines;
        Arcane::RenderGraph      graph;
        const Arcane::RgExecuteDesc desc{ *device, device->Graves(), nullptr, ring, pipelines, 0 };

        Arcane::RgTexture canvas;
        graph.AddNode("batch2d", Arcane::RenderGraph::NodeKind::Raster,
            [&](Arcane::RenderGraphBuilder& builder)
            {
                Arcane::RgTextureDesc textureDesc;
                textureDesc.format = nri::Format::RGBA16_SFLOAT;
                textureDesc.width  = 0;
                textureDesc.height = 0;
                canvas = builder.CreateTexture("canvas", textureDesc);
                builder.Write(canvas, Arcane::RgUsage::ColorWrite);
                graph.SetColorAttachments(std::span<const Arcane::RgTexture>(&canvas, 1));
            },
            [](Arcane::RenderGraphNodeContext&) {});

        const std::uint64_t beforeExecute = Arcane::RenderErrorCount();
        CHECK_FALSE(graph.Execute(desc, CompileOk(graph)));
        CHECK(Arcane::RenderErrorCount() > beforeExecute);

        graph.ReleaseGpuResources();
        device->Graves().Drain();
    }

    // This case deliberately GREW the latch; clear it so no other case
    // inherits the refusal -- the same restore the neighbouring exec cases do.
    Arcane::ResetRenderErrorCount();
}

TEST_CASE("nri graph frame: the canvas transient is swapchain-sized RGBA16F and reused across frames",
          "[nri]")
{
    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth  = 1280;
    shape.canvasHeight = 720;

    Arcane::DeclareGraphFrame(graph, shape, nullptr);
    const Arcane::RgCompiled first = CompileOk(graph);
    REQUIRE(first.transients.size() == 1);
    CHECK(first.poolSlotCount == 1);

    // Re-declaring the SAME shape after a Reset must compile to the same pool
    // demand -- that is what makes the steady-state frame loop allocate
    // nothing (RenderGraph::Reset's contract). Handles from the previous
    // generation are dead, which is exactly why DeclareGraphFrame hands fresh
    // ones back every frame.
    graph.Reset();
    Arcane::DeclareGraphFrame(graph, shape, nullptr);
    const Arcane::RgCompiled second = CompileOk(graph);
    CHECK(second.transients.size() == first.transients.size());
    CHECK(second.poolSlotCount == first.poolSlotCount);
    CHECK(second.nodes.size() == first.nodes.size());
}

// ======================================================================
// THE POST CHAIN IN THE FRAME (Phase 2, Task 10).
//
// Same drive as the cases above -- Arcane::DeclareGraphFrame with a null
// context -- so the nodes, the reads/writes and the derived barrier chain are
// the REAL ones. What makes that possible headlessly is that the post nodes'
// DECLARATIONS depend on the chain's per-pass WIRING and on nothing else: the
// bytecode, the template and the values are PostChainNode::PrepareChain's
// business, and PrepareChain only runs when there is a context (a device).
// ======================================================================
namespace
{
    // A chain expressed as wiring only, in the shape PostChainCache produces
    // it: passes[p].inputs are chain indices, kSceneInput meaning the external
    // scene colour, and chainInputSlots is the max input count anywhere (min
    // 1) -- MaterialSource.cpp's own rule, restated so a change to it has to
    // be made deliberately in both places.
    Arcane::PostChainDesc MakeChain(std::initializer_list<std::vector<std::uint32_t>> wiring)
    {
        Arcane::PostChainDesc desc;
        std::uint32_t slots = 1;
        for (const std::vector<std::uint32_t>& inputs : wiring)
        {
            Arcane::PostChainPassDesc pass;
            pass.inputs = inputs;
            slots = (std::max)(slots, (std::uint32_t)inputs.size());
            desc.passes.push_back(std::move(pass));
        }
        desc.chainInputSlots = slots;
        return desc;
    }

    // The LINEAR chain every real post material produces: pass 0 reads the
    // scene (BuildMaterialChainSource refuses anything else for the base) and
    // pass k reads pass k-1. ReferenceProject's is this with N == 2.
    Arcane::PostChainDesc MakeLinearChain(std::uint32_t passCount)
    {
        Arcane::PostChainDesc desc;
        desc.chainInputSlots = 1;
        for (std::uint32_t p = 0; p < passCount; ++p)
        {
            Arcane::PostChainPassDesc pass;
            pass.inputs.push_back(p == 0 ? Arcane::kSceneInput : p - 1);
            desc.passes.push_back(std::move(pass));
        }
        return desc;
    }

    // The one barrier at `node` that transitions something INTO a colour
    // attachment -- i.e. the pass's own target. Every post node has exactly
    // two: this, and the read of what it samples.
    const Arcane::RgBarrier& ColorWriteBarrier(const Arcane::RgCompiledNode& node)
    {
        for (const Arcane::RgBarrier& barrier : node.preBarriers)
            if (barrier.isTexture &&
                static_cast<std::uint32_t>(barrier.after.layout)
                    == static_cast<std::uint32_t>(nri::Layout::COLOR_ATTACHMENT))
                return barrier;
        FAIL("node declares no transition into COLOR_ATTACHMENT");
        return node.preBarriers.front();
    }
}

TEST_CASE("nri graph frame: the post chain inserts one node per pass between the batch and the "
          "tonemap, and the tonemap samples the LAST pass", "[nri]")
{
    const Arcane::PostChainDesc chain = MakeLinearChain(2);   // ReferenceProject's shape

    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth  = 320;
    shape.canvasHeight = 200;
    shape.post         = &chain;

    const Arcane::RgFrameHandles handles = Arcane::DeclareGraphFrame(graph, shape, nullptr);

    REQUIRE(graph.NodeCount() == 4);
    CHECK(std::string(graph.NodeName(0)) == "batch2d");
    CHECK(std::string(graph.NodeName(1)) == "post0");
    CHECK(std::string(graph.NodeName(2)) == "post1");
    CHECK(std::string(graph.NodeName(3)) == "tonemap");

    CHECK(handles.postPassCount == 2);
    REQUIRE(graph.IsHandleValid(handles.post));
    CHECK(graph.IsTransient(handles.post));
    CHECK(std::string(graph.NameOf(handles.post)) == "post1");

    const Arcane::RgCompiled compiled = CompileOk(graph);
    REQUIRE(compiled.nodes.size() == 4);

    // Node 1 (post0): reads the CANVAS the batch node just wrote, writes its
    // own target. NEITHER transition is hand-written anywhere -- the node
    // declares Read(scene)/Write(target) and the executor derives both.
    REQUIRE(compiled.nodes[1].preBarriers.size() == 2);
    bool sawCanvasRead = false;
    for (const Arcane::RgBarrier& barrier : compiled.nodes[1].preBarriers)
    {
        REQUIRE(barrier.isTexture);
        if (static_cast<std::uint32_t>(barrier.after.layout)
            == static_cast<std::uint32_t>(nri::Layout::SHADER_RESOURCE))
        {
            sawCanvasRead = true;
            CheckState(barrier.before, kColorState);
            CheckState(barrier.after, nri::AccessBits::SHADER_RESOURCE,
                       nri::Layout::SHADER_RESOURCE, kShaderReadStages);
        }
    }
    CHECK(sawCanvasRead);
    // Pass 0's own target is a fresh pool slot: the discarding entry.
    CheckState(ColorWriteBarrier(compiled.nodes[1]).before, kUnknownState);
    CheckState(ColorWriteBarrier(compiled.nodes[1]).after, kColorState);

    // Node 2 (post1): reads pass 0's target -- and writes a target whose pool
    // slot the CANVAS just vacated, which is the ping-pong showing up as a
    // handover rather than as an allocation. `before` therefore carries the
    // canvas's OUTGOING state in full -- SHADER_RESOURCE access, layout AND
    // stages -- so the reused physical texture gets its availability
    // operation. (The layout carries rather than staying UNDEFINED because
    // {UNDEFINED, non-NONE access} is an illegal D3D12 enhanced barrier that
    // fails Close(); losing the discard hint costs nothing here, since the
    // pass fully overwrites its target.)
    REQUIRE(compiled.nodes[2].preBarriers.size() == 2);
    CheckState(ColorWriteBarrier(compiled.nodes[2]).before,
               nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, kShaderReadStages);
    CheckState(ColorWriteBarrier(compiled.nodes[2]).after, kColorState);

    // Node 3 (tonemap): samples pass 1's target (NOT the canvas) and writes
    // the freshly acquired backbuffer.
    REQUIRE(compiled.nodes[3].preBarriers.size() == 2);
    CheckState(ColorWriteBarrier(compiled.nodes[3]).before, kUnknownState);   // the backbuffer
    CheckState(ColorWriteBarrier(compiled.nodes[3]).after, kColorState);

    // ...and the graph still leaves the backbuffer present-ready, alone.
    REQUIRE(compiled.exitBarriers.size() == 1);
    CheckState(compiled.exitBarriers[0].after, kPresentState);

    // THE TRANSIENT-POOL EXERCISE, stated as a number: THREE declared
    // transients (canvas + two pass targets) sharing TWO physical textures.
    // Enumeration order is texture-slot order (RgTransient's contract), i.e.
    // canvas, post0, post1 -- so the alternation is 0, 1, 0.
    REQUIRE(compiled.transients.size() == 3);
    CHECK(compiled.poolSlotCount == 2);
    REQUIRE(compiled.transientPoolSlot.size() == 3);
    CHECK(compiled.transientPoolSlot[0] == compiled.transientPoolSlot[2]);
    CHECK(compiled.transientPoolSlot[0] != compiled.transientPoolSlot[1]);
}

TEST_CASE("nri graph frame: --golden-stage batch drops the post chain; post and full keep it",
          "[nri]")
{
    // The stage vocabulary is the ONE thing that makes a node-by-node cutover
    // comparable: a batch-stage golden must contain the batcher and the
    // tonemap and nothing else, on BOTH recorders. RuntimeApp applies exactly
    // this bypass to the NVRHI path (`stageSkipsPost`), so if this gate broke,
    // a batch golden would compare a graded frame against an ungraded one.
    const Arcane::PostChainDesc chain = MakeLinearChain(2);

    const struct { Arcane::GoldenStage stage; std::size_t nodes; std::uint32_t passes; } cases[] = {
        { Arcane::GoldenStage::Batch, 2, 0 },
        { Arcane::GoldenStage::Post,  4, 2 },
        { Arcane::GoldenStage::Full,  4, 2 },
    };

    for (const auto& expected : cases)
    {
        Arcane::RenderGraph graph;
        Arcane::RgFrameShape shape;
        shape.canvasWidth  = 320;
        shape.canvasHeight = 200;
        shape.post         = &chain;
        shape.stage        = expected.stage;

        const Arcane::RgFrameHandles handles = Arcane::DeclareGraphFrame(graph, shape, nullptr);
        CHECK(graph.NodeCount() == expected.nodes);
        CHECK(handles.postPassCount == expected.passes);
        CHECK(graph.IsHandleValid(handles.post) == (expected.passes > 0));
        // The last node is the tonemap either way -- the chain is inserted
        // BEFORE it, never appended after it.
        CHECK(std::string(graph.NodeName(graph.NodeCount() - 1)) == "tonemap");
    }

    // ...and a frame with no chain at all is byte-for-byte Task 8's, whatever
    // the stage: nothing above may make the no-post path grow a node.
    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth  = 320;
    shape.canvasHeight = 200;
    const Arcane::RgFrameHandles handles = Arcane::DeclareGraphFrame(graph, shape, nullptr);
    CHECK(graph.NodeCount() == 2);
    CHECK(handles.postPassCount == 0);
    CHECK_FALSE(graph.IsHandleValid(handles.post));
}

TEST_CASE("nri graph frame: a four-pass post chain runs THREE tenants through one pool slot, and "
          "each inherits the previous one's outgoing state", "[nri]")
{
    // The cross-tenant handover is generic in RenderGraph::Compile (its POOL
    // HANDOVER block tracks state per pool SLOT, not per tenant), but until
    // this task nothing in the tree declared a frame with more than two
    // tenants on one slot -- so the X -> Y -> Z case was unpinned. A four-pass
    // chain is the smallest frame that produces it.
    const Arcane::PostChainDesc chain = MakeLinearChain(4);

    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth  = 320;
    shape.canvasHeight = 200;
    shape.post         = &chain;

    const Arcane::RgFrameHandles handles = Arcane::DeclareGraphFrame(graph, shape, nullptr);
    REQUIRE(graph.NodeCount() == 6);   // batch2d, post0..post3, tonemap
    CHECK(handles.postPassCount == 4);

    const Arcane::RgCompiled compiled = CompileOk(graph);

    // FIVE transients (canvas + four targets) on TWO physical textures, still
    // -- the alternation does not degrade as the chain grows.
    REQUIRE(compiled.transients.size() == 5);
    CHECK(compiled.poolSlotCount == 2);
    REQUIRE(compiled.transientPoolSlot.size() == 5);
    // Texture-slot enumeration order: canvas, post0, post1, post2, post3.
    const std::uint32_t even = compiled.transientPoolSlot[0];
    const std::uint32_t odd  = compiled.transientPoolSlot[1];
    CHECK(even != odd);
    CHECK(compiled.transientPoolSlot[2] == even);   // canvas -> post1  (tenant 2)
    CHECK(compiled.transientPoolSlot[3] == odd);
    CHECK(compiled.transientPoolSlot[4] == even);   // post1  -> post3  (tenant 3)

    // THE PROPERTY. Pass 1 is `even`'s second tenant and pass 3 is its THIRD;
    // both must see the previous tenant's outgoing SHADER_RESOURCE state --
    // access, LAYOUT and stages (the D2 dx12 fix carries the layout instead of
    // forcing UNDEFINED). If Compile seeded a later tenant from kUnknownState
    // instead, the third would come back with access NONE and perform no
    // availability operation for the second tenant's writes -- the
    // write-after-write hazard the handover exists to close, one tenant
    // further along than anything tested before. ColorWriteBarrier() FAILs
    // when a node declares no transition into COLOR_ATTACHMENT, so this loop
    // is also the "every handover emits a barrier" assertion for a 3-tenant
    // chain.
    for (const std::size_t node : { std::size_t(2), std::size_t(3), std::size_t(4) })
    {
        INFO("post pass node index " << node);
        CheckState(ColorWriteBarrier(compiled.nodes[node]).before,
                   nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, kShaderReadStages);
    }
    // ...while the FIRST tenant of each slot still starts from the discarding
    // entry, which is what makes the three assertions above meaningful.
    CheckState(ColorWriteBarrier(compiled.nodes[1]).before, kUnknownState);
}

TEST_CASE("nri graph frame: a post chain whose base reads the scene keeps the canvas alive past "
          "pass 0", "[nri]")
{
    // A DAG, not a pipe: MaterialSource lets any pass read any EARLIER pass,
    // and (in post mode) any pass read the scene. The declarator wires exactly
    // what the chain says, so the pool allocator -- not a hand-rolled
    // ping-pong -- is what decides the physical layout. Here pass 1 reads the
    // SCENE as well as pass 0, which extends the canvas's lifetime and takes
    // its slot out of the alternation entirely.
    const Arcane::PostChainDesc chain =
        MakeChain({ { Arcane::kSceneInput }, { 0u, Arcane::kSceneInput } });
    CHECK(chain.chainInputSlots == 2);

    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth  = 320;
    shape.canvasHeight = 200;
    shape.post         = &chain;

    const Arcane::RgFrameHandles handles = Arcane::DeclareGraphFrame(graph, shape, nullptr);
    REQUIRE(graph.NodeCount() == 4);

    const Arcane::RgCompiled compiled = CompileOk(graph);
    // THREE pool slots now, not two: the canvas is still live at node 2, so
    // pass 1's target cannot take its slot. That is the pool allocator being
    // driven by real lifetimes rather than by an assumption about chains.
    REQUIRE(compiled.transients.size() == 3);
    CHECK(compiled.poolSlotCount == 3);

    // AND THE POINT THAT COST A REWRITE: the PASS COUNT IS NOT A PROXY for the
    // frame's physical layout. This chain and MakeLinearChain(2) have the same
    // node count and the same postPassCount, and compile to a DIFFERENT number
    // of pool slots -- so a re-save between the two shapes changes which
    // physical texture the tonemap ends up sampling while every count a caller
    // could compare stays put. Anything that keys "did my source change?" on
    // the pass count misses it; the tonemap's per-frame-slot sets and
    // RenderGraph::PoolEpoch do not.
    CHECK(handles.postPassCount == 2);
    {
        const Arcane::PostChainDesc linear = MakeLinearChain(2);
        Arcane::RenderGraph other;
        Arcane::RgFrameShape otherShape = shape;
        otherShape.post = &linear;
        const Arcane::RgFrameHandles otherHandles =
            Arcane::DeclareGraphFrame(other, otherShape, nullptr);
        CHECK(otherHandles.postPassCount == handles.postPassCount);
        CHECK(other.NodeCount() == graph.NodeCount());
        CHECK(CompileOk(other).poolSlotCount != compiled.poolSlotCount);
    }
    // Pass 1 declares TWO reads (pass 0's target and the canvas) plus its own
    // write -- but the canvas is ALREADY in SHADER_RESOURCE from node 1, so no
    // barrier is emitted for it a second time.
    REQUIRE(compiled.nodes[2].preBarriers.size() == 2);
    CheckState(ColorWriteBarrier(compiled.nodes[2]).before, kUnknownState);
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
    // the graph's own last submitted value here rather than 0 (a fixed
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

// =========================================================================
// Task 9: REGISTERED SPRITE MATERIALS in Batch2DNode.
//
// What can be pinned headlessly and what cannot. The material path's GPU half
// -- the constant-buffer arena's mapping, the descriptor-set writes, the PSOs
// and the draws -- is unreachable here for the reason NriUploadRing's header
// states: ImplNONE's MapBuffer returns null unconditionally, so
// Batch2DNode::Create cannot even finish on a NONE device. What IS reachable,
// and is exactly where a silent wrong-descriptor bug would live, is the pure
// half: the pipeline-layout SHAPE (which is also where NRI's register-space
// refusal would bite) and the arena's offset arithmetic.
// =========================================================================

TEST_CASE("nri batch2d material layout: root CONSTANTS only, so the root space and the "
          "texture set legally share space 0", "[nri]")
{
    Arcane::SpriteMaterialLayout layout;
    layout.Build(/*cbSize=*/32, /*textureCount=*/1);

    // THE RULE, pinned. NRI refuses a layout whose rootRegisterSpace equals a
    // descriptor set's registerSpace -- but ONLY when the layout also carries
    // root DESCRIPTORS or root SAMPLERS (Source/Validation/DeviceVal.hpp).
    // sprite_material.hlsl puts every register in the implicit space0 and the
    // NVRHI path is the format-compatibility floor, so BOTH spaces must be 0
    // and these two counts must therefore stay zero. Adding a root descriptor
    // for b1/b2 -- the shape the plan originally sketched -- makes NRI reject
    // the layout at creation, on a device this gate does not have.
    REQUIRE(layout.desc.rootDescriptorNum == 0);
    REQUIRE(layout.desc.rootSamplerNum == 0);
    CHECK(layout.desc.rootRegisterSpace == 0);
    REQUIRE(layout.desc.descriptorSetNum == 1);
    CHECK(layout.set.registerSpace == 0);

    // b0 push constants: the same 16-byte BatchConstants block every 2D
    // pipeline reads, vertex-stage only (no ps_main reads it).
    REQUIRE(layout.desc.rootConstantNum == 1);
    CHECK(layout.rootConstant.registerIndex == 0);
    CHECK(layout.rootConstant.size == 16);
    CHECK(layout.rootConstant.shaderStages == nri::StageBits::VERTEX_SHADER);

    // The register map, and the ORDER that lets NRI's D3D12 backend merge the
    // two CBVs into one root table: CBV b1, CBV b2, SRV t0..tN, SAMPLER s0.
    REQUIRE(layout.set.rangeNum == 4);
    REQUIRE(layout.materialCb != Arcane::SpriteMaterialLayout::kNoRange);
    CHECK(layout.materialCb == 0);
    CHECK(layout.globalsCb  == 1);
    CHECK(layout.textures   == 2);
    CHECK(layout.sampler    == 3);

    CHECK(layout.ranges[layout.materialCb].descriptorType == nri::DescriptorType::CONSTANT_BUFFER);
    CHECK(layout.ranges[layout.materialCb].baseRegisterIndex == 1);   // b1
    CHECK(layout.ranges[layout.materialCb].descriptorNum == 1);
    CHECK(layout.ranges[layout.globalsCb].descriptorType == nri::DescriptorType::CONSTANT_BUFFER);
    CHECK(layout.ranges[layout.globalsCb].baseRegisterIndex == 2);    // b2
    CHECK(layout.ranges[layout.globalsCb].descriptorNum == 1);
    // ONE contiguous SRV range: the sprite's own t0 plus the declared t1..N.
    CHECK(layout.ranges[layout.textures].descriptorType == nri::DescriptorType::TEXTURE);
    CHECK(layout.ranges[layout.textures].baseRegisterIndex == 0);
    CHECK(layout.ranges[layout.textures].descriptorNum == 2);
    CHECK(layout.ranges[layout.sampler].descriptorType == nri::DescriptorType::SAMPLER);
    CHECK(layout.ranges[layout.sampler].baseRegisterIndex == 0);      // s0
    CHECK(layout.ranges[layout.sampler].descriptorNum == 1);

    // NRI validates that every range's stages are a SUBSET of the layout's
    // (DeviceVal.hpp) -- and every range here is both stages, because a
    // template's VERTEX_BODY may sample its textures and read its params.
    for (std::uint32_t i = 0; i < layout.set.rangeNum; ++i)
    {
        const auto stages = (std::uint32_t)layout.ranges[i].shaderStages;
        const auto all    = (std::uint32_t)layout.desc.shaderStages;
        CHECK((stages & all) == stages);
        CHECK((stages & (std::uint32_t)nri::StageBits::VERTEX_SHADER) != 0u);
        CHECK((stages & (std::uint32_t)nri::StageBits::FRAGMENT_SHADER) != 0u);
    }

    // The desc points at THIS object's arrays -- the reason it is not copyable.
    CHECK(layout.desc.descriptorSets == &layout.set);
    CHECK(layout.desc.rootConstants == &layout.rootConstant);
    CHECK(layout.set.ranges == layout.ranges);
}

TEST_CASE("nri batch2d material layout: a template with no numeric params declares no b1 range "
          "at all", "[nri]")
{
    // The stitcher omits the whole material cbuffer block when the snippet
    // declares no numeric params (MaterialSource.cpp), and Batcher2D's own
    // binding layout omits the matching item -- so a b1 range here would name a
    // register the shader never declared.
    Arcane::SpriteMaterialLayout layout;
    layout.Build(/*cbSize=*/0, /*textureCount=*/0);

    CHECK(layout.materialCb == Arcane::SpriteMaterialLayout::kNoRange);
    REQUIRE(layout.set.rangeNum == 3);
    CHECK(layout.globalsCb == 0);
    CHECK(layout.textures  == 1);
    CHECK(layout.sampler   == 2);
    CHECK(layout.ranges[layout.globalsCb].baseRegisterIndex == 2);   // b2, still
    // t0 alone: the sprite's own texture is always bound, declared params or not.
    CHECK(layout.ranges[layout.textures].descriptorNum == 1);
    // ...and the space rule still holds for the degenerate shape.
    CHECK(layout.desc.rootDescriptorNum == 0);
    CHECK(layout.desc.rootSamplerNum == 0);
}

TEST_CASE("nri batch2d material layout: materials of the same SHAPE share one registered layout, "
          "different shapes do not", "[nri]")
{
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    Arcane::NriPipelineCache cache;
    cache.Bind(*device);

    // NriPipelineCache::RegisterLayout dedups BYTE WISE, which is why
    // SpriteMaterialLayout::Build value-initializes every desc before assigning
    // it field by field. Two materials of the same (has-CB, textureCount) shape
    // must therefore collapse onto ONE layout -- otherwise every material in a
    // scene creates its own pipeline layout for an identical binding map.
    Arcane::SpriteMaterialLayout a, b, wider, noCb;
    a.Build(32, 1);
    b.Build(16, 1);        // a different cbSize is the SAME layout: the CB VIEW carries the size
    wider.Build(32, 2);
    noCb.Build(0, 1);

    const std::uint32_t idA = cache.RegisterLayout(a.desc);
    REQUIRE(idA != Arcane::NriPipelineCache::kInvalidLayout);
    CHECK(cache.LayoutCount() == 1);

    CHECK(cache.RegisterLayout(b.desc) == idA);
    CHECK(cache.LayoutCount() == 1);

    const std::uint32_t idWider = cache.RegisterLayout(wider.desc);
    CHECK(idWider != idA);
    CHECK(cache.LayoutCount() == 2);

    const std::uint32_t idNoCb = cache.RegisterLayout(noCb.desc);
    CHECK(idNoCb != idA);
    CHECK(idNoCb != idWider);
    CHECK(cache.LayoutCount() == 3);

    cache.Clear(device->Graves(), 0);
    device->Graves().Drain();
    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("nri batch2d constant arena: every (frame slot, region) pair owns a distinct, "
          "stride-aligned byte range", "[nri]")
{
    // The property the whole arena rests on, and the one a desk run would only
    // ever reveal as "the wrong material's colours". Batch2DNode creates its
    // constant-buffer views ONCE, at fixed offsets, and double-buffers by frame
    // slot -- so two regions overlapping would mean one frame's memcpy
    // corrupting another frame's in-flight constants.
    using Node = Arcane::Batch2DNode;
    CHECK(Node::kCbRegionsPerFrame == Node::kMaxMaterialSlots + 1);

    // THE STRIDE ITSELF, over every alignment a real device might report for
    // deviceDesc.memoryAlignment.constantBufferOffset -- including 512, which is
    // LARGER than kMaterialCbMaxBytes and is the case a naive "just use 256"
    // would get wrong. Two invariants, and violating either is silent: a stride
    // that is not a multiple of the device's alignment misaligns every CB view
    // past the first, and a stride below kMaterialCbMaxBytes lets a material's
    // packed bytes spill into the next region.
    const std::uint64_t alignments[] = { 0, 1, 16, 64, 256, 512 };
    for (const std::uint64_t alignment : alignments)
    {
        const std::uint64_t stride = Arcane::Batch2DNode::CbRegionStride(alignment);
        REQUIRE(stride >= Arcane::Batch2DNode::kMaterialCbMaxBytes);
        if (alignment > 1)
            CHECK(stride % alignment == 0);
        // ...and it is the SMALLEST such value: no region is wasted.
        CHECK(stride - (alignment > 1 ? alignment : 1)
              < Arcane::Batch2DNode::kMaterialCbMaxBytes);
    }
    // A 256-byte alignment is exactly one region -- the D3D12 case the constant
    // was chosen for.
    CHECK(Node::CbRegionStride(256) == Node::kMaterialCbMaxBytes);

    // The OFFSET arithmetic, over two stride values -- deliberately including
    // one (64) no device would produce, because CbRegionOffset must be correct
    // for whatever stride it is handed rather than only for the one this node
    // currently computes.
    const std::uint64_t strides[] = { 256, 64 };
    for (const std::uint64_t stride : strides)
    {
        const std::uint64_t arenaBytes =
            (std::uint64_t)Node::kCbRegionsPerFrame * Arcane::kSwapchainFramesInFlight * stride;
        std::vector<std::uint64_t> seen;
        for (std::uint32_t slot = 0; slot < Arcane::kSwapchainFramesInFlight; ++slot)
        {
            for (std::uint32_t region = 0; region < Node::kCbRegionsPerFrame; ++region)
            {
                const std::uint64_t offset = Node::CbRegionOffset(stride, slot, region);
                CHECK(offset % stride == 0);
                for (const std::uint64_t other : seen)
                    REQUIRE(offset != other);            // no aliasing, ever
                REQUIRE(offset + stride <= arenaBytes);  // and none escapes the buffer
                seen.push_back(offset);
            }
        }
        // Every region of the buffer Batch2DNode allocates is claimed exactly once.
        CHECK(seen.size() == (std::size_t)Node::kCbRegionsPerFrame * Arcane::kSwapchainFramesInFlight);
    }

    // Region 0 of each frame slot is the GLOBALS CB and material slot n is
    // region 1 + n -- the indexing Batch2DNode::ArenaOffset and
    // MaterialSlot::cbRegion agree on.
    CHECK(Node::CbRegionOffset(256, 0, 0) == 0);
    CHECK(Node::CbRegionOffset(256, 0, 1) == 256);
    CHECK(Node::CbRegionOffset(256, 1, 0) == (std::uint64_t)Node::kCbRegionsPerFrame * 256);
}

// ======================================================================
// PostChainNode's pure halves (Phase 2, Task 10)
// ======================================================================

TEST_CASE("nri post chain layout: NO root constants, b0/b1 CBVs, and the chain inputs share ONE "
          "contiguous SRV range with the material's own textures", "[nri]")
{
    // The fullscreen register map is NOT the sprite one, and every difference
    // below is a place a copy-paste from SpriteMaterialLayout would bind the
    // wrong register: fullscreen_material.hlsl has no push constants at all
    // (b0 is the MATERIAL cbuffer, not the batcher's projection block), and
    // GenerateMaterialBindings emits the reserved InputTexture(N) slots
    // immediately after the declared textures.
    Arcane::FullscreenMaterialLayout layout;
    layout.Build(/*cbSize=*/32, /*textureCount=*/1, /*chainInputs=*/2);

    // THE REGISTER-SPACE RULE, same as the sprite twin's: NRI refuses
    // rootRegisterSpace == a set's registerSpace only when the layout carries
    // root DESCRIPTORS or root SAMPLERS, and every register in the fullscreen
    // template is in the implicit space0 -- so both spaces must be 0 and these
    // counts must stay zero.
    REQUIRE(layout.desc.rootDescriptorNum == 0);
    REQUIRE(layout.desc.rootSamplerNum == 0);
    // ...and unlike the sprite layout, there are no root CONSTANTS either.
    REQUIRE(layout.desc.rootConstantNum == 0);
    CHECK(layout.desc.rootConstants == nullptr);
    CHECK(layout.desc.rootRegisterSpace == 0);
    REQUIRE(layout.desc.descriptorSetNum == 1);
    CHECK(layout.set.registerSpace == 0);

    // CBV b0, CBV b1, SRV t0..t2, SAMPLER s0 -- in the order that lets NRI's
    // D3D12 backend merge each consecutive same-type run into one root table.
    REQUIRE(layout.set.rangeNum == 4);
    REQUIRE(layout.materialCb != Arcane::FullscreenMaterialLayout::kNoRange);
    CHECK(layout.materialCb == 0);
    CHECK(layout.globalsCb  == 1);
    CHECK(layout.textures   == 2);
    CHECK(layout.sampler    == 3);

    CHECK(layout.ranges[layout.materialCb].descriptorType == nri::DescriptorType::CONSTANT_BUFFER);
    CHECK(layout.ranges[layout.materialCb].baseRegisterIndex == Arcane::kMaterialCbSlot);   // b0
    CHECK(layout.ranges[layout.globalsCb].descriptorType == nri::DescriptorType::CONSTANT_BUFFER);
    CHECK(layout.ranges[layout.globalsCb].baseRegisterIndex == Arcane::kGlobalCbSlot);      // b1
    // ONE range covering BOTH the declared texture params and the chain
    // inputs -- 1 + 2 here. Splitting them would cost an extra root parameter
    // and, worse, would need the input slots' base register recomputed.
    CHECK(layout.ranges[layout.textures].descriptorType == nri::DescriptorType::TEXTURE);
    CHECK(layout.ranges[layout.textures].baseRegisterIndex == 0);
    CHECK(layout.ranges[layout.textures].descriptorNum == 3);
    CHECK(layout.ranges[layout.sampler].descriptorType == nri::DescriptorType::SAMPLER);
    CHECK(layout.ranges[layout.sampler].baseRegisterIndex == 0);                            // s0

    // NRI validates that every range's stages are a SUBSET of the layout's,
    // and every range here is both stages -- a merged template's VERTEX_BODY
    // may read its params and sample its textures.
    for (std::uint32_t i = 0; i < layout.set.rangeNum; ++i)
    {
        const auto stages = (std::uint32_t)layout.ranges[i].shaderStages;
        const auto all    = (std::uint32_t)layout.desc.shaderStages;
        CHECK((stages & all) == stages);
        CHECK((stages & (std::uint32_t)nri::StageBits::VERTEX_SHADER) != 0u);
        CHECK((stages & (std::uint32_t)nri::StageBits::FRAGMENT_SHADER) != 0u);
    }

    // The desc points at THIS object's arrays -- the reason it is not copyable.
    CHECK(layout.desc.descriptorSets == &layout.set);
    CHECK(layout.set.ranges == layout.ranges);
}

TEST_CASE("nri post chain layout: the degenerate shapes declare exactly what the stitcher emitted",
          "[nri]")
{
    // A chain with no numeric params: MaterialSource omits the cbuffer block
    // entirely, so a b0 range would name a register the shader never declared.
    // The chain input slot keeps both the SRV range and the sampler alive.
    {
        Arcane::FullscreenMaterialLayout layout;
        layout.Build(/*cbSize=*/0, /*textureCount=*/0, /*chainInputs=*/1);
        CHECK(layout.materialCb == Arcane::FullscreenMaterialLayout::kNoRange);
        REQUIRE(layout.set.rangeNum == 3);
        CHECK(layout.globalsCb == 0);
        CHECK(layout.textures  == 1);
        CHECK(layout.sampler   == 2);
        CHECK(layout.ranges[layout.globalsCb].baseRegisterIndex == Arcane::kGlobalCbSlot);
        CHECK(layout.ranges[layout.textures].descriptorNum == 1);   // InputTexture alone, at t0
        CHECK(layout.desc.rootDescriptorNum == 0);
        CHECK(layout.desc.rootSamplerNum == 0);
    }

    // Nothing to sample at all: GenerateMaterialBindings emits no
    // MaterialSampler in that case, so declaring s0 would name a register the
    // shader does not have. Not reachable through the post slot today
    // (chainInputSlots is at least 1) -- pinned because the layout must
    // describe the STITCHER's output rather than the post path's habits.
    {
        Arcane::FullscreenMaterialLayout layout;
        layout.Build(/*cbSize=*/16, /*textureCount=*/0, /*chainInputs=*/0);
        REQUIRE(layout.set.rangeNum == 2);
        CHECK(layout.textures == Arcane::FullscreenMaterialLayout::kNoRange);
        CHECK(layout.sampler  == Arcane::FullscreenMaterialLayout::kNoRange);
        CHECK(layout.materialCb == 0);
        CHECK(layout.globalsCb  == 1);
    }
}

TEST_CASE("nri post chain layout: chains of the same SHAPE share one registered layout", "[nri]")
{
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);

    Arcane::NriPipelineCache cache;
    cache.Bind(*device);

    // Same byte-wise dedup contract SpriteMaterialLayout relies on, and the
    // same reason it matters: one layout per binding SHAPE, not per chain.
    // PostChainNode keys its descriptor-set re-allocation on the layout id
    // changing, so a dedup that failed here would re-allocate sets (and strand
    // the old ones in a fixed pool) on every rebuild.
    Arcane::FullscreenMaterialLayout a, b, wider, noCb;
    a.Build(32, 0, 1);
    b.Build(16, 0, 1);        // a different cbSize is the SAME layout: the CB VIEW carries the size
    wider.Build(32, 0, 2);    // one more input slot IS a different shape
    noCb.Build(0, 0, 1);

    const std::uint32_t idA = cache.RegisterLayout(a.desc);
    REQUIRE(idA != Arcane::NriPipelineCache::kInvalidLayout);
    CHECK(cache.LayoutCount() == 1);

    CHECK(cache.RegisterLayout(b.desc) == idA);
    CHECK(cache.LayoutCount() == 1);

    CHECK(cache.RegisterLayout(wider.desc) != idA);
    CHECK(cache.RegisterLayout(noCb.desc) != idA);
    CHECK(cache.LayoutCount() == 3);

    // ...and a FULLSCREEN layout is never confused with a SPRITE one, even at
    // the same (cbSize, textureCount): the register map differs, and both
    // nodes register into this one shared cache.
    Arcane::SpriteMaterialLayout sprite;
    sprite.Build(32, 0);
    CHECK(cache.RegisterLayout(sprite.desc) != idA);
    CHECK(cache.LayoutCount() == 4);

    cache.Clear(device->Graves(), 0);
    device->Graves().Drain();
    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("nri post chain arena: every (frame slot, region) pair owns a distinct, stride-aligned "
          "byte range", "[nri]")
{
    // The post arena is the second one on this path and it is a SEPARATE,
    // smaller one -- two fixed regions per frame slot (globals + the ONE
    // material CB a chain shares) against Batch2DNode's 1 + kMaxMaterialSlots.
    // Same invariants, same silence when they break: a stride below kCbMaxBytes
    // lets the packed bytes spill into the next region, and one that is not a
    // multiple of the device's alignment misaligns every view past the first.
    using Node = Arcane::PostChainNode;
    CHECK(Node::kCbRegionsPerFrame == 2);
    CHECK(Node::kGlobalsRegion == 0);
    CHECK(Node::kMaterialRegion == 1);

    const std::uint64_t alignments[] = { 0, 1, 16, 64, 256, 512 };
    for (const std::uint64_t alignment : alignments)
    {
        const std::uint64_t stride = Node::CbRegionStride(alignment);
        REQUIRE(stride >= Node::kCbMaxBytes);
        if (alignment > 1)
            CHECK(stride % alignment == 0);
        // ...and it is the SMALLEST such value: no region is wasted. 512 is
        // the case that earns this -- it is LARGER than kCbMaxBytes, so a
        // "just use 256" implementation passes every other alignment.
        CHECK(stride - (alignment > 1 ? alignment : 1) < Node::kCbMaxBytes);
    }
    CHECK(Node::CbRegionStride(256) == Node::kCbMaxBytes);

    const std::uint64_t strides[] = { 256, 64 };
    for (const std::uint64_t stride : strides)
    {
        const std::uint64_t arenaBytes =
            (std::uint64_t)Node::kCbRegionsPerFrame * Arcane::kSwapchainFramesInFlight * stride;
        std::vector<std::uint64_t> seen;
        for (std::uint32_t slot = 0; slot < Arcane::kSwapchainFramesInFlight; ++slot)
        {
            for (std::uint32_t region = 0; region < Node::kCbRegionsPerFrame; ++region)
            {
                const std::uint64_t offset = Node::CbRegionOffset(stride, slot, region);
                CHECK(offset % stride == 0);
                for (const std::uint64_t other : seen)
                    REQUIRE(offset != other);            // no aliasing, ever
                REQUIRE(offset + stride <= arenaBytes);  // and none escapes the buffer
                seen.push_back(offset);
            }
        }
        CHECK(seen.size() == (std::size_t)Node::kCbRegionsPerFrame * Arcane::kSwapchainFramesInFlight);
    }

    // The globals CB and the material CB are DIFFERENT regions of the same
    // frame slot -- the mistake that would otherwise show up as a post chain
    // whose params are the viewport size.
    CHECK(Node::CbRegionOffset(256, 0, Node::kGlobalsRegion) !=
          Node::CbRegionOffset(256, 0, Node::kMaterialRegion));
    CHECK(Node::CbRegionOffset(256, 0, Node::kGlobalsRegion) == 0);
    CHECK(Node::CbRegionOffset(256, 1, Node::kGlobalsRegion)
          == (std::uint64_t)Node::kCbRegionsPerFrame * 256);
}

// ======================================================================
// THE PICK + JFA OUTLINE CHAIN (Phase 2, Task 11).
//
// Same drive as every case above -- Arcane::DeclareGraphFrame with a null
// context -- so these fail when the DECLARATIONS change, not when a
// transcription of them goes stale. What they pin is the whole of the task's
// shape contract: with the flag off the frame is byte-for-byte Task 10's, and
// with it on the chain appears in exactly one place with exactly one derived
// barrier chain.
//
// GPU-free by construction: none of this needs a device, because Compile() is
// pure and the node OBJECTS (which do need one) are only reached from exec fns
// a null context makes inert.
// ======================================================================

TEST_CASE("outline jfa: the graph's jump schedule matches the pinned expected sequence, step for step", "[nri]")
{
    // THE CROSS-ARM CONTRACT (NRI Phase 3, D3c), historically -- and the only
    // place it could be stated without a GPU. JFA is an APPROXIMATION: the
    // nearest seed it settles on a few px from a silhouette depends on the
    // jump SEQUENCE, and a few px from a silhouette is exactly where
    // outline_composite's 1-px AA ramp lives. So "both schedules flood the
    // whole field" was not enough for the editor's `full` golden to compare
    // equal across the arms -- the sequences had to be the same one.
    //
    // NRI Phase 5a, Task 4 deleted SelectionOutline.{hpp,cpp} (the NVRHI twin)
    // along with JfaPassCount, its schedule-length formula and this case's
    // former cross-check partner -- OutlineJfaStepCount/OutlineJfaJump are the
    // only implementation left, so there is no second opinion to compare
    // against anymore. What remains is a literal pin of the exact sequence
    // the twin used to be checked against, which is just as effective a
    // regression guard on a pure function: any future change to the formula
    // still trips here.
    const std::uint32_t steps = Arcane::OutlineJfaStepCount(Arcane::kOutlineMaxThicknessPx);
    REQUIRE(steps <= Arcane::OutlineNode::kMaxJfaSteps);   // never clamped away

    std::vector<std::int32_t> graphJumps;
    for (std::uint32_t step = 0; step < steps; ++step)
        graphJumps.push_back(Arcane::OutlineJfaJump(step, steps));

    // 32 px of thickness -> 32,16,8,4,2,1 and then 1 again.
    const std::vector<std::int32_t> expected = { 32, 16, 8, 4, 2, 1, 1 };
    CHECK(graphJumps == expected);
    // The TRAILING pass specifically -- the half of the schedule a "jumps halve
    // to 1" reading of the code would silently drop (and did, through Phase 2).
    CHECK(graphJumps[steps - 1] == 1);
    CHECK(graphJumps[steps - 2] == 1);

    // The reach still covers the field the composite can actually draw into:
    // every pixel within kOutlineMaxThicknessPx of a silhouette must be able to
    // see it. Beyond that the field is empty BY DESIGN and the composite
    // discards -- which is why the count is thickness-derived, not extent-derived.
    std::uint64_t reach = 0;
    for (const std::int32_t jump : graphJumps)
        reach += (std::uint64_t)jump;
    CHECK(reach >= Arcane::kOutlineMaxThicknessPx);

    // Degenerate thickness still asks for passes rather than none: a zero-step
    // chain would leave the composite reading a never-written transient, which
    // Compile() refuses outright.
    CHECK(Arcane::OutlineJfaStepCount(0) == 2);
    CHECK(Arcane::OutlineJfaStepCount(1) == 2);
    CHECK(Arcane::OutlineJfaJump(0, Arcane::OutlineJfaStepCount(1)) == 1);
}

TEST_CASE("outline jfa: the graph's id pass supersamples by the pinned constant", "[nri]")
{
    // The other half of D3c's divergence, and the one no schedule assertion
    // could see. outline_seed.hlsl averages the ss*ss subsamples of each 1x
    // pixel into a SUB-PIXEL centroid and stores THAT as the seed position; the
    // composite then measures its distance to it. At ss=1 every seed sits at its
    // pixel centre. So an arm that supersamples and an arm that does not would
    // not merely differ in quality -- they would place the outline differently,
    // by up to a quarter pixel, against a 1-px AA ramp. (This case used to be
    // titled "both arms' id pass supersamples by the SAME factor" and pin that
    // property across the NVRHI and graph arms; NRI Phase 5a, Task 4 deleted
    // the NVRHI arm, so there is only one id pass left to pin the constant on.)
    //
    // ONE constant: PickNode sizes its graph transient by it. (Before NRI
    // Phase 5a, Task 4 deleted PickBuffer, EditorApp handed the same number
    // to PickBuffer::Create -- the NVRHI twin's identical id-target sizing.)
    CHECK(Arcane::kPickSupersample >= 1);
    CHECK(Arcane::PickNode::kSuperSample == Arcane::kPickSupersample);
}

TEST_CASE("nri graph frame: the pick + outline chain is absent unless the frame asks for it", "[nri]")
{
    // THE REASON THE STAGE BASELINES STAY THE COMPARE TARGETS: a frame that did
    // not ask for the probe must declare EXACTLY what Task 10 declared. Node
    // count, node names and pool slots all pinned, so a node added "just for
    // the pick path" cannot slip in unconditionally.
    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth  = 320;
    shape.canvasHeight = 200;
    shape.pickOutline  = false;

    const Arcane::RgFrameHandles handles = Arcane::DeclareGraphFrame(graph, shape, nullptr);

    REQUIRE(graph.NodeCount() == 2);
    CHECK(std::string(graph.NodeName(0)) == "batch2d");
    CHECK(std::string(graph.NodeName(1)) == "tonemap");
    CHECK_FALSE(graph.IsHandleValid(handles.pickIds));
    CHECK_FALSE(graph.IsHandleValid(handles.pickReadback));
    CHECK_FALSE(graph.IsHandleValid(handles.outlineField));
    CHECK(handles.jfaStepCount == 0);

    const Arcane::RgCompiled compiled = CompileOk(graph);
    CHECK(compiled.nodes.size() == 2);
    CHECK(compiled.transients.size() == 1);   // the canvas, and nothing else
    CHECK(compiled.poolSlotCount == 1);
}

TEST_CASE("nri graph frame: the pick + outline chain lands between the tonemap and the capture",
          "[nri]")
{
    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth  = 320;
    shape.canvasHeight = 200;
    shape.pickOutline  = true;

    const Arcane::RgFrameHandles handles = Arcane::DeclareGraphFrame(graph, shape, nullptr);

    // 32 px of outline thickness -> 6 halvings plus the trailing jump==1 pass.
    // NOT a function of the 320x200 canvas: see the "the graph's jump schedule
    // matches the pinned expected sequence" case for why the count is
    // thickness-derived.
    const std::uint32_t steps = Arcane::OutlineJfaStepCount(Arcane::kOutlineMaxThicknessPx);
    REQUIRE(steps == 7);
    CHECK(handles.jfaStepCount == steps);

    // SIX fixed nodes -- batch2d, tonemap, pick, pickreadback, outlineseed,
    // outlinecomposite -- plus one outlinejfa per step.
    REQUIRE(graph.NodeCount() == 6u + steps);
    CHECK(std::string(graph.NodeName(0)) == "batch2d");
    CHECK(std::string(graph.NodeName(1)) == "tonemap");
    CHECK(std::string(graph.NodeName(2)) == "pick");
    CHECK(std::string(graph.NodeName(3)) == "pickreadback");
    CHECK(std::string(graph.NodeName(4)) == "outlineseed");
    for (std::uint32_t step = 0; step < steps; ++step)
        CHECK(std::string(graph.NodeName(5 + step)) == "outlinejfa" + std::to_string(step));
    // THE COMPOSITING ORDER: the composite is the LAST node, so it draws onto
    // the backbuffer the tonemap already wrote -- and, on a capture frame,
    // before the copy that reads it (a later case).
    CHECK(std::string(graph.NodeName(5 + steps)) == "outlinecomposite");

    // The id target is a graph TRANSIENT; the readback staging buffer is an
    // IMPORT, because the graph realizes transients in DEVICE memory and those
    // can never be mapped.
    REQUIRE(graph.IsHandleValid(handles.pickIds));
    REQUIRE(graph.IsHandleValid(handles.pickReadback));
    REQUIRE(graph.IsHandleValid(handles.outlineField));
    CHECK(graph.IsTransient(handles.pickIds));
    CHECK_FALSE(graph.IsTransient(handles.pickReadback));
    CHECK(graph.IsTransient(handles.outlineField));
    CHECK(std::string(graph.NameOf(handles.pickIds)) == "pickids");
    CHECK(std::string(graph.NameOf(handles.pickReadback)) == "pickreadback");
    // The field the composite sampled IS the last step's target, not the seed.
    CHECK(std::string(graph.NameOf(handles.outlineField))
          == "outlinejfa" + std::to_string(steps - 1));
}

TEST_CASE("nri graph frame: the pick chain's barriers are derived, including the readback copy",
          "[nri]")
{
    // NOTHING in the chain records a CmdBarrier -- every transition below falls
    // out of the Read/Write declarations alone (RgCompiled's contract block).
    // This is the case that goes red if a node starts emitting its own.
    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth  = 320;
    shape.canvasHeight = 200;
    shape.pickOutline  = true;

    const Arcane::RgFrameHandles handles = Arcane::DeclareGraphFrame(graph, shape, nullptr);
    const std::uint32_t steps = handles.jfaStepCount;
    const Arcane::RgCompiled compiled = CompileOk(graph);
    REQUIRE(compiled.nodes.size() == 6u + steps);

    constexpr nri::AccessLayoutStage kShaderReadState{
        nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, kShaderReadStages };
    constexpr nri::AccessLayoutStage kCopySrcState{
        nri::AccessBits::COPY_SOURCE, nri::Layout::COPY_SOURCE, nri::StageBits::COPY };

    // Node 2 (pick): the id transient's pool slot -> colour attachment.
    REQUIRE(compiled.nodes[2].preBarriers.size() == 1);
    CHECK(compiled.nodes[2].preBarriers[0].isTexture);
    CheckState(compiled.nodes[2].preBarriers[0].after, kColorState);

    // Node 3 (pickreadback): the id target becomes a copy SOURCE and the
    // IMPORTED staging buffer a copy DESTINATION. A buffer barrier's layout is
    // meaningless by contract, hence UNDEFINED -- and RgUsage::ReadbackHost
    // deriving the same state as CopyDst is by design, not an accident.
    REQUIRE(compiled.nodes[3].preBarriers.size() == 2);
    const Arcane::RgBarrier& idToCopy = compiled.nodes[3].preBarriers[0].isTexture
                                          ? compiled.nodes[3].preBarriers[0]
                                          : compiled.nodes[3].preBarriers[1];
    const Arcane::RgBarrier& stagingBarrier = compiled.nodes[3].preBarriers[0].isTexture
                                          ? compiled.nodes[3].preBarriers[1]
                                          : compiled.nodes[3].preBarriers[0];
    CHECK(idToCopy.isTexture);
    CheckState(idToCopy.before, kColorState);
    CheckState(idToCopy.after, kCopySrcState);
    CHECK_FALSE(stagingBarrier.isTexture);
    CheckState(stagingBarrier.after,
               nri::AccessBits::COPY_DESTINATION, nri::Layout::UNDEFINED, nri::StageBits::COPY);

    // Node 4 (outlineseed): the id target goes COPY_SOURCE -> SHADER_RESOURCE
    // (the readback ran first), and the seed target becomes an attachment.
    REQUIRE(compiled.nodes[4].preBarriers.size() == 2);
    bool sawIdRead = false, sawSeedWrite = false;
    for (const Arcane::RgBarrier& barrier : compiled.nodes[4].preBarriers)
    {
        REQUIRE(barrier.isTexture);
        if (static_cast<std::uint32_t>(barrier.after.layout)
            == static_cast<std::uint32_t>(nri::Layout::SHADER_RESOURCE))
        {
            sawIdRead = true;
            CheckState(barrier.before, kCopySrcState);
            CheckState(barrier.after, kShaderReadState);
        }
        else
        {
            sawSeedWrite = true;
            CheckState(barrier.after, kColorState);
        }
    }
    CHECK(sawIdRead);
    CHECK(sawSeedWrite);

    // Every JFA step: read the previous target, write its own. Two barriers,
    // every time, with no hand-written ping-pong anywhere.
    for (std::uint32_t step = 0; step < steps; ++step)
    {
        const Arcane::RgCompiledNode& node = compiled.nodes[5 + step];
        REQUIRE(node.preBarriers.size() == 2);
        bool read = false, write = false;
        for (const Arcane::RgBarrier& barrier : node.preBarriers)
        {
            REQUIRE(barrier.isTexture);
            if (static_cast<std::uint32_t>(barrier.after.layout)
                == static_cast<std::uint32_t>(nri::Layout::SHADER_RESOURCE))
            {
                read = true;
                CheckState(barrier.before, kColorState);
                CheckState(barrier.after, kShaderReadState);
            }
            else
            {
                write = true;
                CheckState(barrier.after, kColorState);
            }
        }
        CHECK(read);
        CHECK(write);
    }

    // The composite: ONE barrier, the field's COLOR_ATTACHMENT ->
    // SHADER_RESOURCE. The backbuffer gets none, and that is DERIVED rather
    // than forgotten -- the tonemap already left it in COLOR_ATTACHMENT and a
    // consecutive same-state declaration produces no transition, which is
    // exactly what nvrhi's own state tracker does for a non-UAV state
    // (CommandListResourceStateTracker::requireTextureState).
    const Arcane::RgCompiledNode& composite = compiled.nodes[5 + steps];
    REQUIRE(composite.preBarriers.size() == 1);
    CHECK(composite.preBarriers[0].isTexture);
    CheckState(composite.preBarriers[0].before, kColorState);
    CheckState(composite.preBarriers[0].after, kShaderReadState);

    // ...and the graph still leaves the backbuffer present-ready. The staging
    // buffer gets NO exit barrier: ImportBuffer takes no exit state.
    REQUIRE(compiled.exitBarriers.size() == 1);
    CHECK(compiled.exitBarriers[0].isTexture);
    CheckState(compiled.exitBarriers[0].after, kPresentState);
}

TEST_CASE("nri graph frame: the outline field ping-pongs through TWO pool slots however many "
          "steps it runs", "[nri]")
{
    // THE PING-PONG IS DERIVED, here as it is for the post chain: every step
    // declares its OWN transient and the pool's lifetime-interval allocator is
    // what collapses N+1 logical field targets onto two physical ones. A
    // regression that hand-rolled the alternation would show up as a slot count
    // that grows with the step count.
    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth  = 320;
    shape.canvasHeight = 200;
    shape.pickOutline  = true;

    const Arcane::RgFrameHandles handles = Arcane::DeclareGraphFrame(graph, shape, nullptr);
    const Arcane::RgCompiled compiled = CompileOk(graph);

    // canvas + pickids + outlineseed + N jfa targets.
    REQUIRE(compiled.transients.size() == 3u + handles.jfaStepCount);
    // ...in FOUR pool slots: the RGBA16F canvas, the R32_UINT id target (a
    // different desc, so it can never share), and the two the whole SNORM field
    // chain alternates through.
    CHECK(compiled.poolSlotCount == 4);

    // And the slot count is genuinely independent of the canvas -- a much
    // smaller one runs the SAME steps through the same two slots. The step
    // count being extent-independent is the D3c change (it used to shrink with
    // the canvas): the schedule is derived from the outline's thickness, which
    // is what makes it identical to the NVRHI arm's at every viewport size.
    Arcane::RenderGraph small;
    Arcane::RgFrameShape smallShape;
    smallShape.canvasWidth  = 16;
    smallShape.canvasHeight = 16;
    smallShape.pickOutline  = true;
    const Arcane::RgFrameHandles smallHandles =
        Arcane::DeclareGraphFrame(small, smallShape, nullptr);
    CHECK(smallHandles.jfaStepCount == handles.jfaStepCount);
    const Arcane::RgCompiled smallCompiled = CompileOk(small);
    CHECK(smallCompiled.poolSlotCount == 4);
}

TEST_CASE("nri graph frame: a capture frame copies the backbuffer AFTER the outline composited "
          "onto it", "[nri]")
{
    // The capture node exists to record what was PRESENTED, so an outline that
    // landed after the copy would be visible on screen and absent from every
    // artifact -- the kind of bug a screenshot cannot show you.
    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth   = 320;
    shape.canvasHeight  = 200;
    shape.pickOutline   = true;
    shape.capture       = true;
    shape.captureBuffer = nullptr;   // ImportBuffer stores the pointer without dereferencing
    shape.captureBytes  = 4096;

    const Arcane::RgFrameHandles handles = Arcane::DeclareGraphFrame(graph, shape, nullptr);
    const std::uint32_t steps = handles.jfaStepCount;

    REQUIRE(graph.NodeCount() == 7u + steps);
    CHECK(std::string(graph.NodeName(5 + steps)) == "outlinecomposite");
    CHECK(std::string(graph.NodeName(6 + steps)) == "capture");

    const Arcane::RgCompiled compiled = CompileOk(graph);
    const Arcane::RgCompiledNode& capture = compiled.nodes[6 + steps];

    // The backbuffer transitions COLOR_ATTACHMENT -> COPY_SOURCE at the CAPTURE
    // node, i.e. it was still an attachment through the composite.
    REQUIRE(capture.preBarriers.size() == 2);
    const Arcane::RgBarrier& textureBarrier = capture.preBarriers[0].isTexture
                                                ? capture.preBarriers[0] : capture.preBarriers[1];
    CHECK(textureBarrier.isTexture);
    CheckState(textureBarrier.before, kColorState);
    CheckState(textureBarrier.after,
               nri::AccessBits::COPY_SOURCE, nri::Layout::COPY_SOURCE, nri::StageBits::COPY);

    // TWO imported buffers now (the pick readback and the capture staging), and
    // neither carries an exit barrier -- only the backbuffer does.
    REQUIRE(compiled.exitBarriers.size() == 1);
    CheckState(compiled.exitBarriers[0].after, kPresentState);
}

TEST_CASE("nri graph frame: the pick + outline chain composes with a post chain", "[nri]")
{
    // The two chains are independent and must not interfere: the post chain
    // sits between the canvas and the tonemap, the outline after it. Pinned
    // because both grow the pool and both ping-pong, and a slot-assignment bug
    // that only appears when both are live would otherwise be desk-only.
    Arcane::PostChainDesc desc;
    desc.chainInputSlots = 1;
    desc.passes.resize(3);
    desc.passes[0].inputs = { Arcane::kSceneInput };
    desc.passes[1].inputs = { 0u };
    desc.passes[2].inputs = { 1u };

    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth  = 320;
    shape.canvasHeight = 200;
    shape.post         = &desc;
    shape.pickOutline  = true;

    const Arcane::RgFrameHandles handles = Arcane::DeclareGraphFrame(graph, shape, nullptr);
    CHECK(handles.postPassCount == 3);
    REQUIRE(graph.IsHandleValid(handles.post));
    REQUIRE(graph.IsHandleValid(handles.outlineField));

    // batch2d + 3 post + tonemap + pick + pickreadback + seed + composite,
    // plus one node per jump-flood step.
    CHECK(graph.NodeCount() == 9u + handles.jfaStepCount);
    CHECK(std::string(graph.NodeName(4)) == "tonemap");
    CHECK(std::string(graph.NodeName(5)) == "pick");
    CHECK(std::string(graph.NodeName(graph.NodeCount() - 1)) == "outlinecomposite");

    const Arcane::RgCompiled compiled = CompileOk(graph);
    // The two chains never share a slot, because their formats differ (RGBA16F
    // vs RGBA16_SNORM) and descsMatch is exact: 2 for the post ping-pong (the
    // canvas hosts the even targets), 1 for the id target, 2 for the field.
    CHECK(compiled.poolSlotCount == 5);
}

TEST_CASE("nri graph frame: the HUD node is absent unless the frame carries draw data", "[nri]")
{
    // Task 12's half of "the stage baselines stay the compare targets". The HUD
    // node is built on EVERY run (unlike the pick pair, which is --pick-probe
    // gated), so the only thing that can keep a batch/post golden's frame
    // byte-for-byte Task 11's is this declaration gate. Pinned both ways.
    {
        Arcane::RenderGraph graph;
        Arcane::RgFrameShape shape;
        shape.canvasWidth  = 320;
        shape.canvasHeight = 200;
        shape.imgui        = false;

        Arcane::DeclareGraphFrame(graph, shape, nullptr);

        REQUIRE(graph.NodeCount() == 2);
        CHECK(std::string(graph.NodeName(0)) == "batch2d");
        CHECK(std::string(graph.NodeName(1)) == "tonemap");

        const Arcane::RgCompiled compiled = CompileOk(graph);
        CHECK(compiled.nodes.size() == 2);
        CHECK(compiled.transients.size() == 1);   // the canvas, and nothing else
        CHECK(compiled.poolSlotCount == 1);
    }

    // ...and with draw data it is ONE node, appended after the tonemap, that
    // creates NO resource of its own: the HUD renders straight onto the
    // imported backbuffer, and its font atlas is node-owned rather than a
    // graph transient. So the pool is untouched.
    {
        Arcane::RenderGraph graph;
        Arcane::RgFrameShape shape;
        shape.canvasWidth  = 320;
        shape.canvasHeight = 200;
        shape.imgui        = true;

        const Arcane::RgFrameHandles handles = Arcane::DeclareGraphFrame(graph, shape, nullptr);

        REQUIRE(graph.NodeCount() == 3);
        CHECK(std::string(graph.NodeName(1)) == "tonemap");
        CHECK(std::string(graph.NodeName(2)) == "imgui");

        const Arcane::RgCompiled compiled = CompileOk(graph);
        REQUIRE(compiled.nodes.size() == 3);
        CHECK(compiled.transients.size() == 1);
        CHECK(compiled.poolSlotCount == 1);

        // The HUD writes the SAME backbuffer the tonemap did -- it does not
        // import a second one.
        REQUIRE(graph.IsHandleValid(handles.backbuffer));
        CHECK(std::string(graph.NameOf(handles.backbuffer)) == "backbuffer");

        // ...and it derives NO barrier: two consecutive ColorWrite
        // declarations on one texture are the same state, and Compile skips
        // same-state transitions. That is deliberately identical to what the
        // outline composite does (Task 11's concern 2) and to what NVRHI's own
        // state tracker does; the ROP's destination read for the alpha blend
        // is inside AccessBits::COLOR_ATTACHMENT.
        CHECK(compiled.nodes[2].preBarriers.empty());

        // The graph -- not the node -- still leaves the backbuffer
        // present-ready, and the HUD did not add a second exit barrier.
        REQUIRE(compiled.exitBarriers.size() == 1);
        CheckState(compiled.exitBarriers[0].after, kPresentState);
    }
}

TEST_CASE("nri graph frame: an imgui-only shape (the editor's chrome) declares clear + tonemap + "
          "HUD, imports its backbuffer, and adds no capture",
          "[nri]")
{
    // NRI Phase 3, Task 10. The shape EditorApp::PresentChromeFrame ASKS FOR:
    // an imgui-only RgFrameShape, i.e. draw data and nothing else -- no
    // batcher, no post chain, no pick/outline, no game HUD, no capture.
    //
    // ===== WHAT THIS CASE DOES AND DOES NOT PIN. READ BEFORE TRUSTING IT. ====
    // It pins the ENGINE SIDE of the arrangement: given an imgui-only shape,
    // DeclareGraphFrame emits exactly batch2d/tonemap/imgui, imports (rather
    // than creates) the backbuffer, adds no capture node, and costs one
    // transient in one pool slot. That is drift protection for
    // DeclareGraphFrame, and it is real -- an engine edit that started
    // declaring a capture or a gameUi node for a shape that asked for neither
    // turns this red.
    //
    // IT DOES NOT PIN EditorApp::PresentChromeFrame'S OWN FrameDesc, and it
    // CANNOT: the shape below is constructed here, by hand, and ArcaneTests
    // does not compile the editor at all (EditorAppFrame.cpp is not in that
    // project). Adding `frame.capture = true` to PresentChromeFrame would leave
    // this case GREEN. The editor's four absences are held by the comment block
    // on PresentChromeFrame and by review -- nothing more -- and the `capture`
    // one is the load-bearing member of that set, because the editor's
    // screenshot/golden semantics capture the VIEWPORT: a capture node on the
    // CHROME frame would copy a chromed backbuffer and quietly redefine what an
    // editor golden contains. Whoever gives the editor a golden harness (Task
    // 13) owes that claim a check that can actually see the editor.
    // ========================================================================
    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth  = 1280;
    shape.canvasHeight = 720;
    // Transcribed from PresentChromeFrame's FrameDesc, which sets `imgui` and
    // leaves everything else default -- a TRANSCRIPTION, with all the drift
    // risk that word carries, not a linkage. See the block above.
    shape.imgui        = true;

    const Arcane::RgFrameHandles handles = Arcane::DeclareGraphFrame(graph, shape, nullptr);

    REQUIRE(graph.NodeCount() == 3);
    CHECK(std::string(graph.NodeName(0)) == "batch2d");
    CHECK(std::string(graph.NodeName(1)) == "tonemap");
    CHECK(std::string(graph.NodeName(2)) == "imgui");

    // Named absences, so the failure message says WHICH node crept in.
    for (std::size_t i = 0; i < graph.NodeCount(); ++i)
    {
        const std::string name = graph.NodeName(i);
        CHECK(name != "gameui");
        CHECK(name != "capture");
        CHECK(name != "pick");
        CHECK(name != "outlinecomposite");
    }

    // IT PRESENTS. The tonemap is the only thing in the tree that produces a
    // backbuffer handle at all (ImportSwapChainTexture), which is why "clear +
    // imgui + present" is three nodes rather than two -- and the imported,
    // non-transient backbuffer is what distinguishes this from the viewport's
    // offscreen frame.
    REQUIRE(graph.IsHandleValid(handles.backbuffer));
    CHECK_FALSE(graph.IsTransient(handles.backbuffer));
    CHECK(std::string(graph.NameOf(handles.backbuffer)) == "backbuffer");
    CHECK_FALSE(graph.IsHandleValid(handles.capture));
    CHECK(handles.postPassCount == 0u);

    const Arcane::RgCompiled compiled = CompileOk(graph);
    REQUIRE(compiled.nodes.size() == 3);

    // The canvas is the frame's ONLY transient, in ONE pool slot: host chrome
    // adds no GPU memory of its own (the font atlas is node-owned, not a graph
    // resource).
    CHECK(compiled.transients.size() == 1);
    CHECK(compiled.poolSlotCount == 1);

    // The HUD derives NO barrier -- it writes the same ColorWrite state the
    // tonemap left the backbuffer in -- and the GRAPH, not the host, is what
    // leaves it present-ready.
    CHECK(compiled.nodes[2].preBarriers.empty());
    REQUIRE(compiled.exitBarriers.size() == 1);
    CHECK(compiled.exitBarriers[0].isTexture);
    CheckState(compiled.exitBarriers[0].after, kPresentState);
}

TEST_CASE("nri graph frame: a capture frame copies the backbuffer AFTER the HUD drew on it",
          "[nri]")
{
    // The `full` golden baseline was captured from the NVRHI path WITH the HUD
    // on it, so a capture that ran before the ImGui node would compare a bare
    // frame against a chromed one -- every run, silently, and only in the
    // stage that is supposed to include chrome.
    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth   = 320;
    shape.canvasHeight  = 200;
    shape.imgui         = true;
    shape.capture       = true;
    shape.captureBuffer = nullptr;   // ImportBuffer stores the pointer without dereferencing
    shape.captureBytes  = 4096;

    Arcane::DeclareGraphFrame(graph, shape, nullptr);

    REQUIRE(graph.NodeCount() == 4);
    CHECK(std::string(graph.NodeName(2)) == "imgui");
    CHECK(std::string(graph.NodeName(3)) == "capture");

    const Arcane::RgCompiled compiled = CompileOk(graph);
    REQUIRE(compiled.nodes.size() == 4);

    // The backbuffer is still a COLOUR ATTACHMENT when the HUD runs and only
    // becomes a copy SOURCE at the capture node -- i.e. the HUD's pixels are in
    // the artifact.
    CHECK(compiled.nodes[2].preBarriers.empty());
    REQUIRE(compiled.nodes[3].preBarriers.size() == 2);
    const Arcane::RgBarrier& textureBarrier = compiled.nodes[3].preBarriers[0].isTexture
                                                ? compiled.nodes[3].preBarriers[0]
                                                : compiled.nodes[3].preBarriers[1];
    CHECK(textureBarrier.isTexture);
    CheckState(textureBarrier.before, kColorState);
    CheckState(textureBarrier.after,
               nri::AccessBits::COPY_SOURCE, nri::Layout::COPY_SOURCE, nri::StageBits::COPY);

    REQUIRE(compiled.exitBarriers.size() == 1);
    CheckState(compiled.exitBarriers[0].after, kPresentState);
}

TEST_CASE("nri graph frame: --golden-stage batch and post drop the HUD; full keeps it", "[nri]")
{
    // The SAME gate RuntimeApp applies to the NVRHI path, where both non-Full
    // stages call ImGui::EndFrame() instead of rendering. Host chrome sits on
    // top of every pixel a batch/post golden exists to compare, so if this gate
    // broke, a stage golden would compare a chromed frame against a bare
    // baseline -- and the diff would look like a batch-node regression.
    const struct { Arcane::GoldenStage stage; std::size_t nodes; bool hud; } cases[] = {
        { Arcane::GoldenStage::Batch, 2, false },
        { Arcane::GoldenStage::Post,  2, false },
        { Arcane::GoldenStage::Full,  3, true  },
    };

    for (const auto& expected : cases)
    {
        Arcane::RenderGraph graph;
        Arcane::RgFrameShape shape;
        shape.canvasWidth  = 320;
        shape.canvasHeight = 200;
        shape.stage        = expected.stage;
        // The driver would not even build draw data on a non-Full stage; this
        // sets it anyway, so what is pinned is the GATE and not the driver's
        // politeness.
        shape.imgui        = true;

        Arcane::DeclareGraphFrame(graph, shape, nullptr);
        REQUIRE(graph.NodeCount() == expected.nodes);
        CHECK((std::string(graph.NodeName(graph.NodeCount() - 1)) == "imgui") == expected.hud);
    }
}

TEST_CASE("nri graph frame: the HUD composes with the post chain and the outline, and stays last",
          "[nri]")
{
    // Everything at once, in the one order that is correct: scene -> grade ->
    // display-referred -> selection chrome -> host chrome -> capture. The HUD
    // must be the LAST VISUAL WRITER; the capture is the only node after it.
    Arcane::PostChainDesc desc;
    desc.chainInputSlots = 1;
    desc.passes.resize(2);
    desc.passes[0].inputs = { Arcane::kSceneInput };
    desc.passes[1].inputs = { 0u };

    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.canvasWidth   = 320;
    shape.canvasHeight  = 200;
    shape.post          = &desc;
    shape.pickOutline   = true;
    shape.imgui         = true;
    shape.capture       = true;
    shape.captureBuffer = nullptr;
    shape.captureBytes  = 4096;

    const Arcane::RgFrameHandles handles = Arcane::DeclareGraphFrame(graph, shape, nullptr);
    const std::uint32_t steps = handles.jfaStepCount;

    // batch2d + 2 post + tonemap + pick + pickreadback + outlineseed + N jfa
    // + outlinecomposite + imgui + capture.
    REQUIRE(graph.NodeCount() == 10u + steps);
    CHECK(std::string(graph.NodeName(3)) == "tonemap");
    CHECK(std::string(graph.NodeName(graph.NodeCount() - 3)) == "outlinecomposite");
    CHECK(std::string(graph.NodeName(graph.NodeCount() - 2)) == "imgui");
    CHECK(std::string(graph.NodeName(graph.NodeCount() - 1)) == "capture");

    const Arcane::RgCompiled compiled = CompileOk(graph);
    // The HUD adds no pool slot however many chains are live: 2 for the post
    // ping-pong, 1 for the id target, 2 for the outline field.
    CHECK(compiled.poolSlotCount == 5);
    // Three consecutive ColorWrite declarations on the backbuffer (tonemap,
    // composite, HUD) still derive exactly one transition into it and one out
    // of it.
    CHECK(compiled.nodes[graph.NodeCount() - 2].preBarriers.empty());
    REQUIRE(compiled.exitBarriers.size() == 1);
    CheckState(compiled.exitBarriers[0].after, kPresentState);
}

TEST_CASE("nri graph frame: the GAME UI node sits between the tonemap and the outline composite",
          "[nri]")
{
    // NRI Phase 3, Task 9. The editor's viewport frame carries a SECOND ImGui
    // node -- the game/plugin HUD -- and where it sits is the whole of what this
    // declaration decides. It is the editor's own compositing order expressed
    // against this recorder: EditorApp::CompositeGameUi (phase 11) runs after
    // the scene render and BEFORE EditorApp::RenderSelectionOutline (phase 12).
    //
    // NOTHING EXERCISES THAT ORDER TODAY, which is exactly why it is pinned
    // here: the two are mutually exclusive by MODE in the editor (Play draws
    // the HUD, Edit draws the outline), so the first frame that ever carries
    // both would be the first chance to discover the order was wrong -- by
    // eye, on a picture, long after the fact.
    {
        // Alone: one node, straight after the tonemap, creating no resource of
        // its own (the atlas is node-owned, not a graph transient).
        Arcane::RenderGraph graph;
        Arcane::RgFrameShape shape;
        shape.canvasWidth  = 320;
        shape.canvasHeight = 200;
        shape.gameUi       = true;

        Arcane::DeclareGraphFrame(graph, shape, nullptr);

        REQUIRE(graph.NodeCount() == 3);
        CHECK(std::string(graph.NodeName(1)) == "tonemap");
        CHECK(std::string(graph.NodeName(2)) == "gameui");

        const Arcane::RgCompiled compiled = CompileOk(graph);
        REQUIRE(compiled.nodes.size() == 3);
        CHECK(compiled.transients.size() == 1);   // the canvas, and nothing else
        CHECK(compiled.poolSlotCount == 1);
        // Same ColorWrite the tonemap declared -> no derived transition, exactly
        // like the host HUD's and the outline composite's.
        CHECK(compiled.nodes[2].preBarriers.empty());
    }

    {
        // ...and with the outline chain, which is the ordering claim itself.
        Arcane::RenderGraph graph;
        Arcane::RgFrameShape shape;
        shape.canvasWidth  = 320;
        shape.canvasHeight = 200;
        shape.gameUi       = true;
        shape.pickOutline  = true;

        const Arcane::RgFrameHandles handles = Arcane::DeclareGraphFrame(graph, shape, nullptr);
        const std::uint32_t steps = handles.jfaStepCount;

        // batch2d + tonemap + gameui + pick + pickreadback + outlineseed
        // + N jfa + outlinecomposite.
        REQUIRE(graph.NodeCount() == 7u + steps);
        CHECK(std::string(graph.NodeName(1)) == "tonemap");
        CHECK(std::string(graph.NodeName(2)) == "gameui");
        CHECK(std::string(graph.NodeName(3)) == "pick");
        CHECK(std::string(graph.NodeName(graph.NodeCount() - 1)) == "outlinecomposite");
    }

    {
        // ...and the two ImGui nodes are DISTINCT nodes with distinct names,
        // in the order gameui-then-hostHUD: the game's HUD is content inside
        // the rendered image, the host's is chrome on top of it. Same frame,
        // both live, plus a capture to prove the HUD is still the last VISUAL
        // writer.
        Arcane::RenderGraph graph;
        Arcane::RgFrameShape shape;
        shape.canvasWidth   = 320;
        shape.canvasHeight  = 200;
        shape.gameUi        = true;
        shape.pickOutline   = true;
        shape.imgui         = true;
        shape.capture       = true;
        shape.captureBuffer = nullptr;
        shape.captureBytes  = 4096;

        const Arcane::RgFrameHandles handles = Arcane::DeclareGraphFrame(graph, shape, nullptr);
        const std::uint32_t steps = handles.jfaStepCount;

        REQUIRE(graph.NodeCount() == 9u + steps);
        CHECK(std::string(graph.NodeName(2)) == "gameui");
        CHECK(std::string(graph.NodeName(graph.NodeCount() - 3)) == "outlinecomposite");
        CHECK(std::string(graph.NodeName(graph.NodeCount() - 2)) == "imgui");
        CHECK(std::string(graph.NodeName(graph.NodeCount() - 1)) == "capture");

        const Arcane::RgCompiled compiled = CompileOk(graph);
        // FOUR consecutive ColorWrite declarations on the backbuffer (tonemap,
        // gameui, outlinecomposite, imgui) still derive exactly one transition
        // out of it, and none between them.
        CHECK(compiled.nodes[2].preBarriers.empty());
        REQUIRE(compiled.exitBarriers.size() == 1);
        CheckState(compiled.exitBarriers[0].after, kPresentState);
        // The game HUD adds no pool slot -- its atlas is node-owned, not a
        // graph transient. 1 for the canvas, 1 for the id target, 2 for the
        // outline field's ping-pong.
        CHECK(compiled.poolSlotCount == 4);
    }

    {
        // IS STAGE-GATED WHERE THE HOST HUD IS, as of NRI Phase 3 Task 13 --
        // this block used to pin the opposite (see git history / the task-9
        // report §9 for Task 9's original ruling and its own note that Task
        // 13 was expected to re-take it). `--golden-stage` names slices of
        // the SCENE render; once the EDITOR grew its own stage vocabulary
        // (Task 13: `full` = +outline/gameui composite), the game HUD became
        // exactly the kind of overlay a batch/post stage golden must mask --
        // the same reasoning the host HUD's gate immediately above already
        // encodes. `full` still draws it; `batch`/`post` now do not.
        //
        // BEHAVIOUR-INERT ON THE RUNTIME: RuntimeFrame.cpp never sets
        // FrameDesc::gameUi (only the editor's ArmGraphViewportFrame does),
        // so this gate is reachable only through the editor -- this case
        // pins the ENGINE side of that (DeclareGraphFrame's declaration),
        // not the editor's own call site, which ArcaneTests cannot compile
        // (EditorApp*.cpp is not a test TU -- see the standing "a green gate
        // proves nothing about either host" rule).
        for (const Arcane::GoldenStage stage : { Arcane::GoldenStage::Batch, Arcane::GoldenStage::Post })
        {
            Arcane::RenderGraph graph;
            Arcane::RgFrameShape shape;
            shape.canvasWidth  = 320;
            shape.canvasHeight = 200;
            shape.stage        = stage;
            shape.gameUi       = true;

            Arcane::DeclareGraphFrame(graph, shape, nullptr);
            REQUIRE(graph.NodeCount() == 2);   // batch2d + tonemap only -- no gameui node
            CHECK(std::string(graph.NodeName(1)) == "tonemap");
        }

        Arcane::RenderGraph graph;
        Arcane::RgFrameShape shape;
        shape.canvasWidth  = 320;
        shape.canvasHeight = 200;
        shape.stage        = Arcane::GoldenStage::Full;
        shape.gameUi       = true;

        Arcane::DeclareGraphFrame(graph, shape, nullptr);
        REQUIRE(graph.NodeCount() == 3);
        CHECK(std::string(graph.NodeName(2)) == "gameui");
    }
}

TEST_CASE("nri pick readback: every frame slot owns a distinct, alignment-legal region", "[nri]")
{
    // NRI's TextureDataLayoutDesc documents the buffer OFFSET as a multiple of
    // uploadBufferTextureSlice, so a region stride that ignored it would make
    // every slot past 0 an invalid CmdReadbackTextureToBuffer destination --
    // silently on some drivers, which is exactly the class of thing no test
    // could catch after the fact.
    using Node = Arcane::PickNode;

    const std::uint64_t slices[] = { 0, 1, 4, 256, 512, 1024 };
    const std::uint64_t rows[]   = { 4, 256, 512 };
    for (const std::uint64_t slice : slices)
    {
        for (const std::uint64_t row : rows)
        {
            const std::uint64_t stride = Node::ReadbackRegionStride(row, slice);
            REQUIRE(stride >= row);            // a region holds a whole aligned row
            REQUIRE(stride >= sizeof(std::uint32_t));
            if (slice > 1)
                CHECK(stride % slice == 0);    // ...and every region offset is legal

            const std::uint64_t bytes = stride * Arcane::kSwapchainFramesInFlight;
            std::vector<std::uint64_t> seen;
            for (std::uint32_t slot = 0; slot < Arcane::kSwapchainFramesInFlight; ++slot)
            {
                const std::uint64_t offset = slot * stride;
                if (slice > 1)
                    CHECK(offset % slice == 0);
                for (const std::uint64_t other : seen)
                    REQUIRE(offset != other);                       // no aliasing, ever
                REQUIRE(offset + sizeof(std::uint32_t) <= bytes);   // and none escapes
                seen.push_back(offset);
            }
        }
    }

    // ONE REGION PER FRAME IN FLIGHT is the whole latency contract: fewer, and
    // the CPU would read a region the GPU is still writing.
    CHECK(Arcane::kSwapchainFramesInFlight >= 2);
}

TEST_CASE("nri outline arena: every (frame slot, region) pair owns a distinct, stride-aligned "
          "byte range", "[nri]")
{
    // The third arena on this path, and the widest: seed + composite + one
    // region per jump-flood step, because every step's CB carries a DIFFERENT
    // jump and the descriptor a set binds bakes its offset in. Same invariants,
    // same silence when they break.
    using Node = Arcane::OutlineNode;
    CHECK(Node::kSeedRegion == 0);
    CHECK(Node::kCompositeRegion == 1);
    CHECK(Node::kJfaRegionBase == 2);
    CHECK(Node::kCbRegionsPerFrame == 2 + Node::kMaxJfaSteps);
    // The cap must cover the schedule the chain actually declares, with room
    // for a thicker outline than today's 32 px if one is ever authored.
    CHECK(Arcane::OutlineJfaStepCount(Arcane::kOutlineMaxThicknessPx) <= Node::kMaxJfaSteps);
    CHECK(Arcane::OutlineJfaStepCount(4096) <= Node::kMaxJfaSteps);

    const std::uint64_t alignments[] = { 0, 1, 16, 64, 256, 512 };
    for (const std::uint64_t alignment : alignments)
    {
        const std::uint64_t stride = Node::CbRegionStride(alignment);
        // 288 bytes is outline_seed.hlsl's SeedCB -- the largest of the three
        // blocks. A stride below it lets the seed spill into the composite's
        // region, which would read as an outline with garbage colours.
        REQUIRE(stride >= Node::kCbMaxBytes);
        if (alignment > 1)
            CHECK(stride % alignment == 0);
        CHECK(stride - (alignment > 1 ? alignment : 1) < Node::kCbMaxBytes);
    }
    // 256-byte alignment (D3D12's) rounds 288 up to 512, not down to 256.
    CHECK(Node::CbRegionStride(256) == 512);

    const std::uint64_t strides[] = { 512, 288 };
    for (const std::uint64_t stride : strides)
    {
        const std::uint64_t arenaBytes =
            (std::uint64_t)Node::kCbRegionsPerFrame * Arcane::kSwapchainFramesInFlight * stride;
        std::vector<std::uint64_t> seen;
        for (std::uint32_t slot = 0; slot < Arcane::kSwapchainFramesInFlight; ++slot)
        {
            for (std::uint32_t region = 0; region < Node::kCbRegionsPerFrame; ++region)
            {
                const std::uint64_t offset = Node::CbRegionOffset(stride, slot, region);
                CHECK(offset % stride == 0);
                for (const std::uint64_t other : seen)
                    REQUIRE(offset != other);            // no aliasing, ever
                REQUIRE(offset + stride <= arenaBytes);  // and none escapes the buffer
                seen.push_back(offset);
            }
        }
        CHECK(seen.size() == (std::size_t)Node::kCbRegionsPerFrame * Arcane::kSwapchainFramesInFlight);
    }

    // Two DIFFERENT jump-flood steps never share a region -- the mistake that
    // would otherwise show up as every step flooding at the same jump distance,
    // i.e. an outline that is subtly wrong rather than obviously broken.
    CHECK(Node::CbRegionOffset(512, 0, Node::kJfaRegionBase)
          != Node::CbRegionOffset(512, 0, Node::kJfaRegionBase + 1));
    CHECK(Node::CbRegionOffset(512, 0, Node::kSeedRegion)
          != Node::CbRegionOffset(512, 0, Node::kCompositeRegion));
    CHECK(Node::CbRegionOffset(512, 0, Node::kSeedRegion) == 0);
}

TEST_CASE("pick geometry: ONE emitter feeds both recorders -- id k+1, back-to-front, per drawable",
          "[render]")
{
    // BuildPickIdGeometry was PickBuffer.cpp's private loop until the graph path
    // grew a pick node of its own. It is shared now because the 1-based id a
    // vertex carries IS the id<->entity mapping every consumer inverts, and two
    // copies of this loop would be two id assignments that agree until one is
    // edited. These are the properties both recorders depend on.
    std::vector<Arcane::PickDrawable> drawables(3);
    drawables[0].kind        = Arcane::PickDrawable::Kind::Quad;
    drawables[0].center      = { 10.0f, 20.0f };
    drawables[0].halfExtents = { 4.0f, 2.0f };
    drawables[1].kind        = Arcane::PickDrawable::Kind::Circle;
    drawables[1].center      = { 50.0f, 60.0f };
    drawables[1].radius      = 7.0f;
    drawables[2].kind        = Arcane::PickDrawable::Kind::Capsule;
    drawables[2].center      = { 90.0f, 5.0f };
    drawables[2].radius      = 3.0f;
    drawables[2].halfLen     = 11.0f;

    std::vector<Arcane::PickIdVertex> vertices;
    std::vector<std::uint32_t>        indices;
    Arcane::BuildPickIdGeometry(drawables, vertices, indices);

    // One quad per drawable: 4 vertices, 6 indices.
    REQUIRE(vertices.size() == 12);
    REQUIRE(indices.size() == 18);

    for (std::size_t d = 0; d < drawables.size(); ++d)
    {
        for (std::size_t v = 0; v < 4; ++v)
        {
            // id = index + 1, on EVERY vertex of the quad. 0 stays reserved for
            // background, which is what the id target clears to.
            CHECK(vertices[d * 4 + v].id == (std::uint32_t)d + 1u);
            CHECK(vertices[d * 4 + v].kind == Arcane::PickKindCode(drawables[d].kind));
        }
        // ...and the indices are per-quad and in submission order, so the
        // LAST-drawn silhouette wins a contested pixel (there is no depth
        // buffer on either path).
        CHECK(indices[d * 6] == (std::uint32_t)d * 4u);
    }

    // The rasterized quad is the drawable's BOUND, not its shape: a circle
    // covers radius x radius, a capsule (halfLen + radius) x radius. The PS
    // discards the rest analytically.
    CHECK(Arcane::PickBoundHalfExtents(drawables[1]) == glm::vec2(7.0f, 7.0f));
    CHECK(Arcane::PickBoundHalfExtents(drawables[2]) == glm::vec2(14.0f, 3.0f));
    CHECK(Arcane::PickBoundHalfExtents(drawables[0]) == glm::vec2(4.0f, 2.0f));

    // Both output vectors are CLEARED, not appended to -- a caller that reuses
    // its buffers every frame (both recorders do) must not accumulate.
    Arcane::BuildPickIdGeometry(std::span<const Arcane::PickDrawable>{}, vertices, indices);
    CHECK(vertices.empty());
    CHECK(indices.empty());
}

// ======================================================================
// TEXTURES ARE NOT GRAPH RESOURCES (NRI Phase 3, Task 2), and what that
// leaves headlessly provable -- READ THIS BEFORE TRUSTING THE CASES BELOW.
//
// A sprite's own texture is now REAL on the graph path: a drained span
// carries the image's asset Guid and Batch2DNode binds the shared
// NriTextureCache's view at t0 through a per-texture descriptor set. The
// question that raises for the frame graph is whether those textures have to
// be DECLARED -- and the answer is no, deliberately: they are persistent,
// uploaded once through HelperInterface::UploadData (which submits and waits,
// at declaration time), and never written by a node. The graph derives
// barriers for resources whose state a frame changes; a texture that is
// SHADER_RESOURCE from its upload until its burial changes none.
//
// WHAT IS PROVED HERE, PRECISELY, AND WHAT IS NOT.
//
// These cases drive DeclareGraphFrame with a NULL context. That means no
// vehicle, therefore no batcher, no drain and no span -- so the first case
// below CANNOT be made red by a textured batch, and it is named for what it
// actually pins: the batch stage's resource census. It is still the right
// assertion for the claim in this block's title (a texture cannot appear in a
// census that has no way to learn about one; a future change that DID declare
// a per-sprite import would land here), but it is a STRUCTURAL statement, not
// an execution of the texture path.
//
// The two cases after it pin the node's own ARITHMETIC -- the pure functions
// Batch2DNode actually runs: DistinctTextureCount (which Prepare() calls as
// the frame's up-front budget check) and PoolSizes() (which CreateBindings()
// passes straight to CreateDescriptorPool). Those are real coverage of real
// production code, and both carry failures a device cannot show.
//
// WHAT HAS NO EXECUTED HEADLESS COVERAGE, AND WHY -- DESK DEBT, OWED AT D3a.
// EnsureSpriteSet, EnsureMaterialVariant, WriteMaterialSet and Record's
// variant scan are NOT executed by any case in the ~[gpu] gate, and cannot be:
// Batch2DNode::Create fails on the NONE backend by construction, because
// CreateConstantArena persistently maps its HOST_UPLOAD arena and ImplNONE's
// MapBuffer returns null unconditionally (the node's own header says so: "this
// node is a [gpu] path from here down"). Even past that, every NONE
// DescriptorSet* is the same dummy pointer and UpdateDescriptorRanges is a
// no-op, so a set-per-texture could not be told from a set-for-everything.
// Their proof is therefore desk-side and specific: D3a's stage compares on the
// textured ReferenceProject scene, where a wrong set selection shows up as the
// wrong image (or the white texel) at t0. BatcherTest.cpp's
// "[gpu][d3d12] a DEVICE-LESS batcher drains identical spans..." WAS the
// other half; that file went with the NVRHI device layer at NRI Phase 5a Task
// 8b, so the desk stage compare is the whole proof now.
// ======================================================================

TEST_CASE("nri graph frame: the batch stage declares one canvas transient and nothing else",
          "[nri]")
{
    // The batch stage's RESOURCE CENSUS. A texture cannot appear in it -- this
    // drive has no vehicle and therefore no span to be textured -- and that is
    // the point: the census is where a future change that DID make a sprite
    // texture a graph resource (an import per sprite, say) would land, instead
    // of in a desk golden. It does not exercise the texture path; see the
    // block comment above for what does and what is owed at D3a.
    Arcane::RenderGraph graph;
    Arcane::RgFrameShape shape;
    shape.stage        = Arcane::GoldenStage::Batch;
    shape.canvasWidth  = 320;
    shape.canvasHeight = 200;

    const Arcane::RgFrameHandles handles = Arcane::DeclareGraphFrame(graph, shape, nullptr);

    CHECK(graph.NodeCount() == 2);                  // batch2d, tonemap
    CHECK(std::string(graph.NodeName(0)) == "batch2d");
    CHECK(graph.IsTransient(handles.canvas));

    const Arcane::RgCompiled compiled = CompileOk(graph);
    // ONE transient (the canvas) in ONE pool slot -- the same census the
    // untextured frame has, because a sprite texture is neither.
    CHECK(compiled.transients.size() == 1);
    CHECK(compiled.poolSlotCount == 1);
    // The batch node transitions exactly one resource: the canvas it writes.
    CHECK(compiled.nodes[0].preBarriers.size() == 1);
}

TEST_CASE("nri graph frame: the batch's descriptor-set count is its distinct texture count",
          "[nri]")
{
    // Driven through a REAL device-less Batcher2D, so what is counted is what
    // the graph path actually drains rather than a hand-built span list -- and
    // DistinctTextureCount is the function Batch2DNode::Prepare itself calls to
    // decide whether the frame is over its per-texture set budget, so this is
    // the node's arithmetic, not a restatement of it. What it does NOT cover is
    // the ALLOCATION that arithmetic gates (EnsureSpriteSet) -- see the block
    // comment above.
    auto batcher = Arcane::Batcher2D::Create(nullptr, nullptr);
    REQUIRE(batcher != nullptr);

    const Arcane::Guid a = *Arcane::Guid::FromString("aaaaaaaa-0000-0000-0000-000000000001");
    const Arcane::Guid b = *Arcane::Guid::FromString("bbbbbbbb-0000-0000-0000-000000000002");

    batcher->Begin(nullptr, nullptr, 64, 64);
    batcher->Rect(glm::vec2(0.0f), glm::vec2(8.0f), glm::vec4(1.0f));      // nil id
    batcher->QuadTextured(Arcane::Batcher2D::kMaterialSprite, a,
                          glm::vec2(0.0f), glm::vec2(8.0f), nullptr,
                          glm::vec2(0.0f), glm::vec2(1.0f), glm::vec4(1.0f));
    batcher->QuadTextured(Arcane::Batcher2D::kMaterialSprite, b,
                          glm::vec2(8.0f, 0.0f), glm::vec2(8.0f), nullptr,
                          glm::vec2(0.0f), glm::vec2(1.0f), glm::vec4(1.0f));
    batcher->QuadTextured(Arcane::Batcher2D::kMaterialSprite, a,   // a REPEAT of `a`
                          glm::vec2(16.0f, 0.0f), glm::vec2(8.0f), nullptr,
                          glm::vec2(0.0f), glm::vec2(1.0f), glm::vec4(1.0f));

    const Arcane::Batch2DDrained drained = batcher->Drain();

    // THREE spans from four quads: the sort key ends in the texture SLOT, so
    // the two `a` quads land adjacent and coalesce even though a `b` quad was
    // submitted between them. And three spans still name only TWO distinct
    // textures -- the nil-id span binds the node's own white texel and costs
    // no per-texture descriptor set at all.
    CHECK(drained.spans.size() == 3);
    CHECK(Arcane::Batch2DNode::DistinctTextureCount(drained.spans) == 2);

    // An untextured batch needs none.
    batcher->Begin(nullptr, nullptr, 64, 64);
    batcher->Rect(glm::vec2(0.0f), glm::vec2(8.0f), glm::vec4(1.0f));
    batcher->Circle(glm::vec2(32.0f), 4.0f, glm::vec4(1.0f));
    const Arcane::Batch2DDrained plain = batcher->Drain();
    CHECK(Arcane::Batch2DNode::DistinctTextureCount(plain.spans) == 0);

    // And the cap that budget is measured against is a real number, not a
    // sentinel -- the node degrades to the white texel past it.
    CHECK(Arcane::Batch2DNode::kMaxSpriteTextures >= 2);
}

TEST_CASE("nri graph frame: the batch node's descriptor pool covers what its caps allow", "[nri]")
{
    // THE POOL ARITHMETIC, and the reason it is worth a case of its own: a
    // descriptor pool's sizes are fixed at creation and NRI cannot free a
    // single set, so a capacity that does not cover what the caps ALLOW is not
    // a compile error and not a wrong pixel -- it is an AllocateDescriptorSets
    // failure part-way through a frame at the desk, after which that material
    // or that texture silently draws with the white texel. Task 2 added a whole
    // dimension to this (a set per sprite texture, and a material set per
    // texture VARIANT per frame slot), which is exactly the kind of change that
    // gets the multiplication wrong.
    //
    // The expectations are recomputed here from the PUBLIC caps rather than
    // copied from the implementation, so a cap that moves without the pool
    // moving with it fails here.
    const nri::DescriptorPoolDesc pool = Arcane::Batch2DNode::PoolSizes();

    constexpr std::uint32_t kSpriteTex   = Arcane::Batch2DNode::kMaxSpriteTextures;
    constexpr std::uint32_t kMatSlots    = Arcane::Batch2DNode::kMaxMaterialSlots;
    constexpr std::uint32_t kMatTextures = Arcane::Batch2DNode::kMaxMaterialTextures;
    constexpr std::uint32_t kFrames      = Arcane::kSwapchainFramesInFlight;

    // Built-in sets: the nil-texture one plus one per distinct sprite texture.
    // No frame-slot dimension -- their contents (t0 + s0) carry nothing
    // per-frame, so each is written once and never rewritten.
    constexpr std::uint32_t builtInSets = 1 + kSpriteTex;
    // Material sets: per material slot, per FRAME SLOT, per texture variant --
    // and variant 0 is the nil-texture one, hence (1 + kSpriteTex).
    constexpr std::uint32_t materialSets = kMatSlots * kFrames * (1 + kSpriteTex);

    CHECK(pool.descriptorSetMaxNum >= builtInSets + materialSets);
    // A built-in set binds t0 alone; a material set binds t0 plus its declared
    // t1..N.
    CHECK(pool.textureMaxNum >= builtInSets + (1 + kMatTextures) * materialSets);
    // Every set of either family binds s0.
    CHECK(pool.samplerMaxNum >= builtInSets + materialSets);
    // Material sets alone carry constant buffers: b1 (numeric params) and b2
    // (globals). Built-in sets carry neither.
    CHECK(pool.constantBufferMaxNum >= 2 * materialSets);

    // The frame-slot dimension is genuinely IN the material count -- a pool
    // sized for one frame slot would double-book the sets a frame in flight is
    // still reading, which is the failure this multiplication exists to avoid.
    static_assert(kFrames >= 2, "the arena and the material sets are double-buffered");
    CHECK(pool.descriptorSetMaxNum >= kMatSlots * kFrames);
}
