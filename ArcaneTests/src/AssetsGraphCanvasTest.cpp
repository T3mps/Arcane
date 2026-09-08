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

    // The panel is auto-fit by default, which leaves it ~zero-height on frame 1
    // and ~10px after -- and DrawGraphLens EARLY-RETURNS on a non-positive
    // canvas region, so an auto-fit harness would silently exercise the rebuild
    // and the context creation and NOTHING ELSE. Pinning a real size every
    // frame is what makes the node/pin/link/chrome submission actually run,
    // which is the entire reason this test drives the real panel. The
    // grid-phase check at the bottom is the witness that it did.
    const ImVec2 kPanelSize(1200.0f, 640.0f);

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
    // A fake, never-dereferenced texture id for ONE guid, so the node body's
    // thumb branch (ImDrawList::AddImage) runs for real on the sprite while
    // every other node still exercises the kind-icon fallback beside it.
    // Device-less this only records a draw command -- nothing samples it, and
    // the draw data is discarded unrendered.
    services.resolveAssetThumb = [spriteId](const Guid& g) -> std::uint64_t
    { return g == spriteId ? 1ull : 0ull; };

    // What the "Assets" window actually measured on the last frame drawn --
    // the harness's own determinism witness, see the assertions below.
    ImVec2 lastPanelSize(0.0f, 0.0f);

    const auto drawFrame = [&]()
    {
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        // Deterministic region -- see kPanelSize.
        ImGui::SetNextWindowSize(kPanelSize, ImGuiCond_Always);
        DrawAssetsPanel(state, model, &*project, docs, services);
        // Re-Begin the same window to read back what it measured. A second
        // Begin on an already-submitted window APPENDS to it (ImGui's
        // documented multi-Begin behaviour) -- it draws nothing here, it only
        // reads. No SetNextWindowSize this time, so it cannot influence what
        // it is measuring.
        ImGui::Begin("Assets");
        lastPanelSize = ImGui::GetWindowSize();
        ImGui::End();
        ImGui::Render();   // draw data discarded -- no backend
    };

    // Several frames: frame 1 creates the editor context, applies the style
    // and seeds node positions; frame 2+ runs every readback path (live node
    // positions, measured sizes, the link channel). Four is the count
    // GraphCanvasHeadlessTest settled on for exactly this reason.
    for (int frame = 0; frame < 4; ++frame)
        drawFrame();

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
    // Captured BY VALUE: `nodes` is a reference into the projection, which the
    // rebuilds below replace under it.
    const std::size_t everythingNodeCount = nodes.size();

    // ...and the canvas region was REAL, which is what makes the checks above
    // evidence about the DRAW rather than about the build alone. Two witnesses,
    // because neither alone is sufficient:
    //
    //   * the window measured what we pinned. An auto-fit "Assets" window
    //     collapses to its toolbar (~10px of body), and ed::Begin over a
    //     region that small hands its child SkipItems -- at which point every
    //     Dummy and every AssetPill inside a node returns immediately and the
    //     node submission this test exists to exercise silently stops
    //     happening. This is the assertion that goes red if the
    //     SetNextWindowSize above is ever dropped.
    //   * the lens reached the grid draw. DrawGraphGridFallback is only
    //     reached past DrawGraphLens's non-positive-region early return, and
    //     the first thing it does is advance the phase (havePrevView latches
    //     there and nowhere else). This is what rules out the early return.
    //
    // Together: a full-size window AND a lens that ran past its region guard.
    CHECK(lastPanelSize.x == kPanelSize.x);
    CHECK(lastPanelSize.y == kPanelSize.y);
    CHECK(state.graphGrid.havePrevView);

    // THE NON-REBUILD GUARD, measured rather than asserted: AssetGraphViewModel
    // counts its own Build() calls, so four frames over an unchanged model must
    // leave EXACTLY ONE build behind. Deleting the panel's three-way dirty
    // guard turns this into 4 and fails here -- which the previous
    // "graphBuiltStamp == entriesStamp" spelling could not do, since every
    // rebuild makes those two agree.
    CHECK(state.graph.buildEpoch == 1u);

    // A model change re-arms the build; an unchanged one does not. Two more
    // frames prove both halves against the live canvas: exactly one MORE build
    // across the pair, not two, and not zero.
    const std::uint32_t stampBefore = state.graphBuiltStamp;
    model.MarkAllDirty();
    REQUIRE(model.RebuildIfDirty(&project->Registry(), fake.Make()));
    CHECK(model.entriesStamp != stampBefore);
    for (int frame = 0; frame < 2; ++frame)
        drawFrame();
    CHECK(state.graph.buildEpoch == 2u);
    CHECK(state.graphBuiltStamp == model.entriesStamp);

    // The focus guid is the guard's other input: changing it rebuilds once,
    // and the two frames after it do not rebuild again.
    state.graphFocus = sceneId;
    for (int frame = 0; frame < 2; ++frame)
        drawFrame();
    CHECK(state.graph.buildEpoch == 3u);
    CHECK(state.graphBuiltFocus == sceneId);
    // Focus-scoped now, so the graph is a strict subset of the everything-mode
    // build -- proof the focus actually reached the projection, not just the
    // state field.
    CHECK(state.graph.nodes.size() < everythingNodeCount);

    // ---- Task 4: the selection bridge's stamp handshake -------------------
    // The MODEL is the selection authority and the lens acknowledges its own
    // stamp EXACTLY ONCE per external change -- whether or not the selected
    // guid is even in the current scope. Both legs are device-less testable;
    // the centering itself is not (ed:: state is lens-local by ruling 1, so
    // the test cannot see a node's canvas selection), and neither are hover,
    // tooltip or menu, which need real mouse input over real node rects.
    const auto graphHasNode = [&state](const Guid& g)
    {
        return std::any_of(state.graph.nodes.begin(), state.graph.nodes.end(),
                           [&g](const GraphNode& n) { return !n.isOverflow && n.guid == g; });
    };

    // Leg 1: an external selection that IS in the graph. Resetting the seen
    // stamp to 0 is also the "lens never visited since the change" case --
    // the first Graph frame after N Browse clicks has exactly this shape.
    REQUIRE(graphHasNode(materialId));
    state.seenSelectionStampGraph = 0;
    model.Select(materialId);
    REQUIRE(state.seenSelectionStampGraph != model.selectionStamp);
    drawFrame();
    CHECK(state.seenSelectionStampGraph == model.selectionStamp);
    CHECK(model.selected == materialId);
    const std::uint32_t buildsBeforeSelect = state.graph.buildEpoch;

    // Leg 2: an external selection FILTERED OUT of the scope -- one of the
    // hub's leaves, which the scene-focused build does not contain. It must
    // still be acknowledged, silently: an un-acknowledged stamp re-arms the
    // centering every frame, forever.
    REQUIRE_FALSE(graphHasNode(leaves.front()));
    model.Select(leaves.front());
    REQUIRE(state.seenSelectionStampGraph != model.selectionStamp);
    drawFrame();
    CHECK(state.seenSelectionStampGraph == model.selectionStamp);
    // ...and the sync stayed ONE-WAY. Nothing is selected on the canvas here
    // (no mouse input has ever reached it), so a naive per-frame
    // "model.Select(whatever the canvas holds)" would have cleared or
    // corrupted the model's selection by now. It did not.
    CHECK(model.selected == leaves.front());
    // Selecting is not a projection input: no rebuild was triggered by either
    // leg (two frames drawn, zero builds).
    CHECK(state.graph.buildEpoch == buildsBeforeSelect);

    // Idempotence: with the stamp already acknowledged, further frames neither
    // re-arm it nor write the model.
    drawFrame();
    CHECK(state.seenSelectionStampGraph == model.selectionStamp);
    CHECK(model.selected == leaves.front());

    // ---- Task 4 fix round 1: the peek dwell is HELD across a rebuild ------
    // Every node id the interaction block reads (hovered / double-clicked /
    // context-menu) was latched by the PREVIOUS frame's ed::End, so on a
    // rebuild frame they index the OLD node vector and the panel refuses to
    // resolve any of them. The dwell then has to distinguish "the pointer left
    // the node" (drop it) from "the ids were unreadable for one frame" (hold
    // it) -- otherwise every rebuild under the cursor makes the user wait out
    // the delay again for a node they never left. That is the branch measured
    // here, and it is measurable without mouse input because it is the
    // NOTHING-hovered path that differs.
    //
    // SCOPE, precisely: this covers the hold-vs-drop decision and the
    // guid-keyed dwell. It does NOT cover the id-staleness guard itself --
    // with no hovered node there is no stale id to mis-resolve. See the fix
    // report for why the open/menu/hover legs of the guard are not reachable
    // device-less.
    state.graphHoverGuid    = materialId;
    state.graphHoverSeconds = 5.0f;          // a long-elapsed dwell
    drawFrame();                             // ordinary frame, nothing hovered
    CHECK_FALSE(state.graphHoverGuid.IsValid());
    CHECK(state.graphHoverSeconds == 0.0f);

    state.graphHoverGuid    = materialId;
    state.graphHoverSeconds = 5.0f;
    state.graphFocus        = Guid{};        // everything-mode again == a REBUILD frame
    const std::uint32_t epochBeforeRebuild = state.graph.buildEpoch;
    drawFrame();
    CHECK(state.graph.buildEpoch == epochBeforeRebuild + 1u);   // it really did rebuild
    CHECK(state.graphHoverGuid == materialId);                  // ...and the dwell survived
    CHECK(state.graphHoverSeconds == 5.0f);

    // The canvas context is released through the panel's own seam, inside the
    // live ImGui context -- the same ordering EditorApp::Shutdown uses.
    DestroyAssetsPanelCanvas(state);
    CHECK(state.graphCanvas == nullptr);
    CHECK_FALSE(state.graphBuilt);
    // Task 4's interaction state is context-derived too, so it goes with it.
    CHECK(state.seenSelectionStampGraph == 0u);
    CHECK_FALSE(state.graphHoverGuid.IsValid());
    CHECK_FALSE(state.graphMenuGuid.IsValid());

    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);

    fs::remove_all(root, ec);
}
