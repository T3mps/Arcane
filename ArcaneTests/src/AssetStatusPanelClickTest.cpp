// Panel-split spec §12 (Task 8, controller ruling): "New: disabled-rule
// coverage" for the Status panel's own three deep-links (spec §7.3) -- the
// half the panel-split arc's earlier tasks compressed out. AssetsGraphCanvasTest.cpp's
// digest-chip case ("digest chip raises showStatus only while the Status
// target is open") already covers the FOURTH leg (Browser/Graph -> Status);
// this file is the promised "Status-panel variant" of that same mouse-harness
// technique, driving the REAL DrawAssetStatusPanel through device-less ImGui
// frames with real mouse input -- an InvisibleButton/Button only exists
// inside a live ImGui frame, so no headless unit test of a pure model can
// stand in for it (same rationale AssetsGraphCanvasTest.cpp's own header
// states).
//
// Sibling file rather than appending to AssetsGraphCanvasTest.cpp (already
// 1300+ lines and Graph-canvas-specific: imgui_node_editor state, zoom
// tables, pin-drag geometry) -- this file's three cases are Status-panel-only
// and need none of that. Test sources are premake-globbed
// (ArcaneTests/src/**.cpp), so no premake5.lua edit is needed; ArcaneEditor's
// own project block already source-compiles AssetStatusPanel.cpp into the
// test exe alongside the other panel units (see that block's own comment) --
// this file only needed writing.
//
// THREE CLICK TARGETS, THREE DIFFERENT TECHNIQUES for locating each button's
// screen rect without pixel-guessing or an ImGui test-engine (not vendored
// here):
//
//   * Reveal (Unreferenced card) and Problems (Needs-attention card) BOTH
//     call `services.resolveAssetThumb` from inside their per-row draw code,
//     at a point where the two files' own source (read in full before this
//     was written) pins EXACTLY where ImGui's cursor sits relative to the
//     button already drawn (Reveal: BEFORE the button, so the probe's own
//     `GetCursorScreenPos()` IS the row's left edge outright; Problems:
//     AFTER the button pair, so the probe reads ImGui's post-item "next
//     line" reset -- `window->DC.CursorPos.x = window->Pos.x +
//     window->DC.Indent.x + window->DC.ColumnsOffset.x`, imgui.cpp's
//     ItemSize -- which lands back at the CARD's own left edge, not the
//     button's, and the Y a derivable button-row-height below the buttons).
//     Both then use each production formula (AssetStatusPanel.cpp's own
//     `revealX`/`buttonsLeft` math, quoted back here) to compute the
//     button's centre from values read live inside the callback -- SAME
//     style/font state the real draw used, so no font/theme assumption is
//     ever made independently of it.
//   * Focus in Graph (Scene card) has NO such callback -- DrawSceneCard
//     never asks the services seam for anything. It is reached instead by
//     REPLAYING DrawAssetStatusBody's own sequence up to that button, using
//     the SAME exported widget functions (StatTile/MeterBar/BeginCardFrame,
//     Widgets/EditorWidgets.hpp) at the SAME window origin/size the real
//     click frames use -- see ComputeFocusInGraphClickPoint below for the
//     citation-by-citation mirror and the handful of AssetStatusPanel.cpp
//     geometry constants it must hand-copy (not exported from that file's
//     anonymous namespace) to do it.
//
// Every case follows the digest-chip test's own discipline: POSITIVE CONTROL
// FIRST (the target open, assert the click lands) so a positioning miss
// fails loudly there instead of the disabled leg passing vacuously.
//
// A FOURTH case at the bottom of this file (not one of spec §12's three
// click legs -- it's this task's OWN step 1) pins the recency line's
// EMPTY-vs-POPULATED contract structurally: no test anywhere in this suite
// reads back a drawn glyph's text content (confirmed by survey before this
// was written -- every existing device-less ImGui case in this arc asserts
// either an ACTION/STATE value or a library-exposed geometry query, never
// rasterized text), so this reaches for the same structural technique
// AssetsGraphCanvasTest.cpp's own error-tooltip case already uses
// (`ImGui::FindWindowByName`/an ImGuiWindow scan, imgui_internal.h) --
// comparing the BOTTOM-BAR CHILD WINDOW's own drawlist vertex count (NOT
// the whole "Asset Status" window tree) with the ring null/empty against
// the SAME frame with one real entry pushed. The body's own Activity feed
// section (DrawAssetStatusBody, a few dozen lines up) reads that SAME
// services.activity and would grow ITS OWN child window's vertex count on
// the identical frame, so scoping the sum to just the bar's child window
// (matched by name containing "assetstatusbottombar", the id string
// BeginAssetPanelBottomBar was called with) is what makes the delta
// attributable to the recency line's own text draw and nothing else.

#include <catch2/catch_test_macros.hpp>

#include "Documents/DocumentHost.hpp"
#include "Panels/AssetActivityLog.hpp"    // AssetActivityLog/Entry/Kind -- the recency-line case's own ring
#include "Panels/AssetPanelCommon.hpp"    // AssetPanelActions/Services, kAssetPanelBottomBarHeight, kTableRowHeight
#include "Panels/AssetPanelModel.hpp"     // AssetPanelModel + AssetPanelProviders
#include "Panels/AssetStatusPanel.hpp"    // DrawAssetStatusPanel
#include "Widgets/EditorWidgets.hpp"      // StatTile/MeterBar/BeginCardFrame/EndCardFrame, kAssetRowThumbSize

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Project/Project.hpp>

#include <imgui.h>
// ImGuiWindow/FindWindowByName -- the recency-line case's own structural
// witness (see the header comment above); the same include
// AssetsGraphCanvasTest.cpp's error-tooltip case already carries for the
// identical reason.
#include <imgui_internal.h>

#include <chrono>
#include <cstdio>
#include <cstring>
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

    // Same canonical-guid-per-index spelling AssetsGraphCanvasTest.cpp uses,
    // duplicated rather than shared (each device-less test file in this arc
    // is self-contained -- AssetPanelCommonTest.cpp follows the identical
    // convention with its own WriteFile/FakeProviders).
    Guid FixtureGuid(int n)
    {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "bbbb0000-0000-4000-8000-%012d", n);
        return Guid::FromString(buf).value();
    }

    // The provider seam, faked -- same minimal shape every device-less asset
    // panel test in this arc uses, plus a per-guid CookState override (this
    // file's Problems case needs Refused for one guid and the default
    // Cooked for everything else).
    struct FakeProviders
    {
        std::unordered_map<Guid, std::vector<AssetRef>> refsByGuid;
        std::unordered_map<Guid, CookState> cookByGuid;

        AssetPanelProviders Make()
        {
            AssetPanelProviders p;
            p.refsFor = [this](const Guid& g) -> std::optional<std::vector<AssetRef>>
            {
                const auto it = refsByGuid.find(g);
                return it == refsByGuid.end() ? std::vector<AssetRef>{} : it->second;
            };
            p.cookStateFor = [this](const Guid& g)
            {
                const auto it = cookByGuid.find(g);
                return it == cookByGuid.end() ? CookState::Cooked : it->second;
            };
            p.surfaceFor = [](const Guid&) -> std::optional<MaterialSurface> { return std::nullopt; };
            return p;
        }
    };

    // One panel frame with the window pinned at a KNOWN screen origin --
    // GraphMouseHarness's own rationale (AssetsGraphCanvasTest.cpp), carried
    // over verbatim for DrawAssetStatusPanel: Status takes NO state struct
    // (spec §6), so this harness is simpler than the Graph one (no canvas
    // lifecycle to release).
    struct StatusMouseHarness
    {
        ImVec2 origin{ 0.0f, 0.0f };
        ImVec2 size{ 1200.0f, 640.0f };

        AssetPanelModel* model   = nullptr;
        const Project*   project = nullptr;
        DocumentHost*    docs    = nullptr;
        AssetPanelServices services{};
        AssetPanelActions  lastActions;

        void Frame()
        {
            ImGuiIO& io = ImGui::GetIO();
            io.DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(origin, ImGuiCond_Always);
            ImGui::SetNextWindowSize(size, ImGuiCond_Always);
            lastActions = DrawAssetStatusPanel(*model, project, *docs, services);
            ImGui::Render();
        }

        void MoveTo(const ImVec2& p) { ImGui::GetIO().AddMousePosEvent(p.x, p.y); }
        void Button(bool down)       { ImGui::GetIO().AddMouseButtonEvent(0, down); }

        // press-move-release-settle, the same 3-frame shape the digest-chip
        // case uses for its own click.
        void Click(const ImVec2& p)
        {
            MoveTo(p); Frame();
            Button(true); Frame();
            Button(false); Frame();
        }
    };

    // ---- Focus in Graph: replay DrawAssetStatusBody's Y-consuming sequence
    // up to the Scenes section's card, using the SAME exported widgets
    // (StatTile/MeterBar/BeginCardFrame -- Widgets/EditorWidgets.hpp) at the
    // SAME window origin/size the real click frames use, so the cursor lands
    // at the EXACT screen position DrawSceneCard's own "Focus in Graph"
    // button occupies when the real panel draws it. Read directly off
    // AssetStatusPanel.cpp's DrawAssetStatusBody (as it stood when this was
    // written) -- the four constants below are that file's OWN file-local
    // geometry (anonymous namespace, not exported), hand-copied here with
    // this citation so a future change to either file's spacing is the one
    // place to look if this test starts missing its target:
    //   kStatusTileMinWidth / kStatusTileHeight / kStatusSectionGap /
    //   kStatusRightColumnWidth.
    //
    // The LEFT column is drawn EMPTY here on purpose -- a table row's two
    // cells share the same TableNextRow() top-Y regardless of each other's
    // content (ImGui table contract), so column 0's own content never
    // affects where column 1 starts; every fixture this file's Focus-in-
    // Graph case uses is ALSO left-column-empty for the real draw, so the
    // two stay in lockstep on this point without needing to prove it twice.
    ImVec2 ComputeFocusInGraphClickPoint(const ImVec2& origin, const ImVec2& size)
    {
        constexpr float kMirrorTileMinWidth    = 72.0f;   // AssetStatusPanel.cpp kStatusTileMinWidth
        constexpr float kMirrorTileHeight      = 64.0f;   // AssetStatusPanel.cpp kStatusTileHeight
        constexpr float kMirrorSectionGap      = 6.0f;    // AssetStatusPanel.cpp kStatusSectionGap
        constexpr float kMirrorRightColumnWidth = 300.0f; // AssetStatusPanel.cpp kStatusRightColumnWidth

        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(origin, ImGuiCond_Always);
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
        ImGui::Begin("##statusmirror");
        ImGui::BeginChild("##assetstatusbody", ImVec2(0.0f, -kAssetPanelBottomBarHeight));
        ImGui::BeginChild("##statusbody", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding);

        const ImGuiStyle& style = ImGui::GetStyle();

        // ---- tiles row (mirrors DrawAssetStatusBody's own tile block).
        {
            const float tileW = std::max(kMirrorTileMinWidth,
                (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x * 3.0f) * 0.25f);
            const ImVec2 tileSize(tileW, kMirrorTileHeight);
            StatTile("##mt0", "0", "assets",        nullptr, 0, tileSize);
            ImGui::SameLine();
            StatTile("##mt1", "0", "cook refused",  nullptr, 1, tileSize);
            ImGui::SameLine();
            StatTile("##mt2", "0", "awaiting cook", nullptr, 0, tileSize);
            ImGui::SameLine();
            StatTile("##mt3", "0", "unreferenced",  nullptr, 0, tileSize);
        }

        // ---- meter (segment VALUES don't affect the Dummy height MeterBar
        // reserves -- only the segment COUNT does, and production always
        // passes 3).
        ImGui::Dummy(ImVec2(0.0f, kMirrorSectionGap));
        ImGui::TextDisabled("Cook pipeline");
        const MeterSegment segs[] = { { "cooked", 0, 0 }, { "queued", 0, 0 }, { "refused", 0, 0 } };
        MeterBar("##mirrormeter", segs, static_cast<int>(std::size(segs)), ImGui::GetContentRegionAvail().x);

        // ---- the two-column table.
        ImGui::Dummy(ImVec2(0.0f, kMirrorSectionGap));
        const float rightColumnWidth = std::max(1.0f,
            std::min(kMirrorRightColumnWidth, ImGui::GetContentRegionAvail().x * 0.45f));

        ImVec2 clickPoint{};
        if (ImGui::BeginTable("##mirrorcolumns", 2, ImGuiTableFlags_NoSavedSettings))
        {
            ImGui::TableSetupColumn("##left",  ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##right", ImGuiTableColumnFlags_WidthFixed, rightColumnWidth);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            // Left column intentionally empty -- see the header comment.

            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("Activity");
            ImGui::TextDisabled("no activity yet");   // this file's fixtures pass a null activity log
            ImGui::Dummy(ImVec2(0.0f, kMirrorSectionGap));
            ImGui::TextDisabled("Scenes");

            if (BeginCardFrame("##mirrorscene", 0, ImGui::GetContentRegionAvail().x))
            {
                // DrawSceneCard's own line 1 (icon+name, no boot pill -- this
                // file's scene fixture is never the boot scene) and line 2
                // (the assets-count TextDisabled) -- the exact TEXT drawn is
                // irrelevant to the Y advance (one line either way, same
                // font), so a placeholder stands in for both.
                ImGui::Text("%s %s", "S", "mirror.arcscene");
                ImGui::TextDisabled("%s", "0 assets \xC2\xB7 all cooked");

                clickPoint = ImGui::GetCursorScreenPos();
                const ImVec2 btnSize(ImGui::CalcTextSize("Focus in Graph").x + style.FramePadding.x * 2.0f,
                                      ImGui::GetFrameHeight());
                clickPoint.x += btnSize.x * 0.5f;
                clickPoint.y += btnSize.y * 0.5f;

                ImGui::Button("Focus in Graph");   // reserves the id space; never clicked here
                EndCardFrame();
            }
            ImGui::EndTable();
        }

        ImGui::EndChild();
        ImGui::EndChild();
        ImGui::End();
        ImGui::Render();
        return clickPoint;
    }
}

// ---------------------------------------------------------------------------
// Leg 1 of spec §12's "New: disabled-rule coverage" -- the Scenes card's
// Focus in Graph button, §7.1/§7.3's `focusInGraph`: raised only while
// `services.graphOpen`, and the panel must not write any Graph state
// directly (it has none to write -- Status takes no state struct at all;
// the observable proxy is that clicking it never touches the SHARED
// selection either, which the pre-split code used to do from inside this
// same button and the split moved to the host's consumer, spec §7.1).
TEST_CASE("Status panel Focus in Graph raises focusInGraph only while Graph is open",
          "[editor][status]")
{
    const fs::path root = fs::temp_directory_path() / "arcane_status_focusgraph_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    REQUIRE(Project::Create(root, "StatusFocusGraph").has_value());
    const fs::path content = root / "Content";

    const Guid sceneGuid = FixtureGuid(1);
    WriteFile(content / "scenes" / "mirror.arcscene",
              R"({"id":")" + sceneGuid.ToString() + R"(","version":4,"entities":[]})");

    auto project = Project::Open(root);
    REQUIRE(project.has_value());
    // Deliberately NOT the boot scene -- see ComputeFocusInGraphClickPoint's
    // own comment on why the mirror never draws the boot pill.

    FakeProviders fake;   // no refs anywhere: zero refused/queued/unreferenced, left column empty

    AssetPanelModel model;
    model.MarkAllDirty();
    REQUIRE(model.RebuildIfDirty(&project->Registry(), fake.Make()));
    REQUIRE(model.Find(sceneGuid));
    REQUIRE(model.Find(sceneGuid)->kind == AssetKind::Scene);

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

    DocumentHost docs;
    StatusMouseHarness hw;
    hw.model = &model;
    hw.project = &*project;
    hw.docs = &docs;
    hw.services.graphOpen = true;

    const ImVec2 clickPoint = ComputeFocusInGraphClickPoint(hw.origin, hw.size);
    // The mirror has to have landed somewhere genuinely on screen, or a
    // click there proves nothing about hitting the real button either.
    REQUIRE(clickPoint.x > hw.origin.x);
    REQUIRE(clickPoint.x < hw.origin.x + hw.size.x);
    REQUIRE(clickPoint.y > hw.origin.y);
    REQUIRE(clickPoint.y < hw.origin.y + hw.size.y);

    for (int i = 0; i < 3; ++i) hw.Frame();
    REQUIRE_FALSE(model.selected.IsValid());   // nothing has selected anything yet

    // POSITIVE CONTROL FIRST -- if the mirror missed, this fails loudly here.
    hw.Click(clickPoint);
    REQUIRE(hw.lastActions.focusInGraph == sceneGuid);
    // ...and the panel did NOT reach into the shared selection itself --
    // that write belongs to the HOST's action consumer now (spec §7.1), not
    // to this button.
    CHECK_FALSE(model.selected.IsValid());

    hw.lastActions = {};
    hw.services.graphOpen = false;              // target closed -> R1 disables
    hw.Click(clickPoint);
    CHECK_FALSE(hw.lastActions.focusInGraph.IsValid());
    CHECK_FALSE(model.selected.IsValid());

    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);
    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// Leg 2 -- the Unreferenced card's Reveal control, §7.3's `revealInBrowse`:
// raised only while `services.browserOpen`.
TEST_CASE("Status panel Reveal control raises revealInBrowse only while Browse is open",
          "[editor][status]")
{
    const fs::path root = fs::temp_directory_path() / "arcane_status_reveal_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    REQUIRE(Project::Create(root, "StatusReveal").has_value());
    const fs::path content = root / "Content";

    const Guid unrefGuid = FixtureGuid(2);
    WriteFile(content / "materials" / "orphan.arcmat",
              R"({"id":")" + unrefGuid.ToString() + R"(","type":"material","kind":"sprite"})");

    auto project = Project::Open(root);
    REQUIRE(project.has_value());

    FakeProviders fake;   // no refs anywhere -> orphan.arcmat has zero inbound refs -> unused

    AssetPanelModel model;
    model.MarkAllDirty();
    REQUIRE(model.RebuildIfDirty(&project->Registry(), fake.Make()));
    REQUIRE(model.Find(unrefGuid));
    REQUIRE(model.Find(unrefGuid)->unused);

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

    DocumentHost docs;
    StatusMouseHarness hw;
    hw.model = &model;
    hw.project = &*project;
    hw.docs = &docs;
    hw.services.browserOpen = true;

    // DrawUnreferencedCard calls `services.resolveAssetThumb(guid)` BEFORE
    // it positions the Reveal button -- nothing moves ImGui's cursor between
    // the row's own `rowMin = GetCursorScreenPos()` and this call
    // (AssetStatusPanel.cpp), so `GetCursorScreenPos()` read live INSIDE the
    // callback below IS that row's left edge, exactly. The rest is
    // DrawUnreferencedCard's own `revealX`/`revealW` formula, quoted back
    // here and evaluated against values read in the SAME callback (same
    // style/font state the real draw used).
    ImVec2 clickPoint{};
    bool probed = false;
    hw.services.resolveAssetThumb = [&](const Guid& g) -> std::uint64_t
    {
        if (g == unrefGuid && !probed)
        {
            probed = true;
            const ImVec2 rowMin = ImGui::GetCursorScreenPos();
            // avail, read at rowMin.x (== the card's inner content left edge,
            // one kCardFramePadding in from the card's own left border):
            // GetContentRegionAvail() measures to the ambient region's right
            // edge regardless of the card's own border (AssetStatusPanel.cpp's
            // own "Important 1" review-fix comment), so wellWidth is this
            // value minus ONE more kCardFramePadding -- see that file's
            // DrawUnreferencedCard for the two-pad derivation this repeats.
            const float avail = ImGui::GetContentRegionAvail().x;
            const float frameH = ImGui::GetFrameHeight();
            const ImGuiStyle& style = ImGui::GetStyle();
            constexpr float kCardFramePaddingMirror = 8.0f;   // EditorWidgets.cpp kCardFramePadding (spec: 8px)
            const float wellWidth = avail - kCardFramePaddingMirror;
            const float revealW = ImGui::CalcTextSize("Reveal").x + style.FramePadding.x * 2.0f;
            const float revealX = rowMin.x + wellWidth - revealW;
            const float revealY = rowMin.y + (kTableRowHeight - frameH) * 0.5f;
            clickPoint = ImVec2(revealX + revealW * 0.5f, revealY + frameH * 0.5f);
        }
        return 0ull;
    };

    for (int i = 0; i < 3; ++i) hw.Frame();
    REQUIRE(probed);
    REQUIRE(clickPoint.x > hw.origin.x);
    REQUIRE(clickPoint.x < hw.origin.x + hw.size.x);
    REQUIRE(clickPoint.y > hw.origin.y);
    REQUIRE(clickPoint.y < hw.origin.y + hw.size.y);

    // POSITIVE CONTROL FIRST.
    hw.Click(clickPoint);
    REQUIRE(hw.lastActions.revealInBrowse == unrefGuid);

    hw.lastActions = {};
    hw.services.browserOpen = false;            // target closed -> R1 disables
    hw.Click(clickPoint);
    CHECK_FALSE(hw.lastActions.revealInBrowse.IsValid());

    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);
    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// Leg 3 -- the Needs-attention (refused) card's Problems control, §7.3's
// `showProblems`: comes under the SAME rule as the other three deep links
// (greyed when Problems is closed), and per spec §7.3's own callout the
// host's consumer DROPS the un-hide it used to perform -- R1: nothing opens
// a panel except the Window menu.
TEST_CASE("Status panel Problems control raises showProblems only while Problems is open",
          "[editor][status]")
{
    const fs::path root = fs::temp_directory_path() / "arcane_status_problems_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    REQUIRE(Project::Create(root, "StatusProblems").has_value());
    const fs::path content = root / "Content";

    const Guid refusedGuid     = FixtureGuid(3);
    const Guid referencerGuid  = FixtureGuid(4);
    // A sidecar-minted guid (PNG carries no embedded id -- AssetRegistry's
    // ResolveSidecarId, ".meta" appended to the full filename).
    WriteFile(content / "textures" / "broken.png", "not a real png, just bytes");
    WriteFile(content / "textures" / "broken.png.meta",
              R"({"guid":")" + refusedGuid.ToString() + R"(","version":1})");
    WriteFile(content / "materials" / "user.arcmat",
              R"({"id":")" + referencerGuid.ToString() + R"(","type":"material","kind":"sprite"})");

    auto project = Project::Open(root);
    REQUIRE(project.has_value());

    FakeProviders fake;
    // referencerGuid -> refusedGuid keeps refusedGuid's inbound count at 1,
    // so it appears ONLY in Needs-attention, never ALSO in Unreferenced --
    // this case's probe (below) needs exactly one resolveAssetThumb call for
    // refusedGuid per frame, not two from two different cards.
    fake.refsByGuid[referencerGuid] = { { refusedGuid, AssetRefKind::References } };
    fake.cookByGuid[refusedGuid] = CookState::Refused;

    AssetPanelModel model;
    model.MarkAllDirty();
    REQUIRE(model.RebuildIfDirty(&project->Registry(), fake.Make()));
    REQUIRE(model.Find(refusedGuid));
    REQUIRE(model.Find(refusedGuid)->cook == CookState::Refused);
    REQUIRE_FALSE(model.Find(refusedGuid)->unused);

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

    DocumentHost docs;
    StatusMouseHarness hw;
    hw.model = &model;
    hw.project = &*project;
    hw.docs = &docs;
    hw.services.problemsOpen = true;

    // DrawAttentionCard calls `services.resolveAssetThumb(e.guid)` AFTER the
    // Recook/Problems pair is already drawn (AssetStatusPanel.cpp: the
    // buttons are positioned and submitted BEFORE the thumb section, "drawn
    // BEFORE the name so the name's ellipsis budget can be measured against
    // where they actually start"). By the time this callback fires, ImGui's
    // OWN post-item "next line" reset has already run (imgui.cpp's
    // ItemSize: `CursorPos.x = window->Pos.x + Indent.x + ColumnsOffset.x`,
    // independent of the buttons' own explicit SetCursorScreenPos), which
    // lands `GetCursorScreenPos()` back at the CARD's own left edge (NOT the
    // button's), one line-height + ItemSpacing.y below the buttons' own row.
    // The rest is DrawAttentionCard's own `innerMin`/`innerW`/`buttonsLeft`
    // formula, quoted back here and evaluated against values read in the
    // SAME callback (same style/font state the real draw used) -- and since
    // Problems is the LAST (right-most) of the two buttons, its own right
    // edge collapses to `innerMin.x + innerW` exactly (Recook's width cancels
    // out of the algebra).
    ImVec2 clickPoint{};
    bool probed = false;
    hw.services.resolveAssetThumb = [&](const Guid& g) -> std::uint64_t
    {
        if (g == refusedGuid && !probed)
        {
            probed = true;
            const ImVec2 probePos = ImGui::GetCursorScreenPos();
            const float avail = ImGui::GetContentRegionAvail().x;   // == the card's own cardWidth
            const float rowH = ImGui::GetFrameHeight();
            const ImGuiStyle& style = ImGui::GetStyle();
            constexpr float kCardFramePaddingMirror = 8.0f;   // EditorWidgets.cpp kCardFramePadding (spec: 8px)
            const float innerMinX = probePos.x + kCardFramePaddingMirror;
            const float innerMinY = probePos.y - rowH - style.ItemSpacing.y;
            const float innerW    = avail - kCardFramePaddingMirror * 2.0f;
            const float problemsW = ImGui::CalcTextSize("Problems").x + style.FramePadding.x * 2.0f;
            const float problemsX1 = innerMinX + innerW;
            const float problemsX0 = problemsX1 - problemsW;
            clickPoint = ImVec2((problemsX0 + problemsX1) * 0.5f, innerMinY + rowH * 0.5f);
        }
        return 0ull;
    };

    for (int i = 0; i < 3; ++i) hw.Frame();
    REQUIRE(probed);
    REQUIRE(clickPoint.x > hw.origin.x);
    REQUIRE(clickPoint.x < hw.origin.x + hw.size.x);
    REQUIRE(clickPoint.y > hw.origin.y);
    REQUIRE(clickPoint.y < hw.origin.y + hw.size.y);

    // POSITIVE CONTROL FIRST.
    hw.Click(clickPoint);
    REQUIRE(hw.lastActions.showProblems);

    hw.lastActions = {};
    hw.services.problemsOpen = false;           // target closed -> R1 disables
    hw.Click(clickPoint);
    CHECK_FALSE(hw.lastActions.showProblems);

    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);
    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// Task 8 step 1 (spec s9.3): the bottom bar's right slot carries the
// activity ring's newest entry -- "last change <rel> - <name>" -- and stays
// EMPTY (never a fabricated "just now") when the ring is null or holds
// nothing. See the file header comment for why a vertex-count delta, not a
// read-back of the drawn string, is this case's witness.
TEST_CASE("Status panel bottom bar recency line is empty until the activity ring has an entry",
          "[editor][status]")
{
    const fs::path root = fs::temp_directory_path() / "arcane_status_recency_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    REQUIRE(Project::Create(root, "StatusRecency").has_value());

    auto project = Project::Open(root);
    REQUIRE(project.has_value());

    FakeProviders fake;
    AssetPanelModel model;
    model.MarkAllDirty();
    REQUIRE(model.RebuildIfDirty(&project->Registry(), fake.Make()));

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

    DocumentHost docs;
    StatusMouseHarness hw;
    hw.model = &model;
    hw.project = &*project;
    hw.docs = &docs;
    // services.activity stays at its default (nullptr) for the first frame.

    // Every leg below SETTLES two frames before the one it measures --
    // AssetsGraphCanvasTest.cpp's own convention (its harness's "frame 1
    // creates ... frame 2+ runs every readback path"), which this case
    // needs for the same reason: a window's first few frames past a state
    // change can carry one-off layout/scrollbar-visibility work the vertex
    // count would otherwise attribute to the wrong leg.
    //
    // The bottom bar is a CHILD window (BeginAssetPanelBottomBar's own
    // BeginChild), which ImGui gives its OWN ImGuiWindow + ImDrawList
    // ("ParentName/childname_HASH", imgui.cpp's BeginChildEx) -- the recency
    // line's text therefore never lands in the TOP-LEVEL "Asset Status"
    // window's own drawlist, or in its "##assetstatusbody"/"##statusbody"
    // body children. Summing every WasActive window whose name merely
    // STARTS WITH "Asset Status" would confound this: the body's own
    // Activity feed section (DrawAssetStatusBody, :762-778) ALSO reads
    // services.activity and grows the BODY child's vertex count on the
    // identical frame, so a broad sum would pass even with the recency
    // line's own draw deleted. This scopes to ONLY the bar's own child
    // window, matched by name CONTAINING "assetstatusbottombar" (the id
    // string BeginAssetPanelBottomBar was called with -- still present
    // verbatim inside the hashed child name, imgui.cpp's "%s/%s_%08X" --
    // without needing to know the hash suffix).
    const auto vtxOf = [&]() -> int
    {
        hw.Frame(); hw.Frame(); hw.Frame();
        ImGuiContext& g = *ImGui::GetCurrentContext();
        int total = 0;
        for (ImGuiWindow* w : g.Windows)
            if (w->WasActive && std::strstr(w->Name, "assetstatusbottombar") != nullptr)
                total += w->DrawList->VtxBuffer.Size;
        REQUIRE(total > 0);
        return total;
    };

    // ---- leg 1: null ring -> the empty state (spec s13: never fabricate).
    const int vtxEmptyRing = vtxOf();

    // ---- leg 2: same fixture, same window, one real entry pushed into a
    // real AssetActivityLog -- nothing else about the frame changes.
    AssetActivityLog activity;
    AssetActivityEntry entry;
    entry.when = std::chrono::steady_clock::now();
    entry.guid = FixtureGuid(5);
    entry.name = "uv_marker.png";
    entry.kind = AssetActivityKind::Cooked;
    activity.Push(entry);
    hw.services.activity = &activity;
    const int vtxPopulated = vtxOf();

    // THE ASSERTION: the populated frame drew strictly more than the empty
    // one -- the recency line's own text glyphs, the one thing that
    // differs between the two frames.
    CHECK(vtxPopulated > vtxEmptyRing);

    // ---- leg 3: an EMPTY (but non-null) ring is the SAME empty state as a
    // null one -- `Size() == 0` is the guard, not merely `!= nullptr`.
    AssetActivityLog emptyActivity;
    hw.services.activity = &emptyActivity;
    const int vtxEmptyRing2 = vtxOf();
    CHECK(vtxEmptyRing2 == vtxEmptyRing);

    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);
    fs::remove_all(root, ec);
}
