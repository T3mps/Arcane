// RenderGraph declaration API (Phase 2, Task 3): headless [nri] coverage of
// pure declaration invariants -- no nri device, no Compile(), no Execute()
// (Tasks 4/6). See RenderGraph.hpp's file header for the design reference
// (Filament's frame graph, Apache-2.0) and the eager-setup/deferred-execute
// model these cases rely on: a node's Setup callback runs synchronously
// inside AddNode(), so a Setup that captures the RenderGraph by reference
// can call SetColorAttachments/SetDepthAttachment (which target "the node
// AddNode() is currently declaring") and every handle CreateTexture/
// CreateBuffer/ImportTexture/ImportBuffer returns is immediately usable by
// the time AddNode() returns.
#include <NRI.h>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/Nri/RenderGraph.hpp>

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
