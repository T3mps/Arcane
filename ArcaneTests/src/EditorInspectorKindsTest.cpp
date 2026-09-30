// Inspector filters (spec 2026-09-29 s2/s3/s5): the kind catalog, the
// exclusion filter and its label -- pure, no ImGui.
#include <catch2/catch_test_macros.hpp>
#include <Panels/InspectorKinds.hpp>

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
