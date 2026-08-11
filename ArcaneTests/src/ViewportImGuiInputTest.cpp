// Arcane Editor viewport ImGui input arbitration ([editor], CPU-only, no ImGui/GPU).
#include <catch2/catch_test_macros.hpp>
#include "ViewportImGuiInput.hpp"

TEST_CASE("GameUiClaimsPointer: only in Play, in viewport, when game UI wants the mouse", "[editor]")
{
    // Play + cursor over a game widget -> game UI owns the click.
    CHECK(Arcane::Editor::GameUiClaimsPointer(/*play*/true,  /*inVp*/true,  /*gameWant*/true));
    // Play but cursor not over a game widget -> falls through to gizmo/gameplay.
    CHECK_FALSE(Arcane::Editor::GameUiClaimsPointer(true,  true,  false));
    // Cursor outside the viewport -> editor panels own it, not the game UI.
    CHECK_FALSE(Arcane::Editor::GameUiClaimsPointer(true,  false, true));
    // Edit mode -> the game UI is not even running.
    CHECK_FALSE(Arcane::Editor::GameUiClaimsPointer(false, true,  true));
}
