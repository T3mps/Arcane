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
// `state.lens = AssetLens::Graph` is set DIRECTLY. That was the branch's ONLY
// reachability until Task 5 flipped kLensEnabledMask to 0b111; it is now one
// route among three (toolbar button, Status's "Focus in Graph", this), and it
// stays the one this harness uses because a device-less frame has no mouse to
// click the other two with. The mask itself is a file-local constexpr in
// AssetsPanel.cpp with no exported reader, so it is not assertable from here --
// its witness is the render capture, not this test.
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
#include "Panels/AssetGraphPanel.hpp"     // AssetsGraphProjectionIsCurrent, DestroyAssetGraphPanelCanvas (Task 5, panel-split)
#include "Panels/AssetsPanel.hpp"
#include "Panels/CreateAssetDialog.hpp"   // CreateAssetKind: what the ghost menu raises

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Project/Project.hpp>

#include <imgui.h>
// The panel exposes its canvas context as a `void*` on purpose
// (AssetsPanel.hpp:340) so that production callers never need this header.
// This test reaches through it anyway, the same way AssetsPanel.cpp itself
// does internally (a reinterpret to ax::NodeEditor::EditorContext*), to query
// the REAL ed::Config the panel's context ended up with -- see the zoom-table
// regression check below.
#include <imgui_node_editor.h>
// The 2026-09-09 desk-pass cases at the bottom read ImGui's OWN id-conflict
// verdict (ImGuiContext::DebugDrawIdConflictsId) and locate an open popup's
// window -- both context internals, neither exposed by imgui.h.
#include <imgui_internal.h>

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
    // Task 5: the panel seeds `graphFocus` from the project's BOOT SCENE on
    // the first frame of a project, which would scope this harness to the
    // scene and leave the hub, its leaves and the overflow companion out of
    // the build. Pre-declaring the seed spent opts THIS state out of it and
    // keeps everything-mode below; the seed itself is asserted at the bottom,
    // from the post-DestroyAssetGraphPanelCanvas state -- which is exactly the
    // shape a project switch hands the panel.
    state.graphFocusSeeded = true;
    // Nil focus = "everything" (ruling 6), which is what puts the hub, its 22
    // leaves, the spine and the tombstone in ONE build.
    DocumentHost docs;
    AssetPanelServices services{};
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
    // ...and what the panel ASKED THE HOST FOR on that frame. The returned
    // actions are the panel's only channel to the app, so an idle frame that
    // raises one is a phantom request (Task 6's create bracket is checked
    // against exactly that below).
    AssetPanelActions lastActions;

    const auto drawFrame = [&]()
    {
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        // Deterministic region -- see kPanelSize.
        ImGui::SetNextWindowSize(kPanelSize, ImGuiCond_Always);
        lastActions = DrawAssetsPanel(state, model, &*project, docs, services);
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

    // ---- 2026-09-09 fix: zoom-table parity with the shader editor ---------
    // The Graph lens's ed::Config used to fall through to the vendored
    // library's own default zoom table (0.1-8.0, imgui_node_editor.cpp:
    // 3309-3312) because AssetsPanel.cpp never called ApplyZoomLevels. Wheel
    // zoom could then reach 8x, bilinearly magnifying the 12-14px canvas text
    // into unmistakable blur -- the shader editor's canvases install
    // GraphZoomLevels.hpp's kZoomLevels table instead and cap at 2.0x. This
    // queries the REAL ed::Config the panel's context ended up with (through
    // the public node-editor header, the same reinterpret AssetsPanel.cpp
    // performs on the `void*` it hands back per AssetsPanel.hpp:340) and
    // asserts the ceiling matches the shader editor's, not the library's.
    {
        auto* ctx = static_cast<ax::NodeEditor::EditorContext*>(state.graphCanvas);
        REQUIRE(ctx != nullptr);
        const ax::NodeEditor::Config& cfg = ax::NodeEditor::GetConfig(ctx);
        // An empty CustomZoomLevels means the config never installed a custom
        // table at all -- the exact state that let the library's 8.0 default
        // through. A non-empty table is the fix's precondition as much as the
        // max-value check below is its outcome.
        REQUIRE(cfg.CustomZoomLevels.Size > 0);
        float maxZoom = 0.0f;
        for (int i = 0; i < cfg.CustomZoomLevels.Size; ++i)
            maxZoom = std::max(maxZoom, cfg.CustomZoomLevels[i]);
        CHECK(maxZoom == 2.0f);   // parity with the shader editor's kZoomLevels ceiling
    }

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

    // ---- Task 5 fix round (review I1): the bottom bar's N gate -----------
    // DrawBottomBar prints the Graph line's N only when the projection on
    // screen was built for the CURRENT focus and the CURRENT entries; on any
    // other frame it prints an em dash instead of a stale or fabricated count
    // (spec §13 -- never render an unknown as a zero).
    //
    // WHAT IS AND IS NOT COVERED HERE, precisely. The transition itself is
    // NOT device-less assertable: it is produced by a MOUSE CLICK on the
    // Status lens's "Focus in Graph" button, nested inside an
    // already-dispatched draw body with no seam to reach, and the string the
    // bar prints is draw-body output with no return value to read. What IS
    // cheaply assertable -- and is what the gate actually keys on -- is the
    // gate CONDITION, in exactly the state that transition frame leaves
    // behind: a lens that is Graph, a focus that changed, and a build that
    // did not run this frame because the body drawn was another lens's.
    //
    // The predicate below is THE PANEL'S OWN
    // (AssetsGraphProjectionIsCurrent, which DrawBottomBar calls to decide
    // between the count and the em dash) -- not a restatement of it here. A
    // mirrored conjunction would keep passing if the panel dropped a conjunct,
    // which is the regression these legs exist to catch.
    const auto graphCurrent = [&] { return AssetsGraphProjectionIsCurrent(state, model); };

    // Baseline: a steady-state Graph frame IS current, so the real N prints.
    state.lens       = AssetLens::Graph;
    state.graphFocus = sceneId;
    drawFrame();
    CHECK(graphCurrent());

    // The Focus-in-Graph shape: this frame's BODY was Status (so no rebuild
    // happened) while the focus moved. That is the state the bar sees on the
    // transition frame, and the gate must refuse it.
    state.lens = AssetLens::Status;
    const std::uint32_t epochBeforeStatus = state.graph.buildEpoch;
    state.graphFocus = materialId;               // "focus" a different asset
    drawFrame();                                 // Status body -- DrawGraphLens never runs
    CHECK(state.graph.buildEpoch == epochBeforeStatus);   // ...proven: no rebuild
    CHECK_FALSE(graphCurrent());                 // ...so N is unknown -> em dash

    // And it re-arms: the next Graph frame rebuilds for the new focus, after
    // which the real N is honest again.
    state.lens = AssetLens::Graph;
    drawFrame();
    CHECK(state.graph.buildEpoch == epochBeforeStatus + 1u);
    CHECK(graphCurrent());

    // The third conjunct on its own: entries moved under a frame whose body
    // was not the Graph lens. The focus is untouched here, so ONLY the stamp
    // can close the gate -- which is what makes this leg evidence about the
    // stamp rather than about the focus a second time.
    state.lens = AssetLens::Status;
    model.MarkAllDirty();
    REQUIRE(model.RebuildIfDirty(&project->Registry(), fake.Make()));
    drawFrame();
    CHECK(state.graphBuiltFocus == state.graphFocus);   // focus leg still satisfied...
    CHECK_FALSE(graphCurrent());                        // ...and the gate still closed
    state.lens = AssetLens::Graph;

    // ---- Task 6: the create bracket runs its no-query path every frame ----
    // The Graph lens opens an `ed::BeginCreate` / `ed::EndCreate` bracket every
    // frame for the pin-drag "Derive Instance..." gesture, and EndCreate is
    // called UNCONDITIONALLY. That is not a style preference:
    // CreateItemAction::Begin() arms `m_InActive` even on the frame it returns
    // false, and its first statement is `IM_ASSERT(false == m_InActive)` -- so
    // a bracket that only ends INSIDE the `if` is fine on frame 1 and ABORTS
    // THE PROCESS on frame 2 (ShaderEditorDocument.cpp:5559-5561; the desk
    // crash GraphCanvasHeadlessTest.cpp was written for, and the reason this
    // harness exists at all).
    //
    // WHAT THESE FRAMES PROVE, precisely. Device-less there is no mouse, so no
    // pin drag, so BeginCreate returns false and the bracket runs its NO-QUERY
    // path -- which is exactly the path the folklore is about. The assertion
    // is REACHING THE LINE AFTER THEM: under a conditional EndCreate the
    // second consecutive Graph frame aborts under IM_ASSERT in this Debug
    // build and the case never reports at all. (Every earlier block in this
    // case already draws consecutive Graph frames, so the coverage is not new
    // -- these frames and this comment make the witness explicit rather than
    // incidental, and the two checks below turn "did not abort" into a
    // positive statement about the bracket's side effects.)
    //
    // WHAT THEY DO NOT PROVE: the accept/stash/menu half. QueryNewNode only
    // reports for a REAL pin drag released over the canvas, which needs mouse
    // input at a pin's screen rect -- a position that lives inside the
    // library's pan/zoom canvas and is not exported. That half is traced
    // structurally in the task report and desk-verified on Task 7's checklist.
    for (int frame = 0; frame < 3; ++frame)
        drawFrame();
    // Nothing was dragged, so nothing may be stashed: a bracket that stashed
    // on an idle frame would arm a ghost menu about an arbitrary node every
    // frame.
    CHECK_FALSE(state.graphWireGuid.IsValid());
    CHECK_FALSE(state.graphWireDerivable);
    // Nor may the IN-FLIGHT source latch: `graphDragGuid` is what keeps the
    // dashed curve alive while the pointer crosses a node body, and it is
    // cleared on every frame BeginCreate reports no live action -- which,
    // device-less, is every frame. A latch that survived an idle frame would
    // paint a wire from a node to the cursor with no drag behind it.
    CHECK_FALSE(state.graphDragGuid.IsValid());
    CHECK_FALSE(state.graphDragRight);
    // ...and the clear really is a per-frame WRITE, not just an untouched
    // default: seed it by hand and one ordinary frame must retire it.
    state.graphDragGuid  = materialId;
    state.graphDragRight = true;
    drawFrame();
    CHECK_FALSE(state.graphDragGuid.IsValid());
    CHECK_FALSE(state.graphDragRight);
    // ...and no create request reached the host either. `requestCreateKind`
    // and `createPrefillParent` are the pair the gesture's one menu entry
    // writes (Task 6 is their first producer), so an idle frame raising either
    // would open the Create dialog unbidden.
    CHECK(lastActions.requestCreateKind == -1);
    CHECK_FALSE(lastActions.createPrefillParent.IsValid());

    // The canvas context is released through the panel's own seam, inside the
    // live ImGui context -- the same ordering EditorApp::Shutdown uses.
    DestroyAssetGraphPanelCanvas(state);
    CHECK(state.graphCanvas == nullptr);
    CHECK_FALSE(state.graphBuilt);
    // Task 4's interaction state is context-derived too, so it goes with it.
    CHECK(state.seenSelectionStampGraph == 0u);
    CHECK_FALSE(state.graphHoverGuid.IsValid());
    CHECK_FALSE(state.graphMenuGuid.IsValid());
    // Task 6's gesture stash likewise: it names an asset of the OUTGOING
    // project, and this seam IS the project switch. Set by hand first, because
    // no device-less frame can produce one -- the check is about the seam, not
    // about the gesture.
    state.graphWireGuid      = materialId;
    state.graphWireDerivable = true;
    state.graphDragGuid      = materialId;
    state.graphDragRight     = true;
    DestroyAssetGraphPanelCanvas(state);
    CHECK_FALSE(state.graphWireGuid.IsValid());
    CHECK_FALSE(state.graphWireDerivable);
    CHECK_FALSE(state.graphDragGuid.IsValid());
    CHECK_FALSE(state.graphDragRight);

    // ---- Task 5: the boot-scene focus seed, and its project-switch reset --
    // DestroyAssetGraphPanelCanvas IS the panel's project-switch seam (EditorApp
    // calls it beside AssetPanelModel::ResetForProjectSwitch), so the state
    // is now shaped exactly like a freshly-switched-to project's: no focus,
    // and the seed re-armed. One frame later the panel must have scoped
    // itself to THIS project's boot scene -- not to "everything", and not to
    // the guid the scope happened to hold before the switch.
    CHECK_FALSE(state.graphFocusSeeded);
    CHECK_FALSE(state.graphFocus.IsValid());
    drawFrame();
    CHECK(state.graphFocusSeeded);
    CHECK(state.graphFocus == sceneId);          // == project->Manifest().bootScene
    // ...and it is a ONE-SHOT seed: a further frame must not re-assert the
    // boot scene over a user's own "everything" pick (the combo's first
    // entry), which is the whole reason the flag exists beside the guid.
    state.graphFocus = Guid{};
    drawFrame();
    CHECK_FALSE(state.graphFocus.IsValid());

    // The seed frames above created a fresh canvas context; release it the
    // same way, inside the live ImGui context.
    DestroyAssetGraphPanelCanvas(state);
    CHECK(state.graphCanvas == nullptr);

    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);

    fs::remove_all(root, ec);
}

// ===========================================================================
// 2026-09-09 desk-pass defects (Plan 3): the two bugs the user raised at a
// live editor on the Graph lens. Both need REAL MOUSE INPUT over REAL node
// rects, which the case above explicitly declined to drive ("neither are
// hover, tooltip or menu, which need real mouse input over real node rects").
// They are drivable after all: ed::GetNodePosition/GetNodeSize/CanvasToScreen
// are public, so a device-less frame CAN aim at a node or a pin, and
// io.AddMousePosEvent/AddMouseButtonEvent supply the input. That is the seam
// these two cases open.
// ===========================================================================

namespace
{
    // One panel frame with the window pinned at a KNOWN screen origin, which
    // is what makes an absolute mouse coordinate mean something here.
    struct GraphMouseHarness
    {
        ImVec2 origin{ 0.0f, 0.0f };
        ImVec2 size{ 1200.0f, 640.0f };

        AssetsPanelState*   state = nullptr;
        AssetPanelModel*    model = nullptr;
        const Project*      project = nullptr;
        DocumentHost*       docs = nullptr;
        AssetPanelServices services{};
        AssetPanelActions  lastActions;

        void Frame()
        {
            ImGuiIO& io = ImGui::GetIO();
            io.DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(origin, ImGuiCond_Always);
            ImGui::SetNextWindowSize(size, ImGuiCond_Always);
            lastActions = DrawAssetsPanel(*state, *model, project, *docs, services);
            ImGui::Render();
        }

        void MoveTo(const ImVec2& p) { ImGui::GetIO().AddMousePosEvent(p.x, p.y); }
        void Button(bool down)       { ImGui::GetIO().AddMouseButtonEvent(0, down); }

        // Node geometry, in SCREEN space, read back from the live canvas.
        // The panel's node ids are index+1 (AssetsPanel.cpp's GraphNodeIdOf).
        ImVec2 NodeScreenCentre(std::uint64_t nodeId)
        {
            auto* ed_ctx = static_cast<ax::NodeEditor::EditorContext*>(state->graphCanvas);
            ax::NodeEditor::SetCurrentEditor(ed_ctx);
            const ImVec2 pos = ax::NodeEditor::GetNodePosition(ax::NodeEditor::NodeId(nodeId));
            const ImVec2 sz  = ax::NodeEditor::GetNodeSize(ax::NodeEditor::NodeId(nodeId));
            const ImVec2 out = ax::NodeEditor::CanvasToScreen(
                ImVec2(pos.x + sz.x * 0.5f, pos.y + sz.y * 0.5f));
            ax::NodeEditor::SetCurrentEditor(nullptr);
            return out;
        }

        // The node's canvas rect, in screen space. Used to prove a node is
        // really on the canvas rather than clipped out of it.
        ImVec2 NodeScreenMin(std::uint64_t nodeId)
        {
            auto* ed_ctx = static_cast<ax::NodeEditor::EditorContext*>(state->graphCanvas);
            ax::NodeEditor::SetCurrentEditor(ed_ctx);
            const ImVec2 out = ax::NodeEditor::CanvasToScreen(
                ax::NodeEditor::GetNodePosition(ax::NodeEditor::NodeId(nodeId)));
            ax::NodeEditor::SetCurrentEditor(nullptr);
            return out;
        }

        ImVec2 NodeScreenMax(std::uint64_t nodeId)
        {
            auto* ed_ctx = static_cast<ax::NodeEditor::EditorContext*>(state->graphCanvas);
            ax::NodeEditor::SetCurrentEditor(ed_ctx);
            const ImVec2 pos = ax::NodeEditor::GetNodePosition(ax::NodeEditor::NodeId(nodeId));
            const ImVec2 sz  = ax::NodeEditor::GetNodeSize(ax::NodeEditor::NodeId(nodeId));
            const ImVec2 out = ax::NodeEditor::CanvasToScreen(
                ImVec2(pos.x + sz.x, pos.y + sz.y));
            ax::NodeEditor::SetCurrentEditor(nullptr);
            return out;
        }

        ImVec2 NodeRightPinScreen(std::uint64_t nodeId)
        {
            auto* ed_ctx = static_cast<ax::NodeEditor::EditorContext*>(state->graphCanvas);
            ax::NodeEditor::SetCurrentEditor(ed_ctx);
            const ImVec2 pos = ax::NodeEditor::GetNodePosition(ax::NodeEditor::NodeId(nodeId));
            const ImVec2 sz  = ax::NodeEditor::GetNodeSize(ax::NodeEditor::NodeId(nodeId));
            const ImVec2 out = ax::NodeEditor::CanvasToScreen(
                ImVec2(pos.x + sz.x, pos.y + sz.y * 0.5f));
            ax::NodeEditor::SetCurrentEditor(nullptr);
            return out;
        }

        // The LEFT (dependencies/outbound) pin, mirroring NodeRightPinScreen
        // above -- GraphLeftPinId's handle, used by the non-derivable leg
        // (AssetsPanel.cpp's `isRightPin` gate) rather than the derive one.
        ImVec2 NodeLeftPinScreen(std::uint64_t nodeId)
        {
            auto* ed_ctx = static_cast<ax::NodeEditor::EditorContext*>(state->graphCanvas);
            ax::NodeEditor::SetCurrentEditor(ed_ctx);
            const ImVec2 pos = ax::NodeEditor::GetNodePosition(ax::NodeEditor::NodeId(nodeId));
            const ImVec2 sz  = ax::NodeEditor::GetNodeSize(ax::NodeEditor::NodeId(nodeId));
            const ImVec2 out = ax::NodeEditor::CanvasToScreen(
                ImVec2(pos.x, pos.y + sz.y * 0.5f));
            ax::NodeEditor::SetCurrentEditor(nullptr);
            return out;
        }

        ImVec2 CanvasPointScreen(const ImVec2& canvasPos)
        {
            auto* ed_ctx = static_cast<ax::NodeEditor::EditorContext*>(state->graphCanvas);
            ax::NodeEditor::SetCurrentEditor(ed_ctx);
            const ImVec2 out = ax::NodeEditor::CanvasToScreen(canvasPos);
            ax::NodeEditor::SetCurrentEditor(nullptr);
            return out;
        }
    };

    // A material fixture whose node COUNT is chosen so the shipped id scheme
    // is guaranteed to collide (see the case below for the arithmetic).
    struct MaterialHubFixture
    {
        fs::path root;
        std::optional<Project> project;
        FakeProviders fake;
        Guid hub;
        std::vector<Guid> referencers;

        void Build(const char* name, int referencerCount)
        {
            root = fs::temp_directory_path() / name;
            std::error_code ec;
            fs::remove_all(root, ec);
            REQUIRE(Project::Create(root, "GraphMouse").has_value());
            const fs::path content = root / "Content";

            hub = FixtureGuid(1);
            WriteFile(content / "materials" / "hub.arcmat",
                      R"({"id":")" + hub.ToString() + R"(","type":"material","kind":"sprite"})");
            for (int i = 0; i < referencerCount; ++i)
            {
                const Guid g = FixtureGuid(10 + i);
                referencers.push_back(g);
                WriteFile(content / "materials" / ("ref" + std::to_string(i) + ".arcmat"),
                          R"({"id":")" + g.ToString() + R"(","type":"material","kind":"sprite"})");
                fake.refsByGuid[g] = { { hub, AssetRefKind::References } };
            }
            project = Project::Open(root);
            REQUIRE(project.has_value());
        }
    };
}

// ---------------------------------------------------------------------------
// DESK-PASS DEFECT 1 -- "Programmer error: 2 visible items with conflicting
// ID!" on the Graph lens, with the detector's red boxes over one node's PIN
// region and a DIFFERENT node's body.
//
// WHY THAT PAIR, exactly. imgui-node-editor hit-tests every node and every pin
// with an ImGui item whose id is the hex of the object's RAW numeric id and
// NOTHING ELSE -- `snprintf(idString, 32, "%p", id.AsPointer())`
// (imgui_node_editor.cpp:2408). ObjectId's Node/Pin/Link TYPE TAG is a
// separate member (imgui_node_editor_internal.h:152-177) and never reaches
// that string, so a node and a pin that happen to share a NUMBER share an
// ImGui id -- two visible items, one id, which is precisely what ImGui 1.92's
// detector reports (imgui.cpp:5076-5081, 11877).
//
// This lens hands it exactly that: node ids are index+1 (1..N) and pin ids are
// nodeId*4+{1,2}, so pin ids land at 5,6,9,10,13,14,... -- INSIDE the node
// range as soon as the graph has five or more nodes.
//
// The fixture below pins the arithmetic down: a hub material referenced by six
// others is SEVEN nodes, so node #1's RIGHT pin (1*4+2 = 6) collides with node
// #6's body. Every asset is a MATERIAL because the derive affordance gives
// every material a right pin unconditionally (AssetsPanel.cpp:3944-3951), so
// the colliding pin is guaranteed to be submitted rather than depending on
// which node the projection happened to order first.
//
// The detector is read the way ImGui itself reads it: hover one of the two,
// and on the frame after next `g.DebugDrawIdConflictsId` is non-zero
// (imgui.cpp:5773-5776 -- the count accumulated while hovering is examined one
// NewFrame later).
TEST_CASE("Assets panel Graph lens submits no conflicting ImGui item ids",
          "[editor][graphcanvas]")
{
    MaterialHubFixture fx;
    fx.Build("arcane_assets_graph_idconflict_test", /*referencerCount=*/6);

    AssetPanelModel model;
    model.MarkAllDirty();
    REQUIRE(model.RebuildIfDirty(&fx.project->Registry(), fx.fake.Make()));

    IMGUI_CHECKVERSION();
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600.0f, 900.0f);
    io.IniFilename = nullptr;
    // The detector under test. On by default (imgui.cpp:1718); asserted rather
    // than assumed, because a green run with it off would prove nothing.
    REQUIRE(io.ConfigDebugHighlightIdConflicts);
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);

    AssetsPanelState state;
    state.lens = AssetLens::Graph;
    state.graphFocusSeeded = true;   // nil focus == everything-mode
    DocumentHost docs;

    GraphMouseHarness hw;
    hw.state = &state;
    hw.model = &model;
    hw.project = &*fx.project;
    hw.docs = &docs;
    hw.services.resolveAssetThumb = [](const Guid&) -> std::uint64_t { return 0ull; };

    // Settle: context creation, layout seed, then live readback frames.
    for (int i = 0; i < 4; ++i)
        hw.Frame();

    const std::size_t nodeCount = state.graph.nodes.size();
    // Seven real nodes, no overflow companion, no tombstone -- the exact
    // shape the collision arithmetic above is stated against.
    REQUIRE(nodeCount == 7u);

    // The two ids that MUST collide under the shipped scheme: node #6's body
    // and node #1's right pin (1*4+2). Probing the NODE BODY rather than the
    // 9px pin is deliberate -- the detector fires from hovering EITHER of the
    // two items, and a node body is an unmissable target.
    const ImVec2 probe = hw.NodeScreenCentre(6);
    // The probe has to actually land on the canvas, or a green result would
    // only mean "the mouse was nowhere".
    REQUIRE(probe.x > hw.origin.x);
    REQUIRE(probe.x < hw.origin.x + hw.size.x);
    REQUIRE(probe.y > hw.origin.y);
    REQUIRE(probe.y < hw.origin.y + hw.size.y);

    // ...and so must the COLLISION PARTNER. Node #6's body and node #1's RIGHT
    // PIN are the duplicated pair, and a conflict needs BOTH items submitted.
    // If a future layout change pushed node #1 out of the canvas clip rect, the
    // library's own hit-test button would bail on the clipped ItemAdd
    // (imgui_node_editor.cpp:2393-2394), the second of the two items would never
    // be submitted, and this case would go VACUOUSLY green. Pinning node #1's
    // whole rect inside the panel is what keeps a pass here meaning "no
    // conflict" rather than "only one of the two items existed".
    const ImVec2 partnerMin = hw.NodeScreenMin(1);
    const ImVec2 partnerMax = hw.NodeScreenMax(1);
    REQUIRE((partnerMin.x > hw.origin.x && partnerMin.y > hw.origin.y));
    REQUIRE((partnerMax.x < hw.origin.x + hw.size.x &&
             partnerMax.y < hw.origin.y + hw.size.y));

    hw.MoveTo(probe);
    hw.Frame();                       // hover registers
    hw.Frame();                       // duplicate ids counted against it
    hw.Frame();                       // NewFrame publishes the verdict

    // The witness that the probe worked at all: something under the cursor
    // took an ImGui id. Without this a mis-aimed probe reads as "no conflict".
    CHECK(ImGui::GetCurrentContext()->HoveredIdPreviousFrame != 0u);

    // THE ASSERTION. Non-zero is ImGui saying "two visible items, one id" --
    // the desk report's tooltip, reproduced.
    CHECK(ImGui::GetCurrentContext()->DebugDrawIdConflictsId == 0u);
    CHECK(ImGui::GetCurrentContext()->HoveredIdPreviousFrameItemCount <= 1);

    // ...AND THE SECOND DESK DEFECT, which is this same fault's other face.
    // ImGui's report is NOT passive chrome. It is drawn through
    // BeginErrorTooltip (imgui.cpp:11921-11943), which -- alone among every
    // tooltip in the library -- omits ImGuiWindowFlags_NoInputs (compare
    // BeginTooltipEx, imgui.cpp:12799, which sets it): it cannot set it,
    // because it hosts a clickable "Item Picker" SmallButton. It then forces
    // itself to the display front AND the focus front on every frame it is
    // up, positioned at the cursor like any other tooltip.
    //
    // So while a conflict is being reported there is an INPUT-TAKING,
    // always-topmost window sitting on the pointer. It owns g.HoveredWindow,
    // and a click near the cursor lands on IT rather than on whatever is
    // underneath -- which is exactly the user's second report: "the drag
    // works, it shows the button, but I can't click it". The ghost menu is
    // what is underneath.
    //
    // Asserting the WINDOW's existence rather than the swallowed click keeps
    // this a statement about the cause: no conflict, no error tooltip, nothing
    // over the cursor to eat the click.
    ImGuiWindow* errorTip = ImGui::FindWindowByName("##Tooltip_Error");
    const bool errorTipUp = errorTip != nullptr && (errorTip->Active || errorTip->WasActive);
    CHECK_FALSE(errorTipUp);

    DestroyAssetGraphPanelCanvas(state);
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);

    std::error_code ec;
    fs::remove_all(fx.root, ec);
}

// ---------------------------------------------------------------------------
// DESK-PASS DEFECT 2 -- "the drag works, it shows the button, but I can't
// click it": the pin-drag ghost menu's `Derive Instance...` entry never
// derives, from any asset.
//
// The gesture SPANS FRAMES, so this drives the whole thing: hover the source
// material's RIGHT (dependents) pin, press, drag past ImGui's threshold onto
// empty canvas, release, and let the library's Create stage land -- then click
// the entry the ghost menu puts under the cursor.
//
// READ THIS BEFORE "SIMPLIFYING" THE FIXTURE. SEVEN nodes on purpose -- squarely
// INSIDE the colliding regime under the OLD id scheme (node #1's right pin was
// 1*4+2 = 6, and node #6 exists at seven nodes). Shrinking the fixture below five
// would take this case OUT of that regime and destroy what it is for.
//
// WHAT THIS CASE IS AND IS NOT, stated here because the distinction is easy to
// lose. It PASSED BEFORE the id-space fix as well as after, so it is NOT a
// RED->GREEN witness for that fix -- the case above is. It is two other things:
//
//   * COVERAGE for the one hop Task 6 shipped with no live verification (the
//     accept/stash/menu half of the gesture; the original case in this file says
//     outright that it does not cover it), and
//   * the DIAGNOSTIC that told the two desk reports apart. The second report read
//     like a broken gesture. This case driving that same gesture GREEN -- on a
//     canvas whose id space collides, but with no error tooltip up over the
//     cursor at the moment of the click -- is what proved the gesture logic was
//     correct and something was sitting ON TOP of the menu eating the click. The
//     case above names what.
TEST_CASE("Assets panel Graph lens pin-drag derives an instance",
          "[editor][graphcanvas]")
{
    MaterialHubFixture fx;
    fx.Build("arcane_assets_graph_pindrag_test", /*referencerCount=*/6);

    AssetPanelModel model;
    model.MarkAllDirty();
    REQUIRE(model.RebuildIfDirty(&fx.project->Registry(), fx.fake.Make()));

    IMGUI_CHECKVERSION();
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600.0f, 900.0f);
    io.IniFilename = nullptr;
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);

    AssetsPanelState state;
    state.lens = AssetLens::Graph;
    state.graphFocusSeeded = true;
    DocumentHost docs;

    GraphMouseHarness hw;
    hw.state = &state;
    hw.model = &model;
    hw.project = &*fx.project;
    hw.docs = &docs;
    hw.services.resolveAssetThumb = [](const Guid&) -> std::uint64_t { return 0ull; };

    for (int i = 0; i < 4; ++i)
        hw.Frame();

    REQUIRE(state.graph.nodes.size() == 7u);
    // The hub is a MATERIAL and therefore carries a right pin unconditionally
    // (AssetsPanel.cpp:3984-3991) -- the handle the gesture starts from.
    std::size_t hubIndex = state.graph.nodes.size();
    for (std::size_t i = 0; i < state.graph.nodes.size(); ++i)
        if (state.graph.nodes[i].guid == fx.hub)
            hubIndex = i;
    REQUIRE(hubIndex < state.graph.nodes.size());
    const std::uint64_t hubNodeId = static_cast<std::uint64_t>(hubIndex) + 1ull;

    const ImVec2 pin   = hw.NodeRightPinScreen(hubNodeId);
    const ImVec2 empty = hw.CanvasPointScreen(ImVec2(620.0f, 300.0f));
    REQUIRE(pin.x > hw.origin.x);
    REQUIRE(pin.x < hw.origin.x + hw.size.x);
    REQUIRE(empty.x < hw.origin.x + hw.size.x);
    REQUIRE(empty.y < hw.origin.y + hw.size.y);

    // ---- the drag ---------------------------------------------------------
    hw.MoveTo(pin);      hw.Frame(); hw.Frame();
    hw.Button(true);     hw.Frame(); hw.Frame();
    // Past ImGui's drag threshold, then out over open canvas. Several frames:
    // the library needs one to DragStart, one to reach its Possible stage, and
    // one more with the pointer over the background to DropNode.
    hw.MoveTo(ImVec2(pin.x + 60.0f, pin.y + 20.0f)); hw.Frame(); hw.Frame();
    hw.MoveTo(empty);    hw.Frame(); hw.Frame(); hw.Frame();
    // The in-flight half must be live here, or the gesture never started and
    // everything below would be measuring the wrong thing.
    CHECK(state.graphDragGuid == fx.hub);
    CHECK(state.graphDragRight);

    hw.Button(false);    hw.Frame();   // release -> DragEnd arms the Create stage
    hw.Frame();                        // Create stage -> stash + OpenPopup

    // ---- what the release stashed ----------------------------------------
    // These two ARE the ghost menu's enabled gate (AssetsPanel.cpp:4690-4691).
    CHECK(state.graphWireGuid == fx.hub);
    CHECK(state.graphWireDerivable);

    // ---- the menu is really up, and the entry is really clickable ---------
    ImGuiContext* g = ImGui::GetCurrentContext();
    REQUIRE(g->OpenPopupStack.Size > 0);
    ImGuiWindow* popup = g->OpenPopupStack[0].Window;
    REQUIRE(popup != nullptr);
    REQUIRE(popup->Size.x > 0.0f);

    // Aim at the popup's single entry: one line in from its padding.
    const ImVec2 entry(popup->Pos.x + popup->Size.x * 0.5f,
                       popup->Pos.y + ImGui::GetStyle().WindowPadding.y +
                           ImGui::GetTextLineHeight() * 0.5f);
    hw.MoveTo(entry);  hw.Frame(); hw.Frame();
    hw.Button(true);   hw.Frame();
    hw.Button(false);  hw.Frame();

    // THE ASSERTION. The entry's one job (AssetsPanel.cpp:4747-4749): raise the
    // unified create request with the source material pre-filled as the parent.
    CHECK(hw.lastActions.requestCreateKind ==
          static_cast<int>(CreateAssetKind::MaterialInstance));
    CHECK(hw.lastActions.createPrefillParent == fx.hub);

    DestroyAssetGraphPanelCanvas(state);
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);

    std::error_code ec;
    fs::remove_all(fx.root, ec);
}

// ---------------------------------------------------------------------------
// 2026-09-09 user ruling: "very confusing to have the same button show up if
// or if not the asset is derivable." A release from a NON-derivable source is
// now a quiet no-op -- no ghost menu at all -- rather than the same popup with
// its one entry disabled. This case is the RED/GREEN witness for that change:
// against the pre-change code (the `else if (ed::AcceptNewItem())` branch
// above that unconditionally stashed and raised `wireCreateRequest`) the
// `OpenPopupStack.Size == 0` assertion below FAILS, because a popup opens with
// its entry merely disabled. Reasoned rather than desk-reverted (cheap to
// re-derive: the old code's only gate on `wireCreateRequest` was reaching this
// `else if` at all, which a LEFT-pin release does exactly as readily as a
// RIGHT-pin one) -- see AssetsPanel.cpp's `derivable` local, ~:4421-4425.
//
// The gesture is driven off the SAME MaterialHubFixture and the SAME hub node
// as the derivable case above (no new fixture needed): the hub's right pin is
// the derivable leg; this drags from a REFERENCER's LEFT pin instead --
// `isRightPin` false by construction, so `derivable` is false regardless of
// the source being a material. (Every fixture asset is a material on purpose,
// per the derivable case's own header comment; a left-pin release is the
// cheapest way to isolate "not the DEPENDENTS pin" from "not a material" as
// the failing conjunct, and AssetsPanel.cpp's ruling comment calls out the
// left pin explicitly as the other way in.)
TEST_CASE("Assets panel Graph lens pin-drag from a non-derivable source is a quiet no-op",
          "[editor][graphcanvas]")
{
    MaterialHubFixture fx;
    fx.Build("arcane_assets_graph_pindrag_nonderivable_test", /*referencerCount=*/6);

    AssetPanelModel model;
    model.MarkAllDirty();
    REQUIRE(model.RebuildIfDirty(&fx.project->Registry(), fx.fake.Make()));

    IMGUI_CHECKVERSION();
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600.0f, 900.0f);
    io.IniFilename = nullptr;
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);

    AssetsPanelState state;
    state.lens = AssetLens::Graph;
    state.graphFocusSeeded = true;
    DocumentHost docs;

    GraphMouseHarness hw;
    hw.state = &state;
    hw.model = &model;
    hw.project = &*fx.project;
    hw.docs = &docs;
    hw.services.resolveAssetThumb = [](const Guid&) -> std::uint64_t { return 0ull; };

    for (int i = 0; i < 4; ++i)
        hw.Frame();

    REQUIRE(state.graph.nodes.size() == 7u);
    // referencers[0] references the hub, so it has a LEFT (outbound) pin --
    // AssetsPanel.cpp:3961-3962 gives `hasLeftPin` only to the edge's `from`
    // side, unlike the hub's right pin which every material gets
    // unconditionally (:3994-4001).
    std::size_t refIndex = state.graph.nodes.size();
    for (std::size_t i = 0; i < state.graph.nodes.size(); ++i)
        if (state.graph.nodes[i].guid == fx.referencers[0])
            refIndex = i;
    REQUIRE(refIndex < state.graph.nodes.size());
    const std::uint64_t refNodeId = static_cast<std::uint64_t>(refIndex) + 1ull;

    // Seed the gesture stash by hand from an EARLIER (imagined) derivable
    // drag, the way DestroyAssetGraphPanelCanvas's own leg does above: a real
    // non-derivable release must OVERWRITE a stale derivable stash, not leave
    // it sitting there for a popup that (per this case) never opens to read
    // back.
    state.graphWireGuid      = fx.hub;
    state.graphWireDerivable = true;

    const ImVec2 pin   = hw.NodeLeftPinScreen(refNodeId);
    const ImVec2 empty = hw.CanvasPointScreen(ImVec2(620.0f, 300.0f));
    REQUIRE(pin.x > hw.origin.x);
    REQUIRE(pin.x < hw.origin.x + hw.size.x);
    REQUIRE(empty.x < hw.origin.x + hw.size.x);
    REQUIRE(empty.y < hw.origin.y + hw.size.y);

    // ---- the drag -- same choreography as the derivable case, off the
    // LEFT pin instead of the right one ------------------------------------
    hw.MoveTo(pin);      hw.Frame(); hw.Frame();
    hw.Button(true);     hw.Frame(); hw.Frame();
    hw.MoveTo(ImVec2(pin.x - 60.0f, pin.y + 20.0f)); hw.Frame(); hw.Frame();
    hw.MoveTo(empty);    hw.Frame(); hw.Frame(); hw.Frame();
    // The in-flight half is still live for EVERY drag, derivable or not --
    // the dashed wire is generic drag feedback and this change does not
    // touch it.
    CHECK(state.graphDragGuid == fx.referencers[0]);
    CHECK_FALSE(state.graphDragRight);

    hw.Button(false);    hw.Frame();   // release -> DragEnd arms the Create stage
    hw.Frame();                        // Create stage -> the non-derivable branch runs

    // ---- THE ASSERTION: quiet no-op ----------------------------------------
    // No ghost menu at all -- not a disabled one. Checked structurally
    // (nothing on ImGui's popup stack) rather than via `menuOpen`, which is a
    // local the panel does not expose.
    ImGuiContext* g = ImGui::GetCurrentContext();
    CHECK(g->OpenPopupStack.Size == 0);

    // The gesture stash is cleared, not left holding the PRIOR (derivable)
    // drag's guid -- the same "named asset of an outgoing gesture must not
    // survive it" posture DestroyAssetGraphPanelCanvas's project-switch reset
    // uses (AssetsPanel.cpp ~:4879-4880), applied here per-release instead of
    // per-project-switch.
    CHECK_FALSE(state.graphWireGuid.IsValid());
    CHECK_FALSE(state.graphWireDerivable);

    // ...and no create request reached the host: the one thing the (now
    // unreachable) entry could have raised.
    CHECK(hw.lastActions.requestCreateKind == -1);
    CHECK_FALSE(hw.lastActions.createPrefillParent.IsValid());

    DestroyAssetGraphPanelCanvas(state);
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);

    std::error_code ec;
    fs::remove_all(fx.root, ec);
}

// ---------------------------------------------------------------------------
// Panel-split spec s7.1/s7.3 (Task 3): the digest chip's click-through now
// raises actions.showStatus rather than writing state.lens directly, and R1
// gates the CLICK -- never the counts, which always render -- on
// services.statusOpen. Real mouse input over the real bottom bar, the same
// reason the pin-drag cases above need one: the InvisibleButton only exists
// inside a live ImGui frame, so no headless unit can stand in for it.
TEST_CASE("digest chip raises showStatus only while the Status target is open",
          "[editor][graphcanvas]")
{
    MaterialHubFixture fx;
    fx.Build("arcane_assets_digestclick_test", /*referencerCount=*/6);

    AssetPanelModel model;
    model.MarkAllDirty();
    REQUIRE(model.RebuildIfDirty(&fx.project->Registry(), fx.fake.Make()));

    IMGUI_CHECKVERSION();
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600.0f, 900.0f);
    io.IniFilename = nullptr;
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);

    AssetsPanelState state;
    state.lens = AssetLens::Graph;
    state.graphFocusSeeded = true;
    DocumentHost docs;

    GraphMouseHarness hw;
    hw.state = &state;
    hw.model = &model;
    hw.project = &*fx.project;
    hw.docs = &docs;
    hw.services.resolveAssetThumb = [](const Guid&) -> std::uint64_t { return 0ull; };
    hw.services.statusOpen = true;

    for (int i = 0; i < 3; ++i) hw.Frame();
    // The digest ends flush at the bottom bar's right edge; click just
    // inside it. POSITIVE CONTROL FIRST -- if this leg misses the chip, the
    // test fails loudly here instead of passing vacuously below.
    const ImVec2 chip(hw.origin.x + hw.size.x - 20.0f,
                      hw.origin.y + hw.size.y - kAssetPanelBottomBarHeight * 0.5f);
    hw.MoveTo(chip); hw.Frame(); hw.Button(true); hw.Frame(); hw.Button(false); hw.Frame();
    REQUIRE(hw.lastActions.showStatus);          // control: the click DOES land

    hw.lastActions = {};
    hw.services.statusOpen = false;              // target closed -> R1 disables
    hw.MoveTo(chip); hw.Frame(); hw.Button(true); hw.Frame(); hw.Button(false); hw.Frame();
    CHECK_FALSE(hw.lastActions.showStatus);

    DestroyAssetGraphPanelCanvas(state);
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);

    std::error_code ec;
    fs::remove_all(fx.root, ec);
}
