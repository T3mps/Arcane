#pragma once

// The shader graph's per-CATEGORY node header fill (node page spec 2026-09-30
// s5.1.4, drafting pick 9.28 #23; critique Shader #7: every header was the
// same grey). The canvas title band (DrawGraphNode only -- the pass canvas
// and comment boxes keep the neutral) and the node page's category chip
// both read this ONE table. Every entry keeps kNodeTitleText (#cecfd4) at
// >= 4.5:1 (ShaderGraphCategoryColorsTest); the Output sink is the one red.
#include <Arcane/Material/MaterialGraph.hpp>
#include <imgui.h>

namespace Arcane::Editor
{
    [[nodiscard]] constexpr ImVec4 GraphCategoryHeaderColor(Arcane::GraphNodeCategory c) noexcept
    {
        constexpr auto rgb = [](unsigned v) constexpr
        {
            return ImVec4(((v >> 16) & 0xFFu) / 255.0f, ((v >> 8) & 0xFFu) / 255.0f, (v & 0xFFu) / 255.0f, 1.0f);
        };
        switch (c)
        {
            case Arcane::GraphNodeCategory::Input:      return rgb(0x24384a);
            case Arcane::GraphNodeCategory::Math:       return rgb(0x26402f);
            case Arcane::GraphNodeCategory::Vector:     return rgb(0x3a2a4a);
            case Arcane::GraphNodeCategory::Procedural: return rgb(0x4a3a22);
            case Arcane::GraphNodeCategory::Output:     return rgb(0x5a2626);   // the sink stands out
            case Arcane::GraphNodeCategory::Utility:    return rgb(0x2e2e33);
            case Arcane::GraphNodeCategory::Uncategorized:
            default:                                    return rgb(0x232326);   // today's kNodeTitleColor
        }
    }
}
