// Sampler smoke: with a hidden window (SDL video up) and no user input,
// Sample returns a sane snapshot. Headless-safe: asserts shape, not
// hardware state beyond "nothing is pressed in an unfocused hidden window".

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Input/InputDevices.hpp>
#include <Arcane/Platform/Window.hpp>

TEST_CASE("input devices: sample from a hidden window", "[input][platform]")
{
    Arcane::Window window;
    Arcane::WindowDesc desc;
    desc.hidden = true;
    desc.width = 64; desc.height = 64;
    REQUIRE(window.Create(desc));

    auto devices = Arcane::InputDevices::Create();
    REQUIRE(devices != nullptr);
    // pump once so SDL's internal device state is initialized before Sample
    (void)window.PumpEvents();

    const auto snap = devices->Sample(false, false);
    CHECK(snap.keycodeCount == 0);  // headless hidden window: no keys should be down
    CHECK_FALSE(snap.wantCaptureKeyboard);

    const auto captured = devices->Sample(true, true);
    CHECK(captured.wantCaptureKeyboard);
    CHECK(captured.wantCaptureMouse);
}
