// Arcane::AssetRegistry: scan-progress reporting (ScanContent's optional
// callback). The rest of AssetRegistry's behaviour (id resolution, native vs.
// imported-binary routing, mount-path shape) is already covered by
// AssetBrowserTest.cpp/MaterialAssetTest.cpp -- this file is scoped to the
// progress-callback addition only. CPU-only.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Project/AssetRegistry.hpp>

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
