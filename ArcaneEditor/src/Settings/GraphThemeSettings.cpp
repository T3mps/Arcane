#include "Settings/GraphThemeSettings.hpp"

#include <Arcane/Config/Settings.hpp>
#include <Arcane/Reflection.hpp>

#include <array>
#include <type_traits>

namespace Arcane::Editor
{
    ARC_REFLECT_TYPE(GraphThemeSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.theme.graph", SettingScope::PreferencesMachine, ApplyMode::Live, Audience::Editor,
                             "Appearance/Graph colours")
        ARC_REFLECT_FIELD(GraphThemeSettings, nodeBody)      ARC_REFLECT_ATTR(DisplayName, "Node body")        ARC_REFLECT_ATTR(Category, "Nodes") ARC_REFLECT_ATTR(Apply, ApplyMode::Restart) ARC_REFLECT_ATTR(Tooltip, "The shader graph's node body. Applies when a graph document is reopened.")
        ARC_REFLECT_FIELD(GraphThemeSettings, nodeTitle)     ARC_REFLECT_ATTR(DisplayName, "Node title band")  ARC_REFLECT_ATTR(Category, "Nodes") ARC_REFLECT_ATTR(Tooltip, "The title band of a pass-canvas node (Scene, Output and each pass).")
        ARC_REFLECT_FIELD(GraphThemeSettings, nodeBorder)    ARC_REFLECT_ATTR(DisplayName, "Node border")      ARC_REFLECT_ATTR(Category, "Nodes") ARC_REFLECT_ATTR(Apply, ApplyMode::Restart) ARC_REFLECT_ATTR(Tooltip, "The shader graph's node outline. Applies when a graph document is reopened.")
        ARC_REFLECT_FIELD(GraphThemeSettings, nodeTitleText) ARC_REFLECT_ATTR(DisplayName, "Node title text")  ARC_REFLECT_ATTR(Category, "Nodes") ARC_REFLECT_ATTR(Tooltip, "A node's title text on its title band.")
        ARC_REFLECT_FIELD(GraphThemeSettings, nodeBadgeText) ARC_REFLECT_ATTR(DisplayName, "Node error title") ARC_REFLECT_ATTR(Category, "Nodes") ARC_REFLECT_ATTR(Tooltip, "The title of a node with an error, shown with its (!) badge.")
        ARC_REFLECT_FIELD(GraphThemeSettings, groupBg)       ARC_REFLECT_ATTR(DisplayName, "Comment box fill") ARC_REFLECT_ATTR(Category, "Nodes") ARC_REFLECT_ATTR(Apply, ApplyMode::Restart) ARC_REFLECT_ATTR(Tooltip, "The wash inside a comment (group) box. Applies when a graph document is reopened.")
        ARC_REFLECT_FIELD(GraphThemeSettings, groupBorder)   ARC_REFLECT_ATTR(DisplayName, "Comment box border") ARC_REFLECT_ATTR(Category, "Nodes") ARC_REFLECT_ATTR(Apply, ApplyMode::Restart) ARC_REFLECT_ATTR(Tooltip, "The outline of a comment (group) box. Applies when a graph document is reopened.")
        ARC_REFLECT_FIELD(GraphThemeSettings, pinTexture)    ARC_REFLECT_ATTR(DisplayName, "Render target pin") ARC_REFLECT_ATTR(Category, "Pins") ARC_REFLECT_ATTR(Tooltip, "The pass canvas's pins and wires: every one carries a full-frame render target.")
        ARC_REFLECT_FIELD(GraphThemeSettings, pinScalar)     ARC_REFLECT_ATTR(DisplayName, "float pin")        ARC_REFLECT_ATTR(Category, "Pins") ARC_REFLECT_ATTR(Tooltip, "A float pin and its wire, on the canvas, the node page and the pin legend.")
        ARC_REFLECT_FIELD(GraphThemeSettings, pinVec2)       ARC_REFLECT_ATTR(DisplayName, "float2 pin")       ARC_REFLECT_ATTR(Category, "Pins") ARC_REFLECT_ATTR(Tooltip, "A float2 pin and its wire, on the canvas, the node page and the pin legend.")
        ARC_REFLECT_FIELD(GraphThemeSettings, pinVec4)       ARC_REFLECT_ATTR(DisplayName, "float4 pin")       ARC_REFLECT_ATTR(Category, "Pins") ARC_REFLECT_ATTR(Tooltip, "A float4 pin and its wire, on the canvas, the node page and the pin legend.")
        ARC_REFLECT_FIELD(GraphThemeSettings, pinDynamic)    ARC_REFLECT_ATTR(DisplayName, "Dynamic pin")      ARC_REFLECT_ATTR(Category, "Pins") ARC_REFLECT_ATTR(Tooltip, "An unresolved dynamic pin, and the ring around a resolved one (it adapts to its input).")
        ARC_REFLECT_FIELD(GraphThemeSettings, gridMinor)     ARC_REFLECT_ATTR(DisplayName, "Grid (minor)")     ARC_REFLECT_ATTR(Category, "Canvas") ARC_REFLECT_ATTR(Tooltip, "The fine lines of both graph canvases' grid; alpha is the octave's peak strength.")
        ARC_REFLECT_FIELD(GraphThemeSettings, gridMajor)     ARC_REFLECT_ATTR(DisplayName, "Grid (major)")     ARC_REFLECT_ATTR(Category, "Canvas") ARC_REFLECT_ATTR(Tooltip, "The coarse lines of both graph canvases' grid; alpha is the octave's peak strength.")
        ARC_REFLECT_FIELD(GraphThemeSettings, hoverBorder)   ARC_REFLECT_ATTR(DisplayName, "Hover border")     ARC_REFLECT_ATTR(Category, "Canvas") ARC_REFLECT_ATTR(Apply, ApplyMode::Restart) ARC_REFLECT_ATTR(Tooltip, "The border of a node under the cursor, and the viewport's hover outline. Graph canvases pick it up when reopened.")
    ARC_END_REFLECT_TYPE()

    ARC_SETTINGS(GraphThemeSettings);

    ARC_REFLECT_TYPE(GraphCategoryThemeSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.theme.graph.category", SettingScope::PreferencesMachine, ApplyMode::Live, Audience::Editor,
                             "Appearance/Graph colours")
        ARC_REFLECT_FIELD(GraphCategoryThemeSettings, input)         ARC_REFLECT_ATTR(DisplayName, "Input")         ARC_REFLECT_ATTR(Category, "Node categories") ARC_REFLECT_ATTR(Tooltip, "The title band of an Input node (parameters, constants, UVs).")
        ARC_REFLECT_FIELD(GraphCategoryThemeSettings, math)          ARC_REFLECT_ATTR(DisplayName, "Math")          ARC_REFLECT_ATTR(Category, "Node categories") ARC_REFLECT_ATTR(Tooltip, "The title band of a Math node.")
        ARC_REFLECT_FIELD(GraphCategoryThemeSettings, vector)        ARC_REFLECT_ATTR(DisplayName, "Vector")        ARC_REFLECT_ATTR(Category, "Node categories") ARC_REFLECT_ATTR(Tooltip, "The title band of a Vector node.")
        ARC_REFLECT_FIELD(GraphCategoryThemeSettings, procedural)    ARC_REFLECT_ATTR(DisplayName, "Procedural")    ARC_REFLECT_ATTR(Category, "Node categories") ARC_REFLECT_ATTR(Tooltip, "The title band of a Procedural node.")
        ARC_REFLECT_FIELD(GraphCategoryThemeSettings, output)        ARC_REFLECT_ATTR(DisplayName, "Output")        ARC_REFLECT_ATTR(Category, "Node categories") ARC_REFLECT_ATTR(Tooltip, "The title band of the Output sink, which should stand out from every other category.")
        ARC_REFLECT_FIELD(GraphCategoryThemeSettings, utility)       ARC_REFLECT_ATTR(DisplayName, "Utility")       ARC_REFLECT_ATTR(Category, "Node categories") ARC_REFLECT_ATTR(Tooltip, "The title band of a Utility node.")
        ARC_REFLECT_FIELD(GraphCategoryThemeSettings, uncategorized) ARC_REFLECT_ATTR(DisplayName, "Uncategorized") ARC_REFLECT_ATTR(Category, "Node categories") ARC_REFLECT_ATTR(Tooltip, "The title band of a node with no category.")
    ARC_END_REFLECT_TYPE()

    ARC_SETTINGS(GraphCategoryThemeSettings);

    ARC_REFLECT_TYPE(AssetGraphThemeSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.theme.assetGraph", SettingScope::PreferencesMachine, ApplyMode::Live, Audience::Editor,
                             "Appearance/Graph colours")
        ARC_REFLECT_FIELD(AssetGraphThemeSettings, legendEdge)   ARC_REFLECT_ATTR(DisplayName, "Legend: derives / samples") ARC_REFLECT_ATTR(Category, "Asset graph") ARC_REFLECT_ATTR(Tooltip, "The asset graph legend's swatch for a derives/samples edge.")
        ARC_REFLECT_FIELD(AssetGraphThemeSettings, legendUsedBy) ARC_REFLECT_ATTR(DisplayName, "Legend: uses")              ARC_REFLECT_ATTR(Category, "Asset graph") ARC_REFLECT_ATTR(Tooltip, "The asset graph legend's swatch for a uses edge.")
    ARC_END_REFLECT_TYPE()

    ARC_SETTINGS(AssetGraphThemeSettings);

    namespace
    {
        namespace D = GraphThemeDefaults;
        using G = GraphThemeSettings;
        using C = GraphCategoryThemeSettings;
        using A = AssetGraphThemeSettings;

        constexpr GraphThemeTokenInfo Graph(std::string_view f, const ImVec4& l, Arcane::CVarColor G::* m) { return { f, l, m, nullptr, nullptr }; }
        constexpr GraphThemeTokenInfo Cat(std::string_view f, const ImVec4& l, Arcane::CVarColor C::* m)   { return { f, l, nullptr, m, nullptr }; }
        constexpr GraphThemeTokenInfo Asset(std::string_view f, const ImVec4& l, Arcane::CVarColor A::* m) { return { f, l, nullptr, nullptr, m }; }

        // Struct order; the field names are the reflected cvar names.
        constexpr std::array<GraphThemeTokenInfo, 24> kGraphThemeTokens = { {
            Graph("graph.nodeBody",      D::kNodeBody,      &G::nodeBody),
            Graph("graph.nodeTitle",     D::kNodeTitle,     &G::nodeTitle),
            Graph("graph.nodeBorder",    D::kNodeBorder,    &G::nodeBorder),
            Graph("graph.nodeTitleText", D::kNodeTitleText, &G::nodeTitleText),
            Graph("graph.nodeBadgeText", D::kNodeBadgeText, &G::nodeBadgeText),
            Graph("graph.groupBg",       D::kGroupBg,       &G::groupBg),
            Graph("graph.groupBorder",   D::kGroupBorder,   &G::groupBorder),
            Graph("graph.pinTexture",    D::kPinTexture,    &G::pinTexture),
            Graph("graph.pinScalar",     D::kPinScalar,     &G::pinScalar),
            Graph("graph.pinVec2",       D::kPinVec2,       &G::pinVec2),
            Graph("graph.pinVec4",       D::kPinVec4,       &G::pinVec4),
            Graph("graph.pinDynamic",    D::kPinDynamic,    &G::pinDynamic),
            Graph("graph.gridMinor",     D::kGridMinor,     &G::gridMinor),
            Graph("graph.gridMajor",     D::kGridMajor,     &G::gridMajor),
            Graph("graph.hoverBorder",   D::kHoverBorder,   &G::hoverBorder),
            Cat("graph.category.input",         D::kCategoryInput,         &C::input),
            Cat("graph.category.math",          D::kCategoryMath,          &C::math),
            Cat("graph.category.vector",        D::kCategoryVector,        &C::vector),
            Cat("graph.category.procedural",    D::kCategoryProcedural,    &C::procedural),
            Cat("graph.category.output",        D::kCategoryOutput,        &C::output),
            Cat("graph.category.utility",       D::kCategoryUtility,       &C::utility),
            Cat("graph.category.uncategorized", D::kCategoryUncategorized, &C::uncategorized),
            Asset("assetGraph.legendEdge",   D::kLegendEdge,   &A::legendEdge),
            Asset("assetGraph.legendUsedBy", D::kLegendUsedBy, &A::legendUsedBy),
        } };
        static_assert(sizeof(G) + sizeof(C) + sizeof(A) == kGraphThemeTokens.size() * sizeof(Arcane::CVarColor),
                      "one token per colour field");

        const GraphThemeColors& Defaults()
        {
            static const GraphThemeColors kDefaults{};
            return kDefaults;
        }

        template <class S>
        const GraphThemeTokenInfo& TokenOf(Arcane::CVarColor S::* field) noexcept
        {
            for (const GraphThemeTokenInfo& t : kGraphThemeTokens)
            {
                if constexpr (std::is_same_v<S, G>) { if (t.graph == field) return t; }
                else if constexpr (std::is_same_v<S, C>) { if (t.category == field) return t; }
                else { if (t.assetGraph == field) return t; }
            }
            return kGraphThemeTokens.front();   // unreachable: every field has a token (static_assert above)
        }
    }

    std::span<const GraphThemeTokenInfo> GraphThemeTokens() noexcept { return kGraphThemeTokens; }

    const GraphThemeTokenInfo* FindGraphThemeToken(std::string_view field) noexcept
    {
        for (const GraphThemeTokenInfo& t : kGraphThemeTokens)
            if (t.field == field) return &t;
        return nullptr;
    }

    Arcane::CVarColor& GraphThemeSlot(GraphThemeColors& c, const GraphThemeTokenInfo& t) noexcept
    {
        if (t.graph) return c.graph.*(t.graph);
        if (t.category) return c.category.*(t.category);
        return c.assetGraph.*(t.assetGraph);
    }

    const Arcane::CVarColor& GraphThemeSlot(const GraphThemeColors& c, const GraphThemeTokenInfo& t) noexcept
    {
        return GraphThemeSlot(const_cast<GraphThemeColors&>(c), t);
    }

    ImVec4 ResolveGraphThemeColor(const GraphThemeColors& colors, const GraphThemeTokenInfo& token) noexcept
    {
        return ResolveDomainColor(GraphThemeSlot(colors, token), GraphThemeSlot(Defaults(), token), token.legacy);
    }

    ImVec4 GraphThemeColor(Arcane::CVarColor GraphThemeSettings::* field)
    {
        return ResolveDomainColor(Arcane::Settings<G>().*field, Defaults().graph.*field, TokenOf(field).legacy);
    }

    ImVec4 GraphCategoryThemeColor(Arcane::CVarColor GraphCategoryThemeSettings::* field)
    {
        return ResolveDomainColor(Arcane::Settings<C>().*field, Defaults().category.*field, TokenOf(field).legacy);
    }

    ImVec4 AssetGraphThemeColor(Arcane::CVarColor AssetGraphThemeSettings::* field)
    {
        return ResolveDomainColor(Arcane::Settings<A>().*field, Defaults().assetGraph.*field, TokenOf(field).legacy);
    }

    GraphThemeColors CurrentGraphThemeColors()
    {
        return { Arcane::Settings<G>(), Arcane::Settings<C>(), Arcane::Settings<A>() };
    }
}
