// Settings arc S6-34: editor.graph.* / editor.graph.lod.* / editor.graph.grid.*
// -- the node canvases' zoom table, chrome metrics, wire budget, LOD tiers and
// backdrop grid tuning, plus the three one-off graph cvars (fitMaxZoom,
// fitMinZoom, showPinLegend) folded into GraphCanvasSettings.

#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include "Settings/GraphCanvasSettings.hpp"
#include "Widgets/GraphZoomLevels.hpp"
#include "Widgets/GraphGridPhase.hpp"
#include "Widgets/GraphNodeLod.hpp"
#include "Widgets/GraphCanvasStyle.hpp"   // GraphPinRingWidth() / GraphPinOuterRingGap() / GraphPinOuterRingWidth()
#include "Documents/DocumentHost.hpp"
#include "Documents/ShaderEditorDocument.hpp"
#include "Helpers/NodePageDocs.hpp"      // HeadlessImGui + SpriteNodeDoc: an open graph document

#include <imgui.h>
#include <imgui_node_editor.h>

#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/Settings.hpp>   // Settings<GraphCanvasSettings>: the published (pending-restart) value

#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Arcane;

namespace Arcane::Editor
{
    namespace
    {
        // The old kZoomLevels array (GraphZoomLevels.hpp:60-65 before S6-34),
        // kept here only, as the identity the default string must parse back to.
        std::vector<float> DefaultZoomLevelsForTest()
        {
            return {
                0.100f, 0.125f, 0.150f, 0.175f, 0.200f,
                0.225f, 0.250f, 0.375f, 0.500f, 0.675f,
                0.750f, 0.875f, 1.000f, 1.250f, 1.375f,
                1.500f, 1.675f, 1.750f, 1.875f, 2.000f,
            };
        }
    }
}

TEST_CASE("sweep: graph canvas defaults are the pre-sweep literals", "[sweep][graph-canvas]")
{
    const Editor::GraphCanvasSettings g{};
    CHECK(g.nodeRounding == 4.0f); CHECK(g.wireThickness == 2.0f); CHECK(g.pinSegments == 12);
    CHECK(g.fitMaxZoom == 1.0f); CHECK(g.fitMinZoom == 0.5f); CHECK(g.showPinLegend);
    CHECK(Editor::GraphLodSettings{}.mediumMax == 0.675f);
    CHECK(Editor::GraphGridSettings{}.majorEvery == 8);
    const std::vector<float> stops = Editor::ParseZoomLevels(g.zoomLevels);
    REQUIRE(stops.size() == 20u);
    CHECK(stops.front() == 0.1f); CHECK(stops.back() == 2.0f);
    CHECK(stops == Editor::DefaultZoomLevelsForTest());   // the old kZoomLevels array, kept in the test only
    Test::RequireDefault("editor.graph.fitMinZoom", CVarValue::Float32(0.5f));
}

TEST_CASE("sweep: every editor.graph.* default is the declared literal", "[sweep][graph-canvas]")
{
    const Editor::GraphCanvasSettings g{};
    CHECK(g.nodeBorderWidth == 1.0f); CHECK(g.nodeBorderHoverWidth == 1.5f); CHECK(g.nodeBorderSelectedWidth == 2.0f);
    CHECK(g.wireHighlight == 0.25f);
    CHECK(g.wireSegmentsMin == 12); CHECK(g.wireSegmentsMax == 64); CHECK(g.wireSegmentsPxPer == 6.0f);
    CHECK(g.nodeHeaderGap == 5.0f); CHECK(g.cullGuardBand == 0.25f); CHECK(g.pinDotRadius == 4.0f);
    CHECK(g.shiftAddsToSelection);
    const Editor::GraphLodSettings lod{};
    CHECK(lod.lowestMax == 0.200f); CHECK(lod.lowMax == 0.250f); CHECK(lod.defaultMax == 1.375f);
    const Editor::GraphGridSettings grid{};
    CHECK(grid.zoomExponent == 0.7f); CHECK(grid.baseSpacing == 20.0f); CHECK(grid.minorTargetPx == 22.0f);

    Test::RequireDefault("editor.graph.zoomLevels", CVarValue::String(g.zoomLevels));
    Test::RequireDefault("editor.graph.nodeRounding", CVarValue::Float32(4.0f));
    Test::RequireDefault("editor.graph.nodeBorderWidth", CVarValue::Float32(1.0f));
    Test::RequireDefault("editor.graph.nodeBorderHoverWidth", CVarValue::Float32(1.5f));
    Test::RequireDefault("editor.graph.nodeBorderSelectedWidth", CVarValue::Float32(2.0f));
    Test::RequireDefault("editor.graph.wireThickness", CVarValue::Float32(2.0f));
    Test::RequireDefault("editor.graph.pinSegments", CVarValue::Int32(12));
    Test::RequireDefault("editor.graph.wireHighlight", CVarValue::Float32(0.25f));
    Test::RequireDefault("editor.graph.wireSegmentsMin", CVarValue::Int32(12));
    Test::RequireDefault("editor.graph.wireSegmentsMax", CVarValue::Int32(64));
    Test::RequireDefault("editor.graph.wireSegmentsPxPer", CVarValue::Float32(6.0f));
    Test::RequireDefault("editor.graph.nodeHeaderGap", CVarValue::Float32(5.0f));
    Test::RequireDefault("editor.graph.cullGuardBand", CVarValue::Float32(0.25f));
    Test::RequireDefault("editor.graph.pinDotRadius", CVarValue::Float32(4.0f));
    Test::RequireDefault("editor.graph.shiftAddsToSelection", CVarValue::Bool(true));
    Test::RequireDefault("editor.graph.fitMaxZoom", CVarValue::Float32(1.0f));
    Test::RequireDefault("editor.graph.showPinLegend", CVarValue::Bool(true));
    Test::RequireDefault("editor.graph.lod.lowestMax", CVarValue::Float32(0.200f));
    Test::RequireDefault("editor.graph.lod.lowMax", CVarValue::Float32(0.250f));
    Test::RequireDefault("editor.graph.lod.mediumMax", CVarValue::Float32(0.675f));
    Test::RequireDefault("editor.graph.lod.defaultMax", CVarValue::Float32(1.375f));
    Test::RequireDefault("editor.graph.grid.zoomExponent", CVarValue::Float32(0.7f));
    Test::RequireDefault("editor.graph.grid.baseSpacing", CVarValue::Float32(20.0f));
    Test::RequireDefault("editor.graph.grid.minorTargetPx", CVarValue::Float32(22.0f));
    Test::RequireDefault("editor.graph.grid.majorEvery", CVarValue::Int32(8));

    // The shape knobs a user sees are not Dev; the vertex budgets, the LOD
    // tiers and the grid tuning are.
    CVarRegistry& reg = CVarRegistry::Get();
    const auto flagsOf = [&](const char* name)
    {
        INFO(name);
        const auto meta = reg.Metadata(reg.Find(name));
        REQUIRE(meta.has_value());
        return meta->flags;
    };
    CHECK(HasFlag(flagsOf("editor.graph.wireThickness"), CVarFlags::Archive));
    CHECK_FALSE(HasFlag(flagsOf("editor.graph.wireThickness"), CVarFlags::Dev));
    CHECK(HasFlag(flagsOf("editor.graph.pinSegments"), CVarFlags::Dev));
    CHECK(HasFlag(flagsOf("editor.graph.lod.mediumMax"), CVarFlags::Dev));
    CHECK(HasFlag(flagsOf("editor.graph.grid.majorEvery"), CVarFlags::Dev));
    CHECK(flagsOf("editor.graph.fitMinZoom") == CVarFlags::Archive);
}

TEST_CASE("sweep: ParseZoomLevels refuses a malformed table and falls back to the default stops", "[sweep][graph-canvas]")
{
    const std::vector<float> def = Editor::DefaultZoomLevelsForTest();
    CHECK(Editor::ParseZoomLevels("0.5 1 2") == std::vector<float>{ 0.5f, 1.0f, 2.0f });
    CHECK(Editor::ParseZoomLevels("  0.25   1.5 ") == std::vector<float>{ 0.25f, 1.5f });   // extra spaces are fine
    CHECK(Editor::ParseZoomLevels("") == def);              // empty
    CHECK(Editor::ParseZoomLevels("1 0.5") == def);         // unsorted
    CHECK(Editor::ParseZoomLevels("0.5 0.5 1") == def);     // not strictly ascending
    CHECK(Editor::ParseZoomLevels("0 1") == def);           // non-positive
    CHECK(Editor::ParseZoomLevels("-0.5 1") == def);        // negative
    CHECK(Editor::ParseZoomLevels("0.5 abc") == def);       // garbage
    CHECK(Editor::ParseZoomLevels("0.5 1.5x") == def);      // trailing garbage
    CHECK(Editor::ParseZoomLevels("0.5 inf") == def);       // non-finite
    CHECK(Editor::ParseZoomLevels("nan") == def);
}

TEST_CASE("sweep: the LOD tiers and the grid read their settings and match the old constants at the defaults", "[sweep][graph-canvas]")
{
    const Editor::GraphLodSettings lod{};
    // The old boundaries (0.200 / 0.250 / 0.675 / 1.375) belong to the LOWER tier.
    CHECK(Editor::NodeLODForScale(0.200f, lod) == Editor::NodeLOD::LowestDetail);
    CHECK(Editor::NodeLODForScale(0.225f, lod) == Editor::NodeLOD::LowDetail);
    CHECK(Editor::NodeLODForScale(0.250f, lod) == Editor::NodeLOD::LowDetail);
    CHECK(Editor::NodeLODForScale(0.375f, lod) == Editor::NodeLOD::MediumDetail);
    CHECK(Editor::NodeLODForScale(0.675f, lod) == Editor::NodeLOD::MediumDetail);
    CHECK(Editor::NodeLODForScale(0.750f, lod) == Editor::NodeLOD::DefaultDetail);
    CHECK(Editor::NodeLODForScale(1.375f, lod) == Editor::NodeLOD::DefaultDetail);
    CHECK(Editor::NodeLODForScale(1.500f, lod) == Editor::NodeLOD::FullyZoomedIn);
    // The snapshot overload is the same answer at the published defaults.
    CHECK(Editor::NodeLODForScale(0.5f) == Editor::NodeLODForScale(0.5f, lod));
    // A moved boundary moves the tier.
    Editor::GraphLodSettings wider = lod;
    wider.mediumMax = 0.8f;
    CHECK(Editor::NodeLODForScale(0.750f, wider) == Editor::NodeLOD::MediumDetail);

    // The grid's tuning at the defaults is the old kZoomExponent / kBaseSpacingPx /
    // kMinorTargetPx / kMajorEvery arithmetic, bit for bit.
    const Editor::GraphGridPhase phase{};
    for (float scale : Editor::DefaultZoomLevelsForTest())
    {
        const float oldGrid = std::pow(scale, 0.7f);
        CHECK(Test::SameBits(phase.GridScale(scale), oldGrid));
        const float oldBase = 20.0f * oldGrid;
        const float oldPm = oldBase * std::exp2(std::floor(std::log2(22.0f / oldBase)));
        CHECK(Test::SameBits(phase.MinorPeriod(phase.GridScale(scale)), oldPm));
    }
}

// S6-44: the pin rings and the shader canvas's node padding (the S5-2 review
// restored them from DERIVED: canvas units, scaled by the graph zoom).
TEST_CASE("sweep: editor.graph.pinRing.* and editor.graph.nodePadding defaults are the pre-sweep literals",
          "[sweep][graph-canvas]")
{
    const Editor::GraphPinRingSettings r{};
    CHECK(r.width == 1.6f); CHECK(r.outerGap == 2.2f); CHECK(r.outerWidth == 1.0f);
    const Editor::GraphCanvasSettings g{};
    CHECK(g.nodePadding.x == 10.0f); CHECK(g.nodePadding.y == 6.0f);
    Test::RequireDefault("editor.graph.pinRing.width",      CVarValue::Float32(1.6f));
    Test::RequireDefault("editor.graph.pinRing.outerGap",   CVarValue::Float32(2.2f));
    Test::RequireDefault("editor.graph.pinRing.outerWidth", CVarValue::Float32(1.0f));
    Test::RequireDefault("editor.graph.nodePadding",        CVarValue::Vec2(CVarVec2{ 10.0f, 6.0f }));
    // The published accessors read them.
    CHECK(Editor::GraphPinRingWidth() == 1.6f);
    CHECK(Editor::GraphPinOuterRingGap() == 2.2f);
    CHECK(Editor::GraphPinOuterRingWidth() == 1.0f);

    CVarRegistry& reg = CVarRegistry::Get();
    const auto describe = [&](std::string_view name)
    {
        const std::optional<CVarDescInfo> d = reg.Describe(name);
        INFO("cvar " << std::string(name));
        REQUIRE(d.has_value());
        CHECK(d->audience == Audience::Editor);
        CHECK(d->scope == SettingScope::PreferencesMachine);
        CHECK(HasFlag(d->flags, CVarFlags::Dev));
        CHECK_FALSE(d->help.empty());
        return *d;
    };
    CHECK(describe("editor.graph.pinRing.width").apply == ApplyMode::Live);
    CHECK(describe("editor.graph.pinRing.outerGap").apply == ApplyMode::Live);
    const CVarDescInfo pad = describe("editor.graph.nodePadding");
    CHECK(pad.apply == ApplyMode::Restart);
    REQUIRE(pad.max.has_value());
    CHECK(*pad.max == CVarValue::Vec2(CVarVec2{ 24.0f, 24.0f }));
}

TEST_CASE("sweep: a published pin ring reaches the shader canvas; node padding waits for a restart",
          "[sweep][graph-canvas][graphcanvas]")
{
    namespace ed = ax::NodeEditor;
    CVarRegistry& reg = CVarRegistry::Get();
    const CVarHandle ring = reg.Find("editor.graph.pinRing.width");
    const CVarHandle pad  = reg.Find("editor.graph.nodePadding");
    REQUIRE_FALSE(ring.IsStale());
    REQUIRE_FALSE(pad.IsStale());
    struct Reset
    {
        CVarHandle a, b;
        ~Reset()
        {
            CVarRegistry& r = CVarRegistry::Get();
            r.ClearRung(a, SetBy::Code);
            r.ClearRung(b, SetBy::Code);
            r.PublishImmediate();
            Editor::CaptureGraphNodePaddingAtBoot();   // leave the default boot value for the next test
        }
    } reset{ ring, pad };
    // "Boot": the editor captures the Restart value once (EditorApp does this
    // at startup; here, the defaults).
    Editor::CaptureGraphNodePaddingAtBoot();
    CHECK(Editor::GraphNodePaddingAtBoot().x == 10.0f); CHECK(Editor::GraphNodePaddingAtBoot().y == 6.0f);

    Test::HeadlessImGui imgui;   // before the docs: their dtors need the context
    const auto frame = [](Editor::ShaderEditorDocument& doc)
    {
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        bool requestClose = false;
        doc.Draw(requestClose);
        ImGui::Render();
    };
    const auto padding = [](Editor::ShaderEditorDocument& doc)
    {
        ed::SetCurrentEditor(doc.GraphCanvasContext());
        const ImVec4 v = ed::GetStyle().NodePadding;
        ed::SetCurrentEditor(nullptr);
        return v;
    };
    {
        Editor::ShaderEditorDocument doc(Editor::DocServices{}, "padding.arcmat", Test::SpriteNodeDoc());
        frame(doc);
        REQUIRE(doc.GraphCanvasContext() != nullptr);
        CHECK(padding(doc).x == 10.0f); CHECK(padding(doc).y == 6.0f);
        CHECK(padding(doc).z == 10.0f); CHECK(padding(doc).w == 6.0f);

        REQUIRE(reg.Set(ring, CVarValue::Float32(3.0f), SetBy::Code, {}, CVarContext::Editor) == SetResult::Applied);
        REQUIRE(reg.Set(pad, CVarValue::Vec2(CVarVec2{ 14.0f, 9.0f }), SetBy::Code, {}, CVarContext::Editor) ==
                SetResult::Applied);
        reg.PublishImmediate();
        // The pin ring is Live: the next draw reads it.
        CHECK(Editor::GraphPinRingWidth() == 3.0f);
        // The padding is Restart (spec s3.4, read once at boot): the edit is
        // published, but the open canvas keeps the boot value...
        CHECK(Settings<Editor::GraphCanvasSettings>().nodePadding.x == 14.0f);
        frame(doc);
        frame(doc);
        CHECK(padding(doc).x == 10.0f); CHECK(padding(doc).y == 6.0f);
        CHECK(padding(doc).z == 10.0f); CHECK(padding(doc).w == 6.0f);
        CHECK(Editor::GraphNodePaddingAtBoot().x == 10.0f); CHECK(Editor::GraphNodePaddingAtBoot().y == 6.0f);
    }
    {
        // ...and so does a canvas opened later in the same session.
        Editor::ShaderEditorDocument doc(Editor::DocServices{}, "padding2.arcmat", Test::SpriteNodeDoc());
        frame(doc);
        REQUIRE(doc.GraphCanvasContext() != nullptr);
        CHECK(padding(doc).x == 10.0f); CHECK(padding(doc).y == 6.0f);
    }
    {
        // A restart captures the edited value; the canvas it opens uses it.
        Editor::CaptureGraphNodePaddingAtBoot();
        Editor::ShaderEditorDocument doc(Editor::DocServices{}, "padding3.arcmat", Test::SpriteNodeDoc());
        frame(doc);
        REQUIRE(doc.GraphCanvasContext() != nullptr);
        CHECK(padding(doc).x == 14.0f); CHECK(padding(doc).y == 9.0f);
        CHECK(padding(doc).z == 14.0f); CHECK(padding(doc).w == 9.0f);
    }
}

// S6-45: the shader canvas's in-node widgets are editor.graph.* (frozen names;
// nodePreviewMinPx / dragSpeed / rangeDragSpeed moved from editor.shader.*).
TEST_CASE("sweep: the shader canvas's in-node widget widths and drag speeds are editor.graph.*", "[sweep][graph]")
{
    const Editor::GraphCanvasSettings g{};
    CHECK(Editor::GraphPinNeutralWidth(g, 1) == 64.0f);
    CHECK(Editor::GraphPinNeutralWidth(g, 2) == 106.0f);
    CHECK(Editor::GraphPinNeutralWidth(g, 3) == 190.0f);
    CHECK(Editor::GraphPinNeutralWidth(g, 4) == 190.0f);
    CHECK(Editor::GraphConstValueWidth(g, 1) == 90.0f);
    CHECK(Editor::GraphConstValueWidth(g, 2) == 140.0f);
    CHECK(Editor::GraphConstValueWidth(g, 4) == 220.0f);
    Test::RequireDefault("editor.graph.constPinNeutralWidth1", CVarValue::Float32(64.0f));
    Test::RequireDefault("editor.graph.constPinNeutralWidth2", CVarValue::Float32(106.0f));
    Test::RequireDefault("editor.graph.constPinNeutralWidth4", CVarValue::Float32(190.0f));
    Test::RequireDefault("editor.graph.constFloatWidth", CVarValue::Float32(90.0f));
    Test::RequireDefault("editor.graph.constFloat2Width", CVarValue::Float32(140.0f));
    Test::RequireDefault("editor.graph.constFloat4Width", CVarValue::Float32(220.0f));
    Test::RequireDefault("editor.graph.constParamRangeWidth", CVarValue::Float32(120.0f));
    Test::RequireDefault("editor.graph.paramNameFieldWidth", CVarValue::Float32(110.0f));
    Test::RequireDefault("editor.graph.passNameFieldWidth", CVarValue::Float32(120.0f));
    Test::RequireDefault("editor.graph.swizzleFieldWidth", CVarValue::Float32(70.0f));
    Test::RequireDefault("editor.graph.nodePreviewMinPx", CVarValue::Float32(96.0f));
    Test::RequireDefault("editor.graph.dragSpeed", CVarValue::Float32(0.01f));
    Test::RequireDefault("editor.graph.rangeDragSpeed", CVarValue::Float32(0.05f));
    CHECK(CVarRegistry::Get().Find("editor.shader.dragSpeed").IsStale());   // the old spelling is gone

    // A published override reaches the width the canvas sets.
    const Test::ScopedCodeRung wide("editor.graph.constPinNeutralWidth2", CVarValue::Float32(150.0f));
    CHECK(Editor::GraphPinNeutralWidth(Arcane::Settings<Editor::GraphCanvasSettings>(), 2) == 150.0f);
}
