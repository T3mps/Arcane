// Arcane/ArcaneTests/src/PanelRegistryTest.cpp
// Window-menu panel registry invariants + the visibility ini-line parse
// (spec 2026-08-10 editor-menu-wiring, Part I). Headless -- no ImGui.

#include <catch2/catch_test_macros.hpp>

#include "Panels/PanelRegistry.hpp"

#include <cstring>
#include <set>
#include <string>

using namespace Arcane::Editor;

TEST_CASE("panel table: entry i has id i, names unique, exactly one permanent",
          "[editor]")
{
    std::set<std::string> names;
    int permanents = 0;
    for (std::size_t i = 0; i < std::size(kPanels); ++i)
    {
        CHECK(static_cast<std::size_t>(kPanels[i].id) == i);
        CHECK(names.insert(kPanels[i].name).second);
        if (kPanels[i].permanent)
            ++permanents;
    }
    CHECK(std::size(kPanels) == static_cast<std::size_t>(PanelId::Count));
    CHECK(permanents == 1);
    CHECK(kPanels[0].id == PanelId::Viewport);
    CHECK(kPanels[0].permanent);
}

TEST_CASE("PanelVisibility defaults all-visible; permanent panel cannot hide",
          "[editor]")
{
    PanelVisibility v;
    for (const PanelInfo& p : kPanels)
        CHECK(v.IsVisible(p.id));

    // The array slot for the permanent panel is ignored by IsVisible and
    // unreachable through OpenFlag.
    v.visible[static_cast<std::size_t>(PanelId::Viewport)] = false;
    CHECK(v.IsVisible(PanelId::Viewport));
    CHECK(v.OpenFlag(PanelId::Viewport) == nullptr);

    // A non-permanent panel round-trips through its OpenFlag (the tab X and
    // the menu checkmark write the same bool).
    bool* console = v.OpenFlag(PanelId::Console);
    REQUIRE(console != nullptr);
    *console = false;
    CHECK(!v.IsVisible(PanelId::Console));
}

TEST_CASE("ParsePanelVisibilityLine: valid lines, junk, and the viewport ban",
          "[editor]")
{
    auto r = ParsePanelVisibilityLine("Console=0");
    REQUIRE(r.has_value());
    CHECK(r->first == PanelId::Console);
    CHECK(r->second == false);

    r = ParsePanelVisibilityLine("Problems=1");
    REQUIRE(r.has_value());
    CHECK(r->first == PanelId::Problems);
    CHECK(r->second == true);

    CHECK(!ParsePanelVisibilityLine(nullptr).has_value());
    CHECK(!ParsePanelVisibilityLine("").has_value());
    CHECK(!ParsePanelVisibilityLine("Console").has_value());
    CHECK(!ParsePanelVisibilityLine("=1").has_value());
    CHECK(!ParsePanelVisibilityLine("Console=2").has_value());
    CHECK(!ParsePanelVisibilityLine("Console=10").has_value());
    CHECK(!ParsePanelVisibilityLine("NoSuchPanel=1").has_value());
    // Permanent panels are never persisted and never parsed back.
    CHECK(!ParsePanelVisibilityLine("Viewport=0").has_value());
}

TEST_CASE("panel table: every row carries a valid section; section labels exist",
          "[editor]")
{
    for (const PanelInfo& p : kPanels)
        CHECK(static_cast<std::size_t>(p.section) <
              static_cast<std::size_t>(PanelSection::Count));
    // One label per section, and the menu order names each section once.
    CHECK(std::size(kSectionLabels) == static_cast<std::size_t>(PanelSection::Count));
    std::set<PanelSection> seen;
    for (PanelSection s : kSectionMenuOrder)
        CHECK(seen.insert(s).second);
    CHECK(seen.size() == static_cast<std::size_t>(PanelSection::Count));
    // All three asset rows sit in the ASSETS section (panel-split spec s4.1/
    // s4.4). Task 7 replaced the single `Assets` row with these three; a
    // fourth asset panel that forgot its section would land in SCENE (enum
    // value 2's default-constructed slot is not what these check -- each row
    // states its section outright, and this is what pins that it is right).
    CHECK(kPanels[static_cast<std::size_t>(PanelId::AssetBrowser)].section == PanelSection::Assets);
    CHECK(kPanels[static_cast<std::size_t>(PanelId::AssetGraph)].section   == PanelSection::Assets);
    CHECK(kPanels[static_cast<std::size_t>(PanelId::AssetStatus)].section  == PanelSection::Assets);
    // ...and they are the ONLY three, so the ASSETS group never silently
    // gains a member (the menu draws whatever carries the section).
    int assetRows = 0;
    for (const PanelInfo& p : kPanels)
        if (p.section == PanelSection::Assets)
            ++assetRows;
    CHECK(assetRows == 3);
    // DIAGNOSTICS order is Problems then Console (board order, spec s4.2).
    CHECK(static_cast<std::size_t>(PanelId::Problems) <
          static_cast<std::size_t>(PanelId::Console));
}

TEST_CASE("ParsePanelVisibilityLine: the three asset panel names round-trip",
          "[editor]")
{
    // Panel-split spec s4.1: each row's name is simultaneously the
    // ImGui::Begin title, the menu label and THIS ini key, so a rename that
    // missed the registry would show up here as an unparseable line.
    auto r = ParsePanelVisibilityLine("Asset Browser=0");
    REQUIRE(r.has_value());
    CHECK(r->first == PanelId::AssetBrowser);
    CHECK(r->second == false);

    r = ParsePanelVisibilityLine("Asset Graph=1");
    REQUIRE(r.has_value());
    CHECK(r->first == PanelId::AssetGraph);
    CHECK(r->second == true);

    r = ParsePanelVisibilityLine("Asset Status=1");
    REQUIRE(r.has_value());
    CHECK(r->first == PanelId::AssetStatus);
    CHECK(r->second == true);

    // ...and the retired name is now simply unknown. An OLD ini carrying
    // "Assets=1" must be ignored, not silently applied to one of the three
    // (name-keyed parsing is what makes that true by construction, and this
    // is the assertion that it stayed true through the rename).
    CHECK(!ParsePanelVisibilityLine("Assets=1").has_value());
    // The space in each name is load-bearing -- a caller that stripped it
    // would parse nothing.
    CHECK(!ParsePanelVisibilityLine("AssetBrowser=1").has_value());
}
