// Inspector filters (spec 2026-09-29 s6, Task 5): AssetInspectorSource -- the
// Asset Browser's shared selection (AssetPanelModel::selected) as an
// Inspector source, and DrawAssetPage -- the old preview pane's content plus
// the texture import settings, now that source's page.
//
// Cases 1-3 are registry-only (AssetPanelModelTest.cpp's pattern: a REAL temp
// dir + real files + registry.ScanContent, the AssetPanelProviders faked
// in-test). Case 4 needs a real Arcane::Project (the texture block resolves
// its source path through Project::ResolveAsset) and drives the REAL
// DrawAssetPage through device-less ImGui frames.

#include <catch2/catch_test_macros.hpp>

#include "Panels/AssetInspectorSource.hpp"
#include "Panels/AssetPanelCommon.hpp"      // AssetPanelServices/AssetPanelActions
#include "Panels/AssetPanelModel.hpp"
#include "Panels/TextureMetaPanel.hpp"      // ReadTextureMetaSettingsDisplay
#include "Widgets/EditorTheme.hpp"          // ApplyEditorTheme (the 1080p fit case)
#include "Widgets/IconsLucide.h"            // ICON_LC_COPY
#include "Widgets/PropertyGrid.hpp"

#include <Arcane/Project/AssetRegistry.hpp>
#include <Arcane/Project/Project.hpp>

#include <imgui.h>
#include <imgui_internal.h>                 // ActivateItemByID

#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace Arcane::Editor;
namespace fs = std::filesystem;

namespace
{
    // ---- copied from AssetPanelModelTest.cpp (WriteFile, GuidForPath,
    // FakeProviders), verbatim ------------------------------------------------

    fs::path WriteFile(const fs::path& dir, const char* name, const std::string& text)
    {
        fs::path p = dir / name;
        std::ofstream(p, std::ios::binary) << text;
        return p;
    }

    Arcane::Guid GuidForPath(const std::vector<std::pair<Arcane::Guid, std::string>>& all,
                            std::string_view mountPath)
    {
        for (const auto& [guid, path] : all)
            if (path == mountPath)
                return guid;
        return Arcane::Guid{};
    }

    // Test double over AssetPanelProviders: seeds per-guid refs/cook-state/
    // surface answers, and counts invocations per guid so case (f) can prove
    // MarkDirty(one guid) re-asks the providers ONLY for that guid.
    struct FakeProviders
    {
        std::unordered_map<Arcane::Guid, std::vector<Arcane::AssetRef>> refsByGuid;
        std::unordered_map<Arcane::Guid, CookState> cookByGuid;
        std::unordered_map<Arcane::Guid, Arcane::MaterialSurface> surfaceByGuid;

        // Plan 2 Task 4: guids whose refsFor answer is a HARD nullopt -- the
        // "exists but could not be read/parsed this walk" shape (spec s3.2's
        // last-known-good contract). Deliberately distinct from an ABSENT
        // refsByGuid entry, which answers an EMPTY vector (a readable asset
        // that simply references nothing) and therefore legitimately retracts
        // every edge it used to contribute.
        std::unordered_set<Arcane::Guid> nullRefsGuids;

        std::unordered_map<Arcane::Guid, int> refsCalls;
        std::unordered_map<Arcane::Guid, int> cookCalls;
        std::unordered_map<Arcane::Guid, int> surfaceCalls;

        AssetPanelProviders Make()
        {
            AssetPanelProviders p;
            p.refsFor = [this](const Arcane::Guid& g) -> std::optional<std::vector<Arcane::AssetRef>>
            {
                ++refsCalls[g];
                if (nullRefsGuids.count(g) != 0)
                    return std::nullopt;
                const auto it = refsByGuid.find(g);
                return it == refsByGuid.end() ? std::vector<Arcane::AssetRef>{} : it->second;
            };
            p.cookStateFor = [this](const Arcane::Guid& g) -> CookState
            {
                ++cookCalls[g];
                const auto it = cookByGuid.find(g);
                return it == cookByGuid.end() ? CookState::Cooked : it->second;
            };
            p.surfaceFor = [this](const Arcane::Guid& g) -> std::optional<Arcane::MaterialSurface>
            {
                ++surfaceCalls[g];
                const auto it = surfaceByGuid.find(g);
                return it == surfaceByGuid.end() ? std::nullopt
                                                  : std::optional<Arcane::MaterialSurface>(it->second);
            };
            return p;
        }
    };

    // ---- the registry-only fixture (cases 1-3) -----------------------------
    // Two sidecar-minted textures (brick.png, stone.png) and one material,
    // scanned into a real registry and built into a real model. Each case
    // names its own temp dir so no two cases share on-disk state.
    struct AssetSourceFixture
    {
        fs::path dir;
        Arcane::AssetRegistry registry;
        FakeProviders fake;
        AssetPanelProviders providers;
        AssetPanelModel model;
        Arcane::Guid gBrick, gStone;

        explicit AssetSourceFixture(const char* name)
            : dir(fs::temp_directory_path() / name)
        {
            std::error_code ec;
            fs::remove_all(dir, ec);
            fs::create_directories(dir);
            WriteFile(dir, "brick.png", "not a real png, just bytes");   // sidecar-minted guid
            WriteFile(dir, "stone.png", "not a real png, just bytes");   // sidecar-minted guid
            WriteFile(dir, "wall.arcmat",
                      R"({"id":"a5500001-0001-4001-8001-000000000001","type":"material","kind":"fullscreen"})");
            REQUIRE(registry.ScanContent(dir, "game") == 3);
            const auto all = registry.All();
            gBrick = GuidForPath(all, "game://brick.png");
            gStone = GuidForPath(all, "game://stone.png");
            REQUIRE(gBrick.IsValid());
            REQUIRE(gStone.IsValid());
            providers = fake.Make();
            model.MarkAllDirty();
            REQUIRE(model.RebuildIfDirty(&registry, providers));
            REQUIRE(model.Find(gBrick) != nullptr);
            REQUIRE(model.Find(gStone) != nullptr);
        }

        ~AssetSourceFixture()
        {
            std::error_code ec;
            fs::remove_all(dir, ec);
        }
    };
}

TEST_CASE("AssetInspectorSource: the model's shared selection is the source's selection", "[editor][inspector]")
{
    AssetSourceFixture fx("arcane_asset_inspector_source_selection_test");
    AssetPanelModel& model = fx.model;
    const Arcane::Guid gBrick = fx.gBrick;
    const Arcane::Guid gStone = fx.gStone;

    AssetInspectorSource src;
    CHECK(src.SelectionKey().empty());                      // unbound
    src.Bind({ &model, nullptr, nullptr, nullptr });
    CHECK(src.SelectionKey().empty());
    CHECK(src.Page() == nullptr);                           // nothing selected: no page
    model.Select(gBrick);
    CHECK(src.SelectionKey() == gBrick.ToString());
    CHECK(src.SelectionEpoch() == model.selectionGesture);
    REQUIRE(src.Page() == &src);
    const auto crumbs = src.Breadcrumb();
    REQUIRE(crumbs.size() == 2);
    CHECK(crumbs[0].label == "Assets");
    CHECK(crumbs[1].label == "brick.png");
    CHECK(crumbs[1].key == gBrick.ToString());
    REQUIRE(src.RestoreSelection(gStone.ToString()));
    CHECK(model.selected == gStone);
    CHECK_FALSE(src.RestoreSelection("not-a-guid"));
    CHECK(model.selected == gStone);                        // a failed restore selects nothing
    REQUIRE(src.PageFor(gBrick.ToString()) == &src);        // a pin's page, without touching the selection
    const auto pinned = src.Breadcrumb();
    REQUIRE(pinned.size() == 2);
    CHECK(pinned[1].label == "brick.png");
    CHECK(model.selected == gStone);
    crumbs[0].select();                                     // the "Assets" crumb clears
    CHECK_FALSE(model.selected.IsValid());
}

TEST_CASE("AssetInspectorSource: a deleted asset stops resolving and its page goes away", "[editor][inspector]")
{
    AssetSourceFixture fx("arcane_asset_inspector_source_deleted_test");
    AssetPanelModel& model = fx.model;
    const Arcane::Guid gBrick = fx.gBrick;

    AssetInspectorSource src;
    src.Bind({ &model, nullptr, nullptr, nullptr });
    model.Select(gBrick);
    REQUIRE(src.Resolves(gBrick.ToString()));
    REQUIRE(src.Page() == &src);

    // Remove the texture from disk and re-scan the SAME registry, then dirty
    // only the removed guid -- AssetPanelModelTest's removal case, exactly.
    std::error_code ec;
    fs::remove(fx.dir / "brick.png", ec);
    fs::remove(fx.dir / "brick.png.meta", ec);   // orphaned sidecar, if written back
    REQUIRE(fx.registry.ScanContent(fx.dir, "game") == 2);
    model.MarkDirty(gBrick);
    REQUIRE(model.RebuildIfDirty(&fx.registry, fx.providers));
    REQUIRE(model.Find(gBrick) == nullptr);

    CHECK_FALSE(src.Resolves(gBrick.ToString()));
    CHECK(src.Page() == nullptr);                           // "No selection", never a stale page
    CHECK(src.PageFor(gBrick.ToString()) == nullptr);
    CHECK(src.Resolves(fx.gStone.ToString()));              // the survivor still resolves
}

TEST_CASE("AssetInspectorSource: re-selecting the selected asset is a selection gesture", "[editor][inspector]")
{
    AssetSourceFixture fx("arcane_asset_inspector_source_gesture_test");
    AssetPanelModel& model = fx.model;
    const Arcane::Guid gBrick = fx.gBrick;

    AssetInspectorSource src;
    CHECK(src.SelectionEpoch() == 0);                       // unbound
    src.Bind({ &model, nullptr, nullptr, nullptr });
    model.Select(gBrick);
    const std::uint64_t epoch = src.SelectionEpoch();
    const std::uint32_t stamp = model.selectionStamp;
    model.Select(gBrick);                                   // a re-click: the Browser/Status click sites call Select without a guid compare
    CHECK(src.SelectionEpoch() == epoch + 1);               // spec s3: a re-selection IS an event
    CHECK(model.selectionStamp == stamp);                   // the Browser scroll / Graph re-center key stays change-only
    model.ResetForProjectSwitch();
    CHECK(src.SelectionEpoch() == epoch + 1);               // monotonic: a project switch never rewinds it
}

TEST_CASE("DrawAssetPage: Copy Path reports an action, never acts; a .png draws the import settings", "[editor][inspector]")
{
    // A REAL project: the texture block resolves its source path through
    // Project::ResolveAsset and draws nothing without one
    // (AssetStatusPanelClickTest.cpp's Project::Create shape).
    const fs::path root = fs::temp_directory_path() / "arcane_asset_page_draw_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    // RAII: a REQUIRE failure below still removes the temp project.
    struct RemoveOnExit { fs::path p; ~RemoveOnExit() { std::error_code e; fs::remove_all(p, e); } } cleanup{ root };
    REQUIRE(Arcane::Project::Create(root, "AssetPage").has_value());
    WriteFile(root / "Content", "brick.png", "not a real png, just bytes");   // sidecar-minted guid
    auto project = Arcane::Project::Open(root);
    REQUIRE(project.has_value());
    const Arcane::Guid gBrick = GuidForPath(project->Registry().All(), "game://brick.png");
    REQUIRE(gBrick.IsValid());
    FakeProviders fake;
    AssetPanelModel model;
    model.MarkAllDirty();
    REQUIRE(model.RebuildIfDirty(&project->Registry(), fake.Make()));
    REQUIRE(model.Find(gBrick) != nullptr);
    REQUIRE(model.Find(gBrick)->kind == AssetKind::Texture);

    // Device-less ImGui: the EditorInspectorVectorTest.cpp:262-275 setup
    // (no backend; a software font atlas satisfies NewFrame).
    struct FrameContext
    {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGuiContext* ctx = ImGui::CreateContext();
        FrameContext()
        {
            ImGui::SetCurrentContext(ctx);
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1280.0f, 1024.0f);
            io.IniFilename = nullptr;
            unsigned char* pixels = nullptr;
            int w = 0, h = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        }
        ~FrameContext() { ImGui::DestroyContext(ctx); ImGui::SetCurrentContext(prev); }
    } fc;

    AssetPanelServices services;                            // no thumbnails, no peeks
    AssetPanelActions actions;
    // One frame of the page. `activate` presses an item BY ID through ImGui's
    // nav-activation path (ActivateItemByID, imgui_internal.h:3629; it lands
    // on the NEXT frame's ButtonBehavior). A rect read after DrawAssetPage
    // would be the LAST item's, never the button's.
    Arcane::Editor::PropertyGridState gs;
    auto frame = [&](std::function<ImGuiID()> activate)
    {
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(640.0f, 1000.0f), ImGuiCond_Always);
        ImGui::Begin("t");
        if (activate) ImGui::ActivateItemByID(activate());
        Arcane::Editor::PropertyGrid grid(gs);
        DrawAssetPage(grid, *model.Find(gBrick), model, &*project, services, actions);
        ImGui::End();
        ImGui::Render();                                    // draw data discarded -- no backend
    };

    frame(nullptr);                                         // warm-up: the window exists
    frame([] { return ImGui::GetID(ICON_LC_COPY "##asset_copypath"); });   // queue the press
    frame(nullptr);                                         // the press lands
    CHECK(actions.copyPath == gBrick);                      // reported, never performed

    // The import settings drew (the ".png only" note returns before the knobs):
    // pressing sRGB flips the value merge-written into the .meta sidecar.
    const fs::path meta = root / "Content" / "brick.png.meta";
    const bool srgbBefore = ReadTextureMetaSettingsDisplay(meta, {}).srgb;
    // PropertyGrid ids: PushID("sRGB") + "##value" under the Import rows table ("##texmeta").
    frame([] { return ImGui::GetIDWithSeed("##value", nullptr, ImGui::GetIDWithSeed("sRGB", nullptr, ImGui::GetID("##texmeta"))); });
    frame(nullptr);
    CHECK(ReadTextureMetaSettingsDisplay(meta, {}).srgb != srgbBefore);
}

TEST_CASE("AssetPageThumbSize: compact and stacked forms clamp the height share between the floor and 140", "[editor][inspector]")
{
    CHECK(AssetPageThumbSize(true, 376.0f, 8.0f, 330.0f, 0.30f, 64.0f, 140.0f) > 98.99f);     // the 392x330 fit: 0.30f x 330 is 99.00001f in float,
    CHECK(AssetPageThumbSize(true, 376.0f, 8.0f, 330.0f, 0.30f, 64.0f, 140.0f) < 99.01f);     // so never compare it with ==
    CHECK(AssetPageThumbSize(true, 250.0f, 8.0f, 600.0f, 0.30f, 64.0f, 140.0f) == 132.0f);    // leaves a 110 px text column
    CHECK(AssetPageThumbSize(true, 376.0f, 8.0f, 1000.0f, 0.30f, 64.0f, 140.0f) == 140.0f);   // never above 140
    CHECK(AssetPageThumbSize(true, 376.0f, 8.0f, 100.0f, 0.30f, 64.0f, 140.0f) == 64.0f);     // never below the floor...
    CHECK(AssetPageThumbSize(false, 50.0f, 8.0f, 100.0f, 0.30f, 64.0f, 140.0f) == 50.0f);     // ...unless the width is smaller
    CHECK(AssetPageThumbSize(false, 213.0f, 8.0f, 350.0f, 0.30f, 64.0f, 140.0f) > 104.99f);   // the 229x350 stacked case (0.30f x 350 = 105.00001f)
    CHECK(AssetPageThumbSize(false, 213.0f, 8.0f, 350.0f, 0.30f, 64.0f, 140.0f) < 105.01f);
}

TEST_CASE("ActionsThatFit: all of the row, else the most that fit beside the overflow button", "[editor][inspector]")
{
    const float w[] = { 30.0f, 30.0f, 30.0f };
    CHECK(ActionsThatFit(w, 30.0f, 8.0f, 106.0f) == 3);    // 30+8+30+8+30 = 106: no overflow button
    CHECK(ActionsThatFit(w, 30.0f, 8.0f, 105.0f) == 1);    // more(30) + 38 = 68; + 38 = 106 > 105
    CHECK(ActionsThatFit(w, 30.0f, 8.0f, 20.0f) == 0);
    CHECK(ActionsThatFit({}, 30.0f, 8.0f, 0.0f) == 0);
}

namespace
{
    // A real project holding one texture (+ optionally a 1:1 sprite folded under it).
    struct AssetPageProject
    {
        fs::path root;
        std::optional<Arcane::Project> project;
        FakeProviders fake;
        AssetPanelModel model;
        Arcane::Guid tex;
        AssetPageProject(const char* name, const char* file, bool derived)
            : root(fs::temp_directory_path() / name)
        {
            std::error_code ec;
            fs::remove_all(root, ec);
            REQUIRE(Arcane::Project::Create(root, "AssetPage").has_value());
            WriteFile(root / "Content", file, "not a real png, just bytes");
            const Arcane::Guid sprite = *Arcane::Guid::FromString("b0000002-0002-4002-8002-000000000002");
            if (derived)
                WriteFile(root / "Content", "brick_sprite.arcsprite",
                          R"({"id":"b0000002-0002-4002-8002-000000000002","type":"sprite","name":"BrickSprite"})");
            project = Arcane::Project::Open(root);
            REQUIRE(project.has_value());
            tex = GuidForPath(project->Registry().All(), std::string("game://") + file);
            REQUIRE(tex.IsValid());
            if (derived) fake.refsByGuid[sprite] = { { tex, Arcane::AssetRefKind::DerivesFrom } };
            model.MarkAllDirty();
            REQUIRE(model.RebuildIfDirty(&project->Registry(), fake.Make()));
            REQUIRE(model.Find(tex) != nullptr);
            if (derived) REQUIRE(model.Find(tex)->derivedChildren.size() == 1);
        }
        ~AssetPageProject() { project.reset(); std::error_code ec; fs::remove_all(root, ec); }
    };

    // The page in a NoTitleBar window at the origin: the page body is the
    // Inspector's title-less ##page child (s5.7).
    struct AssetPageUi
    {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGuiContext* ctx = ImGui::CreateContext();
        Arcane::Editor::PropertyGridState grid;
        std::string logged;
        AssetPageUi()
        {
            ImGui::SetCurrentContext(ctx);
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1280.0f, 1024.0f);
            io.IniFilename = nullptr;
            unsigned char* px = nullptr; int w = 0, h = 0;
            io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        }
        ~AssetPageUi() { ImGui::DestroyContext(ctx); ImGui::SetCurrentContext(prev); }
        void Frame(ImVec2 size, const std::function<void(Arcane::Editor::PropertyGrid&)>& draw,
                   ImGuiID activate = 0, bool log = false)
        {
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
            ImGui::SetNextWindowSize(size, ImGuiCond_Always);
            ImGui::Begin("t", nullptr, ImGuiWindowFlags_NoTitleBar);
            if (activate) ImGui::ActivateItemByID(activate);
            if (log) ImGui::LogToBuffer();
            Arcane::Editor::PropertyGrid g(grid);
            draw(g);
            if (log) { logged = ctx->LogBuffer.c_str(); ImGui::LogFinish(); }
            ImGui::End();
            ImGui::Render();
        }
    };
}

TEST_CASE("DrawAssetPage: a texture with a derived child fits 392x330; every action is reachable", "[editor][inspector]")
{
    AssetPageProject p("arcane_asset_page_fit_test", "brick.png", /*derived*/ true);
    AssetPageUi ui;
    AssetPanelServices services;
    AssetPanelActions actions;
    const auto draw = [&](Arcane::Editor::PropertyGrid& g)
    { DrawAssetPage(g, *p.model.Find(p.tex), p.model, &*p.project, services, actions); };
    ui.Frame(ImVec2(392.0f, 330.0f), draw); ui.Frame(ImVec2(392.0f, 330.0f), draw);
    ImGuiWindow* w = ImGui::FindWindowByName("t");
    REQUIRE(w != nullptr);
    CHECK(w->ScrollMax.y == 0.0f);                                   // the contract (9.3)
    struct Want { const char* icon; const char* label; const char* id; Arcane::Guid* field; };
    const Want wants[] = {
        { ICON_LC_EXTERNAL_LINK, "Open",             "##asset_open",         &actions.openAsset },
        { ICON_LC_FOLDER_OPEN,   "Show in Explorer", "##asset_explorer",     &actions.showInExplorer },
        { ICON_LC_FILE_TEXT,     "Open as text",     "##asset_astext",       &actions.openAsText },
        { ICON_LC_COPY,          "Copy Path",        "##asset_copypath",     &actions.copyPath },
        { ICON_LC_STICKER,       "Create Sprite",    "##asset_createsprite", &actions.createSpriteFrom },
    };
    for (const float width : { 392.0f, 60.0f })
    {
        const ImVec2 size(width, 330.0f);
        int viaMore = 0;
        for (const Want& want : wants)
        {
            INFO(want.label << " at " << width);
            actions = AssetPanelActions{};
            ui.Frame(size, draw);
            ui.Frame(size, draw, w->GetID((std::string(want.icon) + want.id).c_str()));
            ui.Frame(size, draw);
            if (*want.field != p.tex)
            {
                ui.Frame(size, draw, w->GetID(ICON_LC_ELLIPSIS "##asset_more"));
                ui.Frame(size, draw);
                REQUIRE(ImGui::GetCurrentContext()->OpenPopupStack.Size == 1);
                ImGuiWindow* more = ImGui::GetCurrentContext()->OpenPopupStack.back().Window;
                REQUIRE(more != nullptr);
                ui.Frame(size, draw, more->GetID((std::string(want.icon) + " " + want.label + want.id).c_str()));
                ui.Frame(size, draw);
                ++viaMore;
            }
            CHECK(*want.field == p.tex);                             // reported, never performed
        }
        if (width > 300.0f) CHECK(viaMore == 0);                     // all on the row at 392
        else                CHECK(viaMore > 0);                      // what does not fit moved into ##asset_more
    }
}

// s5.6's outcome on the REAL geometry (T3-GATE fix round 1): the 392x330
// window above is a proxy the desk never has. At 1080p the Assets-only
// Inspector's `##page` child (s5.7: no WindowPadding, under the pinned header)
// is about 376x290 by the spec's estimate (:1576), and MEASURED 376x281 on the
// T3 desk (desk-t3/02: the scrollbar track spans 281 px; its 268 px grab is
// 277 x 281/290, i.e. content 290, ScrollMax 9). The editor's font is 16 px,
// not ProggyClean's 13. The test draws the measured 281 and REQUIREs the
// ruling's ceiling (<= 376x288) and the font as preconditions, so the geometry
// cannot drift lenient: a texture with one derived child, Derived and Import
// open, must not scroll the PAGE CHILD.
TEST_CASE("DrawAssetPage: a texture with a derived child fits the 1080p ##page child (376x281, 16 px font)", "[editor][inspector]")
{
    AssetPageProject p("arcane_asset_page_fit1080_test", "brick.png", /*derived*/ true);
    AssetPageUi ui;
    ApplyEditorTheme(ImGui::GetStyle());                             // the editor's metrics (4 px WindowPadding, stock frame paddings, FrameBorderSize 1)
    ImGui::GetStyle().FontSizeBase = 16.0f;                          // InstallEditorFonts' default size: 16 px lines, 22 px frames
    AssetPanelServices services;
    AssetPanelActions actions;
    const auto frame = [&]
    {
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        const ImVec2 pad = ImGui::GetStyle().WindowPadding;             // the theme's, so the child is the desk's 376x281
        ImGui::SetNextWindowSize(ImVec2(376.0f + 2.0f * pad.x, 281.0f + 2.0f * pad.y), ImGuiCond_Always);
        ImGui::Begin("t", nullptr, ImGuiWindowFlags_NoTitleBar);
        (void)ImGui::BeginChild("##page", ImVec2(0.0f, 0.0f), ImGuiChildFlags_NavFlattened);   // InspectorWindows.cpp's flags
        Arcane::Editor::PropertyGrid g(ui.grid);
        DrawAssetPage(g, *p.model.Find(p.tex), p.model, &*p.project, services, actions);
        ImGui::EndChild();
        ImGui::End();
        ImGui::Render();
    };
    // A window's ScrollMax is computed in Begin from the PREVIOUS frame's
    // content: after the third frame it describes frame 2, the first frame laid
    // out with the settled ##previewMeta measurement.
    frame(); frame(); frame();
    ImGuiWindow* w = ImGui::FindWindowByName("t");
    REQUIRE(w != nullptr);
    ImGuiWindow* page = nullptr;
    for (ImGuiWindow* c : ImGui::GetCurrentContext()->Windows)
        if (c->ParentWindow == w && std::string(c->Name).find("##page") != std::string::npos) page = c;
    REQUIRE(page != nullptr);
    REQUIRE(ImGui::GetStyle().FontSizeBase == 16.0f);
    REQUIRE(page->Size.x <= 376.0f);                                 // preconditions: never roomier than the desk
    REQUIRE(page->Size.y <= 288.0f);
    REQUIRE(page->WindowPadding.y == 0.0f);
    INFO("page " << page->Size.x << "x" << page->Size.y << ", content " << page->ContentSize.y);
    CHECK(page->ScrollMax.y == 0.0f);                                // the s5.6 outcome: no scrollbar at 1080p
}

TEST_CASE("DrawAssetPage: the name and guid ellipsize in a 110 px text column", "[editor][inspector]")
{
    AssetPageProject p("arcane_asset_page_ellipsis_test", "a_texture_name_far_too_long_for_its_column.png", false);
    AssetPageUi ui;
    AssetPanelServices services;
    AssetPanelActions actions;
    const auto draw = [&](Arcane::Editor::PropertyGrid& g)
    { DrawAssetPage(g, *p.model.Find(p.tex), p.model, &*p.project, services, actions); };
    const ImVec2 size(266.0f, 600.0f);                               // avail 250: compact, thumb 132, column 110
    ui.Frame(size, draw); ui.Frame(size, draw, 0, true);
    INFO(ui.logged);
    CHECK(ui.logged.find("a_texture_name_far_too_long_for_its_column") == std::string::npos);
    CHECK(ui.logged.find(p.tex.ToString()) == std::string::npos);
    CHECK(ui.logged.find("...") != std::string::npos);
}

TEST_CASE("DrawAssetPage: at 229x350 the page uses the stacked form", "[editor][inspector]")
{
    AssetPageProject p("arcane_asset_page_stacked_test", "brick.png", false);
    AssetPageUi ui;
    std::unordered_map<std::string, ImVec2> probe;                   // PropertyGridState's test seam
    ui.grid.probe = &probe;
    AssetPanelServices services;
    AssetPanelActions actions;
    const auto draw = [&](Arcane::Editor::PropertyGrid& g)
    { DrawAssetPage(g, *p.model.Find(p.tex), p.model, &*p.project, services, actions); };
    ui.Frame(ImVec2(229.0f, 350.0f), draw); ui.Frame(ImVec2(229.0f, 350.0f), draw);
    for (ImGuiWindow* c : ImGui::GetCurrentContext()->Windows)
        CHECK(std::string(c->Name).find("##previewMeta") == std::string::npos);   // no compact column
    const float stackedThumb = AssetPageThumbSize(false, 229.0f - 16.0f, 8.0f, 350.0f, 0.30f, 64.0f, 140.0f);
    CHECK(stackedThumb > 104.99f);                   // 0.30f x 350 = 105.00001f in float: no ==
    CHECK(stackedThumb < 105.01f);
    // The thumb the page DREW (s5.6: thumbSize == the formula): the stacked
    // thumb is the page's first item, so its centre is start + side / 2.
    ImGuiWindow* w = ImGui::FindWindowByName("t");
    REQUIRE(w != nullptr);
    REQUIRE(probe.count("##asset_thumb") == 1);
    const ImVec2 centre = probe.at("##asset_thumb");
    CHECK(2.0f * (centre.x - w->DC.CursorStartPos.x) > stackedThumb - 0.01f);
    CHECK(2.0f * (centre.x - w->DC.CursorStartPos.x) < stackedThumb + 0.01f);
    CHECK(2.0f * (centre.y - w->DC.CursorStartPos.y) > stackedThumb - 0.01f);
    CHECK(2.0f * (centre.y - w->DC.CursorStartPos.y) < stackedThumb + 0.01f);
}
