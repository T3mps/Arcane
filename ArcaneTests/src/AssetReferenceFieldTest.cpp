// The shared asset-reference field (spec 2026-09-30 s4.2). Part 1: the PURE
// halves (DescribeAssetRef / DecideAssetRefDrop / AssetRefCandidates) over a
// REAL project with three assets (AssetInspectorSourceTest.cpp's Project::
// Create shape) and a model built with faked providers. Part 2 (T2-B2) drives
// the cell through device-less ImGui frames; part 3 (T2-B3) the entity page.
#include <catch2/catch_test_macros.hpp>

#include "Panels/AssetPanelModel.hpp"
#include "Panels/AssetReferenceField.hpp"

#include <Arcane/Project/Project.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace Arcane::Editor;
namespace fs = std::filesystem;

namespace
{
    fs::path WriteFile(const fs::path& dir, const char* name, const std::string& text)
    {
        fs::path p = dir / name;
        std::ofstream(p, std::ios::binary) << text;
        return p;
    }

    Arcane::Guid GuidForPath(const std::vector<std::pair<Arcane::Guid, std::string>>& all, std::string_view mountPath)
    {
        for (const auto& [guid, path] : all)
            if (path == mountPath) return guid;
        return Arcane::Guid{};
    }

    // brick.png (Texture), wall.arcmat (Material, CONFIRMED Mesh surface) and
    // sub/wall.arcmat (Material, surface UNKNOWN): two "wall"s so the
    // name-then-path order is observable.
    struct RefFixture
    {
        fs::path root;
        std::optional<Arcane::Project> project;
        AssetPanelModel model;
        Arcane::Guid gBrick, gWall, gSubWall;
        std::unordered_map<Arcane::Guid, Arcane::MaterialSurface> surfaces;
        AssetRefServices services;

        explicit RefFixture(const char* name) : root(fs::temp_directory_path() / name)
        {
            std::error_code ec;
            fs::remove_all(root, ec);
            REQUIRE(Arcane::Project::Create(root, "AssetRef").has_value());
            WriteFile(root / "Content", "brick.png", "not a real png, just bytes");   // sidecar-minted guid
            WriteFile(root / "Content", "wall.arcmat",
                      R"({"id":"a5500002-0002-4002-8002-000000000002","type":"material","kind":"mesh"})");
            fs::create_directories(root / "Content" / "sub");
            WriteFile(root / "Content" / "sub", "wall.arcmat",
                      R"({"id":"a5500003-0003-4003-8003-000000000003","type":"material","kind":"fullscreen"})");
            project = Arcane::Project::Open(root);
            REQUIRE(project.has_value());
            const auto all = project->Registry().All();
            gBrick = GuidForPath(all, "game://brick.png");
            gWall = GuidForPath(all, "game://wall.arcmat");
            gSubWall = GuidForPath(all, "game://sub/wall.arcmat");
            REQUIRE(gBrick.IsValid());
            REQUIRE(gWall.IsValid());
            REQUIRE(gSubWall.IsValid());
            surfaces[gWall] = Arcane::MaterialSurface::Mesh;   // sub/wall stays unknown (nullopt)
            AssetPanelProviders p;
            p.surfaceFor = [this](const Arcane::Guid& g) -> std::optional<Arcane::MaterialSurface>
            {
                const auto it = surfaces.find(g);
                return it == surfaces.end() ? std::nullopt : std::optional<Arcane::MaterialSurface>(it->second);
            };
            p.refsFor = [](const Arcane::Guid&) -> std::optional<std::vector<Arcane::AssetRef>>
            { return std::vector<Arcane::AssetRef>{}; };
            p.cookStateFor = [](const Arcane::Guid&) { return CookState::Cooked; };
            model.MarkAllDirty();
            REQUIRE(model.RebuildIfDirty(&project->Registry(), p));
            REQUIRE(model.Find(gWall) != nullptr);
            REQUIRE(model.Find(gWall)->surface.has_value());
            services.model = &model;
            services.project = [this]() -> const Arcane::Project* { return &*project; };
        }
        ~RefFixture()
        {
            project.reset();
            std::error_code ec;
            fs::remove_all(root, ec);
        }
    };

    std::size_t IndexOf(const std::vector<const AssetPanelEntry*>& rows, const Arcane::Guid& g)
    {
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (rows[i]->guid == g) return i;
        return rows.size();
    }
}

TEST_CASE("DecideAssetRefDrop: kind match sets, read-only and identity refuse, a texture mints only on a minting sprite field", "[editor][assetref]")
{
    const Arcane::Guid g = Arcane::Guid::Generate();
    const AssetDragPayload mat{ g, AssetKind::Material }, tex{ g, AssetKind::Texture }, spr{ g, AssetKind::Sprite };
    AssetRefArgs material;
    material.kindFilter = static_cast<int>(AssetKind::Material);
    CHECK(DecideAssetRefDrop(mat, material) == AssetRefDropVerdict::Set);        // match
    CHECK(DecideAssetRefDrop(tex, material) == AssetRefDropVerdict::Refuse);     // mismatch: texture on a material
    AssetRefArgs any;
    CHECK(DecideAssetRefDrop(tex, any) == AssetRefDropVerdict::Set);             // any kind
    AssetRefArgs ro = any;
    ro.readOnly = true;
    CHECK(DecideAssetRefDrop(tex, ro) == AssetRefDropVerdict::Refuse);
    AssetRefArgs identity = any;
    identity.identityGuid = true;                                                 // the Identity::id drop (InspectorView.cpp:1142-1155)
    CHECK(DecideAssetRefDrop(tex, identity) == AssetRefDropVerdict::Refuse);
    AssetRefArgs sprite;
    sprite.kindFilter = static_cast<int>(AssetKind::Sprite);
    CHECK(DecideAssetRefDrop(spr, sprite) == AssetRefDropVerdict::Set);
    CHECK(DecideAssetRefDrop(tex, sprite) == AssetRefDropVerdict::Refuse);       // no mint without allowTextureMint
    sprite.allowTextureMint = true;
    CHECK(DecideAssetRefDrop(tex, sprite) == AssetRefDropVerdict::MintSprite);
}

TEST_CASE("AssetRefCandidates: kind filter, confirmed-surface exclusion, unknown kept, name-then-path order, search on name and path", "[editor][assetref]")
{
    RefFixture fx("arcane_assetref_candidates_test");
    const int material = static_cast<int>(AssetKind::Material);
    const auto textures = AssetRefCandidates(fx.model, static_cast<int>(AssetKind::Texture), -1, "");
    REQUIRE(textures.size() == 1);
    CHECK(textures[0]->guid == fx.gBrick);

    const auto mats = AssetRefCandidates(fx.model, material, -1, "");
    REQUIRE(mats.size() == 2);
    CHECK(mats[0]->guid == fx.gSubWall);                  // both "wall": "game://sub/wall..." < "game://wall..."
    CHECK(mats[1]->guid == fx.gWall);

    const auto meshSurface = AssetRefCandidates(fx.model, material, static_cast<int>(Arcane::MaterialSurface::Mesh), "");
    CHECK(meshSurface.size() == 2);                       // wall matches, sub/wall unknown -> kept
    const auto spriteSurface = AssetRefCandidates(fx.model, material, static_cast<int>(Arcane::MaterialSurface::Sprite), "");
    REQUIRE(spriteSurface.size() == 1);                   // wall is CONFIRMED Mesh: out
    CHECK(spriteSurface[0]->guid == fx.gSubWall);

    const auto all = AssetRefCandidates(fx.model, -1, -1, "");
    CHECK(IndexOf(all, fx.gBrick) < IndexOf(all, fx.gSubWall));   // "brick" < "wall"
    CHECK(IndexOf(all, fx.gSubWall) < IndexOf(all, fx.gWall));
    CHECK(IndexOf(all, fx.gWall) < all.size());

    const auto byName = AssetRefCandidates(fx.model, -1, -1, "BRI");       // case-insensitive
    REQUIRE(byName.size() == 1);
    CHECK(byName[0]->guid == fx.gBrick);
    const auto byPath = AssetRefCandidates(fx.model, -1, -1, "sub/");
    REQUIRE(byPath.size() == 1);
    CHECK(byPath[0]->guid == fx.gSubWall);
}

TEST_CASE("DescribeAssetRef: identity, mixed, nil, resolved, unknown-to-model, dangling, tombstone, null services", "[editor][assetref]")
{
    RefFixture fx("arcane_assetref_describe_test");
    AssetRefArgs a;

    a.guid = fx.gBrick;
    a.identityGuid = true;                                // never resolved, never browsable
    AssetRefDisplay d = DescribeAssetRef(a, fx.services);
    CHECK(d.text == fx.gBrick.ToString());
    CHECK_FALSE(d.browsable);
    CHECK_FALSE(d.dangling);
    a.identityGuid = false;

    a.mixed = true;
    CHECK(DescribeAssetRef(a, fx.services).text == "--");
    a.identityGuid = true;                                // Identity::id across a multi-selection: "--", never the primary's id
    CHECK(DescribeAssetRef(a, fx.services).text == "--");
    a.identityGuid = false;
    a.mixed = false;

    AssetRefArgs nil;
    CHECK(DescribeAssetRef(nil, fx.services).text == "(none)");

    d = DescribeAssetRef(a, fx.services);                 // resolved
    CHECK(d.text == "brick.png");
    CHECK(d.tooltip == "game://brick.png");
    CHECK(d.browsable);
    CHECK_FALSE(d.dangling);
    CHECK(d.kind == AssetKind::Texture);

    AssetPanelModel empty;                                // the model does not know the guid yet
    AssetRefServices early = fx.services;
    early.model = &empty;
    d = DescribeAssetRef(a, early);
    CHECK(d.text == "brick.png");                         // the mount path's last segment
    CHECK(d.kind == AssetKind::Texture);

    AssetRefArgs gone;
    gone.guid = *Arcane::Guid::FromString("deadbeef-0000-4000-8000-000000000001");
    d = DescribeAssetRef(gone, fx.services);
    CHECK(d.dangling);
    CHECK(d.text == gone.guid.ToString() + " (missing)");
    CHECK_FALSE(d.browsable);
    AssetRefServices tomb = fx.services;
    tomb.tombstoneName = [](const Arcane::Guid&) -> std::optional<std::string> { return std::string("old_wall.arcmat"); };
    d = DescribeAssetRef(gone, tomb);
    CHECK(d.text == "old_wall.arcmat (missing)");
    CHECK(d.tooltip.find(gone.guid.ToString()) != std::string::npos);

    const AssetRefServices none{};                        // headless: raw guid, never dangling
    d = DescribeAssetRef(a, none);
    CHECK(d.text == fx.gBrick.ToString());
    CHECK_FALSE(d.browsable);
    d = DescribeAssetRef(gone, none);
    CHECK(d.text == gone.guid.ToString());
    CHECK_FALSE(d.dangling);
}
