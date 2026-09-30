// Inspector filters (spec 2026-09-29 s2/s3/s5): the kind catalog, the
// exclusion filter and its label -- pure, no ImGui.
#include <catch2/catch_test_macros.hpp>
#include <Panels/InspectorKinds.hpp>
#include <Widgets/IconsLucide.h>

#include <string_view>

using namespace Arcane::Editor;

TEST_CASE("InspectorKinds: the catalog order is scene, assets, input-actions, material, sprite, mesh", "[editor][inspector]")
{
    REQUIRE(kInspectorKinds.size() == 6);
    CHECK(kInspectorKinds[0].id == "scene");
    CHECK(kInspectorKinds[1].id == "assets");
    CHECK(kInspectorKinds[2].id == "input-actions");
    CHECK(kInspectorKinds[3].id == "material");
    CHECK(kInspectorKinds[4].id == "sprite");
    CHECK(kInspectorKinds[5].id == "mesh");
    CHECK(FindInspectorKind("assets")->displayName == "Assets");
    CHECK(FindInspectorKind("material")->displayName == "Materials");
    CHECK(FindInspectorKind("shader") == nullptr);
}

TEST_CASE("InspectorFilter: exclusions admit everything else, and an empty kind only under All", "[editor][inspector]")
{
    InspectorFilter all;
    CHECK(all.IsAll());
    CHECK(all.Admits("scene"));
    CHECK(all.Admits(""));                         // a document with nothing to edit: All only
    CHECK(all.Admits("future-kind"));              // a kind this build does not know: admitted

    const InspectorFilter noAssets = InspectorFilter::AllBut("assets");
    CHECK_FALSE(noAssets.IsAll());
    CHECK(noAssets.Admits("scene"));
    CHECK(noAssets.Admits("material"));
    CHECK_FALSE(noAssets.Admits("assets"));
    CHECK_FALSE(noAssets.Admits(""));              // filtered: empty kinds are out
    CHECK(noAssets.Admits("future-kind"));         // a later kind appears (decision 8.3)

    const InspectorFilter onlyAssets = InspectorFilter::Only("assets");
    CHECK(onlyAssets.excluded == std::vector<std::string>{ "scene", "input-actions", "material", "sprite", "mesh" });
    CHECK(onlyAssets.Admits("assets"));
    CHECK_FALSE(onlyAssets.Admits("scene"));
    CHECK_FALSE(onlyAssets.Admits("material"));
}

TEST_CASE("InspectorFilter: Sanitized drops unknown kinds, duplicates, and an all-excluded set", "[editor][inspector]")
{
    InspectorFilter f;
    f.excluded = { "assets", "bogus", "assets", "scene" };
    CHECK(f.Sanitized().excluded == std::vector<std::string>{ "scene", "assets" });   // catalog order
    f.excluded = { "mesh", "scene", "sprite", "input-actions", "material", "assets" };
    CHECK(f.ExcludesEveryKind());
    CHECK(f.Sanitized().IsAll());                  // nothing left to show: All, never empty
}

TEST_CASE("InspectorFilterLabel: All, one name, All but X, or the ticked list", "[editor][inspector]")
{
    CHECK(InspectorFilterLabel(InspectorFilter{}) == "All");
    CHECK(InspectorFilterLabel(InspectorFilter::Only("scene")) == "Scene");
    CHECK(InspectorFilterLabel(InspectorFilter::AllBut("assets")) == "All but Assets");
    InspectorFilter two;
    two.excluded = { "input-actions", "material", "sprite", "mesh" };   // Scene + Assets ticked
    CHECK(InspectorFilterLabel(two) == "Scene, Assets");
    InspectorFilter four;
    four.excluded = { "scene", "assets" };
    CHECK(InspectorFilterLabel(four) == "Input Actions, Materials, Sprites, Meshes");
}

// The filter dropdown's FACE (user request 2026-09-30): the ticked kinds'
// icons, catalog order, capped at what fits with a "+N" overflow; All is one
// "all" glyph, never six icons.
TEST_CASE("InspectorKinds: every catalog kind carries the Asset Browser's icon", "[editor][inspector]")
{
    for (const InspectorKind& k : kInspectorKinds)
    {
        INFO(k.id);
        REQUIRE(k.icon != nullptr);
        CHECK_FALSE(std::string_view(k.icon).empty());
    }
    CHECK(std::string_view(FindInspectorKind("scene")->icon) == ICON_LC_CLAPPERBOARD);
    CHECK(std::string_view(FindInspectorKind("assets")->icon) == ICON_LC_PACKAGE);
    CHECK(std::string_view(FindInspectorKind("input-actions")->icon) == ICON_LC_GAMEPAD_2);
    CHECK(std::string_view(FindInspectorKind("material")->icon) == ICON_LC_PALETTE);
    CHECK(std::string_view(FindInspectorKind("sprite")->icon) == ICON_LC_STICKER);
    CHECK(std::string_view(FindInspectorKind("mesh")->icon) == ICON_LC_BOX);
}

TEST_CASE("InspectorFilterFace: All is one glyph; otherwise the ticked icons in catalog order, then +N", "[editor][inspector]")
{
    const FilterFace all = InspectorFilterFace(InspectorFilter{}, 6);
    REQUIRE(all.icons.size() == 1);
    CHECK(std::string_view(all.icons[0]) == kInspectorAllIcon);
    CHECK(all.overflow == 0);
    CHECK(InspectorFilterFace(InspectorFilter{}, 1).icons.size() == 1);   // one glyph fits anywhere one icon does

    const FilterFace scene = InspectorFilterFace(InspectorFilter::Only("scene"), 6);
    REQUIRE(scene.icons.size() == 1);
    CHECK(std::string_view(scene.icons[0]) == ICON_LC_CLAPPERBOARD);
    CHECK(scene.overflow == 0);

    const FilterFace noAssets = InspectorFilterFace(InspectorFilter::AllBut("assets"), 5);
    REQUIRE(noAssets.icons.size() == 5);
    CHECK(std::string_view(noAssets.icons[0]) == ICON_LC_CLAPPERBOARD);
    CHECK(std::string_view(noAssets.icons[1]) == ICON_LC_GAMEPAD_2);
    CHECK(std::string_view(noAssets.icons[2]) == ICON_LC_PALETTE);
    CHECK(std::string_view(noAssets.icons[3]) == ICON_LC_STICKER);
    CHECK(std::string_view(noAssets.icons[4]) == ICON_LC_BOX);
    CHECK(noAssets.overflow == 0);
    CHECK(InspectorFilterFace(InspectorFilter::AllBut("assets"), 99).icons.size() == 5);

    const FilterFace narrow = InspectorFilterFace(InspectorFilter::AllBut("assets"), 3);
    REQUIRE(narrow.icons.size() == 3);
    CHECK(std::string_view(narrow.icons[0]) == ICON_LC_CLAPPERBOARD);
    CHECK(std::string_view(narrow.icons[2]) == ICON_LC_PALETTE);
    CHECK(narrow.overflow == 2);

    const FilterFace tiny = InspectorFilterFace(InspectorFilter::AllBut("assets"), 0);   // never an empty face
    CHECK(tiny.icons.size() == 1);
    CHECK(tiny.overflow == 4);
}
