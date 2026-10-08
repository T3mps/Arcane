#include "Settings/GraphCanvasSettings.hpp"
#include "Widgets/GraphZoomLevels.hpp"   // ParseZoomLevels (declared there, defined here beside the default)

#include <Arcane/Base/Log.hpp>
#include <Arcane/Config/Settings.hpp>
#include <Arcane/Reflection.hpp>

#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <system_error>

namespace Arcane::Editor
{
    ARC_REFLECT_TYPE(GraphCanvasSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.graph", SettingScope::PreferencesMachine, ApplyMode::Live, Audience::Editor)
        ARC_REFLECT_FIELD(GraphCanvasSettings, zoomLevels)
            ARC_REFLECT_ATTR(DisplayName, "Zoom stops") ARC_REFLECT_ATTR(Category, "Zoom")
            ARC_REFLECT_ATTR(Apply, ApplyMode::Restart) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Keywords, "zoom levels table wheel")
            ARC_REFLECT_ATTR(Tooltip, "The view scales a node canvas's wheel zoom steps through: space-separated, positive, "
                                      "strictly ascending (1 = 1:1). A malformed list falls back to the default. "
                                      "A canvas reads it when it is created.")
        ARC_REFLECT_FIELD(GraphCanvasSettings, nodeRounding)
            ARC_REFLECT_ATTR(DisplayName, "Node corner rounding") ARC_REFLECT_ATTR(Category, "Nodes")
            ARC_REFLECT_ATTR(Range, 0.0, 16.0) ARC_REFLECT_ATTR(Apply, ApplyMode::Restart)
            ARC_REFLECT_ATTR(Tooltip, "Corner radius of a graph node, in canvas units at zoom 1.")
        ARC_REFLECT_FIELD(GraphCanvasSettings, nodeBorderWidth)
            ARC_REFLECT_ATTR(DisplayName, "Node border width") ARC_REFLECT_ATTR(Category, "Nodes")
            ARC_REFLECT_ATTR(Range, 0.0, 6.0) ARC_REFLECT_ATTR(Apply, ApplyMode::Restart)
            ARC_REFLECT_ATTR(Tooltip, "Width of a graph node's outline, in canvas units at zoom 1.")
        ARC_REFLECT_FIELD(GraphCanvasSettings, nodeBorderHoverWidth)
            ARC_REFLECT_ATTR(DisplayName, "Hovered node border width") ARC_REFLECT_ATTR(Category, "Nodes")
            ARC_REFLECT_ATTR(Range, 0.0, 6.0) ARC_REFLECT_ATTR(Apply, ApplyMode::Restart)
            ARC_REFLECT_ATTR(Tooltip, "Width of the outline of the node under the cursor.")
        ARC_REFLECT_FIELD(GraphCanvasSettings, nodeBorderSelectedWidth)
            ARC_REFLECT_ATTR(DisplayName, "Selected node border width") ARC_REFLECT_ATTR(Category, "Nodes")
            ARC_REFLECT_ATTR(Range, 0.0, 6.0) ARC_REFLECT_ATTR(Apply, ApplyMode::Restart)
            ARC_REFLECT_ATTR(Tooltip, "Width of a selected node's outline.")
        ARC_REFLECT_FIELD(GraphCanvasSettings, wireThickness)
            ARC_REFLECT_ATTR(DisplayName, "Wire thickness") ARC_REFLECT_ATTR(Category, "Wires")
            ARC_REFLECT_ATTR(Range, 0.5, 6.0)
            ARC_REFLECT_ATTR(Tooltip, "Thickness of a graph wire, in canvas units at zoom 1; also how wide its grab area is.")
        ARC_REFLECT_FIELD(GraphCanvasSettings, pinSegments)
            ARC_REFLECT_ATTR(DisplayName, "Pin dot segments") ARC_REFLECT_ATTR(Category, "Pins")
            ARC_REFLECT_ATTR(Range, 6.0, 48.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "How many segments a pin dot and its rings are drawn with.")
        ARC_REFLECT_FIELD(GraphCanvasSettings, wireHighlight)
            ARC_REFLECT_ATTR(DisplayName, "Wire highlight") ARC_REFLECT_ATTR(Category, "Wires")
            ARC_REFLECT_ATTR(Range, 0.0, 1.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "How far a hovered or selected wire's colour lifts toward white (0 = not at all, 1 = white).")
        ARC_REFLECT_FIELD(GraphCanvasSettings, wireSegmentsMin)
            ARC_REFLECT_ATTR(DisplayName, "Wire segments (min)") ARC_REFLECT_ATTR(Category, "Wires")
            ARC_REFLECT_ATTR(Range, 2.0, 256.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Fewest straight segments a hand-drawn wire is walked in, however short it is on screen.")
        ARC_REFLECT_FIELD(GraphCanvasSettings, wireSegmentsMax)
            ARC_REFLECT_ATTR(DisplayName, "Wire segments (max)") ARC_REFLECT_ATTR(Category, "Wires")
            ARC_REFLECT_ATTR(Range, 2.0, 256.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Most straight segments a hand-drawn wire is walked in, however long it is on screen.")
        ARC_REFLECT_FIELD(GraphCanvasSettings, wireSegmentsPxPer)
            ARC_REFLECT_ATTR(DisplayName, "Pixels per wire segment") ARC_REFLECT_ATTR(Category, "Wires")
            ARC_REFLECT_ATTR(Range, 1.0, 64.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Screen pixels of wire per straight segment, between the min and max segment counts.")
        ARC_REFLECT_FIELD(GraphCanvasSettings, nodeHeaderGap)
            ARC_REFLECT_ATTR(DisplayName, "Node header gap") ARC_REFLECT_ATTR(Category, "Nodes")
            ARC_REFLECT_ATTR(Range, 0.0, 16.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Space between a shader graph node's title band and its first row, in canvas units "
                                      "(scaled by the UI scale).")
        ARC_REFLECT_FIELD(GraphCanvasSettings, cullGuardBand)
            ARC_REFLECT_ATTR(DisplayName, "Cull guard band") ARC_REFLECT_ATTR(Category, "Nodes")
            ARC_REFLECT_ATTR(Range, 0.0, 1.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "How far past the visible canvas (a fraction of its size, on every side) a shader graph "
                                      "node is still drawn in full instead of as a stand-in.")
        ARC_REFLECT_FIELD(GraphCanvasSettings, pinDotRadius)
            ARC_REFLECT_ATTR(DisplayName, "Pin dot radius") ARC_REFLECT_ATTR(Category, "Pins")
            ARC_REFLECT_ATTR(Range, 2.0, 10.0)
            ARC_REFLECT_ATTR(Tooltip, "Radius of a shader graph pin dot, in canvas units at zoom 1 (also the node page's pin chips).")
        ARC_REFLECT_FIELD(GraphCanvasSettings, shiftAddsToSelection)
            ARC_REFLECT_ATTR(DisplayName, "Shift adds to selection") ARC_REFLECT_ATTR(Category, "Selection")
            ARC_REFLECT_ATTR(Apply, ApplyMode::Restart)
            ARC_REFLECT_ATTR(Tooltip, "Shift+click and Shift+drag add nodes to the selection (Unreal's modifiers). Off, "
                                      "Shift+drag selects only comment boxes. A canvas reads it when it is created.")
        ARC_REFLECT_FIELD(GraphCanvasSettings, fitMaxZoom)
            ARC_REFLECT_ATTR(DisplayName, "Frame-to-fit max zoom") ARC_REFLECT_ATTR(Category, "Frame to fit")
            ARC_REFLECT_ATTR(Range, 0.1, 2.0)
            ARC_REFLECT_ATTR(Tooltip, "Largest zoom a graph's frame-to-fit may pick (1.0 = never magnify).")
        ARC_REFLECT_FIELD(GraphCanvasSettings, fitMinZoom)
            ARC_REFLECT_ATTR(DisplayName, "Frame-to-fit min zoom") ARC_REFLECT_ATTR(Category, "Frame to fit")
            ARC_REFLECT_ATTR(Range, 0.1, 2.0)
            ARC_REFLECT_ATTR(Tooltip, "Smallest zoom a graph's frame-to-fit may pick; a graph too big for it frames its "
                                      "centre (0.1 = the zoom table's floor, no extra limit).")
        ARC_REFLECT_FIELD(GraphCanvasSettings, showPinLegend)
            ARC_REFLECT_ATTR(DisplayName, "Show pin legend") ARC_REFLECT_ATTR(Category, "Pins")
            ARC_REFLECT_ATTR(Tooltip, "Show the shader graph's pin colour legend (false folds it to a ? chip).")
        ARC_REFLECT_FIELD(GraphCanvasSettings, nodePadding)
            ARC_REFLECT_ATTR(DisplayName, "Node padding") ARC_REFLECT_ATTR(Category, "Nodes")
            ARC_REFLECT_ATTR(Range, 0.0, 24.0) ARC_REFLECT_ATTR(Apply, ApplyMode::Restart)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Keywords, "inset margin")
            ARC_REFLECT_ATTR(Tooltip, "Space between a shader graph node's edge and its content, in canvas units at zoom 1: "
                                      "x = left and right, y = top and bottom.")
        ARC_REFLECT_FIELD(GraphCanvasSettings, nodePreviewMinPx)
            ARC_REFLECT_ATTR(DisplayName, "Node preview minimum size") ARC_REFLECT_ATTR(Category, "Nodes")
            ARC_REFLECT_ATTR(Range, 32.0, 512.0)
            ARC_REFLECT_ATTR(Tooltip, "Smallest side, in canvas units, of the Output node's preview thumbnail.")
        ARC_REFLECT_FIELD(GraphCanvasSettings, dragSpeed)
            ARC_REFLECT_ATTR(DisplayName, "Value drag speed") ARC_REFLECT_ATTR(Category, "Node widgets")
            ARC_REFLECT_ATTR(Range, 0.0001, 1.0)
            ARC_REFLECT_ATTR(Tooltip, "How much a constant or parameter default changes per pixel dragged on a graph node.")
        ARC_REFLECT_FIELD(GraphCanvasSettings, rangeDragSpeed)
            ARC_REFLECT_ATTR(DisplayName, "Range drag speed") ARC_REFLECT_ATTR(Category, "Node widgets")
            ARC_REFLECT_ATTR(Range, 0.0001, 1.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "How much a parameter's min/max range changes per pixel dragged on a graph node.")
        ARC_REFLECT_FIELD(GraphCanvasSettings, constPinNeutralWidth1)
            ARC_REFLECT_ATTR(DisplayName, "Unwired pin field (1 lane)") ARC_REFLECT_ATTR(Category, "Node widgets")
            ARC_REFLECT_ATTR(Range, 8.0, 1024.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Width, in canvas units, of an unwired float input's value field on a shader graph node.")
        ARC_REFLECT_FIELD(GraphCanvasSettings, constPinNeutralWidth2)
            ARC_REFLECT_ATTR(DisplayName, "Unwired pin field (2 lanes)") ARC_REFLECT_ATTR(Category, "Node widgets")
            ARC_REFLECT_ATTR(Range, 8.0, 1024.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Width, in canvas units, of an unwired float2 input's value field on a shader graph node.")
        ARC_REFLECT_FIELD(GraphCanvasSettings, constPinNeutralWidth4)
            ARC_REFLECT_ATTR(DisplayName, "Unwired pin field (3-4 lanes)") ARC_REFLECT_ATTR(Category, "Node widgets")
            ARC_REFLECT_ATTR(Range, 8.0, 1024.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Width, in canvas units, of an unwired float3/float4 input's value field on a shader graph node.")
        ARC_REFLECT_FIELD(GraphCanvasSettings, constFloatWidth)
            ARC_REFLECT_ATTR(DisplayName, "Constant field (1 lane)") ARC_REFLECT_ATTR(Category, "Node widgets")
            ARC_REFLECT_ATTR(Range, 8.0, 1024.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Width, in canvas units, of a Constant node's or a parameter default's float field.")
        ARC_REFLECT_FIELD(GraphCanvasSettings, constFloat2Width)
            ARC_REFLECT_ATTR(DisplayName, "Constant field (2 lanes)") ARC_REFLECT_ATTR(Category, "Node widgets")
            ARC_REFLECT_ATTR(Range, 8.0, 1024.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Width, in canvas units, of a Constant node's or a parameter default's float2 field.")
        ARC_REFLECT_FIELD(GraphCanvasSettings, constFloat4Width)
            ARC_REFLECT_ATTR(DisplayName, "Constant field (3-4 lanes)") ARC_REFLECT_ATTR(Category, "Node widgets")
            ARC_REFLECT_ATTR(Range, 8.0, 1024.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Width, in canvas units, of a Constant node's or a parameter default's float4 / color field.")
        ARC_REFLECT_FIELD(GraphCanvasSettings, constParamRangeWidth)
            ARC_REFLECT_ATTR(DisplayName, "Parameter range field") ARC_REFLECT_ATTR(Category, "Node widgets")
            ARC_REFLECT_ATTR(Range, 8.0, 1024.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Width, in canvas units, of a parameter node's min/max range field.")
        ARC_REFLECT_FIELD(GraphCanvasSettings, paramNameFieldWidth)
            ARC_REFLECT_ATTR(DisplayName, "Parameter name field") ARC_REFLECT_ATTR(Category, "Node widgets")
            ARC_REFLECT_ATTR(Range, 8.0, 1024.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Width, in canvas units, of a parameter node's name field.")
        ARC_REFLECT_FIELD(GraphCanvasSettings, passNameFieldWidth)
            ARC_REFLECT_ATTR(DisplayName, "Pass name field") ARC_REFLECT_ATTR(Category, "Node widgets")
            ARC_REFLECT_ATTR(Range, 8.0, 1024.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Width, in canvas units, of a pass node's name field in the pass chain.")
        ARC_REFLECT_FIELD(GraphCanvasSettings, swizzleFieldWidth)
            ARC_REFLECT_ATTR(DisplayName, "Swizzle mask field") ARC_REFLECT_ATTR(Category, "Node widgets")
            ARC_REFLECT_ATTR(Range, 8.0, 1024.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Width, in canvas units, of a Swizzle node's mask field.")
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_TYPE(GraphLodSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.graph.lod", SettingScope::PreferencesMachine, ApplyMode::Live, Audience::Editor)
        ARC_REFLECT_TYPE_ATTR(Flags, CVarFlags::Dev)
        ARC_REFLECT_FIELD(GraphLodSettings, lowestMax)
            ARC_REFLECT_ATTR(DisplayName, "Lowest detail up to") ARC_REFLECT_ATTR(Range, 0.1, 2.0)
            ARC_REFLECT_ATTR(Tooltip, "Largest canvas zoom drawn at the lowest detail (everything simplified).")
        ARC_REFLECT_FIELD(GraphLodSettings, lowMax)
            ARC_REFLECT_ATTR(DisplayName, "Low detail up to") ARC_REFLECT_ATTR(Range, 0.1, 2.0)
            ARC_REFLECT_ATTR(Tooltip, "Largest canvas zoom drawn at low detail (unreadable text is dropped).")
        ARC_REFLECT_FIELD(GraphLodSettings, mediumMax)
            ARC_REFLECT_ATTR(DisplayName, "Medium detail up to") ARC_REFLECT_ATTR(Range, 0.1, 2.0)
            ARC_REFLECT_ATTR(Tooltip, "Largest canvas zoom drawn at medium detail (text hard to read, still drawn).")
        ARC_REFLECT_FIELD(GraphLodSettings, defaultMax)
            ARC_REFLECT_ATTR(DisplayName, "Default detail up to") ARC_REFLECT_ATTR(Range, 0.1, 2.0)
            ARC_REFLECT_ATTR(Tooltip, "Largest canvas zoom drawn at default detail; above it the canvas is fully zoomed in.")
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_TYPE(GraphGridSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.graph.grid", SettingScope::PreferencesMachine, ApplyMode::Live, Audience::Editor)
        ARC_REFLECT_TYPE_ATTR(Flags, CVarFlags::Dev)
        ARC_REFLECT_FIELD(GraphGridSettings, zoomExponent)
            ARC_REFLECT_ATTR(DisplayName, "Zoom exponent") ARC_REFLECT_ATTR(Range, 0.1, 1.0)
            ARC_REFLECT_ATTR(Tooltip, "How strongly the canvas grid follows zoom: its period scales by zoom^k (1 = welded to the "
                                      "nodes, lower = steadier).")
        ARC_REFLECT_FIELD(GraphGridSettings, baseSpacing)
            ARC_REFLECT_ATTR(DisplayName, "Base spacing") ARC_REFLECT_ATTR(Range, 4.0, 128.0)
            ARC_REFLECT_ATTR(Tooltip, "Grid spacing in canvas units at zoom 1, before the octave snap.")
        ARC_REFLECT_FIELD(GraphGridSettings, minorTargetPx)
            ARC_REFLECT_ATTR(DisplayName, "Minor line target spacing") ARC_REFLECT_ATTR(Range, 4.0, 128.0)
            ARC_REFLECT_ATTR(Tooltip, "On-screen spacing (px, scaled by the UI scale) the minor grid lines are kept near.")
        ARC_REFLECT_FIELD(GraphGridSettings, majorEvery)
            ARC_REFLECT_ATTR(DisplayName, "Major line every") ARC_REFLECT_ATTR(Range, 2.0, 16.0)
            ARC_REFLECT_ATTR(Tooltip, "Minor lines per major line: a power of two (2, 4, 8 or 16); another value rounds down to one.")
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_TYPE(GraphPinRingSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.graph.pinRing", SettingScope::PreferencesMachine, ApplyMode::Live,
                              Audience::Editor)
        ARC_REFLECT_TYPE_ATTR(Flags, CVarFlags::Dev)
        ARC_REFLECT_FIELD(GraphPinRingSettings, width)
            ARC_REFLECT_ATTR(DisplayName, "Ring width") ARC_REFLECT_ATTR(Range, 0.25, 8.0)
            ARC_REFLECT_ATTR(Keywords, "pin outline hollow")
            ARC_REFLECT_ATTR(Tooltip, "Line weight of an unwired (hollow) pin dot's ring, in canvas units at zoom 1.")
        ARC_REFLECT_FIELD(GraphPinRingSettings, outerGap)
            ARC_REFLECT_ATTR(DisplayName, "Outer ring gap") ARC_REFLECT_ATTR(Range, 0.0, 16.0)
            ARC_REFLECT_ATTR(Keywords, "pin halo dynamic adapts")
            ARC_REFLECT_ATTR(Tooltip, "How far outside a pin dot the \"adapts to its input\" halo ring sits, in canvas "
                                      "units at zoom 1.")
        ARC_REFLECT_FIELD(GraphPinRingSettings, outerWidth)
            ARC_REFLECT_ATTR(DisplayName, "Outer ring width") ARC_REFLECT_ATTR(Range, 0.25, 8.0)
            ARC_REFLECT_ATTR(Keywords, "pin halo dynamic adapts")
            ARC_REFLECT_ATTR(Tooltip, "Line weight of the \"adapts to its input\" halo ring, in canvas units at zoom 1.")
    ARC_END_REFLECT_TYPE()

    ARC_SETTINGS(GraphCanvasSettings);
    ARC_SETTINGS(GraphPinRingSettings);
    ARC_SETTINGS(GraphLodSettings);
    ARC_SETTINGS(GraphGridSettings);

    namespace
    {
        // Strict: every space-separated token a whole finite float, positive and
        // above the one before it.
        bool TryParseZoomLevels(std::string_view text, std::vector<float>& out)
        {
            out.clear();
            std::size_t i = 0;
            while (i < text.size())
            {
                while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
                if (i == text.size()) break;
                std::size_t end = i;
                while (end < text.size() && text[end] != ' ' && text[end] != '\t') ++end;
                const char* first = text.data() + i;
                const char* last  = text.data() + end;
                float v = 0.0f;
                const std::from_chars_result r = std::from_chars(first, last, v);
                if (r.ec != std::errc{} || r.ptr != last) return false;   // garbage, or trailing garbage
                if (!std::isfinite(v) || v <= 0.0f) return false;
                if (!out.empty() && v <= out.back()) return false;         // strictly ascending
                out.push_back(v);
                i = end;
            }
            return !out.empty();
        }

        const std::vector<float>& DefaultZoomLevels()
        {
            static const std::vector<float> stops = []
            {
                std::vector<float> s;
                (void)TryParseZoomLevels(GraphCanvasSettings{}.zoomLevels, s);
                return s;
            }();
            return stops;
        }
    }

    namespace
    {
        // editor.graph.nodePadding's boot value (CaptureGraphNodePaddingAtBoot).
        // Main thread only, like every other editor UI read.
        std::optional<CVarVec2> g_nodePaddingAtBoot;
    }

    void CaptureGraphNodePaddingAtBoot()
    {
        g_nodePaddingAtBoot = Settings<GraphCanvasSettings>().nodePadding;
    }

    CVarVec2 GraphNodePaddingAtBoot()
    {
        // A host that never ran the editor's boot (a test opening a shader
        // document directly) latches the value at the first read instead.
        if (!g_nodePaddingAtBoot)
            CaptureGraphNodePaddingAtBoot();
        return *g_nodePaddingAtBoot;
    }

    std::vector<float> ParseZoomLevels(std::string_view text)
    {
        std::vector<float> stops;
        if (TryParseZoomLevels(text, stops))
            return stops;
        ARC_WARN("editor.graph.zoomLevels: \"{}\" is not a list of positive, strictly ascending zoom stops -- "
                 "using the default table",
                 std::string(text));
        return DefaultZoomLevels();
    }
}
