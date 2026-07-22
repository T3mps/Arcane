// Arcane::ProjectManifest: parse + validate a .arcproj JSON document. CPU-only.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Project/ProjectManifest.hpp>

#include <Json.hpp>

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
