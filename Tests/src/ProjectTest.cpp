// Arcane::Project: open/create a project + resolve assets through its mounts. CPU-only.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Project/Project.hpp>

#include <Arcane/Plugin/PluginABI.hpp>

#include <Json.hpp>

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

TEST_CASE("Project::RegisterAsset registers editor-created files incrementally", "[project]")
{
    const auto dir = TempDir("register_asset");
    WriteFile(dir / "Game.arcproj",
              R"({ "formatVersion": 1, "name": "Game", "engine": { "abi": 5 } })");
    std::filesystem::create_directories(dir / "Content");

    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());
    CHECK(proj->Registry().Count() == 0);   // empty Content at open

    // A material created AFTER the open-time scan (the editor's New Material).
    WriteFile(dir / "Content" / "mats" / "new.arcmat",
              R"({ "id": "e7e0c1de-4444-4555-8666-777788889999", "snippet": "x" })");
    const auto g = proj->RegisterAsset(dir / "Content" / "mats" / "new.arcmat");
    REQUIRE(g.has_value());
    CHECK(g->ToString() == "e7e0c1de-4444-4555-8666-777788889999");
    CHECK(proj->Registry().Count() == 1);

    // Immediately resolvable (browser + GUID loads see it now, not on reopen).
    const auto p = proj->ResolveAsset(Arcane::AssetId::FromGuid(*g));
    REQUIRE(p.has_value());
    CHECK(*p == dir / "Content" / "mats" / "new.arcmat");

    // Idempotent re-registration (the document Save path).
    CHECK(proj->RegisterAsset(dir / "Content" / "mats" / "new.arcmat").has_value());
    CHECK(proj->Registry().Count() == 1);

    // Missing id: minted and written back, then registered.
    WriteFile(dir / "Content" / "noid.arcmat", R"({ "snippet": "y" })");
    const auto minted = proj->RegisterAsset(dir / "Content" / "noid.arcmat");
    REQUIRE(minted.has_value());
    CHECK(minted->IsValid());
    CHECK(proj->Registry().Count() == 2);

    // Outside every content root -> refused (warn), not registered.
    WriteFile(dir / "outside.arcmat", R"({ "id": "f8e0c1de-5555-4666-8777-888899990000", "snippet": "z" })");
    CHECK_FALSE(proj->RegisterAsset(dir / "outside.arcmat").has_value());
    // Untracked extension -> refused.
    WriteFile(dir / "Content" / "notes.txt", "hello");
    CHECK_FALSE(proj->RegisterAsset(dir / "Content" / "notes.txt").has_value());
    CHECK(proj->Registry().Count() == 2);
}

TEST_CASE("Project::Open mints a .meta sidecar for an imported binary original", "[project]")
{
    const auto dir = TempDir("meta_mint");
    WriteFile(dir / "Game.arcproj",
              R"({ "formatVersion": 1, "name": "Game", "engine": { "abi": 4 } })");
    // A binary original (extension routes it to the sidecar path) with NO .meta yet ->
    // auto-import mints one on Open. Contents are never parsed for binaries.
    std::filesystem::create_directories(dir / "Content");
    WriteFile(dir / "Content" / "textures" / "hero.png", "not-really-a-png");

    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());

    // The sidecar was written beside the original: hero.png -> hero.png.meta (appended,
    // not extension-replaced, so "a.png"/"a.wav" would not collide on "a.meta").
    const auto meta = dir / "Content" / "textures" / "hero.png.meta";
    REQUIRE(std::filesystem::is_regular_file(meta));

    // The minted guid resolves back to the ORIGINAL file, not the .meta.
    std::ifstream in(meta, std::ios::binary);
    auto sidecar = nlohmann::json::parse(in, nullptr, /*allow_exceptions*/ false);
    REQUIRE(sidecar.is_object());
    const auto g = Arcane::Guid::FromString(sidecar.value("guid", std::string{}));
    REQUIRE(g.has_value());
    REQUIRE(g->IsValid());

    auto p = proj->ResolveAsset(Arcane::AssetId::FromGuid(*g));
    REQUIRE(p.has_value());
    CHECK(*p == dir / "Content" / "textures" / "hero.png");

    // Re-opening is stable: the persisted guid is reused, not regenerated.
    auto proj2 = Arcane::Project::Open(dir);
    REQUIRE(proj2.has_value());
    auto p2 = proj2->ResolveAsset(Arcane::AssetId::FromGuid(*g));
    REQUIRE(p2.has_value());
    CHECK(*p2 == dir / "Content" / "textures" / "hero.png");
}

TEST_CASE("Project::Open reads an existing .meta sidecar's guid", "[project]")
{
    const auto dir = TempDir("meta_existing");
    WriteFile(dir / "Game.arcproj",
              R"({ "formatVersion": 1, "name": "Game", "engine": { "abi": 4 } })");
    std::filesystem::create_directories(dir / "Content");
    WriteFile(dir / "Content" / "music.wav", "RIFF-WAVE-bytes");
    WriteFile(dir / "Content" / "music.wav.meta",
              R"({ "guid": "b6f1d2ef-2222-4333-8444-555566667777", "version": 1 })");

    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());

    const auto g = Arcane::Guid::FromString("b6f1d2ef-2222-4333-8444-555566667777");
    REQUIRE(g.has_value());
    auto p = proj->ResolveAsset(Arcane::AssetId::FromGuid(*g));
    REQUIRE(p.has_value());
    CHECK(*p == dir / "Content" / "music.wav");
}

TEST_CASE("Project::Open mounts + registers an enabled project plugin", "[project]")
{
    const auto dir = TempDir("plugin_enabled");
    WriteFile(dir / "Game.arcproj",
              R"({ "formatVersion": 1, "name": "Game", "engine": { "abi": 5 },)"
              R"( "plugins": [ { "name": "fx", "enabled": true } ] })");
    // The plugin: a <name>.arcplugin descriptor (same shape as .arcproj) + a content asset.
    WriteFile(dir / "Plugins" / "fx" / "fx.arcplugin",
              R"({ "formatVersion": 1, "name": "fx", "engine": { "abi": 5 } })");
    WriteFile(dir / "Plugins" / "fx" / "Content" / "spark.json",
              R"({ "id": "c7a2e3f0-3333-4444-8555-666677778888", "type": "vfx" })");

    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());
    CHECK(proj->Mounts().HasMount("plugin/fx"));

    // The plugin asset resolves by its Guid, exactly like a game asset -> Plugins/fx/Content.
    const auto g = Arcane::Guid::FromString("c7a2e3f0-3333-4444-8555-666677778888");
    REQUIRE(g.has_value());
    auto p = proj->ResolveAsset(Arcane::AssetId::FromGuid(*g));
    REQUIRE(p.has_value());
    CHECK(*p == dir / "Plugins" / "fx" / "Content" / "spark.json");
}

TEST_CASE("Project::Open skips a disabled plugin", "[project]")
{
    const auto dir = TempDir("plugin_disabled");
    WriteFile(dir / "Game.arcproj",
              R"({ "formatVersion": 1, "name": "Game", "engine": { "abi": 5 },)"
              R"( "plugins": [ { "name": "fx", "enabled": false } ] })");
    WriteFile(dir / "Plugins" / "fx" / "fx.arcplugin",
              R"({ "formatVersion": 1, "name": "fx", "engine": { "abi": 5 } })");
    WriteFile(dir / "Plugins" / "fx" / "Content" / "spark.json",
              R"({ "id": "c7a2e3f0-3333-4444-8555-666677778888", "type": "vfx" })");

    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());
    CHECK_FALSE(proj->Mounts().HasMount("plugin/fx"));
    const auto g = Arcane::Guid::FromString("c7a2e3f0-3333-4444-8555-666677778888");
    REQUIRE(g.has_value());
    CHECK_FALSE(proj->ResolveAsset(Arcane::AssetId::FromGuid(*g)).has_value());
}

TEST_CASE("Project::Open skips an enabled plugin with no descriptor but still opens", "[project]")
{
    const auto dir = TempDir("plugin_no_descriptor");
    WriteFile(dir / "Game.arcproj",
              R"({ "formatVersion": 1, "name": "Game", "engine": { "abi": 5 },)"
              R"( "plugins": [ { "name": "ghost", "enabled": true } ] })");
    // Plugins/ghost/ has content but NO ghost.arcplugin descriptor -> the plugin is
    // disabled, but the project still opens.
    WriteFile(dir / "Plugins" / "ghost" / "Content" / "x.json",
              R"({ "id": "d8b3f401-4444-4555-8666-777788889999" })");

    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());
    CHECK_FALSE(proj->Mounts().HasMount("plugin/ghost"));
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

    // Born with its durable identity: a valid guid, stamped at create time so
    // the Open() self-heal never has cause to rewrite a fresh project.
    CHECK(Arcane::Guid::FromString(proj->Manifest().guid).has_value());
}

TEST_CASE("Project::Open self-heals a manifest without a guid", "[project]")
{
    const auto dir = TempDir("guid_heal");
    const auto file = dir / "Legacy.arcproj";
    WriteFile(file, R"({ "formatVersion": 1, "name": "Legacy", "engine": { "abi": 4 } })");

    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());
    // The in-memory manifest and the FILE both carry the stamped guid -- the
    // Hub reads the file, so an in-memory-only guid would be invisible to it.
    REQUIRE(Arcane::Guid::FromString(proj->Manifest().guid).has_value());
    std::ifstream in(file, std::ios::binary);
    const auto doc = nlohmann::json::parse(in);
    CHECK(doc["guid"].get<std::string>() == proj->Manifest().guid);
    // The rewrite touched ONE field; the rest of the manifest is intact.
    CHECK(doc["name"] == "Legacy");
    CHECK(doc["formatVersion"] == 1);
    CHECK(doc["engine"]["abi"] == 4);
}

TEST_CASE("Project::Open keeps an existing guid and is stable across opens", "[project]")
{
    const auto dir = TempDir("guid_stable");
    const char* kGuid = "a5e0c1de-1111-4222-8333-444455556666";
    WriteFile(dir / "G.arcproj",
              R"({ "formatVersion": 1, "name": "G", "engine": { "abi": 4 }, "guid": ")"
                  + std::string(kGuid) + R"(" })");

    auto first = Arcane::Project::Open(dir);
    REQUIRE(first.has_value());
    CHECK(first->Manifest().guid == kGuid);

    // A second open must see the SAME identity -- re-stamping per open would
    // make the guid useless as a durable key.
    auto second = Arcane::Project::Open(dir);
    REQUIRE(second.has_value());
    CHECK(second->Manifest().guid == kGuid);
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

TEST_CASE("EditorLock JSON round-trips and rejects malformed shapes", "[project]")
{
    // MIRRORED FORMAT with the Hub's editorlock.rs -- these cases pin it.
    const Arcane::EditorLock::Info info{4242, 0x1234'5678'9ABC'DEF0ull};
    const auto back = Arcane::EditorLock::Parse(Arcane::EditorLock::ToJson(info));
    REQUIRE(back.has_value());
    CHECK(back->pid == 4242);
    CHECK(back->start == 0x1234'5678'9ABC'DEF0ull);

    CHECK_FALSE(Arcane::EditorLock::Parse("not json").has_value());
    CHECK_FALSE(Arcane::EditorLock::Parse("{}").has_value());
    CHECK_FALSE(Arcane::EditorLock::Parse(R"({"pid":0})").has_value());
    CHECK_FALSE(Arcane::EditorLock::Parse(R"({"pid":-3})").has_value());
    // A missing start degrades to 0 ("no time recorded"), not a refusal.
    const auto noStart = Arcane::EditorLock::Parse(R"({"pid":7})");
    REQUIRE(noStart.has_value());
    CHECK(noStart->start == 0);
}

TEST_CASE("EditorLock: a live lock names this process; a dead one is ignored", "[project]")
{
    const auto dir = TempDir("editor_lock");

    // No lock file -> not running.
    CHECK_FALSE(Arcane::EditorLock::ReadLive(dir).has_value());

    // Written by THIS process -> live, with our own pid.
    Arcane::EditorLock::Write(dir);
    const auto live = Arcane::EditorLock::ReadLive(dir);
    REQUIRE(live.has_value());
    CHECK(*live == Arcane::EditorLock::Self().pid);

    // The staleness defeat: same pid, WRONG creation time -- the process the
    // lock described no longer exists, so it must read as not running. This
    // is the exact false-"already open" failure Unity's lockfile is hated
    // for, pinned here so it can never ship.
    auto stale = Arcane::EditorLock::Self();
    stale.start ^= 1;
    WriteFile(Arcane::EditorLock::FileFor(dir), Arcane::EditorLock::ToJson(stale));
    CHECK_FALSE(Arcane::EditorLock::ReadLive(dir).has_value());

    // Clear removes the file; a cleared project is not running.
    Arcane::EditorLock::Write(dir);
    Arcane::EditorLock::Clear(dir);
    CHECK_FALSE(Arcane::EditorLock::ReadLive(dir).has_value());
    CHECK_FALSE(std::filesystem::exists(Arcane::EditorLock::FileFor(dir)));
}
