#pragma once

// GraphFit: frame-to-fit for the editor's node canvases (node-page phase s4.5).
// ed::NavigateToContent adds a margin but has NO zoom cap, so a small graph
// magnifies into the blurred-glyph range (AssetGraphPanel.cpp:1320-1328). This
// caps the fit at editor.graph.fitMaxZoom (default 1.0 = never magnify) and
// floors it at the zoom table's first stop (kZoomLevels[0] = 0.1), so a huge
// graph frames its centre instead of zooming past the wheel's range.
//
// Header = imgui.h only; GraphFit.cpp alone includes imgui_node_editor_internal.h.
// The SELECTION IS NEVER READ OR WRITTEN: the fit works from content bounds,
// never a select-all (T3's node-page selection mirror depends on this).

#include <imgui.h>

namespace Arcane::Editor
{
    struct GraphRect { ImVec2 min, max; };

    // PURE: resize `content` about its centre so fitting it into `viewPx` lands
    // in [kZoomLevels[0], maxZoom]. Already in range: unchanged. Zero-extent axes
    // never divide by zero. A zero-size view returns `content` unchanged.
    [[nodiscard]] GraphRect ComputeGraphFitRect(const GraphRect& content, ImVec2 viewPx, float maxZoom);

    // Between ed::Begin/ed::End of the CURRENT editor, AFTER this frame's nodes
    // were submitted (a node is live only once BeginNode ran this frame): fit
    // every node. false = nothing to fit (no current editor, no live node,
    // empty bounds, zero-size view) and nothing changed. Duration 0 lands now.
    bool GraphFitToContent(float maxZoom, float durationSeconds = 0.0f);

    // editor.graph.fitMaxZoom's published value; 1.0 if it is absent.
    [[nodiscard]] float GraphFitMaxZoom();
}
