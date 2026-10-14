#pragma once

// Arcane::Editor::ApplyZoomLevels -- the node-canvas zoom table, shared.
//
// Every ed::Config the editor creates should install this table (rather than
// let the vendored library fall through to its own default) so that "how far
// can the wheel zoom" is a navigation FEEL, not a per-canvas accident. The
// table itself is the setting editor.graph.zoomLevels (settings S6-34,
// GraphCanvasSettings); see ZOOM STOPS below for why it replaces the library's.
//
// Its own header, sibling to CanvasPopupScope.hpp rather than folded into it:
// both are node-editor-coupled widgets shared by every canvas in the editor
// (the shader editor's graph canvas and pass canvas, and the Assets panel's
// Graph lens), and neither belongs in EditorWidgets.hpp/.cpp -- that
// vocabulary is imgui.h plus Astra::Range only, and pulling
// imgui_node_editor.h into it would couple every editor widget to the node
// editor (CanvasPopupScope.hpp's own note, repeated here because it applies
// unchanged). Zoom is a second, unrelated concern from CanvasPopupScope's one
// named rule (popup placement), so it gets its own file rather than growing
// that one's scope.
//
// This table used to live twice: once for real in ShaderEditorDocument.cpp,
// and a second time as an omission -- the asset graph's canvas created its
// ed::Config without ever calling the equivalent, so it silently inherited
// the vendored library's 0.1-8.0 default and its 12-14px canvas text blurred
// under 8x bilinear magnification of a baked glyph. Hoisting the one
// definition here (rather than hand-copying it into AssetGraphPanel.cpp, the
// exact smell this arc already paid down once for the guid predicate) is what
// lets both files share it instead of drifting. See docs/specs/
// 2026-09-06-asset-manager-redesign-design.md §19, 2026-09-09 entry for the
// bug/root-cause/fix record.

#include "Settings/GraphCanvasSettings.hpp"   // GraphCanvasSettings::zoomLevels

#include <Arcane/Config/Settings.hpp>

#include <imgui_node_editor.h>

#include <string_view>
#include <vector>

namespace Arcane::Editor
{
    namespace ed = ax::NodeEditor;

    // -------------------------------------------------------------------
    // ZOOM STOPS -- Unreal's graph-editor table, ported exactly.
    //
    // The default of editor.graph.zoomLevels is FFixedZoomLevelsContainer's
    // 20 entries, verbatim and in order (vendored UE at Arcane/.example/
    // UnrealEngine-release/Engine/Source/Editor/GraphEditor/Private/
    // SNodePanel.cpp:53-75): 0.1 .. 2.0. UE calls the number ZoomAmount and it
    // is a VIEW SCALE: 1.000 is 1:1, 2.000 draws everything twice as large. The
    // vendored node editor's ed::Config::CustomZoomLevels is the same quantity
    // -- the table feeds NavigateAction::m_ZoomLevels (imgui_node_editor.cpp:
    // 3333) and m_Zoom is assigned view.Scale (:3639) -- so the numbers
    // transfer with no conversion. (ed::GetCurrentZoom, by contrast, hands back
    // the RECIPROCAL; see each consumer's own ViewScale()/GraphViewScale().)
    //
    // Replacing the vendored default table (0.1 .. 8.0,
    // imgui_node_editor.cpp:3309-3312) is the point of doing this: 8x
    // magnification has no use on a node graph, UE's stops are much finer
    // in the readable band, and the LOD tiers (editor.graph.lod.*) are
    // defined against exactly these numbers.

    // The stops in `text`: space-separated, every one finite and positive,
    // strictly ascending. Anything else (empty, unsorted, a duplicate, zero or
    // negative, non-finite, trailing garbage) WARNs and answers the DEFAULT
    // table, so a canvas always gets a usable one. Defined in
    // Settings/GraphCanvasSettings.cpp, beside the default string.
    [[nodiscard]] std::vector<float> ParseZoomLevels(std::string_view text);

    // The published table, parsed. Read at canvas creation (the setting is
    // Restart: an open canvas keeps the table it was created with).
    [[nodiscard]] inline std::vector<float> CurrentZoomLevels()
    {
        return ParseZoomLevels(Settings<GraphCanvasSettings>().zoomLevels);
    }

    // CustomZoomLevels is an ImVector, so the table is pushed in rather than
    // aggregate-initialized. The caller's `cfg` may die immediately after
    // CreateEditor: the editor holds a Config BY VALUE
    // (imgui_node_editor_internal.h:1486) and its ctor deep-copies through
    // ImVector::operator= (imgui_node_editor.cpp:5785-5789), so the pointer
    // NavigateAction caches at :3333 is into the editor's own copy.
    //
    // inline: this header is included by more than one TU (today
    // ShaderEditorDocument.cpp and AssetGraphPanel.cpp), so a plain function
    // definition here would violate ODR without it.
    inline void ApplyZoomLevels(ed::Config& cfg)
    {
        const std::vector<float> stops = CurrentZoomLevels();
        cfg.CustomZoomLevels.reserve(static_cast<int>(stops.size()));
        for (float z : stops)
            cfg.CustomZoomLevels.push_back(z);
    }
}
