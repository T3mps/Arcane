#pragma once

// GraphFit: frame-to-fit for the editor's node canvases (node-page phase s4.5).
// ed::NavigateToContent adds a margin but has NO zoom cap, so a small graph
// magnifies into the blurred-glyph range (AssetGraphPanel.cpp:1320-1328). This
// caps the fit at editor.graph.fitMaxZoom (default 1.0 = never magnify) and
// floors it at editor.graph.fitMinZoom (FIT-MINZOOM, user 2026-10-03; never
// under the zoom table's first stop, kZoomLevels[0] = 0.1), so a graph too big
// to fit at a readable zoom frames its CENTRE and the user pans for the rest.
// The floor holds for the zoom the navigation LANDS at, margin included
// (GraphFitLandedZoom); a floor above the cap wins, and every fit lands on it.
//
// Header = imgui.h only; GraphFit.cpp alone includes imgui_node_editor_internal.h.
// The SELECTION IS NEVER READ OR WRITTEN: the fit works from content bounds,
// never a select-all (T3's node-page selection mirror depends on this).

#include <imgui.h>

#include <cstdint>

namespace Arcane::Editor
{
    struct GraphRect { ImVec2 min, max; };

    // The zoom band a fit may land in. The effective floor is
    // max(kZoomLevels[0], minZoom); the effective cap is max(maxZoom, floor),
    // so a min above the max wins (both collapse onto the min).
    struct GraphFitZoomRange
    {
        float minZoom = 0.0f;
        float maxZoom = 1.0f;
    };

    // PURE: the view scale the node editor lands at when it navigates to
    // `target` with ZoomMode::WithMargin (what GraphFitToContent issues): the
    // rect grows by c_NavigationZoomMargin (0.1) of its longer side, half on
    // each edge (imgui_node_editor.cpp:144, :3556-3560), then fits the view
    // (CalcCenterView, imgui_canvas.cpp:239-258). 0 for a zero-size view or rect.
    [[nodiscard]] float GraphFitLandedZoom(const GraphRect& target, ImVec2 viewPx);

    // PURE: the rect to hand GraphFitToContent's WithMargin navigation for
    // `content` in `viewPx`, about the content's centre:
    //  - a fit above the cap grows to the cap's frame (the margin only lowers it);
    //  - a fit that would LAND under the floor becomes the view-aspect frame
    //    that lands exactly AT the floor (GraphFitLandedZoom), cropping the
    //    content to its centre;
    //  - otherwise `content` unchanged.
    // Zero-extent axes never divide by zero. A zero-size view returns `content`.
    [[nodiscard]] GraphRect ComputeGraphFitRect(const GraphRect& content, ImVec2 viewPx, GraphFitZoomRange zoom);

    // Between ed::Begin/ed::End of the CURRENT editor, AFTER this frame's nodes
    // were submitted (a node is live only once BeginNode ran this frame): fit
    // every node. false = nothing to fit (no current editor, no live node,
    // empty bounds, zero-size view) and nothing changed. Duration 0 lands now.
    bool GraphFitToContent(GraphFitZoomRange zoom, float durationSeconds = 0.0f);

    // editor.graph.fitMaxZoom's published value; 1.0 if it is absent.
    [[nodiscard]] float GraphFitMaxZoom();

    // editor.graph.fitMinZoom's published value; its default if it is absent.
    [[nodiscard]] float GraphFitMinZoom();

    // { GraphFitMinZoom(), GraphFitMaxZoom() }: what every fit-on-open passes.
    [[nodiscard]] GraphFitZoomRange GraphFitZoomRangeFromCVars();

    // CanvasNavLatch: a one-shot canvas navigation (the fit-on-open, a Problems
    // focus) that CONFIRMS it landed. The node editor's Begin answers a canvas
    // RESIZE by re-centring the view of the PREVIOUS draw and dropping any
    // navigation in flight (imgui_node_editor.cpp:1221-1254: previousVisibleRect
    // is read from the canvas, which only receives a navigated view at the NEXT
    // Begin). So a navigation issued at canvas size S has landed only once a
    // later draw still sees S and its animation had time to finish; a draw that
    // sees another size means it was discarded, and the caller re-issues it.
    // This converges once the layout holds, whatever the transient pattern.
    //
    // Call Update ONCE PER DRAW while armed, after the canvas's node loop, with
    // ed::GetScreenSize() -- also on draws that cannot issue (canIssue=false),
    // so a resize there is still seen. PURE: no ImGui or node-editor state.
    class CanvasNavLatch
    {
    public:
        void Arm() noexcept { m_pending = true; m_issued = false; m_issues = 0; }
        void Disarm() noexcept { m_pending = false; m_issued = false; }
        [[nodiscard]] bool Pending() const noexcept { return m_pending; }
        // Issues since the last Arm (1 = the first, >1 = re-issues after a
        // discarding resize). Disarm keeps it, so a caller can tell one ARMING's
        // navigation from its re-issues without a flag of its own.
        [[nodiscard]] std::uint32_t Issues() const noexcept { return m_issues; }

        // true = (re-)issue the navigation NOW. A navigation issued earlier is
        // confirmed (the latch disarms) on a draw whose size equals the size it
        // was issued at, once settleSeconds (its animation length; 0 for a jump)
        // have passed since; a different size re-issues it.
        [[nodiscard]] bool Update(ImVec2 canvasSize, double now, float settleSeconds, bool canIssue = true) noexcept
        {
            if (!m_pending)
                return false;
            if (m_issued)
            {
                if (canvasSize.x == m_issuedAt.x && canvasSize.y == m_issuedAt.y)
                {
                    if (now - m_issuedTime >= static_cast<double>(settleSeconds))
                        Disarm();
                    return false;
                }
                m_issued = false;   // the resize discarded it
            }
            if (!canIssue)
                return false;
            m_issued = true;
            ++m_issues;
            m_issuedAt = canvasSize;
            m_issuedTime = now;
            return true;
        }

    private:
        ImVec2 m_issuedAt{};
        double m_issuedTime = 0.0;
        std::uint32_t m_issues = 0;
        bool   m_pending = false;
        bool   m_issued = false;
    };
}
