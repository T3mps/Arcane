// Arcane::ProjectManifest: parse + validate a .arcproj JSON document. CPU-only.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Project/Project.hpp>
#include <Arcane/Project/ProjectManifest.hpp>

#include <Arcane/Plugin/PluginABI.hpp>

#include <Json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

TEST_CASE("ProjectManifest parses a full valid document", "[project]")
{
    const auto doc = nlohmann::json::parse(R"({
        "formatVersion": 1,
        "name": "Aphelyon",
        "description": "test",
        "engine": { "abi": 4 },
        "gameModule": "Aphelyon.dll",
        "plugins": [ { "name": "Sandbox", "enabled": false } ],
        "bootScene": "game://scenes/main.ascene"
    })");

    auto m = Arcane::ProjectManifest::FromJson(doc);
    REQUIRE(m.has_value());
    CHECK(m->formatVersion == 1);
    CHECK(m->name == "Aphelyon");
    CHECK(m->description == "test");
    CHECK(m->engineAbi == 4);
    CHECK(m->gameModule == "Aphelyon.dll");
    REQUIRE(m->plugins.size() == 1);
    CHECK(m->plugins[0].name == "Sandbox");
    CHECK(m->plugins[0].enabled == false);
    CHECK(m->bootScene == "game://scenes/main.ascene");
}

TEST_CASE("ProjectManifest defaults optional fields", "[project]")
{
    const auto doc = nlohmann::json::parse(R"({
        "formatVersion": 1, "name": "Bare", "engine": { "abi": 4 }
    })");
    auto m = Arcane::ProjectManifest::FromJson(doc);
    REQUIRE(m.has_value());
    CHECK(m->description.empty());
    CHECK(m->gameModule.empty());
    CHECK(m->plugins.empty());
    CHECK(m->bootScene.empty());
}

TEST_CASE("ProjectManifest rejects missing required fields", "[project]")
{
    // missing name
    CHECK_FALSE(Arcane::ProjectManifest::FromJson(
        nlohmann::json::parse(R"({ "formatVersion": 1, "engine": { "abi": 4 } })")).has_value());
    // missing engine.abi
    CHECK_FALSE(Arcane::ProjectManifest::FromJson(
        nlohmann::json::parse(R"({ "formatVersion": 1, "name": "X" })")).has_value());
    // formatVersion not > 0
    CHECK_FALSE(Arcane::ProjectManifest::FromJson(
        nlohmann::json::parse(R"({ "formatVersion": 0, "name": "X", "engine": { "abi": 4 } })")).has_value());
}

TEST_CASE("ProjectManifest returns nullopt (never throws) on type-mismatched optional fields", "[project]")
{
    // "description" exists but is the wrong type (number, not string). nlohmann's
    // doc.value(key, default) throws json::type_error in this case -- FromJson must
    // catch that and report nullopt rather than letting the exception escape.
    CHECK_FALSE(Arcane::ProjectManifest::FromJson(
        nlohmann::json::parse(R"({
            "formatVersion": 1, "name": "X", "engine": { "abi": 4 },
            "description": 42
        })")).has_value());

    // A plugin entry with a non-bool "enabled" must likewise yield nullopt, not throw.
    CHECK_FALSE(Arcane::ProjectManifest::FromJson(
        nlohmann::json::parse(R"({
            "formatVersion": 1, "name": "X", "engine": { "abi": 4 },
            "plugins": [ { "name": "Sandbox", "enabled": "yes" } ]
        })")).has_value());
}

TEST_CASE("a manifest with no splash block defaults showProgress to false", "[project]")
{
    // Absent block: engine branding, no progress -- a player does not care that
    // we are scanning asset 412 of 1180. UE reaches the same conclusion.
    const auto m = Arcane::ProjectManifest::FromJson(nlohmann::json::parse(R"({
        "formatVersion": 1, "name": "T", "engine": { "abi": 9 }
    })"));
    REQUIRE(m.has_value());
    CHECK(m->splash.enabled);
    CHECK_FALSE(m->splash.showProgress);
}

TEST_CASE("a manifest splash block round-trips its fields", "[project]")
{
    const auto m = Arcane::ProjectManifest::FromJson(nlohmann::json::parse(R"({
        "formatVersion": 1, "name": "T", "engine": { "abi": 9 },
        "splash": { "enabled": false, "image": "game://B/s.png", "showProgress": true,
                    "minDurationSeconds": 1.5 }
    })"));
    REQUIRE(m.has_value());
    CHECK_FALSE(m->splash.enabled);
    CHECK(m->splash.image == "game://B/s.png");
    CHECK(m->splash.showProgress);
    CHECK(m->splash.minDurationSeconds == 1.5f);
}

TEST_CASE("a splash block present but not an object yields defaults, not manifest failure", "[project]")
{
    const auto m = Arcane::ProjectManifest::FromJson(nlohmann::json::parse(R"({
        "formatVersion": 1, "name": "T", "engine": { "abi": 9 }, "splash": 42
    })"));
    REQUIRE(m.has_value());
    CHECK(m->splash.enabled);          // untouched default
    CHECK_FALSE(m->splash.showProgress);
}

TEST_CASE("a splash block with a wrong-typed field fails the whole manifest, not just that field", "[project]")
{
    // Same contract as description/gameModule/plugins[].enabled above:
    // .value() throws json::type_error on a type mismatch, caught by
    // FromJson's own try/catch -- nullopt, not a silently-defaulted field.
    CHECK_FALSE(Arcane::ProjectManifest::FromJson(nlohmann::json::parse(R"({
        "formatVersion": 1, "name": "T", "engine": { "abi": 9 },
        "splash": { "showProgress": "yes" }
    })")).has_value());
}

TEST_CASE("SetBootScene rewrites only that field, preserving key order", "[project]")
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "arcane_set_boot_scene";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "Content", ec);

    // Hand-written so key ORDER is known and an unknown field is present.
    std::ofstream(dir / "P.arcproj") <<
        R"({"formatVersion":1,"name":"P","description":"d","engine":{"abi":)"
        << static_cast<int>(Arcane::kGamePluginABIVersion)
        << R"(},"gameModule":"","plugins":[],"bootScene":"","zzzFuture":42})";

    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());
    CHECK(proj->Manifest().bootScene.empty());

    const Arcane::Guid id = Arcane::Guid::Generate();
    REQUIRE(proj->SetBootScene(id));
    CHECK(proj->Manifest().bootScene == id.ToString());

    // Re-open from disk: the write actually landed.
    auto again = Arcane::Project::Open(dir);
    REQUIRE(again.has_value());
    CHECK(again->Manifest().bootScene == id.ToString());

    // Unknown keys and their ORDER survive -- a project may carry fields a newer
    // engine added, and pointing at a scene must not reorder or drop them. The
    // trailing "guid" is the ONE addition the engine itself makes: the manifest
    // above predates the field, so the first Open() self-heal stamped it (at the
    // end -- ordered_json appends new keys), and it must hold a valid Guid.
    // Braced so `in` closes before the next SetBootScene call: Windows will not let
    // an atomic replace (ReplaceFileW/rename) swap over a path some other handle
    // still has open, even just for reading.
    {
        std::ifstream in(dir / "P.arcproj");
        const nlohmann::ordered_json doc = nlohmann::ordered_json::parse(in);
        CHECK(doc["zzzFuture"] == 42);
        CHECK(Arcane::Guid::FromString(doc["guid"].get<std::string>()).has_value());
        std::vector<std::string> keys;
        for (auto it = doc.begin(); it != doc.end(); ++it) keys.push_back(it.key());
        CHECK(keys == std::vector<std::string>{"formatVersion", "name", "description", "engine",
                                               "gameModule", "plugins", "bootScene", "zzzFuture",
                                               "guid"});
    }

    // A nil Guid clears the field rather than writing "00000000-...".
    REQUIRE(proj->SetBootScene(Arcane::Guid{}));
    CHECK(proj->Manifest().bootScene.empty());

    fs::remove_all(dir, ec);
}

TEST_CASE("RestampEngineAbi rewrites the nested stamp, preserving engine siblings and key order", "[project]")
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "arcane_restamp_abi";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "Content", ec);

    // A STALE stamp plus a sibling inside engine{}: the nested edit must
    // replace abi alone, not rebuild the engine object around it. zzzFuture
    // proves top-level unknowns survive, same as the SetBootScene case.
    std::ofstream(dir / "P.arcproj") <<
        R"({"formatVersion":1,"name":"P","engine":{"abi":8,"zzzEngineFuture":true},)"
        R"("gameModule":"","plugins":[],"bootScene":"","zzzFuture":42})";

    auto proj = Arcane::Project::Open(dir);
    REQUIRE(proj.has_value());
    CHECK(proj->Manifest().engineAbi == 8);

    REQUIRE(proj->RestampEngineAbi(12345));
    CHECK(proj->Manifest().engineAbi == 12345);   // mirrored in memory

    // Re-open from disk: the write landed, and only abi moved.
    auto again = Arcane::Project::Open(dir);
    REQUIRE(again.has_value());
    CHECK(again->Manifest().engineAbi == 12345);
    {
        std::ifstream in(dir / "P.arcproj");
        const nlohmann::ordered_json doc = nlohmann::ordered_json::parse(in);
        CHECK(doc["engine"]["abi"] == 12345);
        CHECK(doc["engine"]["zzzEngineFuture"] == true);
        CHECK(doc["zzzFuture"] == 42);
        std::vector<std::string> keys;
        for (auto it = doc.begin(); it != doc.end(); ++it) keys.push_back(it.key());
        CHECK(keys == std::vector<std::string>{"formatVersion", "name", "engine", "gameModule",
                                               "plugins", "bootScene", "zzzFuture", "guid"});
    }

    fs::remove_all(dir, ec);
}
