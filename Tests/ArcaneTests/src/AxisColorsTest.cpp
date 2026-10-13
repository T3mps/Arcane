// Settings arc S4 (R1): editor.theme.axisX/Y/Z are the one source of every
// axis colour; the outline colours derive from the amber token and the graph
// hover border. Exact at Dark (no golden moves).
//
// Controller ruling S5-2 option A (carry/S4-3.md): only the inspector bars
// bind to the tokens. Grid and gizmo axis colours stay their legacy values
// until a deliberate unification re-bless. The re-tint case below pins that
// hold; DeriveRole itself is still the gain rule a later unify will use.
#include <catch2/catch_test_macros.hpp>
#include "Settings/AxisColors.hpp"
#include "Viewport/ViewportGrid.hpp"
#include "Widgets/EditorTheme.hpp"
#include <Arcane/Edit/Gizmo.hpp>
#include <Arcane/Render/Nri/nodes/GridNode.hpp>
#include <imgui.h>

using namespace Arcane::Editor;

TEST_CASE("Axis roles: at the Dark tokens every consumer keeps today's colour, bit for bit", "[theme][editor]")
{
    const AxisRoleColors r = DeriveAxisRoles(Theme::kDarkPalette);
    const Arcane::GizmoAxisColors gizmo{};
    CHECK(r.gizmo.x == glm::vec4(0.96f, 0.28f, 0.22f, 1.0f));
    CHECK(r.gizmo.y == gizmo.y);
    CHECK(r.gizmo.z == gizmo.z);
    CHECK(r.grid2D[0] == kGridAxisXColor);
    CHECK(r.grid2D[1] == kGridAxisYColor);
    CHECK(r.grid3D[0] == Arcane::GridSceneDesc::kAxisXColor);
    CHECK(r.grid3D[1] == Arcane::GridSceneDesc::kAxisYColor);
    CHECK(r.grid3D[2] == Arcane::GridSceneDesc::kAxisZColor);
    CHECK(r.inspectorBar[0] == IM_COL32(196,  64,  54, 255));
    CHECK(r.inspectorBar[1] == IM_COL32( 96, 166,  58, 255));
    CHECK(r.inspectorBar[2] == IM_COL32( 58, 122, 196, 255));
    CHECK(r.outlineSelect == glm::vec4(1.0f, 0.65f, 0.10f, 1.0f));   // PickOutlineNodes' old kSelectColor
    CHECK(r.outlineHover  == glm::vec4(0.25f, 0.70f, 1.00f, 1.0f));  // its old kHoverColor
}

TEST_CASE("Axis roles: a re-tinted X token moves the inspector bar; grid and gizmo stay held", "[theme][editor]")
{
    Theme::Palette p = Theme::kDarkPalette;
    p.axisX = ImVec4(0.0f, 0.0f, 1.0f, 1.0f);    // X turns blue
    const AxisRoleColors r = DeriveAxisRoles(p);
    CHECK(r.inspectorBar[0] == ImGui::ColorConvertFloat4ToU32(p.axisX));
    CHECK(r.gizmo.x == Arcane::GizmoAxisColors{}.x);                 // S5-2 A: held
    CHECK(r.gizmo.y == Arcane::GizmoAxisColors{}.y);                 // Y untouched
    CHECK(r.grid2D[0] == kGridAxisXColor);
    CHECK(r.grid3D[0] == Arcane::GridSceneDesc::kAxisXColor);
    CHECK(r.grid3D[0].w == Arcane::GridSceneDesc::kAxisXColor.w);
}

TEST_CASE("Outline: the select colour follows the amber token", "[theme][editor]")
{
    Theme::Palette p = Theme::kDarkPalette;
    p.amber = ImVec4(0.2f, 0.9f, 0.3f, 1.0f);
    const AxisRoleColors r = DeriveAxisRoles(p);
    CHECK(r.outlineSelect == glm::vec4(0.2f, 0.9f, 0.3f, 1.0f));
}

TEST_CASE("DeriveRole: exact legacy at the dark token; channel gain otherwise, clamped", "[theme][editor]")
{
    const ImVec4 dark(0.5f, 0.25f, 0.0f, 1.0f);
    const glm::vec4 legacy(0.75f, 0.5f, 0.2f, 0.9f);
    CHECK(DeriveRole(dark, dark, legacy) == legacy);
    const glm::vec4 g = DeriveRole(ImVec4(1.0f, 0.125f, 0.3f, 1.0f), dark, legacy);
    CHECK(g.x == 1.0f);        // 1.0 * 1.5, clamped
    CHECK(g.y == 0.25f);       // 0.125 * 2
    CHECK(g.z == 0.3f);        // a zero dark channel passes the token through
    CHECK(g.w == 0.9f);
}
