// [sandbox] CPU-only: the Sandbox 2D camera world<->screen transform (Task 6).
//
// Pins the CANONICAL camera transform shared by RenderSubmissionSystem and
// DrawPhysicsDebug so sprites + the physics-debug overlay pan/zoom together:
//
//     screen = world * zoom + offset
//     world  = (screen - offset) / zoom   (inverse)
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

    // Known point: world (10,10) * zoom 2 + offset (100,50) -> screen (120,70).
    const glm::vec2 s = cam.WorldToScreen({10.0f, 10.0f});
    CHECK(s.x == Approx(120.0f));
    CHECK(s.y == Approx(70.0f));

    // Inverse round-trips back to the original world point.
    const glm::vec2 w = cam.ScreenToWorld(s);
    CHECK(w.x == Approx(10.0f));
    CHECK(w.y == Approx(10.0f));
}

TEST_CASE("Sandbox camera identity defaults", "[sandbox]")
{
    // Default camera (offset (0,0), zoom 1) is the identity transform: every
    // existing caller that does not set a camera is unchanged.
    Arcane::Sandbox::Camera cam;
    const glm::vec2 p{42.0f, -7.0f};
    const glm::vec2 s = cam.WorldToScreen(p);
    CHECK(s.x == Approx(42.0f));
    CHECK(s.y == Approx(-7.0f));
    const glm::vec2 w = cam.ScreenToWorld(s);
    CHECK(w.x == Approx(42.0f));
    CHECK(w.y == Approx(-7.0f));
}
