// DrawConsoleInputLine (node-page phase s8.2): Tab completes, Enter submits and
// keeps focus, Up recalls -- driven through the real ImGui input path, device-less.
#include <catch2/catch_test_macros.hpp>
#include <Arcane/ImGui/ConsoleInputLine.hpp>
#include <imgui.h>

namespace
{
    struct LineHarness
    {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGuiContext* ctx = nullptr;
        Arcane::CVarRegistry reg;
        Arcane::ConsoleModel model;
        bool submitted = false;
        bool focusNext = true;
        LineHarness()
        {
            REQUIRE_FALSE(reg.Register(Arcane::CVarDesc{ "game.speed", Arcane::CVarType::Int32, Arcane::CVarValue::Int32(1),
                                                         {}, {}, {}, "test cvar", "engine" }).IsStale());
            ctx = ImGui::CreateContext();
            ImGui::SetCurrentContext(ctx);
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(800, 600);
            io.IniFilename = nullptr;
            unsigned char* px = nullptr; int w = 0, h = 0;
            io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        }
        ~LineHarness() { ImGui::DestroyContext(ctx); ImGui::SetCurrentContext(prev); }
        void Frame()
        {
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(640, 200));
            ImGui::Begin("ConsoleLineTest");
            if (focusNext) { ImGui::SetKeyboardFocusHere(); focusNext = false; }
            submitted = Arcane::DrawConsoleInputLine("##line", model, reg, Arcane::Permission::Editor) || submitted;
            ImGui::End();
            ImGui::Render();
        }
        void Key(ImGuiKey k) { ImGui::GetIO().AddKeyEvent(k, true); Frame(); ImGui::GetIO().AddKeyEvent(k, false); Frame(); }
        void Type(const char* s) { ImGui::GetIO().AddInputCharactersUTF8(s); Frame(); }
    };
}

TEST_CASE("DrawConsoleInputLine: Tab completes, Enter submits and keeps focus, Up recalls", "[cvar]")
{
    LineHarness h;
    h.Frame(); h.Frame(); h.Frame();
    REQUIRE(ImGui::GetIO().WantTextInput);       // the line took keyboard focus
    h.Type("game.sp");
    h.Key(ImGuiKey_Tab);
    CHECK(h.model.Input() == "game.speed ");
    h.Type("3");
    h.Key(ImGuiKey_Enter);
    CHECK(h.submitted);
    CHECK(h.model.Input().empty());
    REQUIRE(h.model.Lines().size() == 2);
    CHECK(h.model.Lines()[0].text == "> game.speed 3");
    h.Frame(); h.Frame();
    CHECK(ImGui::GetIO().WantTextInput);         // focus kept after Enter
    h.Key(ImGuiKey_UpArrow);
    CHECK(h.model.Input() == "game.speed 3");
}
