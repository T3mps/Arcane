// Settings sweep S6-27: the graph canvases' colours (node chrome, pins, grid,
// hover border, the per-category title bands and the asset graph's legend)
// are editor.theme.graph.* / .graph.category.* / .assetGraph.* cvars. Their
// defaults draw today's bytes exactly (spec s10.2), and the theme presets
// carry them, so Light and High Contrast re-tone the nodes with the canvas.
#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include "Settings/GraphThemeSettings.hpp"
#include "Settings/ThemePresets.hpp"
#include "Documents/ShaderGraphCategoryColors.hpp"
#include "Documents/ShaderGraphPinTypes.hpp"
#include "Widgets/GraphCanvasStyle.hpp"
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/Settings.hpp>
#include <imgui.h>
#include <filesystem>
#include <string>
using namespace Arcane;
namespace
{
    ImU32 U32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }
    bool SameColor(const ImVec4& a, const ImVec4& b)
    {
        return Test::SameBits(a.x, b.x) && Test::SameBits(a.y, b.y) && Test::SameBits(a.z, b.z) && Test::SameBits(a.w, b.w);
    }
    std::filesystem::path Preset(std::string_view name) { return Editor::ThemePresetDir() / (std::string(name) + ".arctheme"); }

    // Leaves the global registry as found, even when a REQUIRE throws.
    struct EditorUserLayerReset
    {
        ~EditorUserLayerReset()
        {
            CVarRegistry& reg = CVarRegistry::Get();
            reg.RevertLayer(SetBy::EditorUser);
            reg.PublishImmediate();
        }
    };
}

TEST_CASE("sweep: graph palette defaults are the pre-sweep values", "[sweep][graph-theme]")
{
    // Stored LINEAR (CVarColor's contract); the display value is today's.
    const Editor::GraphThemeSettings g{};
    CHECK(U32(Editor::ToDisplayColor(g.pinScalar)) == U32(ImVec4(0.502f, 0.808f, 1.0f, 1.0f)));
    CHECK(U32(Editor::ToDisplayColor(g.hoverBorder)) == U32(ImVec4(0.25f, 0.70f, 1.0f, 1.0f)));
    // What is DRAWN at the default is the pre-sweep constant, to the bit.
    const ImVec4 p = Editor::PinColorForWidth(1);
    CHECK(Test::SameBits(p.x, 0.502f));
    CHECK(Test::SameBits(p.y, 0.808f));
    CHECK(Test::SameBits(p.z, 1.0f));
    const ImVec4 hover = Editor::GraphThemeColor(&Editor::GraphThemeSettings::hoverBorder);
    CHECK(Test::SameBits(hover.x, 0.25f));
    CHECK(Test::SameBits(hover.y, 0.70f));
    Test::RequireDefault("editor.theme.graph.pinScalar",
                         CVarValue::Color(Editor::ToSettingColor(ImVec4(0.502f, 0.808f, 1.0f, 1.0f))));
}

TEST_CASE("sweep: every graph colour draws its pre-sweep constant at the default", "[sweep][graph-theme]")
{
    using G = Editor::GraphThemeSettings;
    CHECK(SameColor(Editor::GraphThemeColor(&G::nodeBody),      ImVec4(0.176f, 0.176f, 0.188f, 1.0f)));
    CHECK(SameColor(Editor::GraphThemeColor(&G::nodeTitle),     ImVec4(0.137f, 0.137f, 0.149f, 1.0f)));
    CHECK(SameColor(Editor::GraphThemeColor(&G::nodeBorder),    ImVec4(0.243f, 0.243f, 0.267f, 1.0f)));
    CHECK(SameColor(Editor::GraphThemeColor(&G::nodeTitleText), ImVec4(0.808f, 0.808f, 0.831f, 1.0f)));
    CHECK(SameColor(Editor::GraphThemeColor(&G::nodeBadgeText), ImVec4(1.0f, 0.4f, 0.3f, 1.0f)));
    CHECK(SameColor(Editor::GraphThemeColor(&G::groupBg),       ImVec4(0.220f, 0.220f, 0.235f, 0.25f)));
    CHECK(SameColor(Editor::GraphThemeColor(&G::groupBorder),   ImVec4(0.290f, 0.290f, 0.310f, 0.60f)));
    CHECK(SameColor(Editor::GraphThemeColor(&G::pinTexture),    ImVec4(0.949f, 0.549f, 0.251f, 1.0f)));
    CHECK(SameColor(Editor::PinColorForWidth(2),                ImVec4(0.549f, 0.863f, 0.549f, 1.0f)));
    CHECK(SameColor(Editor::PinColorForWidth(4),                ImVec4(0.941f, 0.549f, 0.863f, 1.0f)));
    CHECK(SameColor(Editor::PinColorForWidth(0),                ImVec4(0.745f, 0.745f, 0.765f, 1.0f)));
    CHECK(SameColor(Editor::GraphThemeColor(&G::gridMinor),     ImVec4(0.180f, 0.180f, 0.196f, 0.55f)));
    CHECK(SameColor(Editor::GraphThemeColor(&G::gridMajor),     ImVec4(0.235f, 0.235f, 0.255f, 0.90f)));

    using C = GraphNodeCategory;
    const auto rgb = [](unsigned v) { return ImVec4(((v >> 16) & 0xFFu) / 255.0f, ((v >> 8) & 0xFFu) / 255.0f, (v & 0xFFu) / 255.0f, 1.0f); };
    CHECK(SameColor(Editor::GraphCategoryHeaderColor(C::Input),         rgb(0x24384a)));
    CHECK(SameColor(Editor::GraphCategoryHeaderColor(C::Math),          rgb(0x26402f)));
    CHECK(SameColor(Editor::GraphCategoryHeaderColor(C::Vector),        rgb(0x3a2a4a)));
    CHECK(SameColor(Editor::GraphCategoryHeaderColor(C::Procedural),    rgb(0x4a3a22)));
    CHECK(SameColor(Editor::GraphCategoryHeaderColor(C::Output),        rgb(0x5a2626)));
    CHECK(SameColor(Editor::GraphCategoryHeaderColor(C::Utility),       rgb(0x2e2e33)));
    CHECK(SameColor(Editor::GraphCategoryHeaderColor(C::Uncategorized), rgb(0x232326)));

    using A = Editor::AssetGraphThemeSettings;
    CHECK(SameColor(Editor::AssetGraphThemeColor(&A::legendEdge),   ImVec4(0.361f, 0.361f, 0.361f, 1.0f)));
    CHECK(SameColor(Editor::AssetGraphThemeColor(&A::legendUsedBy), ImVec4(0.290f, 0.290f, 0.290f, 1.0f)));

    // The canvas style desc picks the hover border up from the same token.
    CHECK(SameColor(Editor::GraphCanvasStyleDesc{}.hovBorder, ImVec4(0.25f, 0.70f, 1.0f, 1.0f)));
}

TEST_CASE("sweep: graph colour cvars are Editor machine preferences; the canvas-latched ones are Restart", "[sweep][graph-theme]")
{
    const CVarRegistry& reg = CVarRegistry::Get();
    for (const Editor::GraphThemeTokenInfo& t : Editor::GraphThemeTokens())
    {
        const std::string name = Editor::ThemeCvarName(t.field);
        INFO(name);
        const auto e = reg.Explain(name);
        REQUIRE(e.has_value());
        CHECK_FALSE(e->help.empty());
        CHECK(e->audience == Audience::Editor);
        CHECK(e->scope == SettingScope::PreferencesMachine);
        const bool latched = t.field == "graph.nodeBody" || t.field == "graph.nodeBorder" || t.field == "graph.groupBg"
                          || t.field == "graph.groupBorder" || t.field == "graph.hoverBorder";
        CHECK(e->apply == (latched ? ApplyMode::Restart : ApplyMode::Live));
    }
    CHECK(Editor::GraphThemeTokens().size() == 24);
}

TEST_CASE("sweep: a graph colour set away from its default is what gets drawn", "[sweep][graph-theme]")
{
    EditorUserLayerReset reset;
    CVarRegistry& reg = CVarRegistry::Get();
    const ImVec4 red(1.0f, 0.0f, 0.0f, 1.0f);
    REQUIRE(reg.Set(reg.Find("editor.theme.graph.pinScalar"), CVarValue::Color(Editor::ToSettingColor(red)), SetBy::EditorUser,
                    "editor", CVarContext::Editor) == SetResult::Applied);
    REQUIRE(reg.Set(reg.Find("editor.theme.graph.category.output"), CVarValue::Color(Editor::ToSettingColor(red)),
                    SetBy::EditorUser, "editor", CVarContext::Editor) == SetResult::Applied);
    reg.PublishImmediate();
    CHECK(U32(Editor::PinColorForWidth(1)) == IM_COL32(255, 0, 0, 255));
    CHECK(U32(Editor::PinPaintFor(0, 1).color) == IM_COL32(255, 0, 0, 255));   // a dynamic pin resolved to float
    CHECK(U32(Editor::GraphCategoryHeaderColor(GraphNodeCategory::Output)) == IM_COL32(255, 0, 0, 255));
    CHECK(U32(Editor::PinColorForWidth(2)) == U32(ImVec4(0.549f, 0.863f, 0.549f, 1.0f)));   // the others are untouched
}

TEST_CASE("sweep: the theme presets carry the graph colours -- Dark is today's, Light re-tones nodes and grid", "[sweep][graph-theme][theme]")
{
    // Dark: every graph token is present and is today's colour at 8 bits.
    const auto dark = Editor::ReadThemeFile(Preset("Dark"));
    REQUIRE(dark.has_value());
    CHECK(dark->unknownKeys.empty());
    std::size_t graphKeys = 0;
    for (const auto& [field, colour] : dark->colors)
    {
        const Editor::GraphThemeTokenInfo* t = Editor::FindGraphThemeToken(field);
        if (!t) continue;
        ++graphKeys;
        INFO(field);
        CHECK(U32(colour) == U32(t->legacy));
    }
    CHECK(graphKeys == Editor::GraphThemeTokens().size());

    // Light: the node body and the grid follow the light canvas.
    EditorUserLayerReset reset;
    CVarRegistry& reg = CVarRegistry::Get();
    const auto light = Editor::ReadThemeFile(Preset("Light"));
    REQUIRE(light.has_value());
    CHECK(light->unknownKeys.empty());
    CHECK(Editor::ApplyThemeToRegistry(*light, reg) == Editor::kThemeTokens.size() + Editor::GraphThemeTokens().size());
    reg.PublishImmediate();
    using G = Editor::GraphThemeSettings;
    const ImVec4 body = Editor::GraphThemeColor(&G::nodeBody);
    CHECK(U32(body) != U32(ImVec4(0.176f, 0.176f, 0.188f, 1.0f)));
    CHECK(body.x > 0.5f);   // a light node on the light canvas
    CHECK(U32(Editor::GraphThemeColor(&G::gridMinor)) != U32(ImVec4(0.180f, 0.180f, 0.196f, 0.55f)));
    CHECK(U32(Editor::GraphThemeColor(&G::gridMajor)) != U32(ImVec4(0.235f, 0.235f, 0.255f, 0.90f)));
}

TEST_CASE("sweep: the contrast report covers node text on the node body, title and category bands in every preset", "[sweep][graph-theme][theme]")
{
    bool sawBody = false;
    for (const Editor::ContrastRow& row : Editor::ContrastReport(Editor::Theme::kDarkPalette, Editor::GraphThemeColors{}))
        if (row.label == "Node text on node body") sawBody = true;
    CHECK(sawBody);
    for (std::string_view name : Editor::kThemePresetNames)
    {
        INFO(name);
        const auto file = Editor::ReadThemeFile(Preset(name));
        REQUIRE(file.has_value());
        const Editor::GraphThemeColors graph = Editor::ApplyThemeFileTo(*file, Editor::GraphThemeColors{});
        const Editor::Theme::Palette p = Editor::ToPalette(Editor::ApplyThemeFileTo(*file, Editor::EditorThemeSettings{}));
        for (const Editor::ContrastRow& row : Editor::ContrastReport(p, graph))
        {
            INFO(row.label << " " << row.ratio << " (min " << row.minRatio << ")");
            CHECK(row.ok);
        }
    }
}

TEST_CASE("sweep: an exported theme carries the graph colours and imports them back", "[sweep][graph-theme][theme]")
{
    Editor::GraphThemeColors mine{};
    mine.graph.nodeBody = Editor::ToSettingColor(ImVec4(0.5f, 0.25f, 0.125f, 1.0f));
    mine.assetGraph.legendEdge = Editor::ToSettingColor(ImVec4(0.0f, 1.0f, 0.0f, 1.0f));
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "s6-27-roundtrip.arctheme";
    std::string error;
    REQUIRE(Editor::WriteThemeFile(path, "Mine", Editor::Theme::kDarkPalette, mine, &error));
    const auto file = Editor::ReadThemeFile(path);
    REQUIRE(file.has_value());
    CHECK(file->unknownKeys.empty());
    const Editor::GraphThemeColors back = Editor::ApplyThemeFileTo(*file, Editor::GraphThemeColors{});
    for (const Editor::GraphThemeTokenInfo& t : Editor::GraphThemeTokens())
    {
        INFO(t.field);
        CHECK(U32(Editor::ResolveGraphThemeColor(back, t)) == U32(Editor::ResolveGraphThemeColor(mine, t)));
    }
    std::filesystem::remove(path);
}
