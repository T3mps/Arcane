// Arcane/ArcaneTests/src/PanelRegistryTest.cpp
// Window-menu panel registry invariants + the visibility ini-line parse
// (spec 2026-08-10 editor-menu-wiring, Part I). Headless -- no ImGui.

#include <catch2/catch_test_macros.hpp>

#include "PanelRegistry.hpp"

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
