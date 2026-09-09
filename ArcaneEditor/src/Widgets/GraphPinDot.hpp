#pragma once

// The port dot every node canvas draws its pins as -- the PAINT only.
//
// The reading is Shader Graph's: FILLED when a wire is attached, a hollow ring
// when not, so "this port carries something" is visible without tracing the
// wire. Three draw calls, and they were the same three on both canvases.
//
// PAINT ONLY, deliberately: the two canvases place their dots by completely
// different means. The shader editor lets ImGui lay the row out (cursor +
// Dummy, then it derives the centre and hands it back so the pin row can anchor
// its wire pivot off it); the Graph lens computes the node's whole geometry
// before submission and puts the dot on the node's edge by hand, contributing
// nothing to layout. Those are opposite contracts and neither is shared -- only
// the circle pair is.
//
// Its own header, sibling to CanvasPopupScope.hpp / GraphZoomLevels.hpp /
// GraphCanvasStyle.hpp / GraphWire.hpp, on the same one-named-concern rule and
// the same refusal to grow EditorWidgets.hpp into the canvas family.

#include "Widgets/GraphCanvasStyle.hpp"   // kGraphPinSegments / kGraphPinRingWidth

#include <imgui.h>

namespace Arcane::Editor
{
    // `centre` and `radius` are in whatever space the caller's draw list is in
    // (canvas space inside ed::Begin/End on both canvases today).
    //
    // The RADIUS is a parameter because it is the one value the two canvases
    // genuinely differ on -- 4.0 on the shader canvas, 4.5 on the Graph lens for
    // spec §11.2's 9px across. So is `bodyColor`: an unconnected dot is punched
    // out of the NODE BODY it sits on, and the two canvases' node bodies are
    // different tones by a recorded ruling. The segment count and the ring
    // weight are not parameters: those were identical literals on both sides and
    // now have one definition (GraphCanvasStyle.hpp).
    inline void DrawGraphPinDot(ImDrawList* dl, const ImVec2& centre,
                                const ImVec4& color, const ImVec4& bodyColor,
                                float radius, bool connected)
    {
        const ImU32 col = ImGui::GetColorU32(color);
        if (connected)
        {
            dl->AddCircleFilled(centre, radius, col, kGraphPinSegments);
        }
        else
        {
            dl->AddCircleFilled(centre, radius,
                                ImGui::GetColorU32(bodyColor), kGraphPinSegments);
            dl->AddCircle(centre, radius, col, kGraphPinSegments, kGraphPinRingWidth);
        }
    }
}
