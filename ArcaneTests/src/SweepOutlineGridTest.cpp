// Settings sweep S6-21: render.outline.*, editor.viewport.grid.* and
// editor.viewport.grid3D.* -- the defaults rebuild today's outline and both
// grids bit for bit (spec s10.2).
#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include <Arcane/Render/RenderOutlineSettings.hpp>
#include "Settings/EditorGridSettings.hpp"
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
