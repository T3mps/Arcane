// Asset-manager arc (Plan 3, Task 3) diagnostic + regression: drive the REAL
// DrawAssetsPanel with the Graph lens forced on, through device-less ImGui
// frames (null backend -- no window, no GPU; software font atlas).
//
// This is the SAME harness that caught the shader editor's frame-2 EndCreate
// abort (GraphCanvasHeadlessTest.cpp), and it exists here for the same reason:
// the imgui-node-editor canvas only runs inside a live ImGui frame, so no
// headless unit test of a pure model can stand in for it. The failure modes it
// covers are the ones that are invisible on frame 1 and fatal on frame 2+ --
// an armed-but-unserviced editor query, a stale canvas context, a channel
// index past the splitter -- plus the plain "does the whole node/pin/link
// submission path survive at all".
//
// `state.lens = AssetLens::Graph` is set DIRECTLY: until Task 5 flips
// kLensEnabledMask to 0b111 the toolbar's Graph button stays disabled, so this
// programmatic route is the branch's only reachability, which is exactly what
// the task's own step notes.
//
// The fixture is deliberately shaped to hit every node VARIETY the draw path
// branches on, not just the happy one:
//   * plain asset nodes with thumbs/pills, laid out across several layers,
//   * a SCENE that is the project's boot scene (the header's amber pill),
//   * a TOMBSTONE (a reference to a guid no file backs -- ruling 11's ghost),
//   * an OVERFLOW "+N more" companion (a hub over the 20-per-direction
//     breadth cap -- ruling 6), which is also the one node kind whose
//     anchor connector is synthesized by the renderer rather than read off a
//     GraphEdge.

#include <catch2/catch_test_macros.hpp>

#include "Documents/DocumentHost.hpp"
#include "Panels/AssetsPanel.hpp"

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Project/Project.hpp>

#include <imgui.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Arcane;
using namespace Arcane::Editor;
namespace fs = std::filesystem;

namespace
{
    void WriteFile(const fs::path& file, const std::string& text)
    {
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        std::ofstream(file, std::ios::binary) << text;
    }

    // "aaaa0000-0000-4000-8000-0000000000NN" -- a canonical, parseable guid
    // per fixture index, so the test can name every edge endpoint outright.
    Guid FixtureGuid(int n)
    {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "aaaa0000-0000-4000-8000-%012d", n);
        return Guid::FromString(buf).value();
    }

    // The panel's provider seam, faked: this test is about the DRAW path, so
    // the reference topology is stated outright rather than parsed out of the
    // fixture files.
    struct FakeProviders
    {
        std::unordered_map<Guid, std::vector<AssetRef>> refsByGuid;

        AssetPanelProviders Make()
        {
            AssetPanelProviders p;
            p.refsFor = [this](const Guid& g) -> std::optional<std::vector<AssetRef>>
            {
                const auto it = refsByGuid.find(g);
                return it == refsByGuid.end() ? std::vector<AssetRef>{} : it->second;
            };
            p.cookStateFor = [](const Guid&) { return CookState::Cooked; };
            p.surfaceFor   = [](const Guid&) -> std::optional<MaterialSurface>
            { return MaterialSurface::Sprite; };
            return p;
        }
    };
}

TEST_CASE("Assets panel Graph lens survives device-less ImGui frames", "[editor][graphcanvas]")
{
    const fs::path root = fs::temp_directory_path() / "arcane_assets_graph_canvas_test";
    std::error_code ec;
    fs::remove_all(root, ec);

    // A real project, because DrawAssetsPanel's body refuses to draw any lens
    // without one ("No project open") -- the Graph branch included.
    {
        auto created = Project::Create(root, "GraphCanvas");
        REQUIRE(created.has_value());
    }

    const fs::path content = root / "Content";

    // --- the graph's spine: scene -> material -> sprite, plus a dangling ref
    const Guid sceneId    = FixtureGuid(1);
    const Guid materialId = FixtureGuid(2);
    const Guid spriteId   = FixtureGuid(3);
    const Guid missingId  = FixtureGuid(4);   // NO file -- the tombstone
    const Guid hubId      = FixtureGuid(5);   // over the breadth cap

    WriteFile(content / "scenes" / "main.arcscene",
              R"({"id":")" + sceneId.ToString() + R"(","version":4,"entities":[]})");
    WriteFile(content / "materials" / "base.arcmat",
              R"({"id":")" + materialId.ToString() + R"(","type":"material","kind":"sprite"})");
    WriteFile(content / "sprites" / "hero.arcsprite",
              R"({"id":")" + spriteId.ToString() + R"(","type":"sprite","name":"Hero"})");
    WriteFile(content / "materials" / "hub.arcmat",
              R"({"id":")" + hubId.ToString() + R"(","type":"material","kind":"sprite"})");

    // 22 leaves under one hub: two more than the view model's 20-per-node
    // per-direction breadth cap, so the build has to emit a "+2 more"
    // companion and the renderer has to synthesize its anchor connector.
    constexpr int kLeafCount = 22;
    std::vector<Guid> leaves;
    leaves.reserve(kLeafCount);
    for (int i = 0; i < kLeafCount; ++i)
    {
        const Guid g = FixtureGuid(100 + i);
        leaves.push_back(g);
        WriteFile(content / "data" / ("leaf" + std::to_string(i) + ".json"),
                  R"({"id":")" + g.ToString() + R"("})");
    }

    auto project = Project::Open(root);
    REQUIRE(project.has_value());
    // The boot scene drives the node header's amber "boot" pill.
    REQUIRE(project->SetBootScene(sceneId));

    FakeProviders fake;
    fake.refsByGuid[sceneId]    = { { materialId, AssetRefKind::References },
                                    { spriteId,   AssetRefKind::References } };
    fake.refsByGuid[spriteId]   = { { materialId, AssetRefKind::DerivesFrom } };
    // A reference to a guid nothing backs: the index keeps a node for it with
    // exists == false, which is the tombstone the Graph lens draws.
    fake.refsByGuid[materialId] = { { missingId, AssetRefKind::References } };
    {
        std::vector<AssetRef> hubRefs;
        hubRefs.reserve(leaves.size());
        for (const Guid& g : leaves)
            hubRefs.push_back({ g, AssetRefKind::References });
        fake.refsByGuid[hubId] = std::move(hubRefs);
    }

    AssetPanelModel model;
    model.MarkAllDirty();
    REQUIRE(model.RebuildIfDirty(&project->Registry(), fake.Make()));
    REQUIRE(model.Entries().size() >= static_cast<std::size_t>(4 + kLeafCount));

    // Device-less ImGui: no backend; a 1x1 font texture satisfies NewFrame.
    IMGUI_CHECKVERSION();
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.IniFilename = nullptr;
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);   // software build, no upload

    AssetsPanelState state;
    state.lens = AssetLens::Graph;
    // Nil focus = "everything" (ruling 6), which is what puts the hub, its 22
    // leaves, the spine and the tombstone in ONE build.
    DocumentHost docs;
    AssetsPanelServices services{};

    // Several frames: frame 1 creates the editor context, applies the style
    // and seeds node positions; frame 2+ runs every readback path (live node
    // positions, measured sizes, the link channel). Four is the count
    // GraphCanvasHeadlessTest settled on for exactly this reason.
    for (int frame = 0; frame < 4; ++frame)
    {
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        DrawAssetsPanel(state, model, &*project, docs, services);
        ImGui::Render();   // draw data discarded -- no backend
    }

    // The lens really drew a graph, and really drew every variety: a bare
    // "it did not crash" over an empty canvas would prove nothing.
    CHECK(state.graphBuilt);
    CHECK(state.graph.realNodeCount > 0);
    CHECK_FALSE(state.graph.edges.empty());
    const auto& nodes = state.graph.nodes;
    CHECK(std::any_of(nodes.begin(), nodes.end(),
                      [](const GraphNode& n) { return n.isTombstone; }));
    CHECK(std::any_of(nodes.begin(), nodes.end(),
                      [](const GraphNode& n) { return n.isOverflow && n.overflowCount > 0; }));
    // The graph is NOT rebuilt per frame: four frames over an unchanged model
    // leave exactly the stamp the first build recorded.
    CHECK(state.graphBuiltStamp == model.entriesStamp);

    // A model change re-arms the build; an unchanged one does not. Two more
    // frames prove both halves against the live canvas.
    const std::uint32_t stampBefore = state.graphBuiltStamp;
    model.MarkAllDirty();
    REQUIRE(model.RebuildIfDirty(&project->Registry(), fake.Make()));
    CHECK(model.entriesStamp != stampBefore);
    for (int frame = 0; frame < 2; ++frame)
    {
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        DrawAssetsPanel(state, model, &*project, docs, services);
        ImGui::Render();
    }
    CHECK(state.graphBuiltStamp == model.entriesStamp);

    // The canvas context is released through the panel's own seam, inside the
    // live ImGui context -- the same ordering EditorApp::Shutdown uses.
    DestroyAssetsPanelCanvas(state);
    CHECK(state.graphCanvas == nullptr);
    CHECK_FALSE(state.graphBuilt);

    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);

    fs::remove_all(root, ec);
}
