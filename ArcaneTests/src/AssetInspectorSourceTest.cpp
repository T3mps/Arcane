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
#include "Widgets/IconsLucide.h"            // ICON_LC_COPY

#include <Arcane/Project/AssetRegistry.hpp>
#include <Arcane/Project/Project.hpp>

#include <imgui.h>
#include <imgui_internal.h>                 // ActivateItemByID

#include <cstdint>
#include <filesystem>
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
    src.Bind({ &model, nullptr, nullptr, nullptr, nullptr });
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
    src.Bind({ &model, nullptr, nullptr, nullptr, nullptr });
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
    src.Bind({ &model, nullptr, nullptr, nullptr, nullptr });
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
    auto frame = [&](const char* activate)
    {
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(640.0f, 1000.0f), ImGuiCond_Always);
        ImGui::Begin("t");
        if (activate) ImGui::ActivateItemByID(ImGui::GetID(activate));
        DrawAssetPage(*model.Find(gBrick), model, &*project, /*docs*/ nullptr, services, actions);
        ImGui::End();
        ImGui::Render();                                    // draw data discarded -- no backend
    };

    frame(nullptr);                                         // warm-up: the window exists
    frame(ICON_LC_COPY " Copy Path");                       // queue the press
    frame(nullptr);                                         // the press lands
    CHECK(actions.copyPath == gBrick);                      // reported, never performed

    // The import settings drew (the ".png only" note returns before the knobs):
    // pressing sRGB flips the value merge-written into the .meta sidecar.
    const fs::path meta = root / "Content" / "brick.png.meta";
    const bool srgbBefore = ReadTextureMetaSettingsDisplay(meta).srgb;
    frame("sRGB##texmeta");
    frame(nullptr);
    CHECK(ReadTextureMetaSettingsDisplay(meta).srgb != srgbBefore);
}
