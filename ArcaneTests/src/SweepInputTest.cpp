#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Input/InputSettings.hpp>

#include <Json.hpp>
using namespace Arcane;
TEST_CASE("sweep: input defaults are the pre-sweep literals and deterministic", "[sweep][input]")
{
    CHECK(Test::SameBits(InputSettings{}.pressThreshold, 0.5f));
    CHECK(Test::SameBits(InputSettings{}.holdSeconds, 0.4f));
    CHECK(Test::SameBits(InputSettings{}.tapSeconds, 0.2f));
    CHECK(InputSettings{}.maxQueuedTransitions == 256u);
    CHECK(InputSettings{}.baseContext == "demo");
    CHECK(Test::SameBits(InputDeadzoneSettings{}.defaultMin, 0.125f));
    CHECK(Test::SameBits(InputDeadzoneSettings{}.defaultMax, 0.925f));
    CHECK(Test::SameBits(InputRebindSettings{}.axisThreshold, 0.5f));
    for (const char* n : { "input.pressThreshold", "input.holdSeconds", "input.tapSeconds", "input.deadzone.defaultMin" })
        CHECK(HasFlag(CVarRegistry::Get().Explain(n)->flags, CVarFlags::Deterministic));
    Test::RequireDefault("input.holdSeconds", CVarValue::Float32(0.4f));
}

namespace
{
    nlohmann::json TriggerDoc()
    {
        return nlohmann::json::parse(R"JSON({
          "actionMaps": [
            { "name": "demo", "actions": [
              { "name": "fire", "type": "Button",
                "bindings": [ { "path": "<Gamepad>/leftTrigger" } ] },
              { "name": "aim", "type": "Value",
                "bindings": [ { "path": "<Gamepad>/leftTrigger", "processors": [ "deadzone" ] } ] }
            ] }
          ]
        })JSON");
    }
}

TEST_CASE("sweep: input.pressThreshold and input.deadzone.* reach the evaluator live", "[sweep][input]")
{
    auto input = InputActions::Create();
    REQUIRE(input->LoadJson(TriggerDoc()));
    input->SetBaseContext("demo");
    InputSnapshot light;   // leftTrigger below the default press threshold (0.5)
    light.gamepadConnected = true;
    light.gamepadAxes[4] = 0.4f;
    InputSnapshot firm = light;   // leftTrigger inside the default deadzone band [0.125, 0.925]
    firm.gamepadAxes[4] = 0.7f;

    input->Update(1.0 / 60.0, light);
    CHECK_FALSE(input->Down("fire"));
    input->Update(1.0 / 60.0, firm);
    CHECK(input->Strength("aim") == Catch::Approx((0.7f - 0.125f) / (0.925f - 0.125f)));

    CVarRegistry& reg = CVarRegistry::Get();
    reg.Set(reg.Find("input.pressThreshold"), CVarValue::Float32(0.3f), SetBy::Code);
    reg.Set(reg.Find("input.deadzone.defaultMin"), CVarValue::Float32(0.75f), SetBy::Code);
    reg.PublishImmediate();
    // The same loaded asset: no reload needed (Live).
    input->Update(1.0 / 60.0, light);
    const bool firedAtLowerThreshold = input->Down("fire");
    input->Update(1.0 / 60.0, firm);
    const float aimInsideRaisedDeadzone = input->Strength("aim");
    reg.RevertLayer(SetBy::Code);
    reg.PublishImmediate();

    CHECK(firedAtLowerThreshold);
    CHECK(aimInsideRaisedDeadzone == 0.0f);
}
