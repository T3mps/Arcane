#include <catch2/catch_test_macros.hpp>

#include <Arcane/Input/InputActionAsset.hpp>
#include <Arcane/Input/InputActions.hpp>

namespace
{
    constexpr uint32_t kSpace = 32;

    Arcane::InputActionAsset FixedAsset()
    {
        const auto doc = nlohmann::json::parse(R"JSON({
            "version": 1,
            "id": "11111111-1111-4111-8111-111111111111",
            "defaultMap": "22222222-2222-4222-8222-222222222222",
            "controlSchemes": [],
            "actionMaps": [{
                "id": "22222222-2222-4222-8222-222222222222",
                "name": "Player",
                "actions": [{
                    "id": "33333333-3333-4333-8333-333333333333",
                    "name": "Jump", "type": "Button",
                    "bindings": [{
                        "id": "44444444-4444-4444-8444-444444444444",
                        "path": "<Keyboard>/space"
                    }]
                }]
            }]
        })JSON");
        return *Arcane::InputActionAsset::FromJson(doc);
    }

    Arcane::Guid JumpId()
    {
        return *Arcane::Guid::FromString("33333333-3333-4333-8333-333333333333");
    }

    std::unique_ptr<Arcane::InputActions> ReadyInput()
    {
        auto input = Arcane::InputActions::Create();
        REQUIRE(input->LoadAsset(FixedAsset()));
        input->SetBaseContext("Player");
        return input;
    }
}

TEST_CASE("input fixed: press survives a render frame with no fixed step", "[input][fixed]")
{
    auto input = ReadyInput();
    Arcane::InputSnapshot pressed;
    pressed.AddKeycode(kSpace);
    input->Update(1.0 / 60.0, pressed);
    input->Update(1.0 / 60.0, pressed);

    input->BeginFixedStep();
    CHECK(input->PressedThisFixedStep(JumpId()));
    CHECK_FALSE(input->ReleasedThisFixedStep(JumpId()));
    REQUIRE_FALSE(input->TransitionsThisFixedStep().empty());
    CHECK(input->TransitionsThisFixedStep()[0].action == JumpId());
    CHECK(input->TransitionsThisFixedStep()[0].phase == Arcane::InputActionPhase::Started);
}

TEST_CASE("input fixed: press and release before one fixed step retain order", "[input][fixed]")
{
    auto input = ReadyInput();
    Arcane::InputSnapshot pressed;
    pressed.AddKeycode(kSpace);
    input->Update(1.0 / 60.0, pressed);
    input->Update(1.0 / 60.0, Arcane::InputSnapshot{});

    input->BeginFixedStep();
    CHECK(input->PressedThisFixedStep(JumpId()));
    CHECK(input->ReleasedThisFixedStep(JumpId()));
    const auto transitions = input->TransitionsThisFixedStep();
    REQUIRE(transitions.size() >= 3);
    CHECK(transitions.front().phase == Arcane::InputActionPhase::Started);
    CHECK(transitions.back().phase == Arcane::InputActionPhase::Canceled);
    CHECK(transitions.front().sampleIndex < transitions.back().sampleIndex);
}

TEST_CASE("input fixed: edge is delivered only to the first fixed step", "[input][fixed]")
{
    auto input = ReadyInput();
    Arcane::InputSnapshot pressed;
    pressed.AddKeycode(kSpace);
    input->Update(1.0 / 60.0, pressed);

    input->BeginFixedStep();
    REQUIRE(input->PressedThisFixedStep(JumpId()));
    input->BeginFixedStep();
    CHECK_FALSE(input->PressedThisFixedStep(JumpId()));
    CHECK_FALSE(input->ReleasedThisFixedStep(JumpId()));
    CHECK(input->TransitionsThisFixedStep().empty());
    CHECK(input->Value(JumpId()).down);
}
