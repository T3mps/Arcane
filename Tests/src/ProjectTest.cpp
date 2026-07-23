// Arcane::Project: open/create a project + resolve assets through its mounts. CPU-only.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Project/Project.hpp>

#include <Arcane/Plugin/PluginABI.hpp>

#include <filesystem>
#include <fstream>

namespace
{
    // A unique temp dir for a test, cleaned before use. (Date/random are unavailable
    // in some sandboxes; a fixed per-test name + remove_all is deterministic.)
    std::filesystem::path TempDir(const char* leaf)
    {
        std::filesystem::path d = std::filesystem::temp_directory_path() / "arcane_project_test" / leaf;
        std::error_code ec;
        std::filesystem::remove_all(d, ec);
        std::filesystem::create_directories(d);
        return d;
    }

    void WriteFile(const std::filesystem::path& p, const std::string& text)
    {
        std::filesystem::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << text;
    }
}

TEST_CASE("Project::Open loads a folder's manifest and mounts game://", "[project]")
{
    const auto dir = TempDir("open_ok");
    WriteFile(dir / "Aphelyon.arcproj",
              R"({ "formatVersion": 1, "name": "Aphelyon", "engine": { "abi": 4 } })");
    std::filesystem::create_directories(dir / "Content");

    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());
    CHECK(proj->Manifest().name == "Aphelyon");
    CHECK(proj->Root() == dir);
    CHECK(proj->Mounts().HasMount("game"));

    auto p = proj->Mounts().Resolve("game://x.png");
    REQUIRE(p.has_value());
    CHECK(*p == dir / "Content" / "x.png");
}

TEST_CASE("Project::Open fails on missing or ambiguous manifest", "[project]")
{
    const auto none = TempDir("open_none");
    CHECK_FALSE(Arcane::Project::Open(none).has_value());   // no .arcproj

    const auto many = TempDir("open_many");
    WriteFile(many / "A.arcproj", R"({ "formatVersion": 1, "name": "A", "engine": { "abi": 4 } })");
    WriteFile(many / "B.arcproj", R"({ "formatVersion": 1, "name": "B", "engine": { "abi": 4 } })");
    CHECK_FALSE(Arcane::Project::Open(many).has_value());   // ambiguous
}

TEST_CASE("Project::Open accepts a direct .arcproj file path", "[project]")
{
    const auto dir = TempDir("open_direct_file");
    const auto manifestFile = dir / "Direct.arcproj";
    WriteFile(manifestFile,
              R"({ "formatVersion": 1, "name": "Direct", "engine": { "abi": 4 } })");
    std::filesystem::create_directories(dir / "Content");

    auto proj = Arcane::Project::Open(manifestFile);
    REQUIRE(proj.has_value());
    CHECK(proj->Manifest().name == "Direct");
    CHECK(proj->Root() == dir);
    CHECK(proj->Mounts().HasMount("game"));

    auto p = proj->Mounts().Resolve("game://x.png");
    REQUIRE(p.has_value());
    CHECK(*p == dir / "Content" / "x.png");
}

TEST_CASE("Project::Open fails on a schema-invalid manifest", "[project]")
{
    const auto dir = TempDir("open_schema_invalid");
    // Well-formed JSON, but missing the required "name" field.
    WriteFile(dir / "Bad.arcproj", R"({ "formatVersion": 1, "engine": { "abi": 4 } })");

    CHECK_FALSE(Arcane::Project::Open(dir).has_value());
}

TEST_CASE("Project::ResolveAsset maps a registered Guid to a file", "[project]")
{
    const auto dir = TempDir("resolve");
    WriteFile(dir / "Game.arcproj",
              R"({ "formatVersion": 1, "name": "Game", "engine": { "abi": 4 } })");
    // A native asset with a stable embedded id -> the registry keys it on Open.
    std::filesystem::create_directories(dir / "Content");
    WriteFile(dir / "Content" / "thing.json",
              R"({ "id": "a5e0c1de-1111-4222-8333-444455556666", "type": "t" })");

    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());

    const auto g = Arcane::Guid::FromString("a5e0c1de-1111-4222-8333-444455556666");
    REQUIRE(g.has_value());
    auto p = proj->ResolveAsset(Arcane::AssetId::FromGuid(*g));
    REQUIRE(p.has_value());
    CHECK(*p == dir / "Content" / "thing.json");

    // A valid-but-unregistered Guid -> no resolution.
    CHECK_FALSE(proj->ResolveAsset(Arcane::AssetId::FromGuid(Arcane::Guid::Generate())).has_value());
    // Invalid id -> no resolution.
    CHECK_FALSE(proj->ResolveAsset(Arcane::AssetId{}).has_value());
}

TEST_CASE("Project::Create scaffolds the skeleton, manifest, and .gitignore", "[project]")
{
    const auto dir = TempDir("create");

    auto proj = Arcane::Project::Create(dir, "Aphelyon");
    REQUIRE(proj.has_value());

    // Skeleton dirs.
    CHECK(std::filesystem::is_directory(dir / "Source"));
    CHECK(std::filesystem::is_directory(dir / "Content"));
    CHECK(std::filesystem::is_directory(dir / "Config"));
    CHECK(std::filesystem::is_directory(dir / "Plugins"));
    // Manifest + gitignore.
    CHECK(std::filesystem::is_regular_file(dir / "Aphelyon.arcproj"));
    CHECK(std::filesystem::is_regular_file(dir / ".gitignore"));

    // The created project is valid and re-openable.
    CHECK(proj->Manifest().name == "Aphelyon");
    CHECK(proj->Manifest().engineAbi == static_cast<int>(Arcane::kGamePluginABIVersion));
    CHECK(Arcane::Project::Open(dir).has_value());
}

TEST_CASE("Project::Create refuses a non-empty target directory", "[project]")
{
    const auto dir = TempDir("create_nonempty");
    WriteFile(dir / "existing.txt", "x");
    CHECK_FALSE(Arcane::Project::Create(dir, "X").has_value());
}

TEST_CASE("Project::Create refuses a name containing a path separator", "[project]")
{
    const auto dir = TempDir("create_bad_name");
    CHECK_FALSE(Arcane::Project::Create(dir, "a/b").has_value());

    // The bad name must be rejected before any skeleton is created on disk.
    CHECK(std::filesystem::is_empty(dir));
}
