// Grimoire viewport ImGui input arbitration ([grimoire], CPU-only, no ImGui/GPU).
#include <catch2/catch_test_macros.hpp>
#include "ViewportImGuiInput.hpp"

TEST_CASE("GameUiClaimsPointer: only in Play, in viewport, when game UI wants the mouse", "[grimoire]")
{
    // Play + cursor over a game widget -> game UI owns the click.
    CHECK(Grimoire::GameUiClaimsPointer(/*play*/true,  /*inVp*/true,  /*gameWant*/true));
    // Play but cursor not over a game widget -> falls through to gizmo/gameplay.
    CHECK_FALSE(Grimoire::GameUiClaimsPointer(true,  true,  false));
    // Cursor outside the viewport -> editor panels own it, not the game UI.
    CHECK_FALSE(Grimoire::GameUiClaimsPointer(true,  false, true));
    // Edit mode -> the game UI is not even running.
    CHECK_FALSE(Grimoire::GameUiClaimsPointer(false, true,  true));
}
