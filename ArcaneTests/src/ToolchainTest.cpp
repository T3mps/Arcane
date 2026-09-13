// Arcane::Toolchain's PURE-ENOUGH halves ([build]): workspace-file discovery
// and the bundled-premake lookup, both over a real temp directory. The
// vswhere-backed lookups (ResolveMsBuild / ResolveDevenv / VsWhere) spawn
// vswhere.exe and are desk-verify territory -- the same "no spawn test"
// split ModuleBuild and RuntimeLaunch draw.

#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Build/Toolchain.hpp>

namespace
{
    namespace fs = std::filesystem;

    // Unique temp dir per SECTION run; removed by the guard so a failing
    // assertion cannot strand files for the next run to trip on.
    struct TempDir
    {
        fs::path path;
        explicit TempDir(const char* tag)
        {
            path = fs::temp_directory_path() /
                   (std::string("arcane_toolchain_") + tag + "_" +
                    std::to_string(static_cast<unsigned>(
                        std::hash<const void*>{}(this))));
            fs::create_directories(path);
        }
        ~TempDir()
        {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };

    void Touch(const fs::path& p)
    {
        fs::create_directories(p.parent_path());
        std::ofstream(p.string()) << "x";
    }
}

TEST_CASE("Toolchain::DiscoverSolution prefers .slnx over .sln, first lexicographic within a bucket",
          "[build]")
{
    TempDir dir("discover");
    Touch(dir.path / "Zeta.sln");
    Touch(dir.path / "beta.slnx");
    Touch(dir.path / "Alpha.slnx");
    Touch(dir.path / "notes.txt");

    CHECK(Arcane::Toolchain::DiscoverSolution(dir.path).filename() == "Alpha.slnx");
}

TEST_CASE("Toolchain::DiscoverSolution falls back to .sln, and to empty when neither exists",
          "[build]")
{
    TempDir dir("fallback");
    SECTION("only a .sln")
    {
        Touch(dir.path / "Game.sln");
        CHECK(Arcane::Toolchain::DiscoverSolution(dir.path).filename() == "Game.sln");
    }
    SECTION("a hand-generated upper-case extension still counts")
    {
        Touch(dir.path / "Game.SLNX");
        CHECK(Arcane::Toolchain::DiscoverSolution(dir.path).filename() == "Game.SLNX");
    }
    SECTION("neither")
    {
        CHECK(Arcane::Toolchain::DiscoverSolution(dir.path).empty());
    }
    SECTION("a directory that does not exist")
    {
        CHECK(Arcane::Toolchain::DiscoverSolution(dir.path / "missing").empty());
    }
    SECTION("non-recursive: a vendor solution one level down is not ours")
    {
        Touch(dir.path / "ThirdParty" / "vendor.slnx");
        CHECK(Arcane::Toolchain::DiscoverSolution(dir.path).empty());
    }
}

TEST_CASE("Toolchain::ResolvePremake returns the SDK's bundled premake, else the PATH name", "[build]")
{
    TempDir sdk("premake");
    SECTION("bundled copy present")
    {
        Touch(sdk.path / "ThirdParty" / "premake5" / "premake5.exe");
        const fs::path found = Arcane::Toolchain::ResolvePremake(sdk.path);
        CHECK(found.filename() == "premake5.exe");
        CHECK(found.is_absolute());
        // lexically_normal'd: no "." / ".." elements survive.
        CHECK(found == found.lexically_normal());
    }
    SECTION("bundled copy absent -> bare name for cmd's own PATH resolution")
    {
        CHECK(Arcane::Toolchain::ResolvePremake(sdk.path) == fs::path("premake5"));
    }
}
