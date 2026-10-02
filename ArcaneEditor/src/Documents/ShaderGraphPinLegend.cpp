#include "Documents/ShaderGraphPinLegend.hpp"

#include "Documents/ShaderGraphPinTypes.hpp"   // PinPaintFor / PinWidthName / kPinDynamicColor
#include "Widgets/EditorFonts.hpp"
#include "Widgets/EditorTheme.hpp"
#include "Widgets/GraphCanvasStyle.hpp"        // kGraphPinOuterRingGap / kGraphPinOuterRingWidth
#include "Widgets/GraphLegend.hpp"             // the shared legend box chrome
#include "Widgets/GraphPinDot.hpp"             // DrawGraphPinDot -- the canvas's own pin painter

#include <Arcane/Config/CVarDecl.hpp>          // ARC_CVAR_RANGED (s2.4)

#include <algorithm>
#include <cmath>
#include <optional>

namespace Arcane::Editor
{
    namespace
    {
        ARC_CVAR_RANGED("editor.graph.showPinLegend", "editor", Bool, ::Arcane::CVarValue::Bool(true),
                        std::nullopt, std::nullopt, ::Arcane::CVarFlags::Archive,
                        "Show the shader graph's pin colour legend (false folds it to a ? chip).");

        // The shader canvas's kPinDotRadius at zoom 1: the key shows the dot at
        // the size the canvas draws it unzoomed.
        constexpr float kLegendDotRadius = 4.0f;
        // One swatch slot, wide enough for a dot WITH its outer ring, so the
        // ringed entry lines up with the plain ones.
        constexpr float kLegendDotSlot =
            2.0f * (kLegendDotRadius + kGraphPinOuterRingGap + kGraphPinOuterRingWidth);
        constexpr float kLegendDotPairGap = 3.0f;   // the filled/hollow pair's two dots
        constexpr float kLegendRowGap     = 4.0f;
        constexpr const char* kRingText   = "ring = adapts to its input";
        constexpr const char* kFilledText = "filled = wired, hollow = unwired";
        constexpr const char* kChipText   = "?";
        constexpr int kTypeWidths[] = { 1, 2, 4, 0 };   // float, float2, float4, dynamic

        struct LegendFont
        {
            LegendFont() { ImGui::PushFont(GetEditorFonts().interRegular, kGraphLegendFontPx); }
            ~LegendFont() { ImGui::PopFont(); }
            LegendFont(const LegendFont&) = delete;
            LegendFont& operator=(const LegendFont&) = delete;
        };

        // Content widths of the two rows (the caller holds a LegendFont).
        float TypeRowWidth()
        {
            float w = 0.0f;
            for (int i = 0; i < IM_ARRAYSIZE(kTypeWidths); ++i)
                w += (i > 0 ? kGraphLegendEntryGap : 0.0f) + kLegendDotSlot + kGraphLegendSwatchGap +
                     ImGui::CalcTextSize(PinWidthName(kTypeWidths[i])).x;
            return w;
        }
        float RuleRowWidth()
        {
            return kLegendDotSlot + kGraphLegendSwatchGap + ImGui::CalcTextSize(kRingText).x +
                   kGraphLegendEntryGap + kLegendDotSlot * 2.0f + kLegendDotPairGap + kGraphLegendSwatchGap +
                   ImGui::CalcTextSize(kFilledText).x;
        }
        ImVec2 BoxSize(bool expanded)   // the caller holds a LegendFont
        {
            const float lineH = std::floor(ImGui::GetTextLineHeight());
            if (!expanded)
            {
                const float side = lineH + kGraphLegendPadY * 2.0f;
                return ImVec2(side, side);
            }
            const float contentW = std::max(TypeRowWidth(), RuleRowWidth());
            return ImVec2(std::floor(contentW) + kGraphLegendPadX * 2.0f,
                          lineH * 2.0f + kLegendRowGap + kGraphLegendPadY * 2.0f);
        }

        // One dot in the swatch slot whose left edge is `x`, centred on `midY`,
        // through the canvas's painter: the ring is the canvas's ring.
        void SwatchDot(ImDrawList* dl, float x, float midY, const GraphPinPaint& paint, bool filled)
        {
            const ImVec2 c(std::floor(x + kLegendDotSlot * 0.5f) + 0.5f, std::floor(midY) + 0.5f);
            DrawGraphPinDot(dl, c, paint.color, Theme::kChrome, kLegendDotRadius, filled,
                            paint.adapts ? &kPinDynamicColor : nullptr);
        }
    }

    bool GraphPinLegendShown()
    {
        const CVarRegistry& reg = CVarRegistry::Get();
        const std::optional<CVarValue> v = reg.Get(reg.Find("editor.graph.showPinLegend"));
        return (v && v->type == CVarType::Bool) ? v->AsBool() : true;
    }

    void SetGraphPinLegendShown(bool shown)
    {
        CVarRegistry& reg = CVarRegistry::Get();
        (void)reg.Set(reg.Find("editor.graph.showPinLegend"), CVarValue::Bool(shown), SetBy::User, "editor");
    }

    ImVec2 GraphPinLegendBoxSize(bool expanded)
    {
        const LegendFont font;
        return BoxSize(expanded);
    }

    bool GraphPinLegendInteract(const ImVec2& canvasMin, const ImVec2& canvasSize)
    {
        const bool shown = GraphPinLegendShown();
        const ImVec2 size = GraphPinLegendBoxSize(shown);
        const ImVec2 cursor = ImGui::GetCursorScreenPos();   // the canvas's layout is not ours to move
        ImGui::SetCursorScreenPos(GraphLegendBoxMin(canvasMin, canvasSize, size.y));
        if (ImGui::InvisibleButton("##pinlegend", size))
            SetGraphPinLegendShown(!shown);
        const bool hovered = ImGui::IsItemHovered();
        ImGui::SetItemTooltip("%s", shown ? "Pin colours -- click to fold this legend"
                                          : "Pin colours -- click to show the legend");
        ImGui::SetCursorScreenPos(cursor);
        return hovered;
    }

    void DrawGraphPinLegend(const ImVec2& canvasMin, const ImVec2& canvasSize, bool hovered)
    {
        const bool shown = GraphPinLegendShown();
        const LegendFont font;
        const ImVec2 size = BoxSize(shown);
        const ImVec2 boxMin = GraphLegendBoxMin(canvasMin, canvasSize, size.y);
        const ImVec2 boxMax(boxMin.x + size.x, boxMin.y + size.y);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        DrawGraphLegendBox(dl, boxMin, boxMax, hovered ? Theme::kTextDim : Theme::kBorder);

        const ImU32 textCol = ImGui::GetColorU32(Theme::kTextDim);
        const float lineH = std::floor(ImGui::GetTextLineHeight());
        if (!shown)
        {
            const ImVec2 t = ImGui::CalcTextSize(kChipText);
            dl->AddText(ImVec2(std::floor(boxMin.x + (size.x - t.x) * 0.5f), boxMin.y + kGraphLegendPadY),
                        textCol, kChipText);
            return;
        }

        // Row 1: the four types, each dot painted as a wired pin of that width.
        float y = boxMin.y + kGraphLegendPadY;
        float x = boxMin.x + kGraphLegendPadX;
        for (int i = 0; i < IM_ARRAYSIZE(kTypeWidths); ++i)
        {
            if (i > 0)
                x += kGraphLegendEntryGap;
            x = std::floor(x);
            SwatchDot(dl, x, y + lineH * 0.5f, PinPaintFor(kTypeWidths[i], 0), true);
            x += kLegendDotSlot + kGraphLegendSwatchGap;
            const char* word = PinWidthName(kTypeWidths[i]);
            dl->AddText(ImVec2(x, y), textCol, word);
            x += ImGui::CalcTextSize(word).x;
        }

        // Row 2: the ring (a dynamic pin resolved to float4), then filled vs hollow.
        y += lineH + kLegendRowGap;
        x = boxMin.x + kGraphLegendPadX;
        SwatchDot(dl, x, y + lineH * 0.5f, PinPaintFor(0, 4), true);
        x += kLegendDotSlot + kGraphLegendSwatchGap;
        dl->AddText(ImVec2(x, y), textCol, kRingText);
        x = std::floor(x + ImGui::CalcTextSize(kRingText).x + kGraphLegendEntryGap);
        const GraphPinPaint plain = PinPaintFor(0, 0);
        SwatchDot(dl, x, y + lineH * 0.5f, plain, true);
        x += kLegendDotSlot + kLegendDotPairGap;
        SwatchDot(dl, x, y + lineH * 0.5f, plain, false);
        x += kLegendDotSlot + kGraphLegendSwatchGap;
        dl->AddText(ImVec2(x, y), textCol, kFilledText);
    }
}
