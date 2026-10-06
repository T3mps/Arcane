// Settings arc S4 (spec s7.3): the font family list (bundled + the user's
// fonts folder) and the editor.ui.* registration.
#include <catch2/catch_test_macros.hpp>
#include "Settings/EditorUiSettings.hpp"
#include "Widgets/EditorFonts.hpp"
#include <Arcane/Config/CVarRegistry.hpp>
#include <imgui.h>
#include <filesystem>
#include <fstream>

using namespace Arcane::Editor;

TEST_CASE("Editor fonts: bundled families first, then the user's .ttf/.otf; resolve falls back", "[settings-ui][editor]")
{
    const std::filesystem::path exe = "C:/fake/exe";
    const std::filesystem::path user = std::filesystem::temp_directory_path() / "s4-fonts";
    std::filesystem::create_directories(user);
    { std::ofstream(user / "Zed Mono.ttf") << "x"; std::ofstream(user / "Atkinson.otf") << "x"; std::ofstream(user / "notes.txt") << "x"; }
    const auto families = ListEditorFontFamilies(exe, user);
    REQUIRE(families.size() == 5);
    CHECK(families[0].name == "Inter");
    CHECK(families[1].name == "Roboto");
    CHECK(families[2].name == "JetBrains Mono");
    CHECK(families[3].name == "Atkinson");
    CHECK(families[4].name == "Zed Mono");
    CHECK_FALSE(families[3].bundled);
    CHECK(ResolveEditorFontFamily(families, "Zed Mono", "Inter") == user / "Zed Mono.ttf");
    CHECK(ResolveEditorFontFamily(families, "Missing", "Inter") == families[0].file);
    CHECK(DefaultEditorFontRequest(exe).uiFace == families[0].file);
    CHECK(DefaultEditorFontRequest(exe).sizePx == 16.0f);
    std::filesystem::remove_all(user);
}

TEST_CASE("EditorUiSettings registers editor.ui.* with today's defaults", "[settings-ui][editor]")
{
    Arcane::CVarRegistry& reg = Arcane::CVarRegistry::Get();
    reg.PublishImmediate();
    const auto get = [&](std::string_view n) { const auto v = reg.Get(reg.Find(n)); REQUIRE(v.has_value()); return *v; };
    CHECK(get("editor.ui.fontFamily").AsString() == "Inter");
    CHECK(get("editor.ui.monoFontFamily").AsString() == "JetBrains Mono");
    CHECK(get("editor.ui.fontSize").AsFloat32() == 16.0f);
    CHECK(get("editor.ui.scale").AsFloat32() == 1.0f);
    CHECK(get("editor.ui.followDpi").AsBool() == false);
}

namespace
{
    // A fresh ImGui context made current for one test; the font handles it
    // installs are forgotten before it dies so GetEditorFonts() stays all-null
    // for the headless tests (EditorWidgetsTest's MonoFont case).
    struct ScopedFontContext
    {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGuiContext* ctx  = ImGui::CreateContext();
        ScopedFontContext() { ImGui::SetCurrentContext(ctx); }
        ~ScopedFontContext()
        {
            ForgetEditorFonts();
            ImGui::DestroyContext(ctx);
            ImGui::SetCurrentContext(prev);
        }
    };

    void RequireBundledFallback(const EditorFontRequest& request)
    {
        const ScopedFontContext scope;
        ImGuiIO& io = ImGui::GetIO();
        REQUIRE(io.ConfigErrorRecoveryEnableAssert);   // ImGui's default: a user error WOULD assert
        const EditorFontSet& fonts = InstallEditorFonts(request);   // must not assert (Debug)
        CHECK(io.ConfigErrorRecoveryEnableAssert);     // the scoped clear was restored
        REQUIRE(fonts.interRegular != nullptr);
        REQUIRE(fonts.mono != nullptr);
        REQUIRE(io.Fonts->Fonts.Size > 0);
        CHECK(io.Fonts->Fonts[0] == fonts.interRegular);
        CHECK(fonts.mono != fonts.interRegular);
        CHECK(fonts.roboto != nullptr);
        CHECK(fonts.brand != nullptr);
        CHECK(io.Fonts->Fonts.Size == 4);              // the failed faces were rolled back out
    }
}

TEST_CASE("Editor fonts: an unloadable user font falls back to the bundled face without asserting", "[settings-ui][editor]")
{
    // Runs from the exe dir: ArcaneTests' postbuild stages the bundled fonts there.
    const std::filesystem::path bundledUi = std::filesystem::current_path() / "data" / "font" / "inter" / "static" / "Inter_18pt-Regular.ttf";
    REQUIRE(std::filesystem::exists(bundledUi));

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "s4-fonts-bad";
    std::filesystem::create_directories(dir);
    const std::filesystem::path oneByte = dir / "OneByte.ttf";
    { std::ofstream(oneByte, std::ios::binary) << "x"; }
    // A valid sfnt tag with zero tables, past ImGui's 100-byte floor, passes the
    // probe and fails inside the font loader (stbtt_InitFont finds no cmap, an
    // IM_ASSERT_USER_ERROR): the scoped ConfigErrorRecoveryEnableAssert path.
    const std::filesystem::path noTables = dir / "NoTables.ttf";
    {
        const unsigned char bytes[128] = { 0x00, 0x01, 0x00, 0x00 };
        std::ofstream(noTables, std::ios::binary).write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
    }

    SECTION("a 1-byte .ttf as the UI and mono face")
    {
        RequireBundledFallback({ oneByte, oneByte, 16.0f });
    }
    SECTION("a .ttf with a font header but no tables")
    {
        RequireBundledFallback({ noTables, noTables, 16.0f });
    }
    SECTION("a missing file")
    {
        RequireBundledFallback({ dir / "Gone.ttf", dir / "Gone.otf", 16.0f });
    }
    std::filesystem::remove_all(dir);
}
