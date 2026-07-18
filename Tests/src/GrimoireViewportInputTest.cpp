// Grimoire viewport input gating: pure predicates (no ImGui context). CPU-only.

#include <catch2/catch_test_macros.hpp>

#include <ViewportInput.hpp>

TEST_CASE("SceneInputActive is hovered OR focused", "[grimoire]")
{
    CHECK_FALSE(Grimoire::SceneInputActive(false, false));
    CHECK      (Grimoire::SceneInputActive(true,  false));
    CHECK      (Grimoire::SceneInputActive(false, true));
    CHECK      (Grimoire::SceneInputActive(true,  true));
}

TEST_CASE("ToViewportLocal translates inside the rect and rejects outside", "[grimoire]")
{
    const Grimoire::ViewportRect r{ 100.0f, 50.0f, 640.0f, 480.0f };
    float lx = 0, ly = 0;

    CHECK(Grimoire::ToViewportLocal(r, 100.0f, 50.0f, lx, ly));
    CHECK(lx == 0.0f);
    CHECK(ly == 0.0f);

    CHECK(Grimoire::ToViewportLocal(r, 420.0f, 290.0f, lx, ly));
    CHECK(lx == 320.0f);
    CHECK(ly == 240.0f);

    CHECK_FALSE(Grimoire::ToViewportLocal(r, 99.0f,  60.0f, lx, ly));   // left of rect
    CHECK_FALSE(Grimoire::ToViewportLocal(r, 200.0f, 49.0f, lx, ly));   // above rect
    CHECK_FALSE(Grimoire::ToViewportLocal(r, 740.0f, 60.0f, lx, ly));   // right edge (w exclusive)
}
