#pragma once

// The shader graph's per-CATEGORY node header fill (node page spec 2026-09-30
// s5.1.4, drafting pick 9.28 #23; critique Shader #7: every header was the
// same grey). The canvas title band (DrawGraphNode only -- the pass canvas
// and comment boxes keep the neutral) and the node page's category chip
// both read this ONE table. Every entry keeps the node title text (#cecfd4)
// at >= 4.5:1 (ShaderGraphCategoryColorsTest, and the theme page's contrast
// report for any preset); the Output sink is the one red. The colours are
// theme cvars, editor.theme.graph.category.* (settings S6-27).
#include "Settings/GraphThemeSettings.hpp"

#include <Arcane/Material/MaterialGraph.hpp>
#include <imgui.h>

namespace Arcane::Editor
{
    [[nodiscard]] inline ImVec4 GraphCategoryHeaderColor(Arcane::GraphNodeCategory c)
    {
        using S = GraphCategoryThemeSettings;
        switch (c)
        {
            case Arcane::GraphNodeCategory::Input:      return GraphCategoryThemeColor(&S::input);
            case Arcane::GraphNodeCategory::Math:       return GraphCategoryThemeColor(&S::math);
            case Arcane::GraphNodeCategory::Vector:     return GraphCategoryThemeColor(&S::vector);
            case Arcane::GraphNodeCategory::Procedural: return GraphCategoryThemeColor(&S::procedural);
            case Arcane::GraphNodeCategory::Output:     return GraphCategoryThemeColor(&S::output);   // the sink stands out
            case Arcane::GraphNodeCategory::Utility:    return GraphCategoryThemeColor(&S::utility);
            case Arcane::GraphNodeCategory::Uncategorized:
            default:                                    return GraphCategoryThemeColor(&S::uncategorized);   // the node title band's #232326
        }
    }
}
