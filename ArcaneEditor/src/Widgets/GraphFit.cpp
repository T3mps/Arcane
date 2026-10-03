// imgui_internal.h (pulled in by the node editor's internal header) refuses a TU
// whose imgui.h came first without IMGUI_DEFINE_MATH_OPERATORS (imgui_internal.h:114),
// so the define and the internal header lead -- ahead of this file's own header.
// C4996 is silenced around it alone: the internal header pulls in the vendored
// crude_json.h, whose std::aligned_storage member (crude_json.h:150) is
// deprecated in C++23 (STL4034) -- vendored code, not ours to edit.
#define IMGUI_DEFINE_MATH_OPERATORS
#pragma warning(push)
#pragma warning(disable : 4996)
#include <imgui_node_editor_internal.h>   // Detail::EditorContext::GetContentBounds / NavigateTo
#pragma warning(pop)

#include "Widgets/GraphFit.hpp"
#include "Widgets/GraphZoomLevels.hpp"    // kZoomLevels[0]: the fit floor

#include <Arcane/Config/CVarDecl.hpp>     // ARC_CVAR_RANGED (s2.4)

#include <algorithm>
#include <optional>
#include <string_view>

namespace Arcane::Editor
{
    ARC_CVAR_RANGED("editor.graph.fitMaxZoom", "editor", Float32, CVarValue::Float32(1.0f),
                    CVarValue::Float32(0.1f), CVarValue::Float32(2.0f), CVarFlags::Archive,
                    "Largest zoom a graph's frame-to-fit may pick (1.0 = never magnify).");

    // FIT-MINZOOM (user, 2026-10-03). Default 0.5: the smallest zoom stop at
    // which a node's title and pin labels still read as text at 1080p. The
    // canvas draws the 16 px editor font scaled by the zoom (no re-raster), so
    // 0.5 is an 8 px em; the tiers' text gate (pin labels need > kLodLowMax,
    // GraphNodeLod.hpp) only opens at 0.375 (a 6 px em, unreadable), and the
    // 1080p logo_showcase captures (FIT-MINZOOM report) put 0.5 as the first
    // stop whose titles and labels are legible. Range = the zoom table's.
    ARC_CVAR_RANGED("editor.graph.fitMinZoom", "editor", Float32, CVarValue::Float32(0.5f),
                    CVarValue::Float32(0.1f), CVarValue::Float32(2.0f), CVarFlags::Archive,
                    "Smallest zoom a graph's frame-to-fit may pick; a graph too big for it frames its "
                    "centre (0.1 = the zoom table's floor, no extra limit).");

    namespace
    {
        // Mirrors the vendored c_NavigationZoomMargin (imgui_node_editor.cpp:144,
        // file-static there): a fact of the library, not a tunable.
        constexpr float kNavigationZoomMargin = 0.1f;

        float ReadFloatCVar(std::string_view name, float fallback)
        {
            const CVarRegistry& reg = CVarRegistry::Get();
            const std::optional<CVarValue> v = reg.Get(reg.Find(name));
            return (v && v->type == CVarType::Float32) ? v->AsFloat32() : fallback;
        }
    }

    float GraphFitMaxZoom() { return ReadFloatCVar("editor.graph.fitMaxZoom", 1.0f); }
    float GraphFitMinZoom() { return ReadFloatCVar("editor.graph.fitMinZoom", 0.5f); }

    GraphFitZoomRange GraphFitZoomRangeFromCVars()
    {
        return { .minZoom = GraphFitMinZoom(), .maxZoom = GraphFitMaxZoom() };
    }

    float GraphFitLandedZoom(const GraphRect& target, ImVec2 viewPx)
    {
        const float w = target.max.x - target.min.x;
        const float h = target.max.y - target.min.y;
        if (viewPx.x <= 0.0f || viewPx.y <= 0.0f || w <= 0.0f || h <= 0.0f)
            return 0.0f;
        const float grow = kNavigationZoomMargin * std::max(w, h);   // ImRect::Expand(extend * margin * 0.5f): both edges
        return std::min(viewPx.x / (w + grow), viewPx.y / (h + grow));
    }

    GraphRect ComputeGraphFitRect(const GraphRect& content, ImVec2 viewPx, GraphFitZoomRange zoom)
    {
        if (viewPx.x <= 0.0f || viewPx.y <= 0.0f)
            return content;
        const float floorZoom = std::max(kZoomLevels[0], zoom.minZoom);
        const float cap = std::max(zoom.maxZoom, floorZoom);   // a min above the max wins
        const ImVec2 centre((content.min.x + content.max.x) * 0.5f, (content.min.y + content.max.y) * 0.5f);
        float cw = std::max(0.0f, content.max.x - content.min.x);
        float ch = std::max(0.0f, content.max.y - content.min.y);

        // A zero-extent axis must not constrain the fit: give it the view's
        // aspect of the other axis (a point gets the cap's frame outright).
        if (cw <= 0.0f && ch <= 0.0f) { cw = viewPx.x / cap; ch = viewPx.y / cap; }
        else if (cw <= 0.0f)          cw = ch * viewPx.x / viewPx.y;
        else if (ch <= 0.0f)          ch = cw * viewPx.y / viewPx.x;

        const float fit = std::min(viewPx.x / cw, viewPx.y / ch);
        if (fit > cap)
        {
            cw = std::max(cw, viewPx.x / cap);
            ch = std::max(ch, viewPx.y / cap);
        }

        // The floor holds for the zoom the navigation LANDS at: WithMargin grows
        // the rect, so a plain fit just above the floor can land under it. Then
        // hand over the view-aspect frame that lands exactly AT the floor. For
        // a (s*vx/f, s*vy/f) rect the landed zoom is (f/s) * v/(v + m*V), with v/V
        // the view's short/long side (the short axis binds), so s = v/(v + m*V).
        if (GraphFitLandedZoom({ ImVec2(0.0f, 0.0f), ImVec2(cw, ch) }, viewPx) < floorZoom)
        {
            const float shortSide = std::min(viewPx.x, viewPx.y);
            const float longSide = std::max(viewPx.x, viewPx.y);
            const float s = shortSide / (shortSide + kNavigationZoomMargin * longSide);
            cw = s * viewPx.x / floorZoom;
            ch = s * viewPx.y / floorZoom;
        }
        return { ImVec2(centre.x - cw * 0.5f, centre.y - ch * 0.5f),
                 ImVec2(centre.x + cw * 0.5f, centre.y + ch * 0.5f) };
    }

    bool GraphFitToContent(GraphFitZoomRange zoom, float durationSeconds)
    {
        namespace ed = ax::NodeEditor;
        auto* editor = reinterpret_cast<ed::Detail::EditorContext*>(ed::GetCurrentEditor());
        if (!editor)
            return false;
        // Live nodes only (GetBounds skips !m_IsLive): call AFTER this frame's
        // BeginNode/EndNode pass, or the frame's nodes have not gone live yet.
        const ImRect bounds = editor->GetContentBounds();
        if (ImRect_IsEmpty(bounds))
            return false;
        const ImVec2 view = ed::GetScreenSize();
        if (view.x <= 0.0f || view.y <= 0.0f)
            return false;
        const GraphRect fitted = ComputeGraphFitRect({ bounds.Min, bounds.Max }, view, zoom);
        // zoomIn = WithMargin: c_NavigationZoomMargin (imgui_node_editor.cpp:144, :3556-3560) only
        // LOWERS the zoom, so the cap holds; ComputeGraphFitRect pre-shrinks a floored
        // frame by exactly that margin, so the floor holds too (GraphFitLandedZoom).
        // Not snapped to kZoomLevels: SetViewRect takes CalcCenterView's scale as-is
        // (:3635-3640); the next wheel step snaps through MatchZoom.
        editor->NavigateTo(ImRect(fitted.min, fitted.max), /*zoomIn*/ true, std::max(0.0f, durationSeconds));
        return true;
    }
}
