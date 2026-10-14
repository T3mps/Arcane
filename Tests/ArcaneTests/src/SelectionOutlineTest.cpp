// Selection outline: OutlineJfaStepCount reverse-lookup ([outline], CPU) and
// PickPassId/PickSampleTexel ([pick], CPU).
//
// THE GRAPH'S OutlineNode (Render/Nri/nodes/PickOutlineNodes.*) is the only
// selection-outline implementation, and OutlineJfaStepCount below is the sole
// implementation of its jump-schedule formula.
//
// A NAMED COVERAGE GAP: nothing pins the GPU-executed PIXEL correctness of the
// JFA algorithm -- nearest-edge distance field construction, multi-id selection
// membership, the touching-silhouette union (no spurious seam at a shared
// edge), or the amber/cyan anti-aliased straddle composite. RenderGraphTest.cpp
// covers OutlineNode entirely structurally (barriers, pool slots, frame
// composition against a null context -- see its own "GPU-free by construction"
// banner), never a real render + readback. What IS pinned is the jump SCHEDULE
// (RenderGraphTest.cpp's "outline jfa" case) and the node-graph shape.
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/Nri/nodes/PickOutlineNodes.hpp>
#include <Arcane/Render/PickEmit.hpp>
#include <Arcane/Render/RenderOutlineSettings.hpp>

#include "Settings/AxisColors.hpp"
#include "Settings/EditorThemeSettings.hpp"

#include <Astra/Entity/Entity.hpp>

#include <glm/glm.hpp>

#include <vector>

#include "Helpers/SettingsSweep.hpp"

TEST_CASE("OutlineJfaStepCount = ceil(log2(maxThickness)) + 2", "[outline]")
{
    // The sole surviving implementation of the schedule-length formula
    // SelectionOutline.cpp's JfaPassCount used to own (JfaPassCount(x) + 1,
    // since OutlineJfaStepCount counts the trailing repeat step JfaPassCount
    // did not). Same four thickness values JfaPassCount was pinned against.
    CHECK(Arcane::OutlineJfaStepCount(1)  == 2u);
    CHECK(Arcane::OutlineJfaStepCount(3)  == 4u);
    CHECK(Arcane::OutlineJfaStepCount(16) == 6u);
    CHECK(Arcane::OutlineJfaStepCount(32) == 7u);
}

TEST_CASE("PickPassId maps ordered entities to k+1, 0 for absent/invalid", "[pick]")
{
    const Astra::Entity a = Astra::Entity(1, 0);
    const Astra::Entity b = Astra::Entity(2, 0);
    const Astra::Entity c = Astra::Entity(3, 0);
    const std::vector<Astra::Entity> ordered{ a, b, c };

    CHECK(Arcane::PickPassId(ordered, a) == 1u);
    CHECK(Arcane::PickPassId(ordered, b) == 2u);
    CHECK(Arcane::PickPassId(ordered, c) == 3u);
    CHECK(Arcane::PickPassId(ordered, Astra::Entity(9, 0)) == 0u);   // absent
    CHECK(Arcane::PickPassId(ordered, Astra::Entity::Invalid()) == 0u);
    CHECK(Arcane::PickPassId({}, a) == 0u);                          // empty
}

TEST_CASE("PickSampleTexel maps a 1x click to the center subsample, clamped", "[pick]")
{
    // ss=2, id buffer 128x128 (1x 64x64). Click at 1x pixel (10,20) -> 2x texel (21,41).
    CHECK(Arcane::PickSampleTexel(glm::vec2(10.4f, 20.9f), 2u, 128u, 128u) == glm::ivec2(21, 41));
    // ss=1 is identity (floored), clamped to bounds.
    CHECK(Arcane::PickSampleTexel(glm::vec2(3.7f, 4.2f), 1u, 64u, 64u) == glm::ivec2(3, 4));
    // out-of-range clamps into the buffer.
    CHECK(Arcane::PickSampleTexel(glm::vec2(999.0f, -5.0f), 2u, 128u, 128u) == glm::ivec2(127, 0));
}

TEST_CASE("render.outline.*: the latched ceiling and supersample are today's 32 px / 2x, the widths 3/3/1 px", "[outline][sweep]")
{
    // Settings S6-21: the Restart pair is latched for the process and must
    // reproduce the constants it replaced, so the JFA schedule (7 steps) and
    // the id target's extent are unchanged at the defaults.
    CHECK(Arcane::OutlineMaxThicknessPx() == 32u);
    CHECK(Arcane::OutlineJfaStepCount(Arcane::OutlineMaxThicknessPx()) == 7u);
    CHECK(Arcane::PickSupersample() == 2u);
    CHECK(Arcane::PickNode::SuperSample() == 2u);
    Arcane::Test::RequireDefault("render.outline.selectWidthPx", Arcane::CVarValue::Float32(3.0f));
    Arcane::Test::RequireDefault("render.outline.hoverWidthPx", Arcane::CVarValue::Float32(3.0f));
    Arcane::Test::RequireDefault("render.outline.edgeSoftnessPx", Arcane::CVarValue::Float32(1.0f));
    Arcane::Test::RequireDefault("render.outline.maxThicknessPx", Arcane::CVarValue::UInt32(32u));
}

TEST_CASE("Outline colours: the editor's SetColors values at EditorThemeSettings{} are the node's own defaults", "[outline][theme][editor]")
{
    // Settings S6-21 (ruling I5): the editor pushes these two colours into
    // OutlineNode::SetColors every frame (EditorAppFrame, the outline submit),
    // derived from the theme. The node itself needs a device, so this pins the
    // producer half on the CPU: the exact chain the editor runs, from the
    // DEFAULT settings block, lands on the colours OutlineNode keeps as its
    // member defaults (PickOutlineNodes.hpp: m_selectColor / m_hoverColor),
    // which ArcaneRuntime's theme-less --pick-probe outline still draws with.
    // So the editor and the runtime agree at the defaults, and no pixel moves.
    const Arcane::Editor::AxisRoleColors roles =
        Arcane::Editor::DeriveAxisRoles(Arcane::Editor::ToPalette(Arcane::Editor::EditorThemeSettings{}));
    CHECK(roles.outlineSelect == glm::vec4(1.0f, 0.65f, 0.10f, 1.0f));
    CHECK(roles.outlineHover  == glm::vec4(0.25f, 0.70f, 1.00f, 1.0f));
}
