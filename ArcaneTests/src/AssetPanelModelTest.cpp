// Asset-manager arc (Plan 1 Task 4): AssetPanelModel -- the cached, foldable
// model behind the Assets panel's Browse lens. Modeled on
// AssetBrowserTest.cpp:57's pattern (a REAL temp dir + real files +
// registry.ScanContent), with the AssetPanelProviders callables faked
// in-test -- this unit never touches the engine facade, so there is no
// Arcane::Assets/Arcane::Project in sight here either.

#include <catch2/catch_test_macros.hpp>

#include "Panels/AssetPanelModel.hpp"

#include <Arcane/Project/AssetRegistry.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
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

        std::unordered_map<Arcane::Guid, int> refsCalls;
        std::unordered_map<Arcane::Guid, int> cookCalls;
        std::unordered_map<Arcane::Guid, int> surfaceCalls;

        AssetPanelProviders Make()
        {
            AssetPanelProviders p;
            p.refsFor = [this](const Arcane::Guid& g) -> std::optional<std::vector<Arcane::AssetRef>>
            {
                ++refsCalls[g];
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
}

// ---------------------------------------------------------------------------
// (a) grouping + ordering (+ isInstance, a verbatim struct field)
// ---------------------------------------------------------------------------

TEST_CASE("AssetPanelModel groups by folder and sorts groups/rows lexicographically", "[editor]")
{
    const fs::path dir = fs::temp_directory_path() / "arcane_asset_panel_model_group_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "materials");
    fs::create_directories(dir / "fx" / "glow");

    WriteFile(dir / "materials", "beta.arcmat",
             R"({"id":"a0000001-0001-4001-8001-000000000001","type":"material","kind":"fullscreen"})");
    WriteFile(dir / "materials", "alpha.arcmat",
             R"({"id":"a0000001-0001-4001-8001-000000000002","type":"material","kind":"fullscreen"})");
    WriteFile(dir / "fx" / "glow", "particle.arcmat",
             R"({"id":"a0000001-0001-4001-8001-000000000003","type":"material","kind":"fullscreen"})");
    WriteFile(dir, "readme.json", R"({"id":"a0000001-0001-4001-8001-000000000004"})");
    // An instance (parent DerivesFrom) beside the plain materials, pinning
    // AssetPanelEntry::isInstance in the same pass.
    WriteFile(dir / "materials", "inst.arcmat",
             R"({"id":"a0000001-0001-4001-8001-000000000005","type":"material",)"
             R"("parent":"a0000001-0001-4001-8001-000000000002"})");

    Arcane::AssetRegistry registry;
    REQUIRE(registry.ScanContent(dir, "game") == 5);

    const Arcane::Guid betaId     = *Arcane::Guid::FromString("a0000001-0001-4001-8001-000000000001");
    const Arcane::Guid alphaId    = *Arcane::Guid::FromString("a0000001-0001-4001-8001-000000000002");
    const Arcane::Guid particleId = *Arcane::Guid::FromString("a0000001-0001-4001-8001-000000000003");
    const Arcane::Guid readmeId   = *Arcane::Guid::FromString("a0000001-0001-4001-8001-000000000004");
    const Arcane::Guid instId     = *Arcane::Guid::FromString("a0000001-0001-4001-8001-000000000005");

    FakeProviders fake;
    fake.refsByGuid[instId] = { { alphaId, Arcane::AssetRefKind::DerivesFrom } };

    AssetPanelModel model;
    model.MarkAllDirty();
    REQUIRE(model.RebuildIfDirty(&registry, fake.Make()));

    const auto& rows = model.Rows();
    REQUIRE(rows.size() == 8);   // 3 groups + 5 assets

    CHECK(rows[0].type == AssetPanelRow::Type::Group);
    CHECK(rows[0].groupName == "Content/");
    CHECK(rows[0].groupCount == 1);
    CHECK(rows[1].type == AssetPanelRow::Type::Asset);
    CHECK(rows[1].guid == readmeId);

    CHECK(rows[2].type == AssetPanelRow::Type::Group);
    CHECK(rows[2].groupName == "fx/glow/");
    CHECK(rows[2].groupCount == 1);
    CHECK(rows[3].guid == particleId);

    CHECK(rows[4].type == AssetPanelRow::Type::Group);
    CHECK(rows[4].groupName == "materials/");
    CHECK(rows[4].groupCount == 3);
    CHECK(rows[5].guid == alphaId);   // fileName order: alpha < beta < inst
    CHECK(rows[6].guid == betaId);
    CHECK(rows[7].guid == instId);

    const AssetPanelEntry* inst  = model.Find(instId);
    const AssetPanelEntry* alpha = model.Find(alphaId);
    REQUIRE(inst);
    REQUIRE(alpha);
    CHECK(inst->isInstance);
    CHECK_FALSE(alpha->isInstance);
    CHECK(inst->fileName == "inst.arcmat");
    CHECK(alpha->name == "alpha");

    fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// (b) fold: a 1:1 sprite disappears as a peer, appears under its expanded
// texture, with derivedChildren.size() as the count-pill data.
// ---------------------------------------------------------------------------

TEST_CASE("AssetPanelModel folds a 1:1 sprite under its texture; a sliced sprite stays a peer", "[editor]")
{
    const fs::path dir = fs::temp_directory_path() / "arcane_asset_panel_model_fold_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "sprites");

    WriteFile(dir, "hero.png", "not a real png, just bytes");   // sidecar-minted guid
    WriteFile(dir / "sprites", "hero_full.arcsprite",
             R"({"id":"b0000001-0001-4001-8001-000000000001","type":"sprite","name":"HeroFull"})");
    WriteFile(dir / "sprites", "hero_slice.arcsprite",
             R"({"id":"b0000001-0001-4001-8001-000000000002","type":"sprite","name":"HeroSlice"})");

    Arcane::AssetRegistry registry;
    REQUIRE(registry.ScanContent(dir, "game") == 3);
    const auto all = registry.All();

    const Arcane::Guid heroTexId    = GuidForPath(all, "game://hero.png");
    const Arcane::Guid heroFullId   = *Arcane::Guid::FromString("b0000001-0001-4001-8001-000000000001");
    const Arcane::Guid heroSliceId  = *Arcane::Guid::FromString("b0000001-0001-4001-8001-000000000002");
    REQUIRE(heroTexId.IsValid());

    FakeProviders fake;
    fake.refsByGuid[heroFullId]  = { { heroTexId, Arcane::AssetRefKind::DerivesFrom } };   // plain 1:1 -> folds
    fake.refsByGuid[heroSliceId] = { { heroTexId, Arcane::AssetRefKind::References } };    // sliced -> stays a peer

    AssetPanelModel model;
    model.MarkAllDirty();
    AssetPanelProviders providers = fake.Make();
    REQUIRE(model.RebuildIfDirty(&registry, providers));

    const AssetPanelEntry* heroTex   = model.Find(heroTexId);
    const AssetPanelEntry* heroFull  = model.Find(heroFullId);
    const AssetPanelEntry* heroSlice = model.Find(heroSliceId);
    REQUIRE(heroTex);
    REQUIRE(heroFull);
    REQUIRE(heroSlice);

    CHECK(heroFull->foldedUnder == heroTexId);
    CHECK_FALSE(heroFull->sliced);
    CHECK_FALSE(heroSlice->foldedUnder.IsValid());
    CHECK(heroSlice->sliced);
    REQUIRE(heroTex->derivedChildren.size() == 1);   // count-pill data
    CHECK(heroTex->derivedChildren[0] == heroFullId);

    // Default collapsed: hero_full is invisible, hero_slice is a normal peer.
    {
        const auto& rows = model.Rows();
        bool sawFullAsRow = false, sawFullAsChild = false, sawSliceAsRow = false;
        for (const auto& row : rows)
        {
            if (row.guid == heroFullId)
            {
                if (row.type == AssetPanelRow::Type::Asset) sawFullAsRow = true;
                if (row.type == AssetPanelRow::Type::Child) sawFullAsChild = true;
            }
            if (row.guid == heroSliceId && row.type == AssetPanelRow::Type::Asset)
                sawSliceAsRow = true;
        }
        CHECK_FALSE(sawFullAsRow);
        CHECK_FALSE(sawFullAsChild);
        CHECK(sawSliceAsRow);
    }

    // Expand: hero_full now renders as a Child row.
    model.SetChildrenOpen(heroTexId, true);
    REQUIRE(model.RebuildIfDirty(&registry, providers));
    {
        const auto& rows = model.Rows();
        bool sawFullAsChild = false;
        for (const auto& row : rows)
            if (row.guid == heroFullId && row.type == AssetPanelRow::Type::Child)
                sawFullAsChild = true;
        CHECK(sawFullAsChild);
    }

    fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// (c) search filters children independently of their parent's own match.
// ---------------------------------------------------------------------------

TEST_CASE("AssetPanelModel filters child rows independently -- a non-matching parent hides even a matching child", "[editor]")
{
    const fs::path dir = fs::temp_directory_path() / "arcane_asset_panel_model_search_children_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "sprites");

    WriteFile(dir, "match.png", "bytes-a");
    WriteFile(dir, "other.png", "bytes-b");
    WriteFile(dir / "sprites", "kid_alpha.arcsprite",
             R"({"id":"c0000001-0001-4001-8001-000000000001","type":"sprite"})");   // no "match" substring
    WriteFile(dir / "sprites", "kid_matchable.arcsprite",
             R"({"id":"c0000001-0001-4001-8001-000000000002","type":"sprite"})");   // contains "match"

    Arcane::AssetRegistry registry;
    REQUIRE(registry.ScanContent(dir, "game") == 4);
    const auto all = registry.All();

    const Arcane::Guid matchTexId   = GuidForPath(all, "game://match.png");
    const Arcane::Guid otherTexId   = GuidForPath(all, "game://other.png");
    const Arcane::Guid kidAlphaId   = *Arcane::Guid::FromString("c0000001-0001-4001-8001-000000000001");
    const Arcane::Guid kidMatchId   = *Arcane::Guid::FromString("c0000001-0001-4001-8001-000000000002");
    REQUIRE(matchTexId.IsValid());
    REQUIRE(otherTexId.IsValid());

    FakeProviders fake;
    fake.refsByGuid[kidAlphaId] = { { matchTexId, Arcane::AssetRefKind::DerivesFrom } };
    fake.refsByGuid[kidMatchId] = { { otherTexId, Arcane::AssetRefKind::DerivesFrom } };

    AssetPanelModel model;
    model.MarkAllDirty();
    AssetPanelProviders providers = fake.Make();
    REQUIRE(model.RebuildIfDirty(&registry, providers));

    model.SetChildrenOpen(matchTexId, true);
    model.SetChildrenOpen(otherTexId, true);
    model.SetSearch("match");
    REQUIRE(model.RebuildIfDirty(&registry, providers));

    const auto& rows = model.Rows();
    bool sawMatchTex = false, sawOtherTex = false, sawKidAlpha = false, sawKidMatch = false;
    int groupCount = 0;
    for (const auto& row : rows)
    {
        if (row.type == AssetPanelRow::Type::Group) ++groupCount;
        if (row.guid == matchTexId) sawMatchTex = true;
        if (row.guid == otherTexId) sawOtherTex = true;
        if (row.guid == kidAlphaId) sawKidAlpha = true;
        if (row.guid == kidMatchId) sawKidMatch = true;
    }
    CHECK(groupCount == 1);              // only "Content/" (match.png) -- other.png's group would be empty
    CHECK(sawMatchTex);                  // parent matches "match" -> shown
    CHECK_FALSE(sawOtherTex);            // parent does not match -> hidden
    CHECK_FALSE(sawKidAlpha);            // child doesn't independently match, despite an open+shown parent
    CHECK_FALSE(sawKidMatch);            // child WOULD match, but its parent is hidden -> no host row

    fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// (d) kind filter + ShownAssetCount
// ---------------------------------------------------------------------------

TEST_CASE("AssetPanelModel kind filter narrows Rows() and ShownAssetCount", "[editor]")
{
    const fs::path dir = fs::temp_directory_path() / "arcane_asset_panel_model_kindfilter_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "materials");
    fs::create_directories(dir / "sprites");

    WriteFile(dir, "hero.png", "bytes-a");
    WriteFile(dir, "sidekick.png", "bytes-b");
    WriteFile(dir / "materials", "mat.arcmat",
             R"({"id":"d0000001-0001-4001-8001-000000000001","type":"material","kind":"fullscreen"})");
    WriteFile(dir / "sprites", "standalone.arcsprite",
             R"({"id":"d0000001-0001-4001-8001-000000000002","type":"sprite"})");

    Arcane::AssetRegistry registry;
    REQUIRE(registry.ScanContent(dir, "game") == 4);

    FakeProviders fake;
    AssetPanelModel model;
    model.MarkAllDirty();
    AssetPanelProviders providers = fake.Make();
    REQUIRE(model.RebuildIfDirty(&registry, providers));

    CHECK_FALSE(model.Filtered());
    CHECK(model.ShownAssetCount() == 4);
    CHECK(model.Health().total == 4);

    model.SetKindFilter(static_cast<int>(AssetKind::Texture));
    REQUIRE(model.RebuildIfDirty(&registry, providers));

    CHECK(model.Filtered());
    CHECK(model.ShownAssetCount() == 2);   // hero.png + sidekick.png only

    int assetRows = 0;
    for (const auto& row : model.Rows())
        if (row.type == AssetPanelRow::Type::Asset)
            ++assetRows;
    CHECK(assetRows == 2);

    model.SetKindFilter(-1);
    REQUIRE(model.RebuildIfDirty(&registry, providers));
    CHECK_FALSE(model.Filtered());
    CHECK(model.ShownAssetCount() == 4);

    fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// (e) rail counts hide empty kinds
// ---------------------------------------------------------------------------

TEST_CASE("AssetPanelModel rail hides zero-count kinds", "[editor]")
{
    const fs::path dir = fs::temp_directory_path() / "arcane_asset_panel_model_rail_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "materials");

    WriteFile(dir, "hero.png", "bytes-a");
    WriteFile(dir / "materials", "mat.arcmat",
             R"({"id":"e0000001-0001-4001-8001-000000000001","type":"material","kind":"fullscreen"})");

    Arcane::AssetRegistry registry;
    REQUIRE(registry.ScanContent(dir, "game") == 2);

    FakeProviders fake;
    AssetPanelModel model;
    model.MarkAllDirty();
    REQUIRE(model.RebuildIfDirty(&registry, fake.Make()));

    const auto& rail = model.Rail();
    // "All" + Material + Texture only -- Audio/Font/Data/Scene/Sprite/
    // Diagnostic/Mesh/Other are all zero-count here and must be absent.
    REQUIRE(rail.size() == 3);
    CHECK(rail[0].kind == -1);
    CHECK(rail[0].label == "All");
    CHECK(rail[0].count == 2);

    bool sawMaterial = false, sawTexture = false;
    for (const auto& entry : rail)
    {
        if (entry.kind == static_cast<int>(AssetKind::Material)) { sawMaterial = true; CHECK(entry.count == 1); }
        if (entry.kind == static_cast<int>(AssetKind::Texture))  { sawTexture  = true; CHECK(entry.count == 1); }
        CHECK(entry.kind != static_cast<int>(AssetKind::Audio));
        CHECK(entry.kind != static_cast<int>(AssetKind::Sprite));
        CHECK(entry.kind != static_cast<int>(AssetKind::Scene));
    }
    CHECK(sawMaterial);
    CHECK(sawTexture);

    fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// (f) MarkDirty on one guid re-asks providers ONLY for it
// ---------------------------------------------------------------------------

TEST_CASE("AssetPanelModel MarkDirty on one guid re-asks the providers only for that guid", "[editor]")
{
    const fs::path dir = fs::temp_directory_path() / "arcane_asset_panel_model_markdirty_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "materials");
    fs::create_directories(dir / "sprites");

    WriteFile(dir, "hero.png", "bytes-a");
    WriteFile(dir / "materials", "mat.arcmat",
             R"({"id":"f0000001-0001-4001-8001-000000000001","type":"material","kind":"fullscreen"})");
    WriteFile(dir / "sprites", "child.arcsprite",
             R"({"id":"f0000001-0001-4001-8001-000000000002","type":"sprite"})");

    Arcane::AssetRegistry registry;
    REQUIRE(registry.ScanContent(dir, "game") == 3);
    const auto all = registry.All();

    const Arcane::Guid heroTexId = GuidForPath(all, "game://hero.png");
    const Arcane::Guid matId     = *Arcane::Guid::FromString("f0000001-0001-4001-8001-000000000001");
    const Arcane::Guid childId   = *Arcane::Guid::FromString("f0000001-0001-4001-8001-000000000002");
    REQUIRE(heroTexId.IsValid());

    FakeProviders fake;
    fake.refsByGuid[childId] = { { heroTexId, Arcane::AssetRefKind::DerivesFrom } };

    AssetPanelModel model;
    model.MarkAllDirty();
    AssetPanelProviders providers = fake.Make();
    REQUIRE(model.RebuildIfDirty(&registry, providers));

    REQUIRE(fake.refsCalls[heroTexId] == 1);
    REQUIRE(fake.refsCalls[matId] == 1);
    REQUIRE(fake.refsCalls[childId] == 1);
    REQUIRE(fake.cookCalls[heroTexId] == 1);
    REQUIRE(fake.cookCalls[matId] == 1);
    REQUIRE(fake.cookCalls[childId] == 1);
    REQUIRE(fake.surfaceCalls[matId] == 1);        // materials only
    CHECK(fake.surfaceCalls.find(heroTexId) == fake.surfaceCalls.end());
    CHECK(fake.surfaceCalls.find(childId) == fake.surfaceCalls.end());

    model.MarkDirty(heroTexId);
    REQUIRE(model.RebuildIfDirty(&registry, providers));

    CHECK(fake.refsCalls[heroTexId] == 2);         // re-asked
    CHECK(fake.refsCalls[matId] == 1);              // untouched
    CHECK(fake.refsCalls[childId] == 1);             // untouched
    CHECK(fake.cookCalls[heroTexId] == 2);          // re-asked
    CHECK(fake.cookCalls[matId] == 1);               // untouched
    CHECK(fake.cookCalls[childId] == 1);              // untouched
    CHECK(fake.surfaceCalls[matId] == 1);            // untouched (not dirtied)
    CHECK(fake.surfaceCalls.find(heroTexId) == fake.surfaceCalls.end());   // still never a material

    fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// (g) ResetForProjectSwitch clears selection (and everything else)
// ---------------------------------------------------------------------------

TEST_CASE("AssetPanelModel ResetForProjectSwitch clears selection and cached state", "[editor]")
{
    const fs::path dir = fs::temp_directory_path() / "arcane_asset_panel_model_reset_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    WriteFile(dir, "hero.png", "bytes-a");

    Arcane::AssetRegistry registry;
    REQUIRE(registry.ScanContent(dir, "game") == 1);
    const auto all = registry.All();
    const Arcane::Guid heroTexId = GuidForPath(all, "game://hero.png");
    REQUIRE(heroTexId.IsValid());

    FakeProviders fake;
    AssetPanelModel model;
    model.MarkAllDirty();
    REQUIRE(model.RebuildIfDirty(&registry, fake.Make()));

    model.Select(heroTexId);
    REQUIRE(model.selected == heroTexId);
    REQUIRE(model.selectionStamp == 1);
    REQUIRE_FALSE(model.Rows().empty());

    model.ResetForProjectSwitch();

    CHECK(model.selected == Arcane::Guid{});
    CHECK(model.selectionStamp == 0);
    CHECK(model.Rows().empty());
    CHECK(model.Rail().empty());
    CHECK(model.Health().total == 0);
    CHECK(model.Find(heroTexId) == nullptr);

    fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// (h) Health counts refused/queued from the provider
// ---------------------------------------------------------------------------

TEST_CASE("AssetPanelModel Health tallies cook state from the provider", "[editor]")
{
    const fs::path dir = fs::temp_directory_path() / "arcane_asset_panel_model_health_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    WriteFile(dir, "cooked.png", "bytes-a");
    WriteFile(dir, "queued.png", "bytes-b");
    WriteFile(dir, "refused.png", "bytes-c");
    WriteFile(dir, "unknown.png", "bytes-d");

    Arcane::AssetRegistry registry;
    REQUIRE(registry.ScanContent(dir, "game") == 4);
    const auto all = registry.All();

    const Arcane::Guid cookedId  = GuidForPath(all, "game://cooked.png");
    const Arcane::Guid queuedId  = GuidForPath(all, "game://queued.png");
    const Arcane::Guid refusedId = GuidForPath(all, "game://refused.png");
    const Arcane::Guid unknownId = GuidForPath(all, "game://unknown.png");
    REQUIRE(cookedId.IsValid());
    REQUIRE(queuedId.IsValid());
    REQUIRE(refusedId.IsValid());
    REQUIRE(unknownId.IsValid());

    FakeProviders fake;
    fake.cookByGuid[cookedId]  = CookState::Cooked;
    fake.cookByGuid[queuedId]  = CookState::Queued;
    fake.cookByGuid[refusedId] = CookState::Refused;
    fake.cookByGuid[unknownId] = CookState::Unknown;

    AssetPanelModel model;
    model.MarkAllDirty();
    REQUIRE(model.RebuildIfDirty(&registry, fake.Make()));

    const HealthCounts health = model.Health();
    CHECK(health.total == 4);
    CHECK(health.cooked == 1);
    CHECK(health.queued == 1);
    CHECK(health.refused == 1);

    fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// Fix round 1: a fold TARGET removed from the registry via per-guid
// MarkDirty (not MarkAllDirty) must not orphan the dependent that was
// folded under it -- the dependent's stale (but still IsValid()) foldedUnder
// used to hide it from Rows() entirely (neither a peer nor a child).
// ---------------------------------------------------------------------------

TEST_CASE("AssetPanelModel un-folds a sprite when its fold target is removed via per-guid MarkDirty", "[editor]")
{
    const fs::path dir = fs::temp_directory_path() / "arcane_asset_panel_model_fold_target_removed_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "sprites");

    WriteFile(dir, "hero.png", "not a real png, just bytes");   // sidecar-minted guid
    WriteFile(dir / "sprites", "hero_full.arcsprite",
             R"({"id":"9f000001-0001-4001-8001-000000000001","type":"sprite","name":"HeroFull"})");

    Arcane::AssetRegistry registry;
    REQUIRE(registry.ScanContent(dir, "game") == 2);
    const auto all = registry.All();

    const Arcane::Guid heroTexId  = GuidForPath(all, "game://hero.png");
    const Arcane::Guid heroFullId = *Arcane::Guid::FromString("9f000001-0001-4001-8001-000000000001");
    REQUIRE(heroTexId.IsValid());

    FakeProviders fake;
    fake.refsByGuid[heroFullId] = { { heroTexId, Arcane::AssetRefKind::DerivesFrom } };   // plain 1:1 -> folds

    AssetPanelModel model;
    model.MarkAllDirty();
    AssetPanelProviders providers = fake.Make();
    REQUIRE(model.RebuildIfDirty(&registry, providers));

    // Sanity: the fold is in place before the removal.
    REQUIRE(model.Find(heroFullId));
    REQUIRE(model.Find(heroFullId)->foldedUnder == heroTexId);
    REQUIRE(model.Find(heroTexId));
    REQUIRE(model.Find(heroTexId)->derivedChildren.size() == 1);
    REQUIRE(fake.refsCalls[heroFullId] == 1);

    // Remove the texture from disk and re-scan the SAME registry (a real
    // AssetRegistry rebuild, not a fake) -- hero_full.arcsprite keeps its
    // embedded id, so it survives the rescan; hero.png's sidecar-minted guid
    // does not.
    fs::remove(dir / "hero.png", ec);
    fs::remove(dir / "hero.png.meta", ec);   // orphaned sidecar, if written back
    REQUIRE(registry.ScanContent(dir, "game") == 1);

    // Only the REMOVED guid is dirtied -- the sprite itself is untouched.
    model.MarkDirty(heroTexId);
    REQUIRE(model.RebuildIfDirty(&registry, providers));

    // The texture entry is gone...
    CHECK(model.Find(heroTexId) == nullptr);

    // ...and the sprite must be un-folded and visible again -- not orphaned.
    const AssetPanelEntry* heroFull = model.Find(heroFullId);
    REQUIRE(heroFull);
    CHECK_FALSE(heroFull->foldedUnder.IsValid());

    bool sawAsPeer = false, sawAsChild = false;
    for (const auto& row : model.Rows())
    {
        if (row.guid != heroFullId)
            continue;
        if (row.type == AssetPanelRow::Type::Asset) sawAsPeer = true;
        if (row.type == AssetPanelRow::Type::Child) sawAsChild = true;
    }
    CHECK(sawAsPeer);
    CHECK_FALSE(sawAsChild);

    // The dependent WAS re-asked (a directly-affected cascade, not a no-op);
    // this is the "fine to re-ask a dependent" half of the fix's contract.
    CHECK(fake.refsCalls[heroFullId] == 2);

    fs::remove_all(dir, ec);
}
