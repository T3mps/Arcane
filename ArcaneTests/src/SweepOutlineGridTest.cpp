// Settings sweep S6-21: render.outline.*, editor.viewport.grid.* and
// editor.viewport.grid3D.* -- the defaults rebuild today's outline and both
// grids bit for bit (spec s10.2).
#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include <Arcane/Render/RenderOutlineSettings.hpp>
#include "Settings/EditorGridSettings.hpp"
#include "Viewport/ViewportGrid.hpp"

#include <cmath>
using namespace Arcane;
TEST_CASE("sweep: outline and grid defaults rebuild today's grid exactly", "[sweep][outline-grid]")
{
    CHECK(Test::SameBits(RenderOutlineSettings{}.selectWidthPx, 3.0f));
    CHECK(RenderOutlineSettings{}.maxThicknessPx == 32u);
    CHECK(RenderOutlineSettings{}.supersample == 2u);
    const GridSceneDesc g = Editor::MakeGridScene(Editor::EditorGridSettings{}, Editor::EditorGrid3DSettings{});
    CHECK(g.minorSpacing == 1.0f); CHECK(g.majorEvery == 10.0f); CHECK(g.fadeDistance == 200.0f);
    CHECK(g.minorColor == glm::vec4(0.5f, 0.5f, 0.5f, 0.35f));    // lineColor.rgb + minorAlpha
    CHECK(g.majorColor == glm::vec4(0.6f, 0.6f, 0.6f, 0.6f));     // the 3D-only major
    CHECK(Editor::EditorGridSettings{}.majorAlpha == 0.55f);      // the 2D major
    Test::RequireDefault("editor.viewport.grid.minorAlpha", CVarValue::Float32(0.35f));
    Test::RequireDefault("render.outline.supersample", CVarValue::UInt32(2u));
}

// S6-45: the 2D grid's crossfade window is editor.viewport.grid.fade{In,Full}Px
// in screen px -- a setting, not Ui::Px chrome (S5-2 review).
TEST_CASE("sweep: the 2D grid crossfade window is editor.viewport.grid.fadeInPx / fadeFullPx", "[sweep][grid]")
{
    const Editor::EditorGridSettings g{};
    CHECK(g.fadeInPx == 8.0f);
    CHECK(g.fadeFullPx == 24.0f);
    Test::RequireDefault("editor.viewport.grid.fadeInPx", CVarValue::Float32(8.0f));
    Test::RequireDefault("editor.viewport.grid.fadeFullPx", CVarValue::Float32(24.0f));

    // 100 px/m: the 0.1 m level is 10 px apart -- inside the default 8..24 ramp.
    const Editor::Grid2DPlan atDefault = Editor::PlanGrid2D(100.0f);
    REQUIRE(atDefault.count == 3);
    CHECK(std::abs(atDefault.levels[0].spacingMetres - 0.1f) < 1e-6f);
    CHECK(atDefault.levels[0].alpha > 0.0f);
    CHECK(atDefault.levels[0].alpha < Editor::PlanGrid2D(5.0f).levels[0].alpha);   // below the saturated minor
    {
        // Fade-in raised to 12 px: the 0.1 m level drops out, the 1 m level leads.
        const Test::ScopedCodeRung fadeIn("editor.viewport.grid.fadeInPx", CVarValue::Float32(12.0f));
        const Editor::Grid2DPlan raised = Editor::PlanGrid2D(100.0f);
        REQUIRE(raised.count == 3);
        CHECK(std::abs(raised.levels[0].spacingMetres - 1.0f) < 1e-6f);
    }
    {
        // A full spacing at or below fade-in still plans (the window is 1 px
        // wide): 10 px is past 8 + 1, so the 0.1 m level is saturated.
        const Test::ScopedCodeRung fadeFull("editor.viewport.grid.fadeFullPx", CVarValue::Float32(1.0f));
        const Editor::Grid2DPlan narrow = Editor::PlanGrid2D(100.0f);
        REQUIRE(narrow.count == 3);
        CHECK(std::abs(narrow.levels[0].spacingMetres - 0.1f) < 1e-6f);
        CHECK(narrow.levels[0].alpha == Editor::PlanGrid2D(5.0f).levels[0].alpha);
    }
}
