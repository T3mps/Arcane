#pragma once

// The graph canvases' colours as cvars (settings sweep S6-27, inventory Part 3
// "Theme"): editor.theme.graph.* (the shader canvas's node chrome, pins, grid
// and the shared hover border), editor.theme.graph.category.* (the per-
// category node title bands) and editor.theme.assetGraph.* (the asset graph's
// legend swatches). Preferences > Appearance > Graph colours, machine-wide.
//
// Unlike the axis and asset-kind hues these ARE theme-preset tokens: the
// canvas follows Theme::kPanel, so a preset that re-tones the panels must
// re-tone the nodes and grid with it (data/EditorThemes/*.arctheme carry
// every one; Dark is today's values).
//
// Stored LINEAR (CVarColor's contract). Every GraphThemeDefaults constant is
// today's DISPLAY value; the struct default is written from it, and a value
// equal to its default draws the constant itself, bit for bit (goldens).
//
// node body/border, group bg/border and the hover border are latched into the
// node-editor style at ed::CreateEditor (ApplyGraphCanvasStyle), so they are
// Apply(Restart): reopen the document. Everything else is read per frame.

#include "Settings/EditorThemeSettings.hpp"

#include <Arcane/Config/CVarTypes.hpp>

#include <span>
#include <string_view>

namespace Arcane::Editor
{
    // Today's display-referred values (ImGui draws post-tonemap into the
    // backbuffer, imgui.hlsl:1-5).
    namespace GraphThemeDefaults
    {
        // Shader canvas node chrome: the Unity Shader Graph reference tones --
        // a body one step above the canvas, a title band one step below the
        // body, a border one step above the body again.
        inline constexpr ImVec4 kNodeBody      = ImVec4(0.176f, 0.176f, 0.188f, 1.0f);   // #2d2d30
        inline constexpr ImVec4 kNodeTitle     = ImVec4(0.137f, 0.137f, 0.149f, 1.0f);   // #232326
        inline constexpr ImVec4 kNodeBorder    = ImVec4(0.243f, 0.243f, 0.267f, 1.0f);
        inline constexpr ImVec4 kNodeTitleText = ImVec4(0.808f, 0.808f, 0.831f, 1.0f);   // #cecfd4
        inline constexpr ImVec4 kNodeBadgeText = ImVec4(1.0f,   0.4f,   0.3f,   1.0f);
        inline constexpr ImVec4 kGroupBg       = ImVec4(0.220f, 0.220f, 0.235f, 0.25f);
        inline constexpr ImVec4 kGroupBorder   = ImVec4(0.290f, 0.290f, 0.310f, 0.60f);
        // Pins by width (Documents/ShaderGraphPinTypes.hpp) and the pass
        // canvas's one render-target pin colour (red-orange).
        inline constexpr ImVec4 kPinTexture    = ImVec4(0.949f, 0.549f, 0.251f, 1.0f);
        inline constexpr ImVec4 kPinScalar     = ImVec4(0.502f, 0.808f, 1.0f,   1.0f);   // pale azure
        inline constexpr ImVec4 kPinVec2       = ImVec4(0.549f, 0.863f, 0.549f, 1.0f);   // green
        inline constexpr ImVec4 kPinVec4       = ImVec4(0.941f, 0.549f, 0.863f, 1.0f);   // magenta
        inline constexpr ImVec4 kPinDynamic    = ImVec4(0.745f, 0.745f, 0.765f, 1.0f);   // gray
        // Both canvases' two-tier grid; the alphas are each octave's peak
        // strength (GraphGridPhase.hpp), not image opacity.
        inline constexpr ImVec4 kGridMinor     = ImVec4(0.180f, 0.180f, 0.196f, 0.55f);
        inline constexpr ImVec4 kGridMajor     = ImVec4(0.235f, 0.235f, 0.255f, 0.90f);
        // The hover accent of both canvases and of the viewport outline's
        // hover role (AxisColors.cpp).
        inline constexpr ImVec4 kHoverBorder   = ImVec4(0.25f, 0.70f, 1.0f, 1.0f);
        // Node page spec 2026-09-30 s5.1.4: the per-category title band; the
        // Output sink is the one red, Uncategorized is kNodeTitle's #232326.
        inline constexpr ImVec4 kCategoryInput         = ImVec4(0x24 / 255.0f, 0x38 / 255.0f, 0x4a / 255.0f, 1.0f);
        inline constexpr ImVec4 kCategoryMath          = ImVec4(0x26 / 255.0f, 0x40 / 255.0f, 0x2f / 255.0f, 1.0f);
        inline constexpr ImVec4 kCategoryVector        = ImVec4(0x3a / 255.0f, 0x2a / 255.0f, 0x4a / 255.0f, 1.0f);
        inline constexpr ImVec4 kCategoryProcedural    = ImVec4(0x4a / 255.0f, 0x3a / 255.0f, 0x22 / 255.0f, 1.0f);
        inline constexpr ImVec4 kCategoryOutput        = ImVec4(0x5a / 255.0f, 0x26 / 255.0f, 0x26 / 255.0f, 1.0f);
        inline constexpr ImVec4 kCategoryUtility       = ImVec4(0x2e / 255.0f, 0x2e / 255.0f, 0x33 / 255.0f, 1.0f);
        inline constexpr ImVec4 kCategoryUncategorized = ImVec4(0x23 / 255.0f, 0x23 / 255.0f, 0x26 / 255.0f, 1.0f);
        // The asset graph legend's two edge swatches.
        inline constexpr ImVec4 kLegendEdge    = ImVec4(0.361f, 0.361f, 0.361f, 1.0f);   // #5c5c5c
        inline constexpr ImVec4 kLegendUsedBy  = ImVec4(0.290f, 0.290f, 0.290f, 1.0f);   // #4a4a4a
    }

    // editor.theme.graph.*
    struct GraphThemeSettings
    {
        Arcane::CVarColor nodeBody      = ToSettingColor(GraphThemeDefaults::kNodeBody);
        Arcane::CVarColor nodeTitle     = ToSettingColor(GraphThemeDefaults::kNodeTitle);
        Arcane::CVarColor nodeBorder    = ToSettingColor(GraphThemeDefaults::kNodeBorder);
        Arcane::CVarColor nodeTitleText = ToSettingColor(GraphThemeDefaults::kNodeTitleText);
        Arcane::CVarColor nodeBadgeText = ToSettingColor(GraphThemeDefaults::kNodeBadgeText);
        Arcane::CVarColor groupBg       = ToSettingColor(GraphThemeDefaults::kGroupBg);
        Arcane::CVarColor groupBorder   = ToSettingColor(GraphThemeDefaults::kGroupBorder);
        Arcane::CVarColor pinTexture    = ToSettingColor(GraphThemeDefaults::kPinTexture);
        Arcane::CVarColor pinScalar     = ToSettingColor(GraphThemeDefaults::kPinScalar);
        Arcane::CVarColor pinVec2       = ToSettingColor(GraphThemeDefaults::kPinVec2);
        Arcane::CVarColor pinVec4       = ToSettingColor(GraphThemeDefaults::kPinVec4);
        Arcane::CVarColor pinDynamic    = ToSettingColor(GraphThemeDefaults::kPinDynamic);
        Arcane::CVarColor gridMinor     = ToSettingColor(GraphThemeDefaults::kGridMinor);
        Arcane::CVarColor gridMajor     = ToSettingColor(GraphThemeDefaults::kGridMajor);
        Arcane::CVarColor hoverBorder   = ToSettingColor(GraphThemeDefaults::kHoverBorder);
    };

    // editor.theme.graph.category.* -- one per Arcane::GraphNodeCategory.
    struct GraphCategoryThemeSettings
    {
        Arcane::CVarColor input         = ToSettingColor(GraphThemeDefaults::kCategoryInput);
        Arcane::CVarColor math          = ToSettingColor(GraphThemeDefaults::kCategoryMath);
        Arcane::CVarColor vector        = ToSettingColor(GraphThemeDefaults::kCategoryVector);
        Arcane::CVarColor procedural    = ToSettingColor(GraphThemeDefaults::kCategoryProcedural);
        Arcane::CVarColor output        = ToSettingColor(GraphThemeDefaults::kCategoryOutput);
        Arcane::CVarColor utility       = ToSettingColor(GraphThemeDefaults::kCategoryUtility);
        Arcane::CVarColor uncategorized = ToSettingColor(GraphThemeDefaults::kCategoryUncategorized);
    };

    // editor.theme.assetGraph.*
    struct AssetGraphThemeSettings
    {
        Arcane::CVarColor legendEdge   = ToSettingColor(GraphThemeDefaults::kLegendEdge);
        Arcane::CVarColor legendUsedBy = ToSettingColor(GraphThemeDefaults::kLegendUsedBy);
    };

    // The three blocks as one value: what a theme file, an export and the
    // contrast report carry.
    struct GraphThemeColors
    {
        GraphThemeSettings graph;
        GraphCategoryThemeSettings category;
        AssetGraphThemeSettings assetGraph;
    };

    // One graph colour token. Exactly one of the three member pointers is set.
    struct GraphThemeTokenInfo
    {
        std::string_view field;   // "graph.nodeBody": the cvar is editor.theme.<field> (ThemeCvarName)
        ImVec4 legacy;            // today's display value (the GraphThemeDefaults constant)
        Arcane::CVarColor GraphThemeSettings::*         graph      = nullptr;
        Arcane::CVarColor GraphCategoryThemeSettings::* category   = nullptr;
        Arcane::CVarColor AssetGraphThemeSettings::*    assetGraph = nullptr;
    };

    // Every graph token, in struct order (graph, category, assetGraph).
    [[nodiscard]] std::span<const GraphThemeTokenInfo> GraphThemeTokens() noexcept;
    [[nodiscard]] const GraphThemeTokenInfo* FindGraphThemeToken(std::string_view field) noexcept;
    [[nodiscard]] Arcane::CVarColor& GraphThemeSlot(GraphThemeColors& colors, const GraphThemeTokenInfo& token) noexcept;
    [[nodiscard]] const Arcane::CVarColor& GraphThemeSlot(const GraphThemeColors& colors, const GraphThemeTokenInfo& token) noexcept;
    // The token's display colour under `colors` (its legacy constant while it
    // equals its default).
    [[nodiscard]] ImVec4 ResolveGraphThemeColor(const GraphThemeColors& colors, const GraphThemeTokenInfo& token) noexcept;

    // The display colour to draw, from the CURRENT published snapshot.
    [[nodiscard]] ImVec4 GraphThemeColor(Arcane::CVarColor GraphThemeSettings::* field);
    [[nodiscard]] ImVec4 GraphCategoryThemeColor(Arcane::CVarColor GraphCategoryThemeSettings::* field);
    [[nodiscard]] ImVec4 AssetGraphThemeColor(Arcane::CVarColor AssetGraphThemeSettings::* field);
    // The three published blocks, copied (theme export, the contrast report).
    [[nodiscard]] GraphThemeColors CurrentGraphThemeColors();
}
