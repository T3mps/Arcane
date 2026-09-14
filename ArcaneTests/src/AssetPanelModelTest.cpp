// Asset-manager arc (Plan 1 Task 4): AssetPanelModel -- the cached, foldable
// model behind the Assets panel's Browse lens. Modeled on
// AssetBrowserTest.cpp:57's pattern (a REAL temp dir + real files +
// registry.ScanContent), with the AssetPanelProviders callables faked
// in-test -- this unit never touches the engine facade, so there is no
// Arcane::Assets/Arcane::Project in sight here either.

#include <catch2/catch_test_macros.hpp>

#include "Panels/AssetPanelModel.hpp"

#include <Arcane/Base/DiagEnvelope.hpp>   // Diag::Envelope/WriteFile -- valid .arcdiag fixtures
#include <Arcane/Project/AssetRegistry.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

    // A valid .arcdiag envelope (mount-rooted pass, 2026-09-07 third revision):
    // .arcdiag is native-but-not-ResolveNativeId -- its guid lives under the
    // envelope's own "guid" key, never a plain "id" field (AssetRegistry.cpp's
    // own CRITICAL contract comment; AssetRegistryTest.cpp's "AssetRegistry
    // classifies .arcdiag by its envelope guid" case is the precedent this
    // mirrors). A hand-rolled JSON stub the way WriteFile's other callers use
    // for .arcmat/.arcsprite would be REJECTED by Diag::ReadFile and silently
    // skipped by the scan -- Diag::WriteFile is the only correct way to seed
    // one of these fixtures.
    Arcane::Guid WriteDiagFile(const fs::path& dir, const char* name, std::string_view kind = "hang")
    {
        std::error_code ec;
        fs::create_directories(dir, ec);
        Arcane::Diag::Envelope env;
        env.guid = Arcane::Guid::Generate();
        env.kind = std::string(kind);
        REQUIRE(Arcane::Diag::WriteFile(env, dir / name));
        return env.guid;
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
    CHECK(rows[2].groupDepth == 1);   // root-anchored (2026-09-07, 2nd revision): "fx/" is Content/'s child now
    CHECK(rows[2].groupCount == 0);   // bridge row -- no file of its own

    CHECK(rows[3].type == AssetPanelRow::Type::Group);
    CHECK(rows[3].groupName == "fx/glow/");
    CHECK(rows[3].groupLabel == "glow/");
    CHECK(rows[3].groupDepth == 2);   // root-anchored: 1 + nesting below Content/
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
    CHECK(textures->groupDepth == 1);                // root-anchored (2026-09-07, 2nd revision): Content/'s child
    CHECK(textures->groupCount == 1);                // crate_albedo.png only -- own rows, not rolled up

    const AssetPanelRow* patterns = FindGroupRow(rows, "textures/patterns/");
    REQUIRE(patterns);
    CHECK(patterns->groupLabel == "patterns/");      // LEAF segment only, not "textures/patterns/"
    CHECK(patterns->groupDepth == 2);                // root-anchored: 1 + nesting below Content/
    CHECK(patterns->groupCount == 2);                // tiles_stone.png + noise_blue.png

    // Root-anchored (2026-09-07, 2nd revision): this fixture has NO root-level
    // file, so "Content/" is bridged in as "textures/"'s ancestor (own count
    // suppressed, depth 0 -- the one group that never shifts). Labels are
    // untouched by root-anchoring either way (leaf == groupName still holds
    // for every top-level dir); only DEPTH shifted, covered just above.

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
    // Root-anchored (2026-09-07, 2nd revision): Content/(0) -> textures/(1,
    // bridged, no own file) -> patterns/(2). Was depth 1 before root-anchoring.
    CHECK(patterns->groupDepth == 2);

    int swatchDepth = -1, fullDepth = -1;
    for (const auto& row : model.Rows())
    {
        if (row.type == AssetPanelRow::Type::Asset && row.guid == swatchTexId) swatchDepth = row.groupDepth;
        if (row.type == AssetPanelRow::Type::Child && row.guid == swatchFullId) fullDepth = row.groupDepth;
    }
    CHECK(swatchDepth == 2);     // base(=2) + 0 -- the panel adds no fold indent for a plain asset row
    CHECK(fullDepth == 2);       // base(=2) too -- the panel stacks ITS OWN +20 fold indent on top of
                                 // this same depth, giving the compound 20*2 + 20 = 60px total

    fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// Review fix round 1 (2026-09-07): 1 Critical + 4 Important findings against
// the nested-groups pass above, fixed in one round. Every case below is a
// regression pin for one specific finding -- see the pass's own doc comments
// in AssetPanelModel.cpp/AssetBrowserPanel.cpp and
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
    // Root-anchored (2026-09-07, 2nd revision): Content/(0, bridged, no root
    // file here) -> a/(1) -> a/b/(2) -> a/b/c/(3). Depths were 0/1/2 before
    // root-anchoring; the GRANDPARENT-cascade shape this test exists to pin
    // (closing "a/" hides two levels below it) is unaffected by the shift.
    CHECK(FindGroupRow(model.Rows(), "a/b/")->groupDepth == 2);
    CHECK(FindGroupRow(model.Rows(), "a/b/c/")->groupDepth == 3);

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
    // Root-anchored (2026-09-07, 2nd revision): Content/(0)->a or b/(1)->patterns/(2).
    CHECK(aPatterns->groupDepth == 2);
    CHECK(bPatterns->groupDepth == 2);

    // Closing ONE does not affect the other -- proves the label collision
    // never aliases their open-state (both keyed by the FULL path).
    model.SetGroupOpen("a/patterns/", false);
    REQUIRE(model.RebuildIfDirty(&registry, providers));
    CHECK_FALSE(HasAssetRow(model.Rows(), aPatternId));
    CHECK(HasAssetRow(model.Rows(), bPatternId));          // untouched sibling

    fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// Root-anchored folder tree (2026-09-07, 2nd revision): "Content/" becomes the
// table's real depth-0 root -- every content directory, top-level included,
// nests as its indented child (dir depth = 1 + nesting below Content/), and
// Content/ is every directory's ancestor for BOTH cascading collapse and the
// unconditional bridge. See docs/specs/2026-09-06-asset-manager-redesign-
// design.md s6/s17 (2nd revision entry) and followup-treeview-impl-report.md's
// root-anchoring addendum for the ruling this enforces.
// ---------------------------------------------------------------------------

// (a) A top-level directory is now Content/'s CHILD, not its sibling:
// collapsing Content/ empties the WHOLE table (every directory cascades
// through it), and reopening Content/ restores each directory's own
// independently-kept open state -- the same cascade/restore contract §6
// already specified for any parent/child pair, now exercised at the root.
TEST_CASE("AssetPanelModel root-anchored: collapsing Content/ empties the whole table; reopening restores each dir's own state", "[editor]")
{
    NestedFixture f = MakeNestedFixture("arcane_asset_panel_model_root_anchor_cascade_test");

    FakeProviders fake;
    AssetPanelModel model;
    model.MarkAllDirty();
    AssetPanelProviders providers = fake.Make();
    REQUIRE(model.RebuildIfDirty(&f.registry, providers));

    // Baseline: "textures/" is Content/'s own child (depth 1) -- GroupParentOf
    // must resolve it to "Content/", not "" (top-level/no-parent), which is
    // exactly what root-anchoring changes.
    const AssetPanelRow* textures = FindGroupRow(model.Rows(), "textures/");
    REQUIRE(textures);
    CHECK(textures->groupDepth == 1);
    REQUIRE(FindGroupRow(model.Rows(), "textures/patterns/"));
    CHECK(HasAssetRow(model.Rows(), f.crateId));
    CHECK(HasAssetRow(model.Rows(), f.tilesId));

    // Independently collapse "textures/patterns/" (its own toggle) BEFORE
    // touching Content/ -- this is the sub-state that must survive the
    // Content/ round-trip below.
    model.SetGroupOpen("textures/patterns/", false);
    REQUIRE(model.RebuildIfDirty(&f.registry, providers));
    CHECK_FALSE(HasAssetRow(model.Rows(), f.tilesId));

    // Collapse Content/ ITSELF -- every directory (top-level "textures/"
    // included) must vanish from Rows(), since Content/ is now their common
    // ancestor. Content/'s own header stays (own-collapse rule, unchanged).
    model.SetGroupOpen("Content/", false);
    REQUIRE(model.RebuildIfDirty(&f.registry, providers));
    REQUIRE(FindGroupRow(model.Rows(), "Content/"));       // Content/'s own header stays
    CHECK_FALSE(FindGroupRow(model.Rows(), "textures/"));           // cascaded away
    CHECK_FALSE(FindGroupRow(model.Rows(), "textures/patterns/"));  // cascaded away, 2 hops
    CHECK_FALSE(HasAssetRow(model.Rows(), f.crateId));
    CHECK_FALSE(HasAssetRow(model.Rows(), f.tilesId));
    CHECK_FALSE(HasAssetRow(model.Rows(), f.noiseId));

    // Reopen Content/ -- "textures/" comes back (it was never individually
    // touched, so still open), but "textures/patterns/" restores to its OWN
    // previously-set CLOSED state -- proving Content/'s toggle never
    // clobbers a descendant's independent flag, exactly like any other
    // parent/child pair in this tree.
    model.SetGroupOpen("Content/", true);
    REQUIRE(model.RebuildIfDirty(&f.registry, providers));
    REQUIRE(FindGroupRow(model.Rows(), "textures/"));
    CHECK(HasAssetRow(model.Rows(), f.crateId));
    REQUIRE(FindGroupRow(model.Rows(), "textures/patterns/"));   // subtree back
    CHECK_FALSE(HasAssetRow(model.Rows(), f.tilesId));           // ...but still collapsed, as left
    CHECK_FALSE(HasAssetRow(model.Rows(), f.noiseId));

    std::error_code cleanupEc;
    fs::remove_all(f.dir, cleanupEc);
}

// (b) Content/ bridge-only case: a project with ZERO loose root files still
// renders a Content/ group row -- the unconditional ancestor bridge's own
// unconditional case (spec s6: "Content/ is this bridge's unconditional
// case... because every other group now needs it as an ancestor row to hang
// from"). Its count is suppressed (own entries are genuinely zero), and its
// subtree is fully reachable underneath it.
TEST_CASE("AssetPanelModel root-anchored: Content/ bridges in with zero root files, and its subtree is reachable", "[editor]")
{
    NestedFixture f = MakeNestedFixture("arcane_asset_panel_model_root_anchor_bridge_test");
    // MakeNestedFixture writes no root-level file -- exactly the shape this
    // case needs (see its own doc comment above).

    FakeProviders fake;
    AssetPanelModel model;
    model.MarkAllDirty();
    AssetPanelProviders providers = fake.Make();
    REQUIRE(model.RebuildIfDirty(&f.registry, providers));

    const AssetPanelRow* content = FindGroupRow(model.Rows(), "Content/");
    REQUIRE(content);              // bridged in despite zero root files
    CHECK(content->groupDepth == 0);
    CHECK(content->groupCount == 0);   // no root file of its own -- rider 8 suppresses the "0" in the UI

    // The whole subtree is reachable underneath the bridge, by default (open):
    // "textures/" (depth 1) and its own nested "textures/patterns/" (depth 2),
    // with their asset rows visible.
    REQUIRE(FindGroupRow(model.Rows(), "textures/"));
    REQUIRE(FindGroupRow(model.Rows(), "textures/patterns/"));
    CHECK(HasAssetRow(model.Rows(), f.crateId));
    CHECK(HasAssetRow(model.Rows(), f.tilesId));
    CHECK(HasAssetRow(model.Rows(), f.noiseId));

    std::error_code cleanupEc;
    fs::remove_all(f.dir, cleanupEc);
}

// ---------------------------------------------------------------------------
// Mount-rooted asset tree (2026-09-07, third revision): one depth-0 root
// group per POPULATED mount scheme -- "Content/" (game://) and "diagnostics/"
// (diag://) are PEERS in the same table, never one folded into the other.
// See docs/specs/2026-09-06-asset-manager-redesign-design.md s5/s6/s17 (third
// revision entry) and followup-treeview-impl-report.md's mount-rooting
// addendum for the ruling this enforces.
// ---------------------------------------------------------------------------

// (a) + (d): diag:// assets never fold into Content/ (the honest split this
// revision exists for), and a diag-mount directory's depth is measured
// WITHIN the diag mount's own tree -- 1 + nesting below diag's OWN root,
// exactly the same rule Content/'s subtree already uses, applied to a
// second, independent root.
TEST_CASE("AssetPanelModel mount-rooted: diag:// gets its own diagnostics/ root, honestly split from Content/", "[editor]")
{
    const fs::path gameDir = fs::temp_directory_path() / "arcane_asset_panel_model_mountroot_split_game_test";
    const fs::path diagDir = fs::temp_directory_path() / "arcane_asset_panel_model_mountroot_split_diag_test";
    std::error_code ec;
    fs::remove_all(gameDir, ec);
    fs::remove_all(diagDir, ec);
    fs::create_directories(gameDir);
    fs::create_directories(diagDir / "crashes");

    WriteFile(gameDir, "hero.png", "bytes-hero");
    const Arcane::Guid crash1Id = WriteDiagFile(diagDir, "crash1.arcdiag");
    const Arcane::Guid crash2Id = WriteDiagFile(diagDir / "crashes", "crash2.arcdiag");

    Arcane::AssetRegistry registry;
    REQUIRE(registry.ScanContent(gameDir, "game") == 1);
    REQUIRE(registry.AddContent(diagDir, "diag") == 2);   // additive -- ScanContent alone would wipe game://

    const Arcane::Guid heroId = GuidForPath(registry.All(), "game://hero.png");
    REQUIRE(heroId.IsValid());

    FakeProviders fake;
    AssetPanelModel model;
    model.MarkAllDirty();
    AssetPanelProviders providers = fake.Make();
    REQUIRE(model.RebuildIfDirty(&registry, providers));

    // Content/ holds ONLY hero.png -- no diag leakage, its own count stays 1.
    const AssetPanelRow* content = FindGroupRow(model.Rows(), "Content/");
    REQUIRE(content);
    CHECK(content->groupCount == 1);
    CHECK(HasAssetRow(model.Rows(), heroId));
    CHECK_FALSE(HasAssetRow(model.Rows(), crash1Id));
    CHECK_FALSE(HasAssetRow(model.Rows(), crash2Id));

    // Diagnostics/ exists as its OWN depth-0 root (key "diag://", label
    // "Diagnostics/" -- capitalised like its peers Content/ and Source/,
    // 2026-09-12) -- present even collapsed (its own header always
    // shows, same rule every other group's own-collapse follows).
    const AssetPanelRow* diagRoot = FindGroupRow(model.Rows(), "diag://");
    REQUIRE(diagRoot);
    CHECK(diagRoot->groupLabel == "Diagnostics/");
    CHECK(diagRoot->groupDepth == 0);
    CHECK(diagRoot->groupCount == 1);   // crash1.arcdiag only -- its own direct row

    // Open it to inspect the subtree: depth measured WITHIN the diag mount.
    // NOTE: `content` (above) is a pointer into the PRE-rebuild Rows() vector
    // -- RebuildIfDirty clears and repopulates that vector, so it must be
    // re-derived fresh afterward, never reused across a rebuild.
    model.SetGroupOpen("diag://", true);
    REQUIRE(model.RebuildIfDirty(&registry, providers));
    CHECK(HasAssetRow(model.Rows(), crash1Id));
    const AssetPanelRow* crashesGroup = FindGroupRow(model.Rows(), "diag://crashes/");
    REQUIRE(crashesGroup);
    CHECK(crashesGroup->groupLabel == "crashes/");
    CHECK(crashesGroup->groupDepth == 1);   // 1 + nesting below diag's OWN root, not Content/'s
    CHECK(HasAssetRow(model.Rows(), crash2Id));

    // Content/ is untouched by any of the above -- re-derived fresh, not the
    // stale pre-rebuild pointer above.
    const AssetPanelRow* contentAfter = FindGroupRow(model.Rows(), "Content/");
    REQUIRE(contentAfter);
    CHECK(contentAfter->groupCount == 1);
    CHECK(HasAssetRow(model.Rows(), heroId));

    fs::remove_all(gameDir, ec);
    fs::remove_all(diagDir, ec);
}

// Ordering regression pin: "diag://" (a QUALIFIED key) sorts alphabetically
// BETWEEN "Content/" ('C') and "materials/" ('m') on raw bytes -- a first cut
// of mount-rooting used plain lexicographic order over the group keys and
// that is EXACTLY what it produced: diagnostics/ interleaved INSIDE
// Content/'s own subtree instead of after it entirely (caught by the tracked-
// ReferenceProject capture this pass's own report cites, not by any unit
// test -- this fixture is that missing pin). Every mount's subtree must stay
// CONTIGUOUS in Rows(): all of Content/'s own rows (any depth) before diag://
// 's own root row.
TEST_CASE("AssetPanelModel mount-rooted: Content/'s entire subtree renders contiguously before diag://'s root", "[editor]")
{
    const fs::path gameDir = fs::temp_directory_path() / "arcane_asset_panel_model_mountroot_order_game_test";
    const fs::path diagDir = fs::temp_directory_path() / "arcane_asset_panel_model_mountroot_order_diag_test";
    std::error_code ec;
    fs::remove_all(gameDir, ec);
    fs::remove_all(diagDir, ec);
    fs::create_directories(gameDir / "materials");
    fs::create_directories(diagDir);

    WriteFile(gameDir, "hero.png", "bytes-hero");
    WriteFile(gameDir / "materials", "mat.arcmat",
             R"({"id":"c2000001-0001-4001-8001-000000000001","type":"material","kind":"fullscreen"})");
    WriteDiagFile(diagDir, "crash1.arcdiag");

    Arcane::AssetRegistry registry;
    REQUIRE(registry.ScanContent(gameDir, "game") == 2);
    REQUIRE(registry.AddContent(diagDir, "diag") == 1);

    FakeProviders fake;
    AssetPanelModel model;
    model.MarkAllDirty();
    REQUIRE(model.RebuildIfDirty(&registry, fake.Make()));

    const auto& rows = model.Rows();
    auto indexOfGroup = [&](const std::string& key) -> int
    {
        for (int i = 0; i < static_cast<int>(rows.size()); ++i)
            if (rows[i].type == AssetPanelRow::Type::Group && rows[i].groupName == key)
                return i;
        return -1;
    };

    const int iContent   = indexOfGroup("Content/");
    const int iMaterials = indexOfGroup("materials/");
    const int iDiagRoot  = indexOfGroup("diag://");
    REQUIRE(iContent >= 0);
    REQUIRE(iMaterials >= 0);
    REQUIRE(iDiagRoot >= 0);

    // Content/'s own subtree (root + materials/) is entirely contiguous...
    CHECK(iContent < iMaterials);
    // ...and diag://'s root comes AFTER every one of Content/'s rows -- not
    // wedged between them, which plain byte-order ('C' < 'd' < 'm') would do.
    CHECK(iMaterials < iDiagRoot);

    fs::remove_all(gameDir, ec);
    fs::remove_all(diagDir, ec);
}

// (b) diagnostics/ defaults COLLAPSED while every other mount root (Content/
// included) defaults open -- with NO explicit SetGroupOpen call on either,
// straight off MarkAllDirty + RebuildIfDirty.
TEST_CASE("AssetPanelModel mount-rooted: diagnostics/ defaults collapsed while Content/ defaults open", "[editor]")
{
    const fs::path gameDir = fs::temp_directory_path() / "arcane_asset_panel_model_mountroot_defaultopen_game_test";
    const fs::path diagDir = fs::temp_directory_path() / "arcane_asset_panel_model_mountroot_defaultopen_diag_test";
    std::error_code ec;
    fs::remove_all(gameDir, ec);
    fs::remove_all(diagDir, ec);
    fs::create_directories(gameDir);
    fs::create_directories(diagDir);

    WriteFile(gameDir, "hero.png", "bytes-hero");
    const Arcane::Guid crashId = WriteDiagFile(diagDir, "crash1.arcdiag");

    Arcane::AssetRegistry registry;
    REQUIRE(registry.ScanContent(gameDir, "game") == 1);
    REQUIRE(registry.AddContent(diagDir, "diag") == 1);
    const Arcane::Guid heroId = GuidForPath(registry.All(), "game://hero.png");
    REQUIRE(heroId.IsValid());

    FakeProviders fake;
    AssetPanelModel model;
    model.MarkAllDirty();
    REQUIRE(model.RebuildIfDirty(&registry, fake.Make()));   // no SetGroupOpen anywhere -- pure defaults

    REQUIRE(FindGroupRow(model.Rows(), "Content/"));
    CHECK(HasAssetRow(model.Rows(), heroId));            // Content/ default OPEN -> visible

    REQUIRE(FindGroupRow(model.Rows(), "diag://"));       // header always shows regardless
    CHECK_FALSE(HasAssetRow(model.Rows(), crashId));      // diagnostics/ default COLLAPSED -> hidden

    fs::remove_all(gameDir, ec);
    fs::remove_all(diagDir, ec);
}

// (c) The keying-collision pin: a REAL "game://diagnostics/" directory (an
// ordinary user-created folder that happens to be named "diagnostics",
// nothing to do with the diag:// mount) must NOT alias the diag mount's own
// synthetic root, even though the two share the identical DISPLAY label
// ("diagnostics/") -- distinct KEYS, distinct depths, fully independent open
// flags.
TEST_CASE("AssetPanelModel mount-rooted: a real game://diagnostics/ directory never collides with the diag:// root", "[editor]")
{
    const fs::path gameDir = fs::temp_directory_path() / "arcane_asset_panel_model_mountroot_keycollision_game_test";
    const fs::path diagDir = fs::temp_directory_path() / "arcane_asset_panel_model_mountroot_keycollision_diag_test";
    std::error_code ec;
    fs::remove_all(gameDir, ec);
    fs::remove_all(diagDir, ec);
    fs::create_directories(gameDir / "diagnostics");   // a REAL user directory, game-scheme
    fs::create_directories(diagDir);

    WriteFile(gameDir / "diagnostics", "note.json", R"({"id":"c1000001-0001-4001-8001-000000000001"})");
    const Arcane::Guid crashId = WriteDiagFile(diagDir, "crash1.arcdiag");

    Arcane::AssetRegistry registry;
    REQUIRE(registry.ScanContent(gameDir, "game") == 1);
    REQUIRE(registry.AddContent(diagDir, "diag") == 1);
    const auto all = registry.All();
    const Arcane::Guid noteId = GuidForPath(all, "game://diagnostics/note.json");
    REQUIRE(noteId.IsValid());

    FakeProviders fake;
    AssetPanelModel model;
    model.MarkAllDirty();
    AssetPanelProviders providers = fake.Make();
    REQUIRE(model.RebuildIfDirty(&registry, providers));

    // Two DISTINCT group rows -- same label, different keys, different mounts.
    const AssetPanelRow* gameDiagnostics = FindGroupRow(model.Rows(), "diagnostics/");   // UNQUALIFIED: game://
    const AssetPanelRow* diagRoot        = FindGroupRow(model.Rows(), "diag://");         // QUALIFIED: diag:// root
    REQUIRE(gameDiagnostics);
    REQUIRE(diagRoot);
    CHECK(gameDiagnostics->groupName != diagRoot->groupName);
    CHECK(gameDiagnostics->groupLabel == "diagnostics/");     // the real directory's own name, verbatim
    CHECK(diagRoot->groupLabel == "Diagnostics/");            // the mount's label -- differs only by case, and
                                                              // the KEYS are what keep them apart, not the label
    CHECK(gameDiagnostics->groupDepth == 1);                  // Content/'s own child
    CHECK(diagRoot->groupDepth == 0);                         // a mount root in its own right

    // Baseline defaults: game's "diagnostics/" is an ORDINARY directory ->
    // default OPEN (note.json visible); diag's root -> default COLLAPSED
    // (crash1 hidden) -- proven with NO explicit toggle yet.
    CHECK(HasAssetRow(model.Rows(), noteId));
    CHECK_FALSE(HasAssetRow(model.Rows(), crashId));

    // Close the GAME "diagnostics/" -- the diag:// root must be UNAFFECTED
    // (still collapsed, by its own independent default, not because of this).
    model.SetGroupOpen("diagnostics/", false);
    REQUIRE(model.RebuildIfDirty(&registry, providers));
    CHECK_FALSE(HasAssetRow(model.Rows(), noteId));
    CHECK_FALSE(HasAssetRow(model.Rows(), crashId));   // still collapsed, independently

    // Open the DIAG root -- the game "diagnostics/" must stay exactly as THIS
    // test left it (closed), never reopened by the diag root's own toggle.
    model.SetGroupOpen("diag://", true);
    REQUIRE(model.RebuildIfDirty(&registry, providers));
    CHECK(HasAssetRow(model.Rows(), crashId));
    CHECK_FALSE(HasAssetRow(model.Rows(), noteId));    // still closed, independently

    fs::remove_all(gameDir, ec);
    fs::remove_all(diagDir, ec);
}

// ---------------------------------------------------------------------------
// Plan 2 Task 4: the AssetReferenceIndex wired THROUGH the model -- the model
// feeds the index the very same `refsFor` answer each rebuilt guid already
// fetches (never a second ask -- case (f) above is the standing pin on that),
// derives `AssetPanelEntry::unused` from the index's inbound counts under spec
// s9.1's eligibility rule, and tallies it into HealthCounts for the bottom-bar
// digest. Same real-registry + FakeProviders discipline as every case above.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Source/ in the Asset Browser: the project's C++ source tree (source://,
// AssetRegistry's path-derived-guid rule) is a THIRD peer root beside
// Content/ and diagnostics/. Same mount-rooted machinery as diag:// above --
// what is new is the kind (AssetKind::Source), the root label ("Source/"),
// and that, unlike diagnostics/, it defaults OPEN: the whole point of the
// step is making source VISIBLE.
// ---------------------------------------------------------------------------
TEST_CASE("AssetPanelModel mount-rooted: source:// gets its own Source/ root, kind Source, default open", "[editor]")
{
    const fs::path gameDir   = fs::temp_directory_path() / "arcane_asset_panel_model_mountroot_source_game_test";
    const fs::path sourceDir = fs::temp_directory_path() / "arcane_asset_panel_model_mountroot_source_src_test";
    std::error_code ec;
    fs::remove_all(gameDir, ec);
    fs::remove_all(sourceDir, ec);
    fs::create_directories(gameDir);
    fs::create_directories(sourceDir / "sub");

    WriteFile(gameDir, "hero.png", "bytes-hero");
    WriteFile(sourceDir, "Game.cpp", "// cpp\n");
    WriteFile(sourceDir, "Game.hpp", "// hpp\n");
    WriteFile(sourceDir / "sub", "Foo.h", "// h\n");

    Arcane::AssetRegistry registry;
    REQUIRE(registry.ScanContent(gameDir, "game") == 1);
    REQUIRE(registry.AddContent(sourceDir, "source") == 3);

    const Arcane::Guid heroId = GuidForPath(registry.All(), "game://hero.png");
    const Arcane::Guid cppId  = GuidForPath(registry.All(), "source://Game.cpp");
    const Arcane::Guid hId    = GuidForPath(registry.All(), "source://sub/Foo.h");
    REQUIRE(heroId.IsValid());
    REQUIRE(cppId.IsValid());
    REQUIRE(hId.IsValid());

    FakeProviders fake;
    AssetPanelModel model;
    model.MarkAllDirty();
    REQUIRE(model.RebuildIfDirty(&registry, fake.Make()));   // no SetGroupOpen anywhere -- pure defaults

    // Content/ is untouched: still exactly hero.png.
    const AssetPanelRow* content = FindGroupRow(model.Rows(), "Content/");
    REQUIRE(content);
    CHECK(content->groupCount == 1);

    // Source/ is its OWN depth-0 root (key "source://", label "Source/"),
    // counting only its direct files, and default OPEN -- rows visible with
    // no SetGroupOpen call, the opposite of diagnostics/'s default.
    const AssetPanelRow* sourceRoot = FindGroupRow(model.Rows(), "source://");
    REQUIRE(sourceRoot);
    CHECK(sourceRoot->groupLabel == "Source/");
    CHECK(sourceRoot->groupDepth == 0);
    CHECK(sourceRoot->groupCount == 2);   // Game.cpp + Game.hpp -- sub/Foo.h is the nested group's
    CHECK(HasAssetRow(model.Rows(), cppId));

    // Nested directory: depth measured within the source mount's own tree.
    const AssetPanelRow* sub = FindGroupRow(model.Rows(), "source://sub/");
    REQUIRE(sub);
    CHECK(sub->groupLabel == "sub/");
    CHECK(sub->groupDepth == 1);
    CHECK(HasAssetRow(model.Rows(), hId));

    // Classified as Source (its own kind, its own rail entry), never Other.
    const AssetPanelEntry* cpp = model.Find(cppId);
    REQUIRE(cpp);
    CHECK(cpp->kind == AssetKind::Source);
    CHECK(cpp->fileName == "Game.cpp");
    bool railHasSource = false;
    for (const RailEntry& r : model.Rail())
        if (r.kind == static_cast<int>(AssetKind::Source))
        {
            railHasSource = true;
            CHECK(r.label == "Source");
            CHECK(r.count == 3);
        }
    CHECK(railHasSource);

    // Peer ordering, same pin as diag://: every one of Content/'s rows before
    // source://'s root, never interleaved.
    const auto& rows = model.Rows();
    int iContent = -1, iSource = -1;
    for (int i = 0; i < static_cast<int>(rows.size()); ++i)
    {
        if (rows[i].type != AssetPanelRow::Type::Group) continue;
        if (rows[i].groupName == "Content/")  iContent = i;
        if (rows[i].groupName == "source://") iSource  = i;
    }
    REQUIRE(iContent >= 0);
    REQUIRE(iSource >= 0);
    CHECK(iContent < iSource);

    fs::remove_all(gameDir, ec);
    fs::remove_all(sourceDir, ec);
}

// User-directed root order (2026-09-12): Content/ then Source/ then
// diagnostics/. Plain byte order over the qualified keys would put "diag://"
// BEFORE "source://" ('d' < 's'), so the peer order has to be a per-scheme
// RANK, not the key text: source first, diag last, any other scheme between.
// Also pins that the rank never leaks into a mount's OWN subtree order
// (source://sub/ still sits inside source://'s run).
TEST_CASE("AssetPanelModel mount-rooted: root order is Content/ then Source/ then diagnostics/", "[editor]")
{
    const fs::path gameDir   = fs::temp_directory_path() / "arcane_asset_panel_model_mountroot_order3_game_test";
    const fs::path sourceDir = fs::temp_directory_path() / "arcane_asset_panel_model_mountroot_order3_src_test";
    const fs::path diagDir   = fs::temp_directory_path() / "arcane_asset_panel_model_mountroot_order3_diag_test";
    std::error_code ec;
    fs::remove_all(gameDir, ec);
    fs::remove_all(sourceDir, ec);
    fs::remove_all(diagDir, ec);
    fs::create_directories(gameDir / "materials");
    fs::create_directories(sourceDir / "sub");
    fs::create_directories(diagDir);

    WriteFile(gameDir, "hero.png", "bytes-hero");
    WriteFile(gameDir / "materials", "mat.arcmat",
             R"({"id":"c2000001-0001-4001-8001-000000000002","type":"material","kind":"fullscreen"})");
    WriteFile(sourceDir, "Game.cpp", "// cpp\n");
    WriteFile(sourceDir / "sub", "Foo.h", "// h\n");
    WriteDiagFile(diagDir, "crash1.arcdiag");

    Arcane::AssetRegistry registry;
    REQUIRE(registry.ScanContent(gameDir, "game") == 2);
    REQUIRE(registry.AddContent(sourceDir, "source") == 2);
    REQUIRE(registry.AddContent(diagDir, "diag") == 1);

    FakeProviders fake;
    AssetPanelModel model;
    model.MarkAllDirty();
    REQUIRE(model.RebuildIfDirty(&registry, fake.Make()));

    const auto& rows = model.Rows();
    auto indexOfGroup = [&](const std::string& key) -> int
    {
        for (int i = 0; i < static_cast<int>(rows.size()); ++i)
            if (rows[i].type == AssetPanelRow::Type::Group && rows[i].groupName == key)
                return i;
        return -1;
    };
    const int iContent   = indexOfGroup("Content/");
    const int iMaterials = indexOfGroup("materials/");
    const int iSource    = indexOfGroup("source://");
    const int iSourceSub = indexOfGroup("source://sub/");
    const int iDiag      = indexOfGroup("diag://");
    REQUIRE(iContent >= 0);
    REQUIRE(iMaterials >= 0);
    REQUIRE(iSource >= 0);
    REQUIRE(iSourceSub >= 0);
    REQUIRE(iDiag >= 0);

    CHECK(iContent < iMaterials);     // Content/'s subtree first, intact
    CHECK(iMaterials < iSource);      // then Source/'s root...
    CHECK(iSource < iSourceSub);      // ...with its own subtree right under it
    CHECK(iSourceSub < iDiag);        // and diagnostics/ LAST, after all of Source/

    fs::remove_all(gameDir, ec);
    fs::remove_all(sourceDir, ec);
    fs::remove_all(diagDir, ec);
}

namespace
{
    bool ContainsGuid(const std::vector<Arcane::Guid>& v, const Arcane::Guid& g)
    {
        return std::find(v.begin(), v.end(), g) != v.end();
    }
}

// (0) The pure rule itself, EXHAUSTIVELY over AssetKind -- the same treatment
// CookStateOf gets above, and the only place Diagnostic/Other are pinned (no
// fixture below reaches them). Spec s9.1's list is a verbatim requirement, so
// a kind silently changing sides here has to fail a test.
TEST_CASE("IsUnusedEligible: exactly Texture/Material/Sprite/Mesh/Model (spec s9.1, "
          "F2c s4.1)", "[editor]")
{
    CHECK(IsUnusedEligible(AssetKind::Texture));
    CHECK(IsUnusedEligible(AssetKind::Material));
    CHECK(IsUnusedEligible(AssetKind::Sprite));
    CHECK(IsUnusedEligible(AssetKind::Mesh));
    // F2c s4.1, Task 9: Model joins the list -- its one consumer (the
    // companion .arcmesh's DerivesFrom edge) is fully visible to the
    // reference index, same as every other eligible kind here.
    CHECK(IsUnusedEligible(AssetKind::Model));

    CHECK_FALSE(IsUnusedEligible(AssetKind::Scene));        // roots -- never unused
    CHECK_FALSE(IsUnusedEligible(AssetKind::Data));         // consumed by game code the index cannot see
    CHECK_FALSE(IsUnusedEligible(AssetKind::Audio));
    CHECK_FALSE(IsUnusedEligible(AssetKind::Font));
    CHECK_FALSE(IsUnusedEligible(AssetKind::Diagnostic));
    CHECK_FALSE(IsUnusedEligible(AssetKind::Source));       // no index sees who includes a header
    CHECK_FALSE(IsUnusedEligible(AssetKind::Other));

    // Exhaustive: every value of AssetKind is accounted for above, so a newly
    // added kind can never default into eligibility unnoticed.
    int eligible = 0;
    for (int i = 0; i < kAssetKindCount; ++i)
        if (IsUnusedEligible(static_cast<AssetKind>(i)))
            ++eligible;
    CHECK(eligible == 5);
}

// (1) Eligibility: spec s9.1's list is EXACTLY {Texture, Material, Sprite,
// Mesh}. Scene/Data/Audio/Font/Diagnostic/Other are exempt -- their consumers
// are game code the index cannot see, so a zero-inbound one is never accused.
// Also pins the CASCADE half of the rule: a dead 1:1 sprite has zero inbound
// of its own and flags FIRST (removing it is what would then surface its
// texture), so this fixture's count is 2, not 1.
TEST_CASE("AssetPanelModel unused: zero-inbound eligible kinds only (spec s9.1)", "[editor]")
{
    const fs::path dir = fs::temp_directory_path() / "arcane_asset_panel_model_unused_kinds_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "sprites");

    WriteFile(dir, "used.png", "bytes-used");        // texture A -- the sprite derives from it
    WriteFile(dir, "orphan.png", "bytes-orphan");    // texture B -- nothing points at it
    WriteFile(dir / "sprites", "used_full.arcsprite",
             R"({"id":"d1000001-0001-4001-8001-000000000001","type":"sprite","name":"UsedFull"})");
    WriteFile(dir, "notes.json", R"({"id":"d1000001-0001-4001-8001-000000000002"})");        // Data -- exempt
    WriteFile(dir, "level.arcscene",
             R"({"id":"d1000001-0001-4001-8001-000000000003","version":4,"entities":[]})");   // Scene -- a root, exempt

    Arcane::AssetRegistry registry;
    REQUIRE(registry.ScanContent(dir, "game") == 5);
    const auto all = registry.All();

    const Arcane::Guid usedTexId   = GuidForPath(all, "game://used.png");
    const Arcane::Guid orphanTexId = GuidForPath(all, "game://orphan.png");
    const Arcane::Guid spriteId    = *Arcane::Guid::FromString("d1000001-0001-4001-8001-000000000001");
    const Arcane::Guid notesId     = *Arcane::Guid::FromString("d1000001-0001-4001-8001-000000000002");
    const Arcane::Guid sceneId     = *Arcane::Guid::FromString("d1000001-0001-4001-8001-000000000003");
    REQUIRE(usedTexId.IsValid());
    REQUIRE(orphanTexId.IsValid());

    FakeProviders fake;
    fake.refsByGuid[spriteId] = { { usedTexId, Arcane::AssetRefKind::DerivesFrom } };

    AssetPanelModel model;
    model.MarkAllDirty();
    REQUIRE(model.RebuildIfDirty(&registry, fake.Make()));

    REQUIRE(model.Find(usedTexId));
    REQUIRE(model.Find(orphanTexId));
    REQUIRE(model.Find(spriteId));       // a folded child is still an ENTRY
    REQUIRE(model.Find(notesId));
    REQUIRE(model.Find(sceneId));

    // The index saw the sprite's DerivesFrom edge -- inbound counts include
    // BOTH edge kinds (References AND DerivesFrom), which is what makes the
    // cascade rule work at all.
    CHECK(model.RefIndex().InboundCount(usedTexId) == 1);
    CHECK(model.RefIndex().InboundCount(orphanTexId) == 0);
    CHECK(model.RefIndex().InboundCount(spriteId) == 0);

    CHECK_FALSE(model.Find(usedTexId)->unused);   // one inbound -- in use
    CHECK(model.Find(orphanTexId)->unused);        // texture, zero inbound
    CHECK(model.Find(spriteId)->unused);           // sprite, zero inbound -- the cascade root

    // Exempt kinds: zero inbound (proven, not assumed) yet never flagged.
    CHECK(model.RefIndex().InboundCount(notesId) == 0);
    CHECK(model.RefIndex().InboundCount(sceneId) == 0);
    CHECK_FALSE(model.Find(notesId)->unused);
    CHECK_FALSE(model.Find(sceneId)->unused);

    CHECK(model.Health().unused == 2);

    // UnusedGuids() is sorted by entry NAME -- "orphan" < "used_full".
    const std::vector<Arcane::Guid> unused = model.UnusedGuids();
    REQUIRE(unused.size() == 2);
    CHECK(unused[0] == orphanTexId);
    CHECK(unused[1] == spriteId);

    fs::remove_all(dir, ec);
}

// (2) Incremental: re-pointing one asset's refs and dirtying ONLY it must swap
// the flags on both the old and the new target -- the remove-before-re-add
// discipline flowing through the model seam. Untouched entries' inbound counts
// change here, which is exactly why the unused pass re-runs over ALL entries
// on a per-guid rebuild.
TEST_CASE("AssetPanelModel unused updates incrementally through MarkDirty", "[editor]")
{
    const fs::path dir = fs::temp_directory_path() / "arcane_asset_panel_model_unused_incremental_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "materials");

    WriteFile(dir, "t1.png", "bytes-t1");
    WriteFile(dir, "t2.png", "bytes-t2");
    WriteFile(dir / "materials", "m.arcmat",
             R"({"id":"d2000001-0001-4001-8001-000000000001","type":"material","kind":"fullscreen"})");

    Arcane::AssetRegistry registry;
    REQUIRE(registry.ScanContent(dir, "game") == 3);
    const auto all = registry.All();

    const Arcane::Guid t1Id = GuidForPath(all, "game://t1.png");
    const Arcane::Guid t2Id = GuidForPath(all, "game://t2.png");
    const Arcane::Guid matId = *Arcane::Guid::FromString("d2000001-0001-4001-8001-000000000001");
    REQUIRE(t1Id.IsValid());
    REQUIRE(t2Id.IsValid());

    FakeProviders fake;
    // References (not DerivesFrom): a DerivesFrom to a texture would FOLD the
    // material under it, which is a different behavior entirely -- this case
    // is about inbound counts, nothing else.
    fake.refsByGuid[matId] = { { t1Id, Arcane::AssetRefKind::References } };

    AssetPanelModel model;
    model.MarkAllDirty();
    AssetPanelProviders providers = fake.Make();
    REQUIRE(model.RebuildIfDirty(&registry, providers));

    CHECK(model.RefIndex().InboundCount(t1Id) == 1);
    CHECK(model.RefIndex().InboundCount(t2Id) == 0);
    CHECK_FALSE(model.Find(t1Id)->unused);
    CHECK(model.Find(t2Id)->unused);
    CHECK(model.Find(matId)->unused);   // the material itself has zero inbound

    // Re-point the material at t2 and dirty ONLY the material.
    fake.refsByGuid[matId] = { { t2Id, Arcane::AssetRefKind::References } };
    model.MarkDirty(matId);
    REQUIRE(model.RebuildIfDirty(&registry, providers));

    CHECK(model.RefIndex().InboundCount(t1Id) == 0);
    CHECK(model.RefIndex().InboundCount(t2Id) == 1);
    CHECK(model.Find(t1Id)->unused);         // flags swapped...
    CHECK_FALSE(model.Find(t2Id)->unused);   // ...both ways
    CHECK(model.Health().unused == 2);       // t1 + the material

    // The single-ask discipline: the index is fed the SAME answer the entry
    // build already fetched, so only the dirtied guid was re-asked.
    CHECK(fake.refsCalls[matId] == 2);
    CHECK(fake.refsCalls[t1Id] == 1);
    CHECK(fake.refsCalls[t2Id] == 1);

    fs::remove_all(dir, ec);
}

// (3) Last-known-good (spec s3.2): a nullopt refs answer for an asset that
// still EXISTS keeps its outbound edges -- and therefore every inbound count
// they contribute -- exactly as they were. An unreadable material must never
// make its texture look orphaned.
TEST_CASE("AssetPanelModel keeps last-known-good inbound counts on a nullopt refs answer", "[editor]")
{
    const fs::path dir = fs::temp_directory_path() / "arcane_asset_panel_model_unused_lastknowngood_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "materials");

    WriteFile(dir, "tex.png", "bytes-tex");
    WriteFile(dir / "materials", "m.arcmat",
             R"({"id":"d3000001-0001-4001-8001-000000000001","type":"material","kind":"fullscreen"})");

    Arcane::AssetRegistry registry;
    REQUIRE(registry.ScanContent(dir, "game") == 2);
    const auto all = registry.All();

    const Arcane::Guid texId = GuidForPath(all, "game://tex.png");
    const Arcane::Guid matId = *Arcane::Guid::FromString("d3000001-0001-4001-8001-000000000001");
    REQUIRE(texId.IsValid());

    FakeProviders fake;
    fake.refsByGuid[matId] = { { texId, Arcane::AssetRefKind::References } };

    AssetPanelModel model;
    model.MarkAllDirty();
    AssetPanelProviders providers = fake.Make();
    REQUIRE(model.RebuildIfDirty(&registry, providers));

    REQUIRE(model.RefIndex().InboundCount(texId) == 1);
    REQUIRE_FALSE(model.Find(texId)->unused);

    // Second ask answers nullopt (unreadable/unparsable this walk), NOT an
    // empty list -- the distinction the whole last-known-good rule turns on.
    fake.nullRefsGuids.insert(matId);
    model.MarkDirty(matId);
    REQUIRE(model.RebuildIfDirty(&registry, providers));

    CHECK(fake.refsCalls[matId] == 2);              // it WAS re-asked...
    CHECK(model.RefIndex().InboundCount(texId) == 1);   // ...and nothing was retracted
    CHECK_FALSE(model.Find(texId)->unused);
    REQUIRE(model.RefIndex().Find(matId));
    CHECK(model.RefIndex().Find(matId)->outbound.size() == 1);   // forward edge kept too

    fs::remove_all(dir, ec);
}

// (4) Prune: an entry the registry no longer carries is retracted from the
// index through the SAME prune loop that drops it from m_entries -- both
// directions of the tombstone contract.
//   - delete the REFERENCER: its node vanishes entirely (nothing pointed at
//     it), and its former target loses that inbound edge -> newly unused.
//   - delete the TARGET: its node SURVIVES as a tombstone (its referencer
//     still points at it) and shows up in DanglingTargets().
// Both halves are exercised twice: once through the per-guid MarkDirty prune
// loop (the incremental path), then again through a full MarkAllDirty rebuild
// (the Clear-and-refill path), which must agree.
TEST_CASE("AssetPanelModel prunes a deleted asset into the index tombstone path", "[editor]")
{
    std::error_code ec;

    // --- Half A: the REFERENCER is deleted -------------------------------
    {
        const fs::path dir = fs::temp_directory_path() / "arcane_asset_panel_model_prune_referencer_test";
        fs::remove_all(dir, ec);
        fs::create_directories(dir / "materials");

        WriteFile(dir, "tex.png", "bytes-tex");
        WriteFile(dir / "materials", "m.arcmat",
                 R"({"id":"d4000001-0001-4001-8001-000000000001","type":"material","kind":"fullscreen"})");

        Arcane::AssetRegistry registry;
        REQUIRE(registry.ScanContent(dir, "game") == 2);
        const Arcane::Guid texId = GuidForPath(registry.All(), "game://tex.png");
        const Arcane::Guid matId = *Arcane::Guid::FromString("d4000001-0001-4001-8001-000000000001");
        REQUIRE(texId.IsValid());

        FakeProviders fake;
        fake.refsByGuid[matId] = { { texId, Arcane::AssetRefKind::References } };

        AssetPanelModel model;
        model.MarkAllDirty();
        AssetPanelProviders providers = fake.Make();
        REQUIRE(model.RebuildIfDirty(&registry, providers));
        REQUIRE(model.RefIndex().InboundCount(texId) == 1);
        REQUIRE_FALSE(model.Find(texId)->unused);

        // Delete the material and re-scan the SAME registry (the mechanics the
        // existing fold-target-removed prune case uses).
        fs::remove(dir / "materials" / "m.arcmat", ec);
        REQUIRE(registry.ScanContent(dir, "game") == 1);

        model.MarkDirty(matId);   // only the REMOVED guid is dirtied
        REQUIRE(model.RebuildIfDirty(&registry, providers));

        CHECK(model.Find(matId) == nullptr);                    // entry pruned...
        CHECK(model.RefIndex().Find(matId) == nullptr);          // ...and its node with it
        CHECK(model.RefIndex().InboundCount(texId) == 0);        // its edge retracted
        REQUIRE(model.Find(texId));
        CHECK(model.Find(texId)->unused);                        // newly unused
        CHECK_FALSE(ContainsGuid(model.RefIndex().DanglingTargets(), matId));
        CHECK(model.RefIndex().DanglingTargets().empty());
        CHECK(model.Health().unused == 1);

        // The full-rebuild path must agree with the incremental one.
        model.MarkAllDirty();
        REQUIRE(model.RebuildIfDirty(&registry, providers));
        CHECK(model.RefIndex().InboundCount(texId) == 0);
        CHECK(model.Find(texId)->unused);
        CHECK(model.RefIndex().DanglingTargets().empty());
        CHECK(model.Health().unused == 1);

        fs::remove_all(dir, ec);
    }

    // --- Half B: the TARGET is deleted (the tombstone case) --------------
    {
        const fs::path dir = fs::temp_directory_path() / "arcane_asset_panel_model_prune_target_test";
        fs::remove_all(dir, ec);
        fs::create_directories(dir / "materials");

        WriteFile(dir, "tex.png", "bytes-tex");
        WriteFile(dir / "materials", "m.arcmat",
                 R"({"id":"d5000001-0001-4001-8001-000000000001","type":"material","kind":"fullscreen"})");

        Arcane::AssetRegistry registry;
        REQUIRE(registry.ScanContent(dir, "game") == 2);
        const Arcane::Guid texId = GuidForPath(registry.All(), "game://tex.png");
        const Arcane::Guid matId = *Arcane::Guid::FromString("d5000001-0001-4001-8001-000000000001");
        REQUIRE(texId.IsValid());

        FakeProviders fake;
        fake.refsByGuid[matId] = { { texId, Arcane::AssetRefKind::References } };

        AssetPanelModel model;
        model.MarkAllDirty();
        AssetPanelProviders providers = fake.Make();
        REQUIRE(model.RebuildIfDirty(&registry, providers));
        REQUIRE(model.RefIndex().DanglingTargets().empty());

        // The texture's guid is sidecar-minted, so removing both the file and
        // its .meta is what actually drops it from the rescan.
        fs::remove(dir / "tex.png", ec);
        fs::remove(dir / "tex.png.meta", ec);
        REQUIRE(registry.ScanContent(dir, "game") == 1);

        model.MarkDirty(texId);
        REQUIRE(model.RebuildIfDirty(&registry, providers));

        CHECK(model.Find(texId) == nullptr);   // entry pruned...
        // ...but the NODE survives as a tombstone: the material still points
        // at it, so "who referenced the missing asset" is still answerable.
        CHECK(ContainsGuid(model.RefIndex().DanglingTargets(), texId));
        REQUIRE(model.RefIndex().Find(texId));
        CHECK_FALSE(model.RefIndex().Find(texId)->exists);
        CHECK(model.RefIndex().InboundCount(texId) == 1);
        REQUIRE(model.Find(matId));
        CHECK(model.Find(matId)->unused);      // the material itself: zero inbound
        CHECK(model.Health().unused == 1);     // the deleted texture is no longer an entry

        // Full rebuild: the tombstone is re-minted by the material's own walk.
        model.MarkAllDirty();
        REQUIRE(model.RebuildIfDirty(&registry, providers));
        CHECK(ContainsGuid(model.RefIndex().DanglingTargets(), texId));
        CHECK(model.Health().unused == 1);

        fs::remove_all(dir, ec);
    }
}

// (5) HealthCounts.unused is exactly the flagged-entry count -- the number the
// bottom-bar digest renders -- and UnusedGuids() enumerates the same set in a
// stable, name-sorted UI order (Task 8's Unreferenced card reads it verbatim).
TEST_CASE("AssetPanelModel HealthCounts.unused feeds the digest numbers", "[editor]")
{
    const fs::path dir = fs::temp_directory_path() / "arcane_asset_panel_model_unused_digest_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "materials");
    fs::create_directories(dir / "meshes");
    fs::create_directories(dir / "sprites");
    fs::create_directories(dir / "audio");
    fs::create_directories(dir / "fonts");

    WriteFile(dir, "hero.png", "bytes-hero");                    // Texture -- referenced twice
    WriteFile(dir / "materials", "glow.arcmat",
             R"({"id":"d6000001-0001-4001-8001-000000000001","type":"material","kind":"fullscreen"})");
    WriteFile(dir / "meshes", "cube.arcmesh",
             R"({"id":"d6000001-0001-4001-8001-000000000002","type":"mesh"})");
    WriteFile(dir / "sprites", "slice.arcsprite",
             R"({"id":"d6000001-0001-4001-8001-000000000003","type":"sprite"})");
    WriteFile(dir / "audio", "beep.wav", "bytes-beep");           // Audio -- exempt
    WriteFile(dir / "fonts", "ui.ttf", "bytes-ui");               // Font  -- exempt

    Arcane::AssetRegistry registry;
    REQUIRE(registry.ScanContent(dir, "game") == 6);
    const auto all = registry.All();

    const Arcane::Guid heroId  = GuidForPath(all, "game://hero.png");
    const Arcane::Guid beepId  = GuidForPath(all, "game://audio/beep.wav");
    const Arcane::Guid fontId  = GuidForPath(all, "game://fonts/ui.ttf");
    const Arcane::Guid glowId  = *Arcane::Guid::FromString("d6000001-0001-4001-8001-000000000001");
    const Arcane::Guid cubeId  = *Arcane::Guid::FromString("d6000001-0001-4001-8001-000000000002");
    const Arcane::Guid sliceId = *Arcane::Guid::FromString("d6000001-0001-4001-8001-000000000003");
    REQUIRE(heroId.IsValid());
    REQUIRE(beepId.IsValid());
    REQUIRE(fontId.IsValid());

    FakeProviders fake;
    fake.refsByGuid[glowId]  = { { heroId, Arcane::AssetRefKind::References } };
    fake.refsByGuid[sliceId] = { { heroId, Arcane::AssetRefKind::References } };   // sliced -> stays a peer

    AssetPanelModel model;
    model.MarkAllDirty();
    REQUIRE(model.RebuildIfDirty(&registry, fake.Make()));

    const HealthCounts health = model.Health();
    CHECK(health.total == 6);

    int flagged = 0;
    for (const auto& [guid, e] : model.Entries())
        if (e.unused)
            ++flagged;
    CHECK(health.unused == flagged);
    CHECK(health.unused == 3);   // glow (material), cube (mesh), slice (sprite)

    CHECK_FALSE(model.Find(heroId)->unused);   // two inbound
    CHECK_FALSE(model.Find(beepId)->unused);   // Audio -- exempt
    CHECK_FALSE(model.Find(fontId)->unused);   // Font  -- exempt

    // Name-sorted: "cube" < "glow" < "slice".
    const std::vector<Arcane::Guid> unused = model.UnusedGuids();
    REQUIRE(unused.size() == static_cast<std::size_t>(health.unused));
    CHECK(unused[0] == cubeId);
    CHECK(unused[1] == glowId);
    CHECK(unused[2] == sliceId);

    fs::remove_all(dir, ec);
}

TEST_CASE("asset model: a companion .arcmesh folds under its Model source", "[editor]")
{
    // s4.2/s8: the sprite-under-texture foldedUnder machinery, reused with ZERO new
    // code -- all it needed was the DerivesFrom edge (Plan 1 Task 12) and a fold
    // predicate that accepts a Model target as well as a Texture one.
    const fs::path dir = fs::temp_directory_path() / "arcane_asset_panel_model_model_fold_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    WriteFile(dir, "prop.glb", "not a real glb, just bytes");
    WriteFile(dir, "prop.arcmesh",
             R"({"id":"e1000001-0001-4001-8001-000000000001","type":"mesh","name":"Prop"})");

    Arcane::AssetRegistry registry;
    REQUIRE(registry.ScanContent(dir, "game") == 2);
    const auto all = registry.All();

    const Arcane::Guid modelGuid = GuidForPath(all, "game://prop.glb");
    const Arcane::Guid meshGuid  = *Arcane::Guid::FromString("e1000001-0001-4001-8001-000000000001");
    REQUIRE(modelGuid.IsValid());

    FakeProviders fake;
    fake.refsByGuid[meshGuid] = { { modelGuid, Arcane::AssetRefKind::DerivesFrom } };

    AssetPanelModel model;
    model.MarkAllDirty();
    REQUIRE(model.RebuildIfDirty(&registry, fake.Make()));

    const AssetPanelEntry* mesh  = model.Find(meshGuid);
    const AssetPanelEntry* modelE = model.Find(modelGuid);
    REQUIRE(mesh);
    REQUIRE(modelE);
    CHECK(mesh->kind == AssetKind::Mesh);
    CHECK(modelE->kind == AssetKind::Model);
    CHECK(mesh->foldedUnder == modelGuid);
    REQUIRE(modelE->derivedChildren.size() == 1u);
    CHECK(modelE->derivedChildren[0] == meshGuid);

    fs::remove_all(dir, ec);
}

TEST_CASE("asset model: a .arcmesh deriving from something that is NOT a Model or a"
          " Texture does not fold", "[editor]")
{
    // The predicate stays a whitelist, not "anything with one DerivesFrom" -- a fold
    // under an arbitrary asset would put a mesh inside a scene row.
    const fs::path dir = fs::temp_directory_path() / "arcane_asset_panel_model_mesh_mat_nofold_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "materials");

    WriteFile(dir / "materials", "body.arcmat",
             R"({"id":"e2000001-0001-4001-8001-000000000001","type":"material","kind":"mesh"})");
    WriteFile(dir, "body.arcmesh",
             R"({"id":"e2000001-0001-4001-8001-000000000002","type":"mesh","name":"Body"})");

    Arcane::AssetRegistry registry;
    REQUIRE(registry.ScanContent(dir, "game") == 2);
    const Arcane::Guid matId  = *Arcane::Guid::FromString("e2000001-0001-4001-8001-000000000001");
    const Arcane::Guid meshId = *Arcane::Guid::FromString("e2000001-0001-4001-8001-000000000002");

    FakeProviders fake;
    fake.refsByGuid[meshId] = { { matId, Arcane::AssetRefKind::DerivesFrom } };

    AssetPanelModel model;
    model.MarkAllDirty();
    REQUIRE(model.RebuildIfDirty(&registry, fake.Make()));

    const AssetPanelEntry* mesh = model.Find(meshId);
    REQUIRE(mesh);
    CHECK_FALSE(mesh->foldedUnder.IsValid());

    fs::remove_all(dir, ec);
}

TEST_CASE("asset model: an unreferenced Model reports as unused", "[editor]")
{
    // IsUnusedEligible(Model) was set in Plan 1 Task 9; this is the end-to-end half --
    // a .glb nobody imports (no companion) shows in the Unreferenced card, exactly
    // like a texture no sprite uses.
    const fs::path dir = fs::temp_directory_path() / "arcane_asset_panel_model_unused_model_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    WriteFile(dir, "lonely.glb", "not a real glb, just bytes");

    Arcane::AssetRegistry registry;
    REQUIRE(registry.ScanContent(dir, "game") == 1);
    const auto all = registry.All();
    const Arcane::Guid modelGuid = GuidForPath(all, "game://lonely.glb");
    REQUIRE(modelGuid.IsValid());

    FakeProviders fake;
    AssetPanelModel model;
    model.MarkAllDirty();
    REQUIRE(model.RebuildIfDirty(&registry, fake.Make()));

    const AssetPanelEntry* entry = model.Find(modelGuid);
    REQUIRE(entry);
    CHECK(entry->kind == AssetKind::Model);
    CHECK(entry->unused);
    CHECK(ContainsGuid(model.UnusedGuids(), modelGuid));

    fs::remove_all(dir, ec);
}
