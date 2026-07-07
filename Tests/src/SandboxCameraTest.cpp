// [sandbox] CPU-only: the Sandbox 2D camera world<->screen transform (Task 6).
//
// Pins the CANONICAL camera transform shared by RenderSubmissionSystem and
// DrawPhysicsDebug so sprites + the physics-debug overlay pan/zoom together. World
// is METERS (MKS); the canvas is pixels. kPixelsPerMeter folds the unit conversion
// into the single transform pair everything routes through:
//
//     screen = world * (kPixelsPerMeter * zoom) + offset
//     world  = (screen - offset) / (kPixelsPerMeter * zoom)   (inverse)
//
// Camera.hpp is a pure, dependency-free, header-only struct (glm::vec2 only), so
// the test includes it directly by relative path -- the ArcaneTests project does
// NOT carry Sandbox/src on its include path (Sandbox is a plugin), and adding it
// just for a one-struct header would be heavier than this relative include.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "../../Sandbox/src/Camera.hpp"

using Catch::Approx;

TEST_CASE("Sandbox camera world<->screen round-trips", "[sandbox]")
{
    Arcane::Sandbox::Camera cam;
    cam.offset = {100.0f, 50.0f};
    cam.zoom   = 2.0f;

    // Known point: world (10,10) m * (kPixelsPerMeter 100 * zoom 2) + offset (100,50)
    // -> screen (10*200+100, 10*200+50) = (2100, 2050).
    const glm::vec2 s = cam.WorldToScreen({10.0f, 10.0f});
    CHECK(s.x == Approx(2100.0f));
    CHECK(s.y == Approx(2050.0f));

    // Inverse round-trips back to the original world point (meters).
    const glm::vec2 w = cam.ScreenToWorld(s);
    CHECK(w.x == Approx(10.0f));
    CHECK(w.y == Approx(10.0f));
}

TEST_CASE("Sandbox camera identity defaults", "[sandbox]")
{
    // Default camera (offset (0,0), zoom 1) maps meters -> pixels at 100 px/m:
    // WorldToScreen(p) == p * kPixelsPerMeter.
    Arcane::Sandbox::Camera cam;
    const glm::vec2 p{42.0f, -7.0f};
    const glm::vec2 s = cam.WorldToScreen(p);
    CHECK(s.x == Approx(4200.0f));
    CHECK(s.y == Approx(-700.0f));
    const glm::vec2 w = cam.ScreenToWorld(s);
    CHECK(w.x == Approx(42.0f));
    CHECK(w.y == Approx(-7.0f));

    // Framing: the 12.8 x 7.2 m layout fills a 1280x720 canvas exactly at zoom 1 --
    // the meter scenes at ppm=100 frame like the px original did.
    const glm::vec2 frame = cam.WorldToScreen({12.8f, 7.2f});
    CHECK(frame.x == Approx(1280.0f));
    CHECK(frame.y == Approx(720.0f));
}
