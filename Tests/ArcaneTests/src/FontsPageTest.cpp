// Settings arc S4 (spec s7.3): the fonts/scale page writes editor.ui.* at the
// EditorUser rung; drawing it is balanced.
#include <catch2/catch_test_macros.hpp>
#include "Settings/EditorUiSettings.hpp"
#include "Settings/FontsPage.hpp"
#include "Settings/SettingsModel.hpp"
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/Settings.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>

using namespace Arcane::Editor;

TEST_CASE("Fonts page: a scale and a family edit reach Settings<EditorUiSettings>", "[settings-ui][editor]")
{
    REQUIRE(SetUiSetting("scale", Arcane::CVarValue::Float32(1.25f)));
    REQUIRE(SetUiSetting("fontFamily", Arcane::CVarValue::String("Roboto")));
    CHECK(Arcane::Settings<EditorUiSettings>().scale == 1.25f);
    CHECK(Arcane::Settings<EditorUiSettings>().fontFamily == "Roboto");
    CHECK_FALSE(SetUiSetting("noSuchField", Arcane::CVarValue::Bool(true)));
    Arcane::CVarRegistry::Get().RevertLayer(Arcane::SetBy::EditorUser);
    Arcane::CVarRegistry::Get().PublishImmediate();
    CHECK(Arcane::Settings<EditorUiSettings>().scale == 1.0f);
}

TEST_CASE("Fonts page: draws balanced with the family list", "[settings-ui][editor]")
{
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1000, 700); io.IniFilename = nullptr;
    unsigned char* px = nullptr; int w = 0, h = 0; io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    FontsPageState st;
    st.exeDir = "C:/fake/exe";
    int drift = 0;
    for (int i = 0; i < 2; ++i)
    {
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        ImGui::Begin("Page");
        const ImGuiContext& g = *ImGui::GetCurrentContext();
        const int c0 = g.ColorStack.Size, v0 = g.StyleVarStack.Size, f0 = g.FontStack.Size;
        DrawFontsPage(&st);
        drift += (g.ColorStack.Size - c0) + (g.StyleVarStack.Size - v0) + (g.FontStack.Size - f0);
        ImGui::End();
        ImGui::Render();
    }
    CHECK(drift == 0);
    CHECK(st.families.size() == 3);   // the bundled three; no user folder
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);
}

TEST_CASE("Fonts page: the Preferences tree has Appearance/Fonts and Scale with the page and the editor.ui cvars", "[settings-ui][editor]")
{
    Arcane::CVarRegistry& reg = Arcane::CVarRegistry::Get();
    const auto scale = reg.Describe("editor.ui.scale");
    REQUIRE(scale.has_value());
    CHECK(scale->categoryPath == kFontsPageCategory);

    SettingsModel m;
    m.SetPages({ SettingsPageRef{ Arcane::SettingScope::PreferencesMachine, std::string(kFontsPageCategory), "Fonts and Scale" } });
    m.Rebuild(reg, Arcane::SettingScope::PreferencesMachine);
    const SettingsTreeNode* node = m.Find(kFontsPageCategory);
    REQUIRE(node != nullptr);
    CHECK(node->label == "Fonts and Scale");
    for (const char* name : { "editor.ui.fontFamily", "editor.ui.monoFontFamily", "editor.ui.fontSize", "editor.ui.scale", "editor.ui.followDpi" })
        CHECK(std::find(node->cvars.begin(), node->cvars.end(), name) != node->cvars.end());
    CHECK(m.Find("Editor/Ui") == nullptr);
    CHECK(m.Find("editor.ui") == nullptr);
}
