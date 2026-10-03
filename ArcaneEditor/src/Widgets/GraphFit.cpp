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

namespace Arcane::Editor
{
    ARC_CVAR_RANGED("editor.graph.fitMaxZoom", "editor", Float32, CVarValue::Float32(1.0f),
                    CVarValue::Float32(0.1f), CVarValue::Float32(2.0f), CVarFlags::Archive,
                    "Largest zoom a graph's frame-to-fit may pick (1.0 = never magnify).");

    float GraphFitMaxZoom()
    {
        const CVarRegistry& reg = CVarRegistry::Get();
        const std::optional<CVarValue> v = reg.Get(reg.Find("editor.graph.fitMaxZoom"));
        return (v && v->type == CVarType::Float32) ? v->AsFloat32() : 1.0f;
    }

    GraphRect ComputeGraphFitRect(const GraphRect& content, ImVec2 viewPx, float maxZoom)
    {
        if (viewPx.x <= 0.0f || viewPx.y <= 0.0f)
            return content;
        const float floorZoom = kZoomLevels[0];
        const float cap = std::max(maxZoom, floorZoom);
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
        else if (fit < floorZoom)
        {
            cw = std::min(cw, viewPx.x / floorZoom);
            ch = std::min(ch, viewPx.y / floorZoom);
        }
        return { ImVec2(centre.x - cw * 0.5f, centre.y - ch * 0.5f),
                 ImVec2(centre.x + cw * 0.5f, centre.y + ch * 0.5f) };
    }

    bool GraphFitToContent(float maxZoom, float durationSeconds)
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
        const GraphRect fitted = ComputeGraphFitRect({ bounds.Min, bounds.Max }, view, maxZoom);
        // zoomIn = WithMargin: c_NavigationZoomMargin (imgui_node_editor.cpp:144, :3543) only
        // LOWERS the zoom, so the cap holds; at the floor the fit may land a little under 0.1.
        // Not snapped to kZoomLevels: SetViewRect takes CalcCenterView's scale as-is
        // (:3635-3640); the next wheel step snaps through MatchZoom.
        editor->NavigateTo(ImRect(fitted.min, fitted.max), /*zoomIn*/ true, std::max(0.0f, durationSeconds));
        return true;
    }
}
