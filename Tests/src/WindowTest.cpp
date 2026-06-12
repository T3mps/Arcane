// Platform smoke: a hidden SDL3 window creates, reports its pixel size and
// Win32 handle, and survives one event pump. Hidden so it runs on the CI
// agent without flashing windows.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Platform/Window.hpp>

TEST_CASE("window: hidden window creates, reports size and native handle", "[platform]")
{
    Arcane::Window window;
    Arcane::WindowDesc desc;
    desc.title  = "ArcaneTests hidden window";
    desc.width  = 640;
    desc.height = 360;
    desc.hidden = true;
    REQUIRE(window.Create(desc));

    uint32_t w = 0, h = 0;
    window.GetPixelSize(w, h);
    REQUIRE(w > 0);
    REQUIRE(h > 0);
    REQUIRE(window.NativeHandle() != nullptr);
    REQUIRE_FALSE(window.IsMinimized());

    auto events = window.PumpEvents();
    REQUIRE_FALSE(events.quitRequested);

    window.Destroy();
}
