#pragma once

// A node canvas's LEGEND chrome -- the box both canvas legends sit in, paint
// only. The Assets panel's Graph lens legend (AssetGraphPanel.cpp, transcribed
// from OptionD.dc.html's `<!-- legend -->` block) set these values; the shader
// graph's pin legend (ShaderGraphPinLegend.cpp, T3-D1) reads the same ones, so
// the editor's two canvas keys look like one family:
//   position: absolute; left: 12px; bottom: 12px;
//   background: #191919 (Theme::kChrome); border: 1px solid #0d0d0d
//   (Theme::kBorder); padding: 5px 10px; font-size: 13px; color: #737373
//   (Theme::kTextDim); entries 14px apart, swatch-to-text 6px.
// What goes INSIDE the box -- the swatches and their words -- stays each
// canvas's own: a wire key and a pin key share nothing past the chrome.
//
// Chrome, NOT a node: both callers draw it after ed::End in SCREEN space, so
// it does not pan, zoom or sort against the graph. Its own header, sibling to
// GraphPinDot.hpp / GraphCanvasStyle.hpp, on the one-named-concern rule.

#include "Widgets/EditorTheme.hpp"   // kChrome / kBorder / kTextDim
#include "Widgets/UiMetrics.hpp"

#include <imgui.h>

#include <cmath>

namespace Arcane::Editor
{
    // The CSS pixels above at UI scale 1.0 / font 16; they scale with
    // editor.ui.scale and editor.ui.fontSize (settings S4, UiMetrics.hpp).
    [[nodiscard]] inline float GraphLegendInset() noexcept     { return Ui::Px(12.0f); }
    [[nodiscard]] inline float GraphLegendPadX() noexcept      { return Ui::Px(10.0f); }
    [[nodiscard]] inline float GraphLegendPadY() noexcept      { return Ui::Px(5.0f); }
    [[nodiscard]] inline float GraphLegendEntryGap() noexcept  { return Ui::Px(14.0f); }
    [[nodiscard]] inline float GraphLegendSwatchGap() noexcept { return Ui::Px(6.0f); }
    [[nodiscard]] inline float GraphLegendFontPx() noexcept    { return Ui::FontPx(13.0f); }

    // The box's min corner for a box `boxH` tall, pinned GraphLegendInset() in
    // from the canvas's bottom-left corner. SNAPPED TO WHOLE PIXELS: a 1px
    // border (and any 2px rule inside) is what a half-pixel origin visibly
    // softens. Safe to snap, unlike anything inside the canvas: the legend is
    // chrome in SCREEN space, with no zoom to make the rounding lie.
    [[nodiscard]] inline ImVec2 GraphLegendBoxMin(const ImVec2& canvasMin, const ImVec2& canvasSize, float boxH)
    {
        const float inset = GraphLegendInset();
        return ImVec2(std::floor(canvasMin.x + inset),
                      std::floor(canvasMin.y + canvasSize.y - inset - boxH));
    }

    // The box itself: kChrome fill, a 1px kBorder edge (`border` overrides
    // the edge colour -- the pin legend brightens it while hovered, its one
    // interactive affordance).
    inline void DrawGraphLegendBox(ImDrawList* dl, const ImVec2& boxMin, const ImVec2& boxMax,
                                   const ImVec4& border = Theme::kBorder)
    {
        dl->AddRectFilled(boxMin, boxMax, ImGui::GetColorU32(Theme::kChrome));
        dl->AddRect(boxMin, boxMax, ImGui::GetColorU32(border));
    }
}
