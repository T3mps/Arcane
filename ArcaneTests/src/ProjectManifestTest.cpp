// Arcane::ProjectManifest: parse + validate a .arcproj JSON document. CPU-only.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

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

TEST_CASE("a manifest physics block sets gravity; absent keeps the default", "[project]")
{
    const auto with = Arcane::ProjectManifest::FromJson(nlohmann::json::parse(R"({
        "formatVersion": 2, "name": "T", "engine": { "abi": 28 },
        "physics": { "gravity": [0.0, 12.5] }
    })"));
    REQUIRE(with.has_value());
    CHECK(with->physics.gravity.x == 0.0f);
    CHECK(with->physics.gravity.y == Catch::Approx(12.5f));

    const auto without = Arcane::ProjectManifest::FromJson(nlohmann::json::parse(R"({
        "formatVersion": 1, "name": "T", "engine": { "abi": 28 }
    })"));
    REQUIRE(without.has_value());
    CHECK(without->physics.gravity.y == Catch::Approx(-9.81f));   // +Y up (F4 plan 1 T2)
}

TEST_CASE("formatVersion 2 negates a v1 physics.gravity stamp; v2 and an absent block read as-is", "[project]")
{
    // F4 plan 1 final review, F2a: the Hub stamped "physics": {"gravity":
    // [0, 9.81]} (+Y DOWN) into every manifest it created between 2026-09-11
    // and F4; the engine is +Y up since F4 plan 1 T2. A v1 manifest carrying
    // the block is read with its y negated; a v2 manifest is read verbatim; a
    // v1 manifest WITHOUT the block gets the +Y-up default.
    const auto v1 = Arcane::ProjectManifest::FromJson(nlohmann::json::parse(R"({
        "formatVersion": 1, "name": "T", "engine": { "abi": 32 },
        "physics": { "gravity": [0.0, 9.81] }
    })"));
    REQUIRE(v1.has_value());
    CHECK(v1->formatVersion == 1);
    CHECK(v1->physics.gravity.x == Catch::Approx(0.0f));
    CHECK(v1->physics.gravity.y == Catch::Approx(-9.81f));

    const auto v2 = Arcane::ProjectManifest::FromJson(nlohmann::json::parse(R"({
        "formatVersion": 2, "name": "T", "engine": { "abi": 32 },
        "physics": { "gravity": [0.0, -9.81] }
    })"));
    REQUIRE(v2.has_value());
    CHECK(v2->formatVersion == Arcane::ProjectManifest::kFormatVersion);
    CHECK(v2->physics.gravity.y == Catch::Approx(-9.81f));

    const auto v1Bare = Arcane::ProjectManifest::FromJson(nlohmann::json::parse(R"({
        "formatVersion": 1, "name": "T", "engine": { "abi": 32 }
    })"));
    REQUIRE(v1Bare.has_value());
    CHECK(v1Bare->physics.gravity.x == Catch::Approx(0.0f));
    CHECK(v1Bare->physics.gravity.y == Catch::Approx(-9.81f));
}

TEST_CASE("a malformed physics gravity leaves the default rather than failing the manifest", "[project]")
{
    // Same lenient spirit as splash.backgroundColor: present-but-malformed
    // (wrong type, too short, a non-number element) keeps the default.
    for (const char* body : { R"("gravity": 5)", R"("gravity": [1.0])", R"("gravity": [1.0, "x"])" })
    {
        const auto m = Arcane::ProjectManifest::FromJson(nlohmann::json::parse(
            std::string(R"({"formatVersion": 1, "name": "T", "engine": { "abi": 28 }, "physics": {)") + body + "}}"));
        REQUIRE(m.has_value());
        CHECK(m->physics.gravity.y == Catch::Approx(-9.81f));   // +Y up (F4 plan 1 T2)
    }
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

// sourceDir (2026-09-16, decision record docs/research/2026-09-16-multiplayer-
// shape-and-project-layout.md s5, ruling L3): the directory under Source/ that
// build/arcane.lua compiles into gameModule and the editor's Create C++ Class
// defaults to. ONE field, three readers (build, editor, docs) -- so the rule
// lives here once and the test pins it.
TEST_CASE("ProjectManifest parses sourceDir and defaults it to Source", "[project]")
{
    auto full = Arcane::ProjectManifest::FromJson(nlohmann::json::parse(R"({
        "formatVersion": 1, "name": "X", "engine": { "abi": 4 }, "sourceDir": "Source/Game"
    })"));
    REQUIRE(full.has_value());
    CHECK(full->sourceDir == "Source/Game");

    auto bare = Arcane::ProjectManifest::FromJson(nlohmann::json::parse(R"({
        "formatVersion": 1, "name": "X", "engine": { "abi": 4 }
    })"));
    REQUIRE(bare.has_value());
    CHECK(bare->sourceDir == "Source");

    // A trailing slash is normalised away; "Source" itself is allowed explicitly.
    auto slash = Arcane::ProjectManifest::FromJson(nlohmann::json::parse(R"({
        "formatVersion": 1, "name": "X", "engine": { "abi": 4 }, "sourceDir": "Source/Game/"
    })"));
    REQUIRE(slash.has_value());
    CHECK(slash->sourceDir == "Source/Game");
    auto plain = Arcane::ProjectManifest::FromJson(nlohmann::json::parse(R"({
        "formatVersion": 1, "name": "X", "engine": { "abi": 4 }, "sourceDir": "Source"
    })"));
    REQUIRE(plain.has_value());
    CHECK(plain->sourceDir == "Source");
}

TEST_CASE("ProjectManifest rejects a sourceDir outside Source/ or escaping it", "[project]")
{
    auto reject = [](const char* value)
    {
        const nlohmann::json doc = {
            { "formatVersion", 1 }, { "name", "X" }, { "engine", { { "abi", 4 } } }, { "sourceDir", value }
        };
        return !Arcane::ProjectManifest::FromJson(doc).has_value();
    };
    CHECK(reject("Src"));                 // not under Source/
    CHECK(reject("Sources/Game"));        // prefix trick: "Source" + "s"
    CHECK(reject("Source/../Other"));     // escapes the mount
    CHECK(reject("Source\\Game"));        // backslashes: the manifest is forward-slashed
    CHECK(reject(""));                    // empty: say Source or omit the key
    CHECK(reject("/Source/Game"));        // absolute
    // Wrong TYPE follows the other optionals' contract: type_error -> nullopt.
    CHECK_FALSE(Arcane::ProjectManifest::FromJson(nlohmann::json::parse(R"({
        "formatVersion": 1, "name": "X", "engine": { "abi": 4 }, "sourceDir": 7
    })")).has_value());
}
