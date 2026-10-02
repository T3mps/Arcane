// Panel-split arc (2026-09-09), Task 3: RevealAssetInBrowser (spec s7.2) --
// today's Reveal sequence (the Unreferenced card's own click handler,
// pre-split) extracted to a free function so the host's `revealInBrowse`
// consumer can run it without one panel reaching into a sibling's state.
// Headless, no ImGui -- the function touches only AssetBrowserPanelState
// and AssetPanelModel, modeled on AssetPanelModelTest.cpp's fixture-building
// (a REAL temp dir + real files + registry.ScanContent, fake providers).

#include <catch2/catch_test_macros.hpp>

#include "Panels/AssetBrowserPanel.hpp"   // AssetBrowserPanelState -- the state this helper writes
#include "Panels/AssetPanelCommon.hpp"
#include "Panels/AssetPanelModel.hpp"
#include "Widgets/EditorTheme.hpp"         // Theme::kAmber / kTextDim -- DigestRefusedStyle's two looks
#include "Widgets/IconsLucide.h"

#include <Arcane/Project/AssetRegistry.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace Arcane::Editor;
namespace fs = std::filesystem;

namespace
{
    fs::path WriteFile(const fs::path& dir, const char* name, const std::string& text)
    {
        std::error_code ec;
        fs::create_directories(dir, ec);
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

    // The provider seam, faked -- same minimal shape AssetsGraphCanvasTest.cpp's
    // own FakeProviders uses: this test is about RevealAssetInBrowser's pure
    // sequencing, not about cook state or material surfaces.
    struct FakeProviders
    {
        std::unordered_map<Arcane::Guid, std::vector<Arcane::AssetRef>> refsByGuid;

        AssetPanelProviders Make()
        {
            AssetPanelProviders p;
            p.refsFor = [this](const Arcane::Guid& g) -> std::optional<std::vector<Arcane::AssetRef>>
            {
                const auto it = refsByGuid.find(g);
                return it == refsByGuid.end() ? std::vector<Arcane::AssetRef>{} : it->second;
            };
            p.cookStateFor = [](const Arcane::Guid&) { return CookState::Cooked; };
            p.surfaceFor   = [](const Arcane::Guid&) -> std::optional<Arcane::MaterialSurface>
            { return std::nullopt; };
            return p;
        }
    };
}

// RevealAssetInBrowser (panel-split spec s7.2): today's Reveal sequence
// (the Unreferenced card's own handler, pre-split) as a host-callable helper
// -- clears search + kind filter BOTH places, walks the folder ancestry open
// BOTH places, forces the derived fold, selects.
TEST_CASE("RevealAssetInBrowser clears filters, opens ancestry, selects", "[editor]")
{
    const fs::path dir = fs::temp_directory_path() / "arcane_reveal_asset_in_browser_test";
    std::error_code ec;
    fs::remove_all(dir, ec);

    // A nested entry (folder "props/crates/") that is ALSO a 1:1 derived
    // sprite folded under its own texture -- one fixture pins both legs the
    // brief calls out: the ancestry walk (two levels deep) and the fold-open
    // write (foldedUnder).
    WriteFile(dir / "props" / "crates", "crate.png", "not a real png, just bytes");   // sidecar-minted guid
    WriteFile(dir / "props" / "crates", "crate.arcsprite",
             R"({"id":"c0000001-0001-4001-8001-000000000001","type":"sprite","name":"Crate"})");

    Arcane::AssetRegistry registry;
    REQUIRE(registry.ScanContent(dir, "game") == 2);
    const auto all = registry.All();

    const Arcane::Guid textureGuid = GuidForPath(all, "game://props/crates/crate.png");
    const Arcane::Guid targetGuid  = *Arcane::Guid::FromString("c0000001-0001-4001-8001-000000000001");
    REQUIRE(textureGuid.IsValid());

    FakeProviders fake;
    fake.refsByGuid[targetGuid] = { { textureGuid, Arcane::AssetRefKind::DerivesFrom } };   // plain 1:1 -> folds

    AssetPanelModel model;
    model.MarkAllDirty();
    AssetPanelProviders providers = fake.Make();
    REQUIRE(model.RebuildIfDirty(&registry, providers));

    // Sanity: the fixture is really shaped the way the test needs before the
    // call under test runs at all.
    const AssetPanelEntry* target = model.Find(targetGuid);
    REQUIRE(target);
    REQUIRE(target->folder == "props/crates/");
    REQUIRE(target->foldedUnder == textureGuid);

    AssetBrowserPanelState state;
    std::snprintf(state.search, sizeof(state.search), "zzz-no-match");
    state.railKind = 2;
    model.SetSearch(state.search);
    model.SetKindFilter(state.railKind);
    // The direct ancestor starts CLOSED, both places -- proving the walk
    // below really forces it back open rather than finding it already so.
    state.groupOpen["props/"] = false;
    model.SetGroupOpen("props/", false);

    RevealAssetInBrowser(state, model, targetGuid);

    CHECK(state.search[0] == '\0');
    CHECK(state.railKind == -1);
    CHECK_FALSE(model.Filtered());
    CHECK(state.groupOpen.at("props/"));
    CHECK(state.groupOpen.at("props/crates/"));
    CHECK(state.childrenOpen.at(textureGuid));
    CHECK(model.selected == targetGuid);
    CHECK(state.revealPending);   // the Browser scrolls even when the guid was already selected (T3-D4)

    fs::remove_all(dir, ec);
}

// A stale guid (the action was raised, then the model moved on before the
// host consumed it) must not crash or half-run the sequence -- RevealAssetInBrowser
// reads `e->folder`/`e->foldedUnder` off the found entry, so a missing one has
// nothing to walk.
TEST_CASE("RevealAssetInBrowser is a no-op for a guid the model no longer knows", "[editor]")
{
    AssetPanelModel model;
    AssetBrowserPanelState state;
    std::snprintf(state.search, sizeof(state.search), "keep-me");
    state.railKind = 1;
    model.SetSearch(state.search);
    model.SetKindFilter(state.railKind);

    RevealAssetInBrowser(state, model, Arcane::Guid::Generate());

    // Untouched -- nothing here to clear, walk or select.
    CHECK(std::string(state.search) == "keep-me");
    CHECK(state.railKind == 1);
    CHECK_FALSE(model.selected.IsValid());
    CHECK_FALSE(state.revealPending);
}

// Node page phase s6.7: the refused count's look, shared by the health
// digest and the Status panel's refused tile. Pure -- no ImGui context.
namespace
{
    bool SameRgba(const ImVec4& a, const ImVec4& b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }
}

TEST_CASE("DigestRefusedStyle: zero is quiet, any refusal is the amber alarm", "[editor][assets]")
{
    const std::string_view triangle = ICON_LC_TRIANGLE_ALERT;
    for (int quiet : { 0, -3 })
    {
        INFO(quiet);
        const RefusedStyle s = DigestRefusedStyle(quiet);
        CHECK_FALSE(s.alarm);
        CHECK(s.text == "0 refused");
        CHECK(s.text.find(triangle) == std::string::npos);
        CHECK(SameRgba(s.color, Theme::kTextDim));
        CHECK(s.tileVariant == 0);
    }
    for (int loud : { 1, 12 })
    {
        INFO(loud);
        const RefusedStyle s = DigestRefusedStyle(loud);
        CHECK(s.alarm);
        CHECK(s.text.rfind(triangle, 0) == 0);   // starts with the triangle
        CHECK(s.text == std::string(triangle) + " " + std::to_string(loud) + " refused");
        CHECK(SameRgba(s.color, Theme::kAmber));
        CHECK(s.tileVariant == 1);
    }
}
