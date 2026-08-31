// Arcane::AssetRegistry: scan-progress reporting (ScanContent's optional
// callback). The rest of AssetRegistry's behaviour (id resolution, native vs.
// imported-binary routing, mount-path shape) is already covered by
// AssetBrowserTest.cpp/MaterialAssetTest.cpp -- this file is scoped to the
// progress-callback addition and All()'s deterministic ordering. CPU-only.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/DiagEnvelope.hpp>
#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Project/AssetRegistry.hpp>
#include <Arcane/Project/Project.hpp>

#include <Panels/DiagnosticStore.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
    std::filesystem::path TempDir(const char* leaf)
    {
        std::filesystem::path d = std::filesystem::temp_directory_path() / "arcane_asset_registry_test" / leaf;
        std::error_code ec;
        std::filesystem::remove_all(d, ec);
        std::filesystem::create_directories(d);
        return d;
    }
}

TEST_CASE("ScanContent reports monotonic progress ending at the total", "[project]")
{
    const auto dir = TempDir("progress_basic");

    std::ofstream(dir / "a.arcmat", std::ios::binary)
        << R"({ "id": "aaaa1111-1111-4111-8111-111111111111" })";
    std::ofstream(dir / "b.arcmat", std::ios::binary)
        << R"({ "id": "bbbb2222-2222-4222-8222-222222222222" })";
    std::ofstream(dir / "c.arcmat", std::ios::binary)
        << R"({ "id": "cccc3333-3333-4333-8333-333333333333" })";

    std::vector<std::pair<std::size_t, std::size_t>> seen;
    Arcane::AssetRegistry reg;
    const std::size_t n = reg.ScanContent(dir, "game",
        [&](std::size_t done, std::size_t total) { seen.emplace_back(done, total); });

    CHECK(n == 3);
    REQUIRE_FALSE(seen.empty());
    for (std::size_t i = 1; i < seen.size(); ++i)
        CHECK(seen[i].first >= seen[i - 1].first);      // monotonic
    CHECK(seen.back().first == seen.back().second);      // terminates at total
    CHECK(seen.back().second == 3);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("ScanContent with no callback returns the same result as with one", "[project]")
{
    // Pins that the callback-less fast path (delegates straight to
    // AddContent, no counting pass) and the callback path (two-pass, reports
    // progress) register the identical set of assets -- the optimisation
    // Step 2 describes must not change WHAT gets scanned, only whether
    // progress is reported.
    const auto dir = TempDir("progress_parity");
    std::ofstream(dir / "a.arcmat", std::ios::binary)
        << R"({ "id": "aaaa1111-1111-4111-8111-111111111111" })";
    std::filesystem::create_directories(dir / "sub");
    std::ofstream(dir / "sub" / "b.arcmat", std::ios::binary)
        << R"({ "id": "bbbb2222-2222-4222-8222-222222222222" })";

    Arcane::AssetRegistry noCallback;
    const std::size_t nNoCallback = noCallback.ScanContent(dir, "game");

    Arcane::AssetRegistry withCallback;
    std::size_t calls = 0;
    const std::size_t nWithCallback = withCallback.ScanContent(dir, "game",
        [&](std::size_t, std::size_t) { ++calls; });

    CHECK(nNoCallback == 2);
    CHECK(nWithCallback == 2);
    CHECK(calls == 2);
    CHECK(noCallback.All().size() == withCallback.All().size());

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("ScanContent on an empty directory reports zero assets and never calls back", "[project]")
{
    const auto dir = TempDir("progress_empty");

    bool called = false;
    Arcane::AssetRegistry reg;
    const std::size_t n = reg.ScanContent(dir, "game",
        [&](std::size_t, std::size_t) { called = true; });

    CHECK(n == 0);
    CHECK_FALSE(called);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// Task 9: two native assets carrying the SAME embedded id -- the scan keeps the
// first registration and warns (AssetRegistry.cpp's "duplicate id ... keeping
// first" ARC_WARN); this proves the SAME event is now ALSO visible as a
// structured "assets" diagnostic, not just a log line. Materials are the
// cheapest native asset to author here (SaveMaterialAsset).
TEST_CASE("A duplicate asset id publishes an assets diagnostic", "[diagnostics]")
{
    Arcane::Editor::DiagnosticStore store;
    store.InstallAsEngineSink();

    const auto dir = TempDir("dup_id");

    Arcane::MaterialAssetData a;
    a.id      = Arcane::Guid::Generate();
    a.name    = "A";
    a.snippet = "float4 shade(Varyings v) { return 1; }\n";
    REQUIRE(Arcane::SaveMaterialAsset(dir / "a.arcmat", a));

    Arcane::MaterialAssetData b = a;   // same id on purpose
    b.name = "B";
    REQUIRE(Arcane::SaveMaterialAsset(dir / "b.arcmat", b));

    Arcane::AssetRegistry reg;
    reg.ScanContent(dir, "game");

    const std::vector<Arcane::Diagnostic> rows = store.Snapshot();
    REQUIRE_FALSE(rows.empty());
    CHECK(rows[0].code == "assets.id.duplicate");
    CHECK(rows[0].scope == Arcane::DiagScope::Assets);
    CHECK(rows[0].severity == Arcane::DiagSeverity::Warning);
    CHECK(rows[0].locator.kind == Arcane::DiagLocator::Kind::Asset);
    CHECK(rows[0].locator.asset == a.id);

    store.UninstallEngineSink();

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// A clean rescan (no duplicates this time) must RETRACT the prior scan's
// diagnostic -- Diagnostics::Publish is a publication-group replace, and
// ScanContent publishes unconditionally after every walk (see AssetRegistry.cpp).
TEST_CASE("A clean rescan retracts a previous duplicate-id diagnostic", "[diagnostics]")
{
    Arcane::Editor::DiagnosticStore store;
    store.InstallAsEngineSink();

    const auto dir = TempDir("dup_id_retract");

    Arcane::MaterialAssetData a;
    a.id      = Arcane::Guid::Generate();
    a.name    = "A";
    a.snippet = "float4 shade(Varyings v) { return 1; }\n";
    REQUIRE(Arcane::SaveMaterialAsset(dir / "a.arcmat", a));

    Arcane::MaterialAssetData b = a;
    b.name = "B";
    REQUIRE(Arcane::SaveMaterialAsset(dir / "b.arcmat", b));

    Arcane::AssetRegistry reg;
    reg.ScanContent(dir, "game");
    REQUIRE_FALSE(store.Snapshot().empty());

    // Fix the collision on disk (give b its own id), then rescan the SAME
    // registry instance -- ScanContent clears and rebuilds from scratch.
    std::error_code ec;
    std::filesystem::remove(dir / "b.arcmat", ec);
    Arcane::MaterialAssetData c = a;
    c.id   = Arcane::Guid::Generate();
    c.name = "C";
    REQUIRE(Arcane::SaveMaterialAsset(dir / "c.arcmat", c));

    reg.ScanContent(dir, "game");
    CHECK(store.Snapshot().empty());

    store.UninstallEngineSink();

    std::filesystem::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// GPU crash diagnostics arc, Task 9: diag:// mount + .arcdiag classification.
// Tagged [project] (not the arc's usual [diag] family) to match this file's
// own convention -- every other TEST_CASE here about scan/mount mechanics
// carries [project]; [diagnostics] is reserved (above) for cases that assert
// on the structured Diagnostics::Publish/Sink seam specifically.
// ---------------------------------------------------------------------------

// CRITICAL cross-task contract: a .arcdiag's guid lives under the top-level
// key "guid" (DiagEnvelope.hpp's Envelope::guid / Parse), NOT "id" -- the key
// ResolveNativeId reads for .json/.arcmat/.arcscene/.arcsprite. Diag::WriteFile
// never writes an "id" field, so if AddFile ever regresses to routing
// .arcdiag through ResolveNativeId, this test catches it directly: Resolve
// would come up empty (ResolveNativeId minted a DIFFERENT, unrelated guid
// instead of reading the real one), never a coincidental pass.
TEST_CASE("AssetRegistry classifies .arcdiag by its envelope guid, not a top-level id field", "[project]")
{
    const auto dir = TempDir("diag_classify");

    Arcane::Diag::Envelope env;
    env.guid = Arcane::Guid::Generate();
    env.kind = "hang";
    REQUIRE(Arcane::Diag::WriteFile(env, dir / "x.arcdiag"));

    Arcane::AssetRegistry reg;
    const std::size_t n = reg.ScanContent(dir, "diag");

    CHECK(n == 1);
    const auto mountPath = reg.Resolve(env.guid);
    REQUIRE(mountPath.has_value());
    CHECK(*mountPath == "diag://x.arcdiag");

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// The Project-level half of the same contract: a temp project fixture with
// Saved/Diagnostics/x.arcdiag ALREADY on disk (written via Diag::WriteFile,
// the same call WriteReportImpl makes -- Diagnostics.cpp) scans into the
// registry under diag://x.arcdiag with the envelope's guid, AND the diag://
// scheme itself is mounted (Project::Open, not just AssetRegistry) so
// ResolveAsset can turn that guid back into a real path.
TEST_CASE("Project::Open mounts diag:// and registers an existing .arcdiag by its envelope guid", "[project]")
{
    const auto dir = TempDir("diag_mount_present");

    // Project::Create's own internal Open() runs before Saved/Diagnostics
    // exists -- that first open is not what this test is about; the SECOND
    // Open() below, after the report file lands on disk, is.
    auto created = Arcane::Project::Create(dir, "DiagMountPresent");
    REQUIRE(created.has_value());

    const std::filesystem::path diagDir = dir / "Saved" / "Diagnostics";
    std::filesystem::create_directories(diagDir);

    Arcane::Diag::Envelope env;
    env.guid = Arcane::Guid::Generate();
    env.kind = "gpu-stall";
    REQUIRE(Arcane::Diag::WriteFile(env, diagDir / "x.arcdiag"));

    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());

    CHECK(proj->Mounts().HasMount("diag"));

    const auto mountPath = proj->Registry().Resolve(env.guid);
    REQUIRE(mountPath.has_value());
    CHECK(*mountPath == "diag://x.arcdiag");

    const auto resolved = proj->Mounts().Resolve(*mountPath);
    REQUIRE(resolved.has_value());
    CHECK(resolved->filename() == "x.arcdiag");
    CHECK(std::filesystem::exists(*resolved));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// The negative half of the same binding constraint: a project that has never
// crashed has no Saved/Diagnostics yet, and Open() must stay silent about it
// -- no mount, no scan attempt, no error/failed Open.
TEST_CASE("Project::Open with no Saved/Diagnostics mounts nothing and does not fail", "[project]")
{
    const auto dir = TempDir("diag_mount_absent");

    auto proj = Arcane::Project::Create(dir, "DiagMountAbsent");
    REQUIRE(proj.has_value());   // Create()'s own Open() must still succeed

    CHECK_FALSE(std::filesystem::is_directory(dir / "Saved" / "Diagnostics"));
    CHECK_FALSE(proj->Mounts().HasMount("diag"));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("AssetRegistry::All() is ordered deterministically, not by hash", "[project]")
{
    const auto dir = TempDir("all_deterministic_order");

    // Eight assets whose NAMES sort alphabetically but whose GUIDs deliberately
    // do not. m_byGuid is keyed by Guid, so an unordered_map walk tracks the
    // guid hash and has ~1/40320 odds of coming out name-sorted by luck --
    // which is what makes this test capable of failing against the old code.
    const char* names[] = { "h", "c", "a", "f", "b", "g", "d", "e" };
    const char* guids[] = {
        "11111111-1111-4111-8111-111111111111", "22222222-2222-4222-8222-222222222222",
        "33333333-3333-4333-8333-333333333333", "44444444-4444-4444-8444-444444444444",
        "55555555-5555-4555-8555-555555555555", "66666666-6666-4666-8666-666666666666",
        "77777777-7777-4777-8777-777777777777", "88888888-8888-4888-8888-888888888888",
    };
    for (int i = 0; i < 8; ++i)
        std::ofstream(dir / (std::string(names[i]) + ".arcmat"), std::ios::binary)
            << R"({ "id": ")" << guids[i] << R"(" })";

    Arcane::AssetRegistry reg;
    reg.ScanContent(dir, "game");

    // A genuine mount-path TIE, needed to exercise the Guid tiebreak itself:
    // m_byGuid keys on Guid, not on mount path, so AddContent never rejects
    // two DISTINCT ids that resolve to the identical "<scheme>://<relative>"
    // string -- only a duplicate ID (not a duplicate PATH) is warned-and-kept-
    // first (AddFile, AssetRegistry.cpp). Eight separate single-file content
    // roots, each holding one "tie.arcmat", folded in under the SAME "game"
    // scheme used above: every one of the eight resolves to the identical
    // mount path "game://tie.arcmat", so these eight rows can be placed in
    // order ONLY by the Guid tiebreak -- an implementation that dropped the
    // tiebreak (comparing solely by mount path) could not distinguish them
    // and would leave their relative order to hash-bucket iteration, same
    // 1/8! odds of accidentally already being ascending-guid as the block
    // above relies on for mount path.
    // Digits 0/9/a-f (disjoint from the 1-8 used above, so no id collides with
    // the block above and triggers the UNRELATED duplicate-ID-kept-first path
    // instead of registering all sixteen).
    const auto tieRoot = TempDir("all_deterministic_order_ties");
    const char* tieGuids[] = {
        "ffffffff-ffff-4fff-8fff-ffffffffffff", "99999999-9999-4999-8999-999999999999",
        "cccccccc-cccc-4ccc-8ccc-cccccccccccc", "00000000-0000-4000-8000-000000000000",
        "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee", "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
        "dddddddd-dddd-4ddd-8ddd-dddddddddddd", "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb",
    };
    for (int i = 0; i < 8; ++i)
    {
        const auto sub = tieRoot / std::to_string(i);
        std::filesystem::create_directories(sub);
        std::ofstream(sub / "tie.arcmat", std::ios::binary)
            << R"({ "id": ")" << tieGuids[i] << R"(" })";
    }
    for (int i = 0; i < 8; ++i)
        reg.AddContent(tieRoot / std::to_string(i), "game");

    const auto all = reg.All();
    REQUIRE(all.size() == 16);

    // THE PROPERTY: sortedness over a TOTAL key makes the sequence a function
    // of the content alone -- independent of hash and insertion order -- which
    // is what makes the editor-ui golden image reproducible on any machine.
    // With the mount-path tie above in the mix, this assertion can only pass
    // if the Guid tiebreak is actually applied, not merely present in source.
    CHECK(std::is_sorted(all.begin(), all.end(),
        [](const std::pair<Arcane::Guid, std::string>& a,
           const std::pair<Arcane::Guid, std::string>& b)
        {
            if (a.second != b.second) return a.second < b.second;
            return a.first < b.first;
        }));
}
