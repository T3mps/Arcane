#include "Documents/ShaderGraphPinLegend.hpp"

#include "Documents/ShaderGraphPinTypes.hpp"   // PinPaintFor / PinWidthName / PinColorForWidth
#include "Widgets/EditorFonts.hpp"
#include "Widgets/EditorTheme.hpp"
#include "Widgets/GraphCanvasStyle.hpp"        // kGraphPinOuterRingGap / kGraphPinOuterRingWidth
#include "Widgets/GraphLegend.hpp"             // the shared legend box chrome
#include "Widgets/GraphPinDot.hpp"             // DrawGraphPinDot -- the canvas's own pin painter

#include <Arcane/Config/CVarDecl.hpp>          // ARC_CVAR (settings spec s4.3)

#include <algorithm>
#include <cmath>

namespace Arcane::Editor
{
    namespace
    {
        ARC_CVAR(cvar_showPinLegend, "editor.graph.showPinLegend", bool, true,
                 .flags = ::Arcane::CVarFlags::Archive, .audience = ::Arcane::Audience::Editor,
                 .scope = ::Arcane::SettingScope::PreferencesMachine,
                 .help = "Show the shader graph's pin colour legend (false folds it to a ? chip).");

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
            LegendFont() { ImGui::PushFont(GetEditorFonts().interRegular, GraphLegendFontPx()); }
            ~LegendFont() { ImGui::PopFont(); }
            LegendFont(const LegendFont&) = delete;
            LegendFont& operator=(const LegendFont&) = delete;
        };

        // Content widths of the two rows (the caller holds a LegendFont).
        float TypeRowWidth()
        {
            float w = 0.0f;
            for (int i = 0; i < IM_ARRAYSIZE(kTypeWidths); ++i)
                w += (i > 0 ? GraphLegendEntryGap() : 0.0f) + kLegendDotSlot + GraphLegendSwatchGap() +
                     ImGui::CalcTextSize(PinWidthName(kTypeWidths[i])).x;
            return w;
        }
        float RuleRowWidth()
        {
            return kLegendDotSlot + GraphLegendSwatchGap() + ImGui::CalcTextSize(kRingText).x +
                   GraphLegendEntryGap() + kLegendDotSlot * 2.0f + kLegendDotPairGap + GraphLegendSwatchGap() +
                   ImGui::CalcTextSize(kFilledText).x;
        }
        ImVec2 BoxSize(bool expanded)   // the caller holds a LegendFont
        {
            const float lineH = std::floor(ImGui::GetTextLineHeight());
            if (!expanded)
            {
                const float side = lineH + GraphLegendPadY() * 2.0f;
                return ImVec2(side, side);
            }
            const float contentW = std::max(TypeRowWidth(), RuleRowWidth());
            return ImVec2(std::floor(contentW) + GraphLegendPadX() * 2.0f,
                          lineH * 2.0f + kLegendRowGap + GraphLegendPadY() * 2.0f);
        }

        // One dot in the swatch slot whose left edge is `x`, centred on `midY`,
        // through the canvas's painter: the ring is the canvas's ring.
        void SwatchDot(ImDrawList* dl, float x, float midY, const GraphPinPaint& paint, bool filled)
        {
            const ImVec2 c(std::floor(x + kLegendDotSlot * 0.5f) + 0.5f, std::floor(midY) + 0.5f);
            const ImVec4 ring = PinColorForWidth(0);   // the dynamic grey, as on the canvas
            DrawGraphPinDot(dl, c, paint.color, Theme::kChrome, kLegendDotRadius, filled,
                            paint.adapts ? &ring : nullptr);
        }
    }

    bool GraphPinLegendShown()
    {
        return cvar_showPinLegend.Get();
    }

    void SetGraphPinLegendShown(bool shown)
    {
        (void)CVarRegistry::Get().Set(cvar_showPinLegend.Handle(), CVarValue::Bool(shown), SetBy::User, "editor");
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
            dl->AddText(ImVec2(std::floor(boxMin.x + (size.x - t.x) * 0.5f), boxMin.y + GraphLegendPadY()),
                        textCol, kChipText);
            return;
        }

        // Row 1: the four types, each dot painted as a wired pin of that width.
        float y = boxMin.y + GraphLegendPadY();
        float x = boxMin.x + GraphLegendPadX();
        for (int i = 0; i < IM_ARRAYSIZE(kTypeWidths); ++i)
        {
            if (i > 0)
                x += GraphLegendEntryGap();
            x = std::floor(x);
            SwatchDot(dl, x, y + lineH * 0.5f, PinPaintFor(kTypeWidths[i], 0), true);
            x += kLegendDotSlot + GraphLegendSwatchGap();
            const char* word = PinWidthName(kTypeWidths[i]);
            dl->AddText(ImVec2(x, y), textCol, word);
            x += ImGui::CalcTextSize(word).x;
        }

        // Row 2: the ring (a dynamic pin resolved to float4), then filled vs hollow.
        y += lineH + kLegendRowGap;
        x = boxMin.x + GraphLegendPadX();
        SwatchDot(dl, x, y + lineH * 0.5f, PinPaintFor(0, 4), true);
        x += kLegendDotSlot + GraphLegendSwatchGap();
        dl->AddText(ImVec2(x, y), textCol, kRingText);
        x = std::floor(x + ImGui::CalcTextSize(kRingText).x + GraphLegendEntryGap());
        const GraphPinPaint plain = PinPaintFor(0, 0);
        SwatchDot(dl, x, y + lineH * 0.5f, plain, true);
        x += kLegendDotSlot + kLegendDotPairGap;
        SwatchDot(dl, x, y + lineH * 0.5f, plain, false);
        x += kLegendDotSlot + GraphLegendSwatchGap();
        dl->AddText(ImVec2(x, y), textCol, kFilledText);
    }
}
