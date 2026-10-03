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

#include <cstdint>

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
