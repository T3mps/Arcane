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
    // 4 groups + 5 assets: "fx/" itself holds no file of its own (only the
    // nested "fx/glow/" does) -- review fix round 1, Critical 1's
    // unconditional ancestor bridge means it STILL gets a group row (own
    // count 0), immediately before "fx/glow/" in preorder, so a stale-closed
    // "fx/" is never left with no chevron to reopen it. This fixture
    // predates that fix and is exactly the shape it targets; the row count
    // and indices below reflect the corrected (bridged) behavior.
    REQUIRE(rows.size() == 9);

    CHECK(rows[0].type == AssetPanelRow::Type::Group);
    CHECK(rows[0].groupName == "Content/");
    CHECK(rows[0].groupCount == 1);
    CHECK(rows[1].type == AssetPanelRow::Type::Asset);
    CHECK(rows[1].guid == readmeId);

    CHECK(rows[2].type == AssetPanelRow::Type::Group);
    CHECK(rows[2].groupName == "fx/");
    CHECK(rows[2].groupLabel == "fx/");
    CHECK(rows[2].groupDepth == 0);
    CHECK(rows[2].groupCount == 0);   // bridge row -- no file of its own

    CHECK(rows[3].type == AssetPanelRow::Type::Group);
    CHECK(rows[3].groupName == "fx/glow/");
    CHECK(rows[3].groupLabel == "glow/");
    CHECK(rows[3].groupDepth == 1);
    CHECK(rows[3].groupCount == 1);
    CHECK(rows[4].guid == particleId);

    CHECK(rows[5].type == AssetPanelRow::Type::Group);
    CHECK(rows[5].groupName == "materials/");
    CHECK(rows[5].groupCount == 3);
    CHECK(rows[6].guid == alphaId);   // fileName order: alpha < beta < inst
    CHECK(rows[7].guid == betaId);
    CHECK(rows[8].guid == instId);

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
// (i) CookStateOf: the pure cook-state mapping (Task 5). Refusal always wins;
// only Texture/Sprite have a real cook pipeline (pending is meaningless for
// everything else -- they report Cooked unconditionally); an unrecognized
// kind with no diagnostic defaults to Cooked.
// ---------------------------------------------------------------------------

TEST_CASE("CookStateOf: refused wins over pending", "[editor]")
{
    CHECK(CookStateOf(AssetKind::Texture, /*permanentDiag=*/true, /*pending=*/true) == CookState::Refused);
    CHECK(CookStateOf(AssetKind::Texture, /*permanentDiag=*/true, /*pending=*/false) == CookState::Refused);
    CHECK(CookStateOf(AssetKind::Material, /*permanentDiag=*/true, /*pending=*/false) == CookState::Refused);
}

TEST_CASE("CookStateOf: a pending texture is Queued", "[editor]")
{
    CHECK(CookStateOf(AssetKind::Texture, /*permanentDiag=*/false, /*pending=*/true) == CookState::Queued);
    CHECK(CookStateOf(AssetKind::Sprite, /*permanentDiag=*/false, /*pending=*/true) == CookState::Queued);
    CHECK(CookStateOf(AssetKind::Texture, /*permanentDiag=*/false, /*pending=*/false) == CookState::Cooked);
}

TEST_CASE("CookStateOf: a material never reports Queued -- it has no cook pipeline of its own", "[editor]")
{
    CHECK(CookStateOf(AssetKind::Material, /*permanentDiag=*/false, /*pending=*/true) == CookState::Cooked);
    CHECK(CookStateOf(AssetKind::Material, /*permanentDiag=*/false, /*pending=*/false) == CookState::Cooked);
}

TEST_CASE("CookStateOf: an unrecognized kind with no diagnostic defaults to Cooked", "[editor]")
{
    CHECK(CookStateOf(AssetKind::Other, /*permanentDiag=*/false, /*pending=*/true) == CookState::Cooked);
    CHECK(CookStateOf(AssetKind::Data, /*permanentDiag=*/false, /*pending=*/false) == CookState::Cooked);
    CHECK(CookStateOf(AssetKind::Scene, /*permanentDiag=*/false, /*pending=*/false) == CookState::Cooked);
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

// ---------------------------------------------------------------------------
// Task 14: MaterialSurfaceFilterForComponent -- the owning-component-context
// sibling of AssetKindFilterForFieldName (both live in Panels/
// AssetPanelModel.hpp as of Task 15; this unit still never touches ImGui or
// the engine facade, matching this file's own header comment).
// ---------------------------------------------------------------------------

TEST_CASE("MaterialSurfaceFilterForComponent maps the owning component to its required MaterialSurface",
          "[editor]")
{
    CHECK(MaterialSurfaceFilterForComponent("SpriteRenderer")
          == static_cast<int>(Arcane::MaterialSurface::Sprite));
    CHECK(MaterialSurfaceFilterForComponent("MeshRenderer")
          == static_cast<int>(Arcane::MaterialSurface::Mesh));
    // A namespace-qualified type name (TypeMeta::typeName's actual shape, see
    // InspectorView.cpp's visitor) still resolves -- the call site is never
    // expected to strip the namespace first.
    CHECK(MaterialSurfaceFilterForComponent("Arcane::SpriteRenderer")
          == static_cast<int>(Arcane::MaterialSurface::Sprite));
    // An unrecognised (or unrelated) component leaves the field unfiltered --
    // this is the "do not break other material fields" half of the contract.
    CHECK(MaterialSurfaceFilterForComponent("Transform") == -1);
    CHECK(MaterialSurfaceFilterForComponent("") == -1);
}

// ---------------------------------------------------------------------------
// 2026-09-07 follow-up: in-table nested folder groups (spec s6/s11.2, design
// report .superpowers/sdd/2026-09-06-asset-manager-plan1/followup-treeview-
// design-report.md). Every case below scans a REAL nested directory tree via
// AssetRegistry::ScanContent -- no synthetic folder strings -- the same
// discipline this whole file already uses.
// ---------------------------------------------------------------------------

namespace
{
    // Shared fixture for (a)/(b)/(c): "textures/" holds one own asset
    // (crate_albedo.png), nested "textures/patterns/" (depth 1) holds two
    // (tiles_stone.png, noise_blue.png) -- the same shape as the re-blessed
    // redline (OptionBC-Browse-FINAL.png)'s own textures/patterns/ example.
    struct NestedFixture
    {
        fs::path dir;
        Arcane::AssetRegistry registry;
        Arcane::Guid crateId, tilesId, noiseId;
    };

    NestedFixture MakeNestedFixture(const char* dirName)
    {
        NestedFixture f;
        f.dir = fs::temp_directory_path() / dirName;
        std::error_code ec;
        fs::remove_all(f.dir, ec);
        fs::create_directories(f.dir / "textures" / "patterns");

        WriteFile(f.dir / "textures", "crate_albedo.png", "bytes-crate");
        WriteFile(f.dir / "textures" / "patterns", "tiles_stone.png", "bytes-tiles");
        WriteFile(f.dir / "textures" / "patterns", "noise_blue.png", "bytes-noise");

        REQUIRE(f.registry.ScanContent(f.dir, "game") == 3);
        const auto all = f.registry.All();
        f.crateId = GuidForPath(all, "game://textures/crate_albedo.png");
        f.tilesId = GuidForPath(all, "game://textures/patterns/tiles_stone.png");
        f.noiseId = GuidForPath(all, "game://textures/patterns/noise_blue.png");
        REQUIRE(f.crateId.IsValid());
        REQUIRE(f.tilesId.IsValid());
        REQUIRE(f.noiseId.IsValid());
        return f;
    }

    const AssetPanelRow* FindGroupRow(const std::vector<AssetPanelRow>& rows, const std::string& fullPath)
    {
        for (const auto& row : rows)
            if (row.type == AssetPanelRow::Type::Group && row.groupName == fullPath)
                return &row;
        return nullptr;
    }

    bool HasAssetRow(const std::vector<AssetPanelRow>& rows, const Arcane::Guid& guid,
                     AssetPanelRow::Type type = AssetPanelRow::Type::Asset)
    {
        for (const auto& row : rows)
            if (row.type == type && row.guid == guid)
                return true;
        return false;
    }
}

// (a) nested dirs produce depth-tagged groups with leaf labels + full-path
// open-state keys -- the core geometry data the panel's 20px/level indent
// and PushID/SetGroupOpen keying both read directly off these fields.
TEST_CASE("AssetPanelModel nested folder groups carry depth, leaf-segment labels and full-path keys", "[editor]")
{
    NestedFixture f = MakeNestedFixture("arcane_asset_panel_model_nested_depth_test");

    FakeProviders fake;
    AssetPanelModel model;
    model.MarkAllDirty();
    REQUIRE(model.RebuildIfDirty(&f.registry, fake.Make()));

    const auto& rows = model.Rows();

    const AssetPanelRow* textures = FindGroupRow(rows, "textures/");
    REQUIRE(textures);
    CHECK(textures->groupLabel == "textures/");     // top-level: leaf == full name, unchanged
    CHECK(textures->groupDepth == 0);
    CHECK(textures->groupCount == 1);                // crate_albedo.png only -- own rows, not rolled up

    const AssetPanelRow* patterns = FindGroupRow(rows, "textures/patterns/");
    REQUIRE(patterns);
    CHECK(patterns->groupLabel == "patterns/");      // LEAF segment only, not "textures/patterns/"
    CHECK(patterns->groupDepth == 1);
    CHECK(patterns->groupCount == 2);                // tiles_stone.png + noise_blue.png

    // Root "Content/" and other top-level groups are untouched by this pass:
    // depth 0, label == groupName, exactly as before nesting existed.
    // (No root-level file in this fixture -- covered structurally by every
    // pre-existing flat test in this file staying green, case (e).)

    // Preorder: "textures/" group precedes its own asset row, which precedes
    // the nested "patterns/" group, which precedes ITS asset rows -- the
    // std::map/std::set lexicographic-with-trailing-slash property the impl
    // relies on.
    auto indexOf = [&](auto pred) -> int
    {
        for (int i = 0; i < static_cast<int>(rows.size()); ++i)
            if (pred(rows[i])) return i;
        return -1;
    };
    const int iTextures = indexOf([](const AssetPanelRow& r) { return r.type == AssetPanelRow::Type::Group && r.groupName == "textures/"; });
    const int iCrate     = indexOf([&](const AssetPanelRow& r) { return r.type == AssetPanelRow::Type::Asset && r.guid == f.crateId; });
    const int iPatterns  = indexOf([](const AssetPanelRow& r) { return r.type == AssetPanelRow::Type::Group && r.groupName == "textures/patterns/"; });
    const int iTiles     = indexOf([&](const AssetPanelRow& r) { return r.type == AssetPanelRow::Type::Asset && r.guid == f.tilesId; });
    REQUIRE((iTextures >= 0 && iCrate >= 0 && iPatterns >= 0 && iTiles >= 0));
    CHECK(iTextures < iCrate);
    CHECK(iCrate < iPatterns);
    CHECK(iPatterns < iTiles);

    std::error_code cleanupEc;
    fs::remove_all(f.dir, cleanupEc);
}

// (b) cascading collapse: closing the parent hides its whole subtree
// (descendant group row AND its asset rows) from Rows(); each descendant
// group keeps its OWN open flag independently, so reopening the parent
// restores whatever sub-state the descendant was left in.
TEST_CASE("AssetPanelModel cascading collapse hides the whole subtree; reopen restores descendant open-state", "[editor]")
{
    NestedFixture f = MakeNestedFixture("arcane_asset_panel_model_nested_cascade_test");

    FakeProviders fake;
    AssetPanelModel model;
    model.MarkAllDirty();
    AssetPanelProviders providers = fake.Make();
    REQUIRE(model.RebuildIfDirty(&f.registry, providers));

    // Baseline: everything default-open.
    REQUIRE(FindGroupRow(model.Rows(), "textures/"));
    REQUIRE(FindGroupRow(model.Rows(), "textures/patterns/"));
    REQUIRE(HasAssetRow(model.Rows(), f.crateId));
    REQUIRE(HasAssetRow(model.Rows(), f.tilesId));
    REQUIRE(HasAssetRow(model.Rows(), f.noiseId));

    // Close the NESTED group itself first (its own toggle) -- its header
    // stays visible, its own asset rows do not.
    model.SetGroupOpen("textures/patterns/", false);
    REQUIRE(model.RebuildIfDirty(&f.registry, providers));
    REQUIRE(FindGroupRow(model.Rows(), "textures/patterns/"));   // header still shown
    CHECK_FALSE(HasAssetRow(model.Rows(), f.tilesId));
    CHECK_FALSE(HasAssetRow(model.Rows(), f.noiseId));
    CHECK(HasAssetRow(model.Rows(), f.crateId));                  // untouched sibling

    // Now close the PARENT ("textures/") -- the whole subtree (patterns/'s
    // group row included) must vanish from Rows(), on top of textures/'s
    // own crate_albedo.png row.
    model.SetGroupOpen("textures/", false);
    REQUIRE(model.RebuildIfDirty(&f.registry, providers));
    REQUIRE(FindGroupRow(model.Rows(), "textures/"));             // textures/'s own header still shown
    CHECK_FALSE(HasAssetRow(model.Rows(), f.crateId));
    CHECK_FALSE(FindGroupRow(model.Rows(), "textures/patterns/"));   // cascaded away entirely
    CHECK_FALSE(HasAssetRow(model.Rows(), f.tilesId));
    CHECK_FALSE(HasAssetRow(model.Rows(), f.noiseId));

    // Reopen the parent: patterns/'s group row comes back, but its OWN
    // closed flag (set above, never touched by the parent's toggle) is
    // restored exactly as left -- its asset rows stay hidden.
    model.SetGroupOpen("textures/", true);
    REQUIRE(model.RebuildIfDirty(&f.registry, providers));
    REQUIRE(FindGroupRow(model.Rows(), "textures/"));
    CHECK(HasAssetRow(model.Rows(), f.crateId));
    REQUIRE(FindGroupRow(model.Rows(), "textures/patterns/"));    // subtree back
    CHECK_FALSE(HasAssetRow(model.Rows(), f.tilesId));            // ...but still collapsed, as left
    CHECK_FALSE(HasAssetRow(model.Rows(), f.noiseId));

    std::error_code cleanupEc;
    fs::remove_all(f.dir, cleanupEc);
}

// (c) search overrides collapse, consistent with the existing fold-child
// rule (§8) generalized one level: a matching row under a collapsed
// ancestor still shows, and every group row on the path down to it shows
// too (tree context), even ones with zero OWN matching entries. A non-
// matching sibling stays hidden; groups with truly nothing visible under
// them are still dropped (never asserted here because none exist in this
// fixture -- every group on the path to a match).
TEST_CASE("AssetPanelModel search overrides collapse for nested groups, showing ancestor group rows for context", "[editor]")
{
    NestedFixture f = MakeNestedFixture("arcane_asset_panel_model_nested_search_test");

    FakeProviders fake;
    AssetPanelModel model;
    model.MarkAllDirty();
    AssetPanelProviders providers = fake.Make();
    REQUIRE(model.RebuildIfDirty(&f.registry, providers));

    // Close "textures/" -- the ANCESTOR of the match below.
    model.SetGroupOpen("textures/", false);
    model.SetSearch("tiles");     // matches tiles_stone.png only
    REQUIRE(model.RebuildIfDirty(&f.registry, providers));

    const auto& rows = model.Rows();

    // The match itself shows despite its collapsed ancestor.
    CHECK(HasAssetRow(rows, f.tilesId));

    // Both group rows on the path to it show, for tree context -- including
    // "textures/", whose own direct entry (crate_albedo.png) does NOT match
    // "tiles" and so contributes zero to its own count.
    const AssetPanelRow* texturesRow = FindGroupRow(rows, "textures/");
    REQUIRE(texturesRow);
    CHECK(texturesRow->groupCount == 0);          // own entries only, none match -- see impl report
    REQUIRE(FindGroupRow(rows, "textures/patterns/"));

    // Non-matching rows, in and out of the collapsed subtree, stay hidden.
    CHECK_FALSE(HasAssetRow(rows, f.noiseId));    // sibling inside patterns/, doesn't match
    CHECK_FALSE(HasAssetRow(rows, f.crateId));    // textures/'s own entry, doesn't match

    std::error_code cleanupEc;
    fs::remove_all(f.dir, cleanupEc);
}

// (d) compound indent DATA: a fold child (derived sprite) inside a depth-1
// group carries the same groupDepth as its parent asset row -- the panel
// stacks its own existing +20px fold indent on top of that (base + 20 + 20).
// No fixture in the design pass exercised this combination (design report's
// own "concerns" section); this is that fixture.
TEST_CASE("AssetPanelModel fold child inside a nested group carries its group's depth on both rows", "[editor]")
{
    const fs::path dir = fs::temp_directory_path() / "arcane_asset_panel_model_nested_fold_compound_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "textures" / "patterns");
    fs::create_directories(dir / "sprites");

    WriteFile(dir / "textures" / "patterns", "swatch.png", "bytes-swatch");
    WriteFile(dir / "sprites", "swatch_full.arcsprite",
             R"({"id":"a1000001-0001-4001-8001-000000000001","type":"sprite","name":"SwatchFull"})");

    Arcane::AssetRegistry registry;
    REQUIRE(registry.ScanContent(dir, "game") == 2);
    const auto all = registry.All();

    const Arcane::Guid swatchTexId  = GuidForPath(all, "game://textures/patterns/swatch.png");
    const Arcane::Guid swatchFullId = *Arcane::Guid::FromString("a1000001-0001-4001-8001-000000000001");
    REQUIRE(swatchTexId.IsValid());

    FakeProviders fake;
    fake.refsByGuid[swatchFullId] = { { swatchTexId, Arcane::AssetRefKind::DerivesFrom } };   // 1:1 -> folds

    AssetPanelModel model;
    model.MarkAllDirty();
    AssetPanelProviders providers = fake.Make();
    REQUIRE(model.RebuildIfDirty(&registry, providers));

    REQUIRE(model.Find(swatchTexId));
    REQUIRE(model.Find(swatchTexId)->derivedChildren.size() == 1);

    model.SetChildrenOpen(swatchTexId, true);   // fold children default collapsed
    REQUIRE(model.RebuildIfDirty(&registry, providers));

    const AssetPanelRow* patterns = FindGroupRow(model.Rows(), "textures/patterns/");
    REQUIRE(patterns);
    CHECK(patterns->groupDepth == 1);

    int swatchDepth = -1, fullDepth = -1;
    for (const auto& row : model.Rows())
    {
        if (row.type == AssetPanelRow::Type::Asset && row.guid == swatchTexId) swatchDepth = row.groupDepth;
        if (row.type == AssetPanelRow::Type::Child && row.guid == swatchFullId) fullDepth = row.groupDepth;
    }
    CHECK(swatchDepth == 1);     // base(=1) + 0 -- the panel adds no fold indent for a plain asset row
    CHECK(fullDepth == 1);       // base(=1) too -- the panel stacks ITS OWN +20 fold indent on top of
                                 // this same depth, giving the compound 20*1 + 20 = 40px total

    fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// Review fix round 1 (2026-09-07): 1 Critical + 4 Important findings against
// the nested-groups pass above, fixed in one round. Every case below is a
// regression pin for one specific finding -- see the pass's own doc comments
// in AssetPanelModel.cpp/AssetsPanel.cpp and
// docs/specs/2026-09-06-asset-manager-redesign-design.md s6/s17 for the
// rulings these enforce.
// ---------------------------------------------------------------------------

// Important 3 (i): search overrides a group's OWN closed flag (not just an
// ancestor's) -- the `searchActive && !open && ownCount>0` branch, distinct
// from case (c) above (which closed the ANCESTOR "textures/", not "patterns/"
// itself). This branch already existed before this review round; it simply
// had no dedicated test until now.
TEST_CASE("AssetPanelModel search overrides a nested group's OWN closed flag, not just an ancestor's", "[editor]")
{
    NestedFixture f = MakeNestedFixture("arcane_asset_panel_model_selfcollapse_search_test");

    FakeProviders fake;
    AssetPanelModel model;
    model.MarkAllDirty();
    AssetPanelProviders providers = fake.Make();
    REQUIRE(model.RebuildIfDirty(&f.registry, providers));

    // Close "patterns/" ITSELF (its ancestor "textures/" stays open) --
    // baseline: without search, its own rows are hidden, header stays.
    model.SetGroupOpen("textures/patterns/", false);
    REQUIRE(model.RebuildIfDirty(&f.registry, providers));
    REQUIRE(FindGroupRow(model.Rows(), "textures/patterns/"));
    CHECK_FALSE(HasAssetRow(model.Rows(), f.tilesId));

    // Search for "tiles" -- patterns/'s OWN closed flag must not hide its own
    // matching entry.
    model.SetSearch("tiles");
    REQUIRE(model.RebuildIfDirty(&f.registry, providers));
    CHECK(HasAssetRow(model.Rows(), f.tilesId));
    CHECK_FALSE(HasAssetRow(model.Rows(), f.noiseId));   // doesn't match "tiles"

    std::error_code cleanupEc;
    fs::remove_all(f.dir, cleanupEc);
}

// Important 3 (ii): the FLAT-project twin of the case above -- proves the
// group-collapse-plus-search override is not a nesting-only effect (spec s17,
// Important 5's own entry). No folder nesting anywhere in this fixture: a
// single top-level "Content/" group, no depth>0 group exists at all.
TEST_CASE("AssetPanelModel search overrides a collapsed TOP-LEVEL group in a flat project (no nesting anywhere)", "[editor]")
{
    const fs::path dir = fs::temp_directory_path() / "arcane_asset_panel_model_flat_collapse_search_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    WriteFile(dir, "glow_effect.png", "bytes-glow");
    WriteFile(dir, "other.png", "bytes-other");

    Arcane::AssetRegistry registry;
    REQUIRE(registry.ScanContent(dir, "game") == 2);
    const auto all = registry.All();
    const Arcane::Guid glowId  = GuidForPath(all, "game://glow_effect.png");
    const Arcane::Guid otherId = GuidForPath(all, "game://other.png");
    REQUIRE(glowId.IsValid());
    REQUIRE(otherId.IsValid());

    FakeProviders fake;
    AssetPanelModel model;
    model.MarkAllDirty();
    AssetPanelProviders providers = fake.Make();
    REQUIRE(model.RebuildIfDirty(&registry, providers));

    // Baseline (no search): closing the only group hides its rows, exactly
    // as pre-nesting Plan 1 behaved.
    model.SetGroupOpen("Content/", false);
    REQUIRE(model.RebuildIfDirty(&registry, providers));
    REQUIRE(FindGroupRow(model.Rows(), "Content/"));
    CHECK_FALSE(HasAssetRow(model.Rows(), glowId));
    CHECK_FALSE(HasAssetRow(model.Rows(), otherId));

    // Search while still closed: the matching row must show -- this is the
    // behavior spec s17 (Important 5) records as changed from pre-nesting
    // Plan 1, and pins as depth-independent (a depth-0-only project is
    // affected exactly like a nested one).
    model.SetSearch("glow");
    REQUIRE(model.RebuildIfDirty(&registry, providers));
    CHECK(HasAssetRow(model.Rows(), glowId));
    CHECK_FALSE(HasAssetRow(model.Rows(), otherId));   // doesn't match "glow"

    fs::remove_all(dir, ec);
}

// Important 3 (iii) + Important 4 (controller ruling): search now overrides
// FOLD collapse too, not just group collapse -- a matching derived sprite
// under a texture whose fold is at its (default) COLLAPSED state must still
// show. Both the texture and its derived sprite must match the search term
// for the child to be reachable at all (case (c) in the fold-child tests
// above: "a non-matching parent hides even a matching child" -- unchanged by
// this round).
TEST_CASE("AssetPanelModel search overrides a texture's OWN fold collapse (derived child), uniformly with group collapse", "[editor]")
{
    const fs::path dir = fs::temp_directory_path() / "arcane_asset_panel_model_fold_search_override_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "sprites");

    WriteFile(dir, "hero.png", "not a real png, just bytes");   // sidecar-minted guid
    WriteFile(dir / "sprites", "hero_full.arcsprite",
             R"({"id":"a2000001-0001-4001-8001-000000000001","type":"sprite","name":"HeroFull"})");

    Arcane::AssetRegistry registry;
    REQUIRE(registry.ScanContent(dir, "game") == 2);
    const auto all = registry.All();
    const Arcane::Guid heroTexId  = GuidForPath(all, "game://hero.png");
    const Arcane::Guid heroFullId = *Arcane::Guid::FromString("a2000001-0001-4001-8001-000000000001");
    REQUIRE(heroTexId.IsValid());

    FakeProviders fake;
    fake.refsByGuid[heroFullId] = { { heroTexId, Arcane::AssetRefKind::DerivesFrom } };   // 1:1 -> folds

    AssetPanelModel model;
    model.MarkAllDirty();
    AssetPanelProviders providers = fake.Make();
    REQUIRE(model.RebuildIfDirty(&registry, providers));
    REQUIRE(model.Find(heroTexId)->derivedChildren.size() == 1);

    // Baseline, no search: fold defaults COLLAPSED (never called
    // SetChildrenOpen) -- the derived child is not a row at all.
    bool sawFullBaseline = false;
    for (const auto& row : model.Rows())
        if (row.guid == heroFullId) sawFullBaseline = true;
    CHECK_FALSE(sawFullBaseline);

    // Search for "hero" -- matches BOTH hero.png (the parent, so it's a
    // visible row at all) and hero_full.arcsprite (the folded child). The
    // fold's own childrenOpen flag was never touched -- still default
    // COLLAPSED -- yet the matching child must now show.
    model.SetSearch("hero");
    REQUIRE(model.RebuildIfDirty(&registry, providers));

    bool sawFullAsChild = false;
    for (const auto& row : model.Rows())
        if (row.guid == heroFullId && row.type == AssetPanelRow::Type::Child)
            sawFullAsChild = true;
    CHECK(sawFullAsChild);

    fs::remove_all(dir, ec);
}

// Critical 1's own regression pin: a kind filter (not search) leaving an
// ANCESTOR with zero own visible entries, while a STALE closed flag sits on
// that ancestor from before the filter was applied, must not strand the
// descendant permanently unreachable. Before this fix, "fx/" (all-texture)
// vanished from Rows() entirely once the rail filtered to Materials (its
// only entry no longer matched), taking "fx/glow/" (the one material) down
// with it -- with no chevron anywhere to bring it back.
TEST_CASE("AssetPanelModel bridges a kind-filtered-empty ancestor unconditionally, so a stale-closed ancestor stays reachable", "[editor]")
{
    const fs::path dir = fs::temp_directory_path() / "arcane_asset_panel_model_kindfilter_bridge_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "fx" / "glow");

    WriteFile(dir / "fx", "spark.png", "bytes-spark");
    WriteFile(dir / "fx" / "glow", "glow.arcmat",
             R"({"id":"a3000001-0001-4001-8001-000000000001","type":"material","kind":"fullscreen"})");

    Arcane::AssetRegistry registry;
    REQUIRE(registry.ScanContent(dir, "game") == 2);
    const auto all = registry.All();
    const Arcane::Guid sparkId = GuidForPath(all, "game://fx/spark.png");
    const Arcane::Guid glowId  = *Arcane::Guid::FromString("a3000001-0001-4001-8001-000000000001");
    REQUIRE(sparkId.IsValid());

    FakeProviders fake;
    AssetPanelModel model;
    model.MarkAllDirty();
    AssetPanelProviders providers = fake.Make();
    REQUIRE(model.RebuildIfDirty(&registry, providers));

    // Simulate the real sequence: the user closes "fx/" while it still has
    // its own visible entry (under "All", no filter yet) -- a perfectly
    // ordinary interaction, nothing stale about it YET.
    model.SetGroupOpen("fx/", false);
    REQUIRE(model.RebuildIfDirty(&registry, providers));
    REQUIRE(FindGroupRow(model.Rows(), "fx/"));         // header still shows (own-collapse rule)
    CHECK_FALSE(HasAssetRow(model.Rows(), sparkId));    // ...but its content is hidden, as closed

    // Now the rail filters to Materials. "fx/"'s own entry (a texture) no
    // longer matches -- its own byFolder bucket is empty -- while "fx/glow/"
    // (a material) still does. "fx/"'s closed flag is now STALE relative to
    // what's visible under this filter.
    model.SetKindFilter(static_cast<int>(AssetKind::Material));
    REQUIRE(model.RebuildIfDirty(&registry, providers));

    // The bridge: "fx/" still gets a row (Critical 1) -- zero own count,
    // rider 8 says its UI paints no "0", but the DATA is exactly 0 here.
    const AssetPanelRow* fxRow = FindGroupRow(model.Rows(), "fx/");
    REQUIRE(fxRow);
    CHECK(fxRow->groupCount == 0);

    // "fx/glow/" is correctly HIDDEN (its ancestor is genuinely closed, and
    // no search is active to override that) -- but, critically, REACHABLE:
    // there is a chevron on "fx/" to click.
    CHECK_FALSE(FindGroupRow(model.Rows(), "fx/glow/"));
    CHECK_FALSE(HasAssetRow(model.Rows(), glowId));

    // Reopen "fx/" (the user clicking the chevron the bridge row provides) --
    // "fx/glow/" and its material become visible. Before Critical 1's fix
    // there was no "fx/" row left to click at all under this filter.
    model.SetGroupOpen("fx/", true);
    REQUIRE(model.RebuildIfDirty(&registry, providers));
    REQUIRE(FindGroupRow(model.Rows(), "fx/glow/"));
    CHECK(HasAssetRow(model.Rows(), glowId));

    fs::remove_all(dir, ec);
}

// Rider (reviewer minor 6): cascade across >1 level -- closing a GRANDPARENT
// must hide both its child AND grandchild groups, not just the immediate
// child (the only depth tested before this round was a single parent->child
// hop). Every level here has its own file, so no bridging is involved --
// this isolates the AncestorsOpen multi-hop walk specifically.
TEST_CASE("AssetPanelModel cascading collapse reaches through a grandparent (depth >= 2)", "[editor]")
{
    const fs::path dir = fs::temp_directory_path() / "arcane_asset_panel_model_depth2_cascade_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "a" / "b" / "c");

    WriteFile(dir / "a", "file1.png", "bytes-1");
    WriteFile(dir / "a" / "b", "file2.png", "bytes-2");
    WriteFile(dir / "a" / "b" / "c", "file3.png", "bytes-3");

    Arcane::AssetRegistry registry;
    REQUIRE(registry.ScanContent(dir, "game") == 3);
    const auto all = registry.All();
    const Arcane::Guid file1Id = GuidForPath(all, "game://a/file1.png");
    const Arcane::Guid file2Id = GuidForPath(all, "game://a/b/file2.png");
    const Arcane::Guid file3Id = GuidForPath(all, "game://a/b/c/file3.png");
    REQUIRE(file1Id.IsValid());
    REQUIRE(file2Id.IsValid());
    REQUIRE(file3Id.IsValid());

    FakeProviders fake;
    AssetPanelModel model;
    model.MarkAllDirty();
    AssetPanelProviders providers = fake.Make();
    REQUIRE(model.RebuildIfDirty(&registry, providers));

    REQUIRE(FindGroupRow(model.Rows(), "a/"));
    REQUIRE(FindGroupRow(model.Rows(), "a/b/"));
    REQUIRE(FindGroupRow(model.Rows(), "a/b/c/"));
    CHECK(FindGroupRow(model.Rows(), "a/b/")->groupDepth == 1);
    CHECK(FindGroupRow(model.Rows(), "a/b/c/")->groupDepth == 2);

    // Close the GRANDPARENT "a/" only -- "a/b/" and "a/b/c/" are never
    // touched, so their own open flags stay at the default (open).
    model.SetGroupOpen("a/", false);
    REQUIRE(model.RebuildIfDirty(&registry, providers));

    REQUIRE(FindGroupRow(model.Rows(), "a/"));            // "a/"'s own header stays
    CHECK_FALSE(HasAssetRow(model.Rows(), file1Id));
    CHECK_FALSE(FindGroupRow(model.Rows(), "a/b/"));       // cascaded away, 1 hop down
    CHECK_FALSE(HasAssetRow(model.Rows(), file2Id));
    CHECK_FALSE(FindGroupRow(model.Rows(), "a/b/c/"));     // cascaded away, 2 hops down
    CHECK_FALSE(HasAssetRow(model.Rows(), file3Id));

    // Reopen "a/" -- everything comes back, "a/b/" and "a/b/c/" restored to
    // their own (never-touched, still-open) state.
    model.SetGroupOpen("a/", true);
    REQUIRE(model.RebuildIfDirty(&registry, providers));
    CHECK(HasAssetRow(model.Rows(), file1Id));
    REQUIRE(FindGroupRow(model.Rows(), "a/b/"));
    CHECK(HasAssetRow(model.Rows(), file2Id));
    REQUIRE(FindGroupRow(model.Rows(), "a/b/c/"));
    CHECK(HasAssetRow(model.Rows(), file3Id));

    fs::remove_all(dir, ec);
}

// Rider (reviewer minor 7): sibling leaf-name collision -- two DIFFERENT
// top-level directories that both nest a "patterns/" subfolder must produce
// two DISTINCT group rows (distinct groupName full-path keys, hence
// independent open-state), even though they share the identical groupLabel.
TEST_CASE("AssetPanelModel keeps sibling groups with the same leaf label distinct (a/patterns/ vs b/patterns/)", "[editor]")
{
    const fs::path dir = fs::temp_directory_path() / "arcane_asset_panel_model_sibling_leaf_collision_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "a" / "patterns");
    fs::create_directories(dir / "b" / "patterns");

    WriteFile(dir / "a" / "patterns", "a_pattern.png", "bytes-a-pattern");
    WriteFile(dir / "b" / "patterns", "b_pattern.png", "bytes-b-pattern");

    Arcane::AssetRegistry registry;
    REQUIRE(registry.ScanContent(dir, "game") == 2);
    const auto all = registry.All();
    const Arcane::Guid aPatternId = GuidForPath(all, "game://a/patterns/a_pattern.png");
    const Arcane::Guid bPatternId = GuidForPath(all, "game://b/patterns/b_pattern.png");
    REQUIRE(aPatternId.IsValid());
    REQUIRE(bPatternId.IsValid());

    FakeProviders fake;
    AssetPanelModel model;
    model.MarkAllDirty();
    AssetPanelProviders providers = fake.Make();
    REQUIRE(model.RebuildIfDirty(&registry, providers));

    const AssetPanelRow* aPatterns = FindGroupRow(model.Rows(), "a/patterns/");
    const AssetPanelRow* bPatterns = FindGroupRow(model.Rows(), "b/patterns/");
    REQUIRE(aPatterns);
    REQUIRE(bPatterns);
    CHECK(aPatterns->groupLabel == "patterns/");
    CHECK(bPatterns->groupLabel == "patterns/");           // same LABEL...
    CHECK(aPatterns->groupName != bPatterns->groupName);   // ...but distinct KEYS
    CHECK(aPatterns->groupDepth == 1);
    CHECK(bPatterns->groupDepth == 1);

    // Closing ONE does not affect the other -- proves the label collision
    // never aliases their open-state (both keyed by the FULL path).
    model.SetGroupOpen("a/patterns/", false);
    REQUIRE(model.RebuildIfDirty(&registry, providers));
    CHECK_FALSE(HasAssetRow(model.Rows(), aPatternId));
    CHECK(HasAssetRow(model.Rows(), bPatternId));          // untouched sibling

    fs::remove_all(dir, ec);
}
