// The host slice's HEADLESS remainder.
//
// A NAMED COVERAGE GAP: nothing in ArcaneTests drives a loaded plugin's render
// submission all the way to a GPU surface. That would be an NRI RenderGraph
// executing a Batch2DNode over a plugin's submissions, and it does not exist as
// a test. What is here is the half that never needed a device.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Input/InputSnapshot.hpp>

#include "Helpers/TestTypeContext.hpp"

// Headless: the input-store path the host wires in its frame loop (SetInputSnapshot ->
// Input()). No device needed -- the plugin reads input through Runtime::Input(), so
// the host's per-frame store must round-trip the snapshot verbatim. ([sandbox] so it
// runs alongside the rest of the v2 sandbox wiring under ~[gpu].)
TEST_CASE("Host input store: Runtime::Input reflects the last SetInputSnapshot", "[sandbox]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());

    Arcane::InputSnapshot snap;
    snap.SetScancode(42);              // arbitrary physical key down
    snap.AddKeycode(0x61);             // 'a'
    snap.mouseButtons        = 0x05;   // LMB + MMB
    snap.mouseX              = 123.5f;
    snap.mouseY              = 456.25f;
    snap.gamepadConnected    = true;
    snap.gamepadAxes[0]      = -0.5f;
    snap.wantCaptureKeyboard = true;

    rt.SetInputSnapshot(snap);

    const Arcane::InputSnapshot& got = rt.Input();
    CHECK(got.ScancodeDown(42));
    CHECK(got.KeycodeDown(0x61));
    CHECK(got.mouseButtons == 0x05);
    CHECK(got.mouseX == 123.5f);
    CHECK(got.mouseY == 456.25f);
    CHECK(got.gamepadConnected);
    CHECK(got.gamepadAxes[0] == -0.5f);
    CHECK(got.wantCaptureKeyboard);
}
