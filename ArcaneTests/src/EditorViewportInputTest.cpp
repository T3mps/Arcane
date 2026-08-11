// Arcane Editor viewport input gating: pure predicates (no ImGui context). CPU-only.

#include <catch2/catch_test_macros.hpp>

#include <Viewport/ViewportInput.hpp>

TEST_CASE("SceneInputActive is hovered OR focused", "[editor]")
{
    CHECK_FALSE(Arcane::Editor::SceneInputActive(false, false));
    CHECK      (Arcane::Editor::SceneInputActive(true,  false));
    CHECK      (Arcane::Editor::SceneInputActive(false, true));
    CHECK      (Arcane::Editor::SceneInputActive(true,  true));
}

TEST_CASE("ToViewportLocal translates inside the rect and rejects outside", "[editor]")
{
    const Arcane::Editor::ViewportRect r{ 100.0f, 50.0f, 640.0f, 480.0f };
    float lx = 0, ly = 0;

    CHECK(Arcane::Editor::ToViewportLocal(r, 100.0f, 50.0f, lx, ly));
    CHECK(lx == 0.0f);
    CHECK(ly == 0.0f);

    CHECK(Arcane::Editor::ToViewportLocal(r, 420.0f, 290.0f, lx, ly));
    CHECK(lx == 320.0f);
    CHECK(ly == 240.0f);

    CHECK_FALSE(Arcane::Editor::ToViewportLocal(r, 99.0f,  60.0f, lx, ly));   // left of rect
    CHECK_FALSE(Arcane::Editor::ToViewportLocal(r, 200.0f, 49.0f, lx, ly));   // above rect
    CHECK_FALSE(Arcane::Editor::ToViewportLocal(r, 740.0f, 60.0f, lx, ly));   // right edge (w exclusive)
}
