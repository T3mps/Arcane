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

namespace
{
    nlohmann::json InteractionDoc()
    {
        return nlohmann::json::parse(R"JSON({
          "actionMaps": [
            { "name": "demo", "actions": [
              { "name": "charge", "type": "Button", "interactions": [ "hold" ],
                "bindings": [ { "path": "<Gamepad>/leftTrigger" } ] },
              { "name": "chargeFixed", "type": "Button", "interactions": [ "hold(duration=0.5)" ],
                "bindings": [ { "path": "<Gamepad>/leftTrigger" } ] },
              { "name": "flick", "type": "Button", "interactions": [ "tap" ],
                "bindings": [ { "path": "<Gamepad>/leftTrigger" } ] }
            ] }
          ]
        })JSON");
    }

    // Holds the trigger for `frames` updates of dt = 0.25 s (exact in binary;
    // heldTime reads 0, 0.25, 0.5, ...), then releases it for one update.
    // Returns the 1-based frame each hold action performed on (0 = never) and
    // whether the tap action performed on the release.
    struct HoldRun { int charge = 0; int chargeFixed = 0; bool flickPerformed = false; };
    HoldRun RunHold(InputActions& input, int frames)
    {
        InputSnapshot held;
        held.gamepadConnected = true;
        held.gamepadAxes[4] = 0.7f;
        InputSnapshot released;
        released.gamepadConnected = true;
        HoldRun run;
        for (int f = 1; f <= frames; ++f)
        {
            input.Update(0.25, held);
            if (input.Performed("charge") && run.charge == 0) run.charge = f;
            if (input.Performed("chargeFixed") && run.chargeFixed == 0) run.chargeFixed = f;
        }
        input.Update(0.25, released);
        run.flickPerformed = input.Performed("flick");
        return run;
    }
}

TEST_CASE("sweep: input.holdSeconds and input.tapSeconds reach a loaded asset live", "[sweep][input]")
{
    auto input = InputActions::Create();
    REQUIRE(input->LoadJson(InteractionDoc()));
    input->SetBaseContext("demo");

    // Defaults: hold 0.4 s performs at heldTime 0.5 (frame 3); a 0.5 s press is
    // too long for the 0.2 s tap.
    const HoldRun atDefault = RunHold(*input, 3);
    CHECK(atDefault.charge == 3);
    CHECK(atDefault.chargeFixed == 3);
    CHECK_FALSE(atDefault.flickPerformed);

    CVarRegistry& reg = CVarRegistry::Get();
    reg.Set(reg.Find("input.holdSeconds"), CVarValue::Float32(1.0f), SetBy::Code);
    reg.Set(reg.Find("input.tapSeconds"), CVarValue::Float32(1.0f), SetBy::Code);
    reg.PublishImmediate();
    // The same loaded asset: no reload needed (Live). Undecorated hold now
    // performs at heldTime 1.0 (frame 5); the explicit duration=0.5 is
    // unaffected; a 1.0 s press is now a tap.
    const HoldRun live = RunHold(*input, 5);
    reg.RevertLayer(SetBy::Code);
    reg.PublishImmediate();

    CHECK(live.charge == 5);
    CHECK(live.chargeFixed == 3);
    // 5 frames = heldTime 1.0 at release, still <= tapSeconds 1.0.
    CHECK(live.flickPerformed);
}
