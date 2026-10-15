#include <catch2/catch_test_macros.hpp>

#include <Arcane/Input/InputActionAsset.hpp>

#include <Json.hpp>

#include <string>
#include <vector>

namespace
{
    nlohmann::json PlayerAsset()
    {
        return nlohmann::json::parse(R"JSON({
            "version": 1,
            "id": "11111111-1111-4111-8111-111111111111",
            "defaultMap": "22222222-2222-4222-8222-222222222222",
            "topExtension": { "owner": "example" },
            "controlSchemes": [
                { "id": "44444444-4444-4444-8444-444444444444",
                  "name": "KeyboardMouse", "bindingGroup": "KeyboardMouse",
                  "schemeExtension": 7 },
                { "id": "55555555-5555-4555-8555-555555555555",
                  "name": "Gamepad", "bindingGroup": "Gamepad" }
            ],
            "actionMaps": [
                { "id": "22222222-2222-4222-8222-222222222222",
                  "name": "Player", "blocking": false, "priority": 3,
                  "mapExtension": "keep",
                  "actions": [
                    { "id": "66666666-6666-4666-8666-666666666666",
                      "name": "Move", "type": "Axis2D",
                      "actionExtension": true,
                      "bindings": [
                        { "id": "88888888-8888-4888-8888-888888888888",
                          "composite": "2DVector",
                          "processors": ["normalizeVector2"],
                          "interactions": ["press"],
                          "groups": ["KeyboardMouse"],
                          "bindingExtension": { "color": "blue" },
                          "parts": [
                            { "id": "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb",
                              "name": "up", "path": "<Keyboard>/w",
                              "groups": ["KeyboardMouse"], "partExtension": 1 },
                            { "id": "cccccccc-cccc-4ccc-8ccc-cccccccccccc",
                              "name": "down", "path": "<Keyboard>/s" },
                            { "id": "dddddddd-dddd-4ddd-8ddd-dddddddddddd",
                              "name": "left", "path": "<Keyboard>/a" },
                            { "id": "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee",
                              "name": "right", "path": "<Keyboard>/d" }
                          ] },
                        { "id": "99999999-9999-4999-8999-999999999999",
                          "path": "<Gamepad>/leftStick",
                          "processors": ["deadzone(min=0.125,max=0.925)"],
                          "groups": ["Gamepad"] }
                      ] },
                    { "id": "77777777-7777-4777-8777-777777777777",
                      "name": "Jump", "type": "Button",
                      "bindings": [
                        { "id": "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
                          "path": "<Keyboard>/space",
                          "interactions": ["tap(duration=0.2)"],
                          "groups": ["KeyboardMouse"] }
                      ] }
                  ] },
                { "id": "33333333-3333-4333-8333-333333333333",
                  "name": "Menu", "blocking": true, "actions": [] }
            ]
        })JSON");
    }
}

TEST_CASE("input asset: parses and round-trips a version 1 document", "[input][asset]")
{
    const auto source = PlayerAsset();
    std::string error;
    auto asset = Arcane::InputActionAsset::FromJson(source, &error);
    INFO(error);
    REQUIRE(asset.has_value());

    CHECK(asset->id.ToString() == source["id"].get<std::string>());
    REQUIRE(asset->defaultMap.has_value());
    CHECK(asset->defaultMap->ToString() == source["defaultMap"].get<std::string>());
    REQUIRE(asset->actionMaps.size() == 2);
    CHECK(asset->actionMaps[0].name == "Player");
    CHECK(asset->actionMaps[1].name == "Menu");
    CHECK(asset->actionMaps[1].blocking);
    CHECK(asset->actionMaps[0].priority == 3);
    REQUIRE(asset->actionMaps[0].actions.size() == 2);
    CHECK(asset->actionMaps[0].actions[0].name == "Move");
    CHECK(asset->actionMaps[0].actions[1].name == "Jump");
    CHECK(asset->actionMaps[0].actions[0].type == Arcane::InputActionType::Axis2D);
    CHECK(asset->actionMaps[0].actions[1].type == Arcane::InputActionType::Button);
    REQUIRE(asset->actionMaps[0].actions[0].bindings.size() == 2);
    const auto& composite = asset->actionMaps[0].actions[0].bindings[0];
    CHECK(composite.id.ToString() == "88888888-8888-4888-8888-888888888888");
    CHECK(composite.composite == "2DVector");
    CHECK(composite.processors == std::vector<std::string>{"normalizeVector2"});
    CHECK(composite.interactions == std::vector<std::string>{"press"});
    REQUIRE(composite.parts.size() == 4);
    CHECK(composite.parts[0].name == "up");
    CHECK(composite.parts[0].path == "<Keyboard>/w");
    CHECK(composite.parts[3].name == "right");
    CHECK(composite.parts[3].id.ToString() == "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee");
    CHECK(asset->actionMaps[0].actions[0].bindings[1].path == "<Gamepad>/leftStick");
    CHECK(asset->actionMaps[0].actions[0].bindings[1].groups == std::vector<std::string>{"Gamepad"});
    REQUIRE(asset->controlSchemes.size() == 2);
    CHECK(asset->controlSchemes[0].bindingGroup == "KeyboardMouse");
    CHECK(asset->controlSchemes[1].name == "Gamepad");
    CHECK(asset->ToJson() == source);
}

TEST_CASE("input asset: rejects duplicate or missing stable IDs", "[input][asset]")
{
    std::string error;
    auto duplicate = PlayerAsset();
    duplicate["actionMaps"][0]["actions"][1]["id"] = duplicate["actionMaps"][0]["actions"][0]["id"];
    CHECK_FALSE(Arcane::InputActionAsset::FromJson(duplicate, &error));
    CHECK(error.find("Jump") != std::string::npos);

    auto missing = PlayerAsset();
    missing["actionMaps"][0]["actions"][0]["bindings"][0].erase("id");
    CHECK_FALSE(Arcane::InputActionAsset::FromJson(missing, &error));
    CHECK(error.find("Move") != std::string::npos);

    auto nil = PlayerAsset();
    nil["actionMaps"][0]["id"] = "00000000-0000-0000-0000-000000000000";
    CHECK_FALSE(Arcane::InputActionAsset::FromJson(nil, &error));
    CHECK(error.find("Player") != std::string::npos);
}

TEST_CASE("input asset: validates the default map reference", "[input][asset]")
{
    std::string error;
    auto missing = PlayerAsset();
    missing.erase("defaultMap");
    CHECK_FALSE(Arcane::InputActionAsset::FromJson(missing, &error));
    CHECK(error.find("defaultMap") != std::string::npos);

    auto stale = PlayerAsset();
    stale["defaultMap"] = "ffffffff-ffff-4fff-8fff-ffffffffffff";
    CHECK_FALSE(Arcane::InputActionAsset::FromJson(stale, &error));
    CHECK(error.find("defaultMap") != std::string::npos);

    auto empty = Arcane::InputActionAsset::CreateDefault();
    CHECK(empty.id.IsValid());
    const auto emptyJson = empty.ToJson();
    CHECK(emptyJson["version"] == 1);
    CHECK(emptyJson["actionMaps"].empty());
    CHECK(emptyJson["controlSchemes"].empty());
    CHECK(Arcane::InputActionAsset::FromJson(emptyJson, &error).has_value());
}

TEST_CASE("input asset: rejects invalid composite references and scheme groups", "[input][asset]")
{
    std::string error;
    auto wrongPart = PlayerAsset();
    wrongPart["actionMaps"][0]["actions"][0]["bindings"][0]["parts"][0]["name"] = "forward";
    CHECK_FALSE(Arcane::InputActionAsset::FromJson(wrongPart, &error));
    CHECK(error.find("Move") != std::string::npos);

    auto missingParts = PlayerAsset();
    missingParts["actionMaps"][0]["actions"][0]["bindings"][0]["parts"] = nlohmann::json::array();
    CHECK_FALSE(Arcane::InputActionAsset::FromJson(missingParts, &error));
    CHECK(error.find("Move") != std::string::npos);

    auto unknownGroup = PlayerAsset();
    unknownGroup["actionMaps"][0]["actions"][0]["bindings"][1]["groups"] = {"MissingScheme"};
    CHECK_FALSE(Arcane::InputActionAsset::FromJson(unknownGroup, &error));
    CHECK(error.find("Move") != std::string::npos);
}

TEST_CASE("input asset: rejects unsupported versions", "[input][asset]")
{
    std::string error;
    auto future = PlayerAsset();
    future["version"] = 2;
    CHECK_FALSE(Arcane::InputActionAsset::FromJson(future, &error));
    CHECK(error.find("version") != std::string::npos);

    CHECK_FALSE(Arcane::InputActionAsset::FromJson(nlohmann::json::array(), &error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("input asset: preserves unknown fields", "[input][asset]")
{
    std::string error;
    auto asset = Arcane::InputActionAsset::FromJson(PlayerAsset(), &error);
    INFO(error);
    REQUIRE(asset.has_value());
    asset->actionMaps[0].name = "RenamedPlayer";
    const auto saved = asset->ToJson();
    CHECK(saved["actionMaps"][0]["name"] == "RenamedPlayer");
    CHECK(saved["topExtension"]["owner"] == "example");
    CHECK(saved["controlSchemes"][0]["schemeExtension"] == 7);
    CHECK(saved["actionMaps"][0]["mapExtension"] == "keep");
    CHECK(saved["actionMaps"][0]["actions"][0]["actionExtension"] == true);
    CHECK(saved["actionMaps"][0]["actions"][0]["bindings"][0]["bindingExtension"]["color"] == "blue");
    CHECK(saved["actionMaps"][0]["actions"][0]["bindings"][0]["parts"][0]["partExtension"] == 1);
}
