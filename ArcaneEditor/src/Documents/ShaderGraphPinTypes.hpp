#pragma once

// The shader graph's pin TYPE vocabulary, explained (T3-D1, 2026-10-01 desk):
// the palette, the paint rule for a pin on a resolved graph, and the type /
// tooltip words. The canvas dots, the wires, the node page's type dots and the
// canvas legend all read this ONE header, so none of them can disagree with
// another -- and the legend can never contradict the canvas it explains.
//
// The palette maps 1:1 onto the value types: float / float2 / float4 /
// dynamic (GraphPinDesc::width 1 / 2 / 4 / 0, MaterialGraph.hpp). There is no
// float3 and no texture pin type in a material graph. Unity's convention
// mapped onto Arcane's pin domain; Unity's vec3-yellow has no counterpart here
// (no 3-lane pin), so that row of the reference table is absent rather than
// mapped onto something it does not mean. Its texture row is spent on the PASS
// canvas (kPinTextureColor, ShaderEditorDocument.cpp), not here.
//
// A DYNAMIC pin whose node has resolved (ResolveGraphNodeWidths -- the same
// resolution codegen emits from) is painted in the colour of the width it
// resolved to, plus a thin grey outer ring that says "adapts to its input".
// An unresolved dynamic pin stays plain grey. Pure: no ImGui context needed.

#include <imgui.h>

#include <string>
#include <string_view>

namespace Arcane::Editor
{
    // DISPLAY-REFERRED, like the rest of the canvas palette (ImGui draws
    // post-tonemap into the backbuffer).
    inline constexpr ImVec4 kPinScalarColor  = ImVec4(0.502f, 0.808f, 1.0f,   1.0f); // pale azure
    inline constexpr ImVec4 kPinVec2Color    = ImVec4(0.549f, 0.863f, 0.549f, 1.0f); // green
    inline constexpr ImVec4 kPinVec4Color    = ImVec4(0.941f, 0.549f, 0.863f, 1.0f); // magenta
    inline constexpr ImVec4 kPinDynamicColor = ImVec4(0.745f, 0.745f, 0.765f, 1.0f); // gray

    [[nodiscard]] constexpr ImVec4 PinColorForWidth(int width) noexcept
    {
        switch (width)
        {
            case 1:  return kPinScalarColor;
            case 2:  return kPinVec2Color;
            case 4:  return kPinVec4Color;
            default: return kPinDynamicColor;   // 0 = adapts to what feeds it
        }
    }

    // How one pin is painted. `adapts` = draw the grey outer ring (the pin is
    // dynamic AND resolved); `color` is the fill/inner ring either way.
    struct GraphPinPaint
    {
        ImVec4 color = kPinDynamicColor;
        bool   adapts = false;
    };

    // `declaredWidth` = GraphPinDesc::width (0 = dynamic); `resolvedWidth` =
    // the node's resolution for that side (GraphNodeWidths::inputs/outputs,
    // 0 = unresolved). Fixed pins ignore the resolution.
    [[nodiscard]] constexpr GraphPinPaint PinPaintFor(int declaredWidth, int resolvedWidth) noexcept
    {
        if (declaredWidth != 0)
            return { PinColorForWidth(declaredWidth), false };
        if (resolvedWidth > 0)
            return { PinColorForWidth(resolvedWidth), true };
        return { kPinDynamicColor, false };
    }

    // THE width -> word map: 1/2/4 -> float/float2/float4, anything else
    // (0 = dynamic) -> "dynamic".
    [[nodiscard]] constexpr const char* PinWidthName(int width) noexcept
    {
        return width == 1 ? "float" : width == 2 ? "float2" : width == 4 ? "float4" : "dynamic";
    }

    // The type word a pin shows (node page rows, canvas tooltip):
    // "float" / "float2" / "float4" for fixed pins, "dynamic (now float4)" for
    // a resolved dynamic pin, "dynamic (unresolved)" for an unresolved one.
    [[nodiscard]] inline std::string PinTypeText(int declaredWidth, int resolvedWidth)
    {
        if (declaredWidth != 0)
            return PinWidthName(declaredWidth);
        if (resolvedWidth > 0)
            return std::string("dynamic (now ") + PinWidthName(resolvedWidth) + ")";
        return "dynamic (unresolved)";
    }

    // The wiring line: an input names its source ("wired from Param 'tint'.out"),
    // an output counts its consumers ("wired to 2 inputs"); "not connected"
    // either way when there is no wire. `source` is ignored for outputs,
    // `fanout` for inputs.
    [[nodiscard]] inline std::string PinWiringText(bool isInput, std::string_view source, int fanout)
    {
        if (isInput)
            return source.empty() ? std::string("not connected") : "wired from " + std::string(source);
        if (fanout <= 0)
            return "not connected";
        return "wired to " + std::to_string(fanout) + (fanout == 1 ? " input" : " inputs");
    }

    // The canvas pin tooltip: three lines -- the pin name, its type word, its
    // wiring.
    [[nodiscard]] inline std::string PinTooltipText(std::string_view name, int declaredWidth, int resolvedWidth,
                                                    bool isInput, std::string_view source, int fanout)
    {
        return std::string(name) + "\n" + PinTypeText(declaredWidth, resolvedWidth) + "\n" +
               PinWiringText(isInput, source, fanout);
    }
}
