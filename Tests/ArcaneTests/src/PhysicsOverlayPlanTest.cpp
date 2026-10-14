// The overlay decision, spec 2026-09-11-physics-2d-wiring s6.3, kept pure so
// the ImGui-side call (EditorAppFrame::SubmitSceneToBatcher) has nothing to
// decide: selected-body outline always in Edit mode; the whole world only
// behind View -> Physics Overlay, in Edit and Play alike.
#include <catch2/catch_test_macros.hpp>
#include "Scene/PhysicsOverlay.hpp"
using Arcane::Editor::PlanPhysicsOverlay;
TEST_CASE("PlanPhysicsOverlay: selection outline in Edit; whole world only when toggled", "[editor][physics]")
{
    // (inPlayMode, toggled, hasSelectedBody) -> (draw, wholeWorld)
    CHECK_FALSE(PlanPhysicsOverlay(false, false, false).draw);
    CHECK(PlanPhysicsOverlay(false, false, true).draw);
    CHECK_FALSE(PlanPhysicsOverlay(false, false, true).wholeWorld);
    CHECK_FALSE(PlanPhysicsOverlay(true, false, true).draw);      // Play: no selection outline
    CHECK(PlanPhysicsOverlay(true, true, false).draw);            // Play + toggle: whole world
    CHECK(PlanPhysicsOverlay(true, true, false).wholeWorld);
    CHECK(PlanPhysicsOverlay(false, true, true).wholeWorld);      // toggle wins over selection
}
