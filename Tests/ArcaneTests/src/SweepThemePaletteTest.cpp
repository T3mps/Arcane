// Settings sweep S6-26: the editor's domain palettes (axis bars, header bands,
// input pills, channel markers, asset kinds, the camera frame and the
// stripe/dim/overline alphas) are editor.theme.* tokens whose defaults draw
// today's bytes exactly (spec s10.2).
#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include "Settings/EditorThemeSettings.hpp"
#include "Panels/AssetPanelModel.hpp"
#include <imgui.h>
using namespace Arcane;
namespace
{
    ImU32 U32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }
}
TEST_CASE("sweep: the theme domain palettes keep their exact bytes", "[sweep][theme-palette]")
{
    const Editor::EditorThemeSettings t{};
    CHECK(ImGui::ColorConvertFloat4ToU32(Editor::ToDisplayColor(t.axisX)) == IM_COL32(196, 64, 54, 255));
    CHECK(ImGui::ColorConvertFloat4ToU32(Editor::ToDisplayColor(t.axisY)) == IM_COL32(96, 166, 58, 255));
    CHECK(ImGui::ColorConvertFloat4ToU32(Editor::ToDisplayColor(t.axisZ)) == IM_COL32(58, 122, 196, 255));
    CHECK(ImGui::ColorConvertFloat4ToU32(Editor::ToDisplayColor(t.headerBand)) == IM_COL32(48, 48, 52, 255));
    CHECK(Test::SameBits(t.unfocusedOverlineAlpha, 0.45f));
    CHECK(Test::SameBits(t.modalDim.a, 0.55f));
    Test::RequireDefault("editor.theme.unfocusedOverlineAlpha", CVarValue::Float32(0.45f));
}

TEST_CASE("sweep: the default palette is today's header bands, acting-on frame and channel markers", "[sweep][theme-palette]")
{
    const Editor::Theme::Palette p = Editor::ToPalette(Editor::EditorThemeSettings{});
    CHECK(U32(p.headerBand)        == IM_COL32(48, 48, 52, 255));
    CHECK(U32(p.headerBandHovered) == IM_COL32(58, 58, 64, 255));
    CHECK(U32(p.headerBandActive)  == IM_COL32(66, 66, 73, 255));
    CHECK(U32(p.actingOnFrame)     == IM_COL32(0x7a, 0x5a, 0x20, 255));
    CHECK(U32(p.channelR) == IM_COL32(240, 20, 20, 255));
    CHECK(U32(p.channelG) == IM_COL32(20, 240, 20, 255));
    CHECK(U32(p.channelB) == IM_COL32(20, 20, 240, 255));
    CHECK(U32(p.channelW) == IM_COL32(140, 140, 140, 255));
    // The ImGui unfocused-tab overline: the accent at the setting's alpha.
    ImGuiStyle style;
    Editor::ApplyEditorTheme(style);
    CHECK(Test::SameBits(style.Colors[ImGuiCol_TabDimmedSelectedOverline].w, 0.45f));
    Editor::ApplyEditorThemeColors(style, 0.25f);
    CHECK(Test::SameBits(style.Colors[ImGuiCol_TabDimmedSelectedOverline].w, 0.25f));
}

TEST_CASE("sweep: the input pills, asset kinds and camera frame resolve to today's colours", "[sweep][theme-palette]")
{
    const Editor::EditorThemeInputPillSettings pill{};
    CHECK(U32(Editor::ResolveDomainColor(pill.blueBorder,   pill.blueBorder,   Editor::kInputPillBlueBorder))   == IM_COL32(0x3a, 0x4a, 0x5c, 255));
    CHECK(U32(Editor::ResolveDomainColor(pill.blueText,     pill.blueText,     Editor::kInputPillBlueText))     == IM_COL32(0x9f, 0xb3, 0xc8, 255));
    CHECK(U32(Editor::ResolveDomainColor(pill.violetBorder, pill.violetBorder, Editor::kInputPillVioletBorder)) == IM_COL32(0x4a, 0x3a, 0x5c, 255));
    CHECK(U32(Editor::ResolveDomainColor(pill.violetText,   pill.violetText,   Editor::kInputPillVioletText))   == IM_COL32(0xb8, 0xa3, 0xc8, 255));
    // The stored linear defaults are the sRGB bytes (a round trip lands on them).
    CHECK(U32(Editor::ToDisplayColor(pill.blueBorder)) == IM_COL32(0x3a, 0x4a, 0x5c, 255));

    const Editor::EditorThemeAssetKindSettings kinds{};
    CHECK(Editor::KindAccentRgb(Editor::AssetKind::Texture,      kinds) == 0xb06a5bu);
    CHECK(Editor::KindAccentRgb(Editor::AssetKind::Material,     kinds) == 0x6a9b5bu);
    CHECK(Editor::KindAccentRgb(Editor::AssetKind::Mesh,         kinds) == 0x5b9bb0u);
    CHECK(Editor::KindAccentRgb(Editor::AssetKind::Sprite,       kinds) == 0x9b5bb0u);
    CHECK(Editor::KindAccentRgb(Editor::AssetKind::Scene,        kinds) == 0xb09b5bu);
    CHECK(Editor::KindAccentRgb(Editor::AssetKind::InputActions, kinds) == 0x8b7ab5u);
    CHECK(Editor::KindAccentRgb(Editor::AssetKind::Model,        kinds) == 0x5b7fb0u);
    CHECK(Editor::KindAccentRgb(Editor::AssetKind::Data,         kinds) == 0u);   // no row: the grab-gray fallback

    // A user colour reaches the table (and stays non-zero, so it is not the fallback).
    Editor::EditorThemeAssetKindSettings custom{};
    custom.texture = Editor::ToSettingColor(ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
    CHECK(Editor::KindAccentRgb(Editor::AssetKind::Texture, custom) == 0xff0000u);

    const Editor::EditorThemeViewportSettings vp{};
    const ImVec4 frame = Editor::ResolveDomainColor(vp.cameraFrame, vp.cameraFrame, Editor::kCameraFrameColor);
    CHECK(Test::SameBits(frame.x, 0.45f));
    CHECK(Test::SameBits(frame.y, 0.62f));
    CHECK(Test::SameBits(frame.z, 0.78f));
    CHECK(Test::SameBits(frame.w, 0.75f));

    Test::RequireDefault("editor.theme.headerBand", CVarValue::Color(Editor::EditorThemeSettings{}.headerBand));
    Test::RequireDefault("editor.theme.inputPill.blueBorder", CVarValue::Color(pill.blueBorder));
    Test::RequireDefault("editor.theme.assetKind.model", CVarValue::Color(kinds.model));
    Test::RequireDefault("editor.theme.viewport.cameraFrame", CVarValue::Color(vp.cameraFrame));
}

// Fix round 1 (finding 2): editor.theme.channelR reached only the narrow boxes;
// the wide ColorValue row, the popup's Linear row and ColorPicker4's own RGB
// row are stock ColorEdit4 calls that drew ImGui's static marker table. A
// non-default channelR must show on every one of them, and the table must be
// restored once the picker is done. Device-less ImGui.
#include "Widgets/ColorPickerPopup.hpp"
#include "Widgets/EditorTheme.hpp"
#include <imgui_internal.h>
namespace
{
    struct MarkerHarness
    {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGuiContext* ctx = nullptr;
        MarkerHarness()
        {
            ctx = ImGui::CreateContext();
            ImGui::SetCurrentContext(ctx);
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1600, 1000);
            io.IniFilename = nullptr;
            unsigned char* px = nullptr; int w = 0, h = 0;
            io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        }
        ~MarkerHarness() { ImGui::DestroyContext(ctx); ImGui::SetCurrentContext(prev); }
        template <class F> bool Drew(ImU32 want, F&& body)
        {
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(1500, 900));
            ImGui::Begin("Host");
            body();
            ImGui::End();
            ImGui::Render();
            bool drew = false;
            for (const ImDrawVert& v : ImGui::FindWindowByName("Host")->DrawList->VtxBuffer) drew = drew || v.col == want;
            return drew;
        }
    };
    constexpr ImU32 kStockRed = IM_COL32(240, 20, 20, 255);
}

TEST_CASE("sweep: a non-default channelR draws on the wide colour rows and the picker, not only the narrow boxes", "[sweep][theme-palette]")
{
    using namespace Arcane::Editor;
    MarkerHarness h;
    Theme::Palette p = Theme::kDarkPalette;
    p.channelR = ImVec4(10.0f / 255.0f, 200.0f / 255.0f, 230.0f / 255.0f, 1.0f);
    const ImU32 custom = U32(p.channelR);
    const Theme::ScopedLivePalette live(p);
    float linear[4] = { 0.5f, 0.25f, 0.125f, 1.0f };
    float original[4] = { 0.5f, 0.25f, 0.125f, 1.0f };

    SECTION("the wide ColorValue row (stock ColorEdit4, three decimals)")
    {
        bool drewStock = false;
        const bool drewCustom = h.Drew(custom, [&] { ImGui::SetNextItemWidth(900.0f); (void)ColorValue("##c", linear, original); });
        for (const ImDrawVert& v : ImGui::FindWindowByName("Host")->DrawList->VtxBuffer) drewStock = drewStock || v.col == kStockRed;
        CHECK(drewCustom);
        CHECK_FALSE(drewStock);
    }
    SECTION("the narrow ColorValue row (NarrowColorBoxes)")
    {
        CHECK(h.Drew(custom, [&] { ImGui::SetNextItemWidth(150.0f); (void)ColorValue("##c", linear, original); }));
    }
    SECTION("the popup body: ColorPicker4's RGB row and the Linear row")
    {
        bool drewStock = false;
        const bool drewCustom = h.Drew(custom, [&] { (void)ColorPopupBody(linear, original, false); });
        for (const ImDrawVert& v : ImGui::FindWindowByName("Host")->DrawList->VtxBuffer) drewStock = drewStock || v.col == kStockRed;
        CHECK(drewCustom);
        CHECK_FALSE(drewStock);
    }
    // The table is the stock one again outside the picker.
    CHECK(ImGui::GetColorMarkerColors()[0] == kStockRed);
}

TEST_CASE("sweep: at the Dark default the picker draws ImGui's stock red marker", "[sweep][theme-palette]")
{
    using namespace Arcane::Editor;
    MarkerHarness h;
    const Theme::ScopedLivePalette live(Theme::kDarkPalette);
    float linear[4] = { 0.5f, 0.25f, 0.125f, 1.0f };
    float original[4] = { 0.5f, 0.25f, 0.125f, 1.0f };
    CHECK(h.Drew(kStockRed, [&] { ImGui::SetNextItemWidth(900.0f); (void)ColorValue("##c", linear, original); }));
}
