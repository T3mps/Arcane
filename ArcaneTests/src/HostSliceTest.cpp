// The host slice's HEADLESS remainder.
//
// This file used to be a [gpu] live integration: drive PlaygroundGame through a
// real Runtime + Batcher2D into an offscreen HDR canvas and assert both that
// the plugin's RenderSubmissionSystem submitted its two sprites and that
// validation stayed silent. That case built its device with
// Arcane::RenderDevice::Create, and NRI Phase 5a Task 8b deleted the NVRHI
// device layer -- there is no longer any way for a test executable to obtain an
// nvrhi::IDevice, so the case was retired rather than faked.
//
// THE COVERAGE THAT LEAVES WITH IT, named rather than dropped: nothing in
// ArcaneTests now drives a loaded plugin's render submission all the way to a
// GPU surface. The graph-side replacement (an NRI RenderGraph executing a
// Batch2DNode over a plugin's submissions) does not exist as a test yet; it is
// a Phase 5b item. What survives here is the half that never needed a device.

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
