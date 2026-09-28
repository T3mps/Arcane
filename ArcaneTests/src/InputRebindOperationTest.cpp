#include <catch2/catch_test_macros.hpp>

#include <Arcane/Input/InputRebindOperation.hpp>

namespace
{
    Arcane::Guid Binding() { return *Arcane::Guid::FromString("11111111-1111-4111-8111-111111111111"); }
}

TEST_CASE("input profile: capture ignores initiating control until released", "[input][profile]")
{
    Arcane::InputRebindOperation capture;
    Arcane::InputSnapshot initiating;
    initiating.SetScancode(44); // Space held to open the capture UI.
    capture.Begin(Binding(), Arcane::InputDevice::Kbm, 5.0f, initiating);
    capture.Observe(initiating, 0.1f);
    CHECK(capture.Result().state == Arcane::InputRebindState::Waiting);
    capture.Observe(Arcane::InputSnapshot{}, 0.1f);
    Arcane::InputSnapshot replacement;
    replacement.SetScancode(26); // W
    capture.Observe(replacement, 0.1f);
    CHECK(capture.Result().state == Arcane::InputRebindState::Completed);
    CHECK(capture.Result().bindingId == Binding());
    CHECK(capture.Result().replacementPath == "<Keyboard>/scancode/w");
}

TEST_CASE("input profile: capture device filter cancel and timeout", "[input][profile]")
{
    Arcane::InputRebindOperation capture;
    capture.Begin(Binding(), Arcane::InputDevice::Gamepad, 0.2f, {});
    Arcane::InputSnapshot key;
    key.SetScancode(26);
    capture.Observe(key, 0.05f);
    CHECK(capture.Result().state == Arcane::InputRebindState::Waiting);
    Arcane::InputSnapshot pad;
    pad.gamepadConnected = true;
    pad.gamepadButtons = 1;
    capture.Observe(pad, 0.05f);
    CHECK(capture.Result().state == Arcane::InputRebindState::Completed);
    CHECK(capture.Result().replacementPath == "<Gamepad>/buttonSouth");

    capture.Begin(Binding(), std::nullopt, 0.1f, {});
    capture.Observe({}, 0.11f);
    CHECK(capture.Result().state == Arcane::InputRebindState::TimedOut);
    capture.Begin(Binding(), std::nullopt, 1.0f, {});
    capture.Cancel();
    CHECK(capture.Result().state == Arcane::InputRebindState::Canceled);
}

TEST_CASE("input profile: capture ignores keys and buttons the UI has claimed", "[input][profile]")
{
    // The evaluator already treats wantCaptureKeyboard/Mouse as "not for the
    // game"; a capture armed from a controls screen must do the same, or a
    // key typed into any ImGui field completes the rebind (hygiene pass
    // 2026-09-28).
    Arcane::InputRebindOperation capture;
    capture.Begin(Binding(), Arcane::InputDevice::Kbm, 5.0f, {});

    Arcane::InputSnapshot typed;
    typed.SetScancode(26);   // W, but the UI owns the keyboard this frame
    typed.wantCaptureKeyboard = true;
    capture.Observe(typed, 0.1f);
    CHECK(capture.Result().state == Arcane::InputRebindState::Waiting);

    Arcane::InputSnapshot clicked;
    clicked.mouseButtons = 0x1;   // LMB on a UI widget
    clicked.wantCaptureMouse = true;
    capture.Observe(clicked, 0.1f);
    CHECK(capture.Result().state == Arcane::InputRebindState::Waiting);

    capture.Observe({}, 0.1f);   // everything released
    Arcane::InputSnapshot pressed;
    pressed.SetScancode(26);
    capture.Observe(pressed, 0.1f);
    CHECK(capture.Result().state == Arcane::InputRebindState::Completed);
    CHECK(capture.Result().replacementPath == "<Keyboard>/scancode/w");
}
