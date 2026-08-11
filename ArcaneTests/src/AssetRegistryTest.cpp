// Arcane::AssetRegistry: scan-progress reporting (ScanContent's optional
// callback). The rest of AssetRegistry's behaviour (id resolution, native vs.
// imported-binary routing, mount-path shape) is already covered by
// AssetBrowserTest.cpp/MaterialAssetTest.cpp -- this file is scoped to the
// progress-callback addition only. CPU-only.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Project/AssetRegistry.hpp>

#include <Panels/DiagnosticStore.hpp>

#include <filesystem>
#include <fstream>
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
