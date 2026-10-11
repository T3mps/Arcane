// DocumentPageSelection (spec 2026-09-29 s3): opened = selected; a click in a
// document's CONTENT re-selects its page; the title bar / a tab never does.
#include <catch2/catch_test_macros.hpp>
#include <Documents/DocumentPageSelection.hpp>
#include <imgui.h>
#include <optional>

using namespace Arcane::Editor;

namespace
{
    struct BareContext
    {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGuiContext* ctx = ImGui::CreateContext();
        BareContext()
        {
            ImGui::SetCurrentContext(ctx);
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = ImVec2(800.0f, 600.0f);
            unsigned char* pixels = nullptr;
            int w = 0, h = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        }
        ~BareContext() { ImGui::DestroyContext(ctx); ImGui::SetCurrentContext(prev); }
    };
}

TEST_CASE("DocumentPageSelection: selected at open; a content click re-selects; a click outside does not", "[editor][inspector]")
{
    BareContext bc;
    DocumentPageSelection sel{ "material" };
    CHECK(sel.epoch == 1);
    CHECK(sel.Resolves("material"));
    CHECK_FALSE(sel.Resolves("mesh"));

    const ImVec2 pos(100.0f, 100.0f), size(300.0f, 200.0f);
    // Every frame submits the same window at the same rect: IsWindowHovered
    // reads the hovered window NewFrame computed from the PREVIOUS frame's
    // windows, so the first frame is a warm-up. A button event only registers
    // as a click when it CHANGES the button state, hence the release frames.
    auto frame = [&](std::optional<ImVec2> mouse, std::optional<bool> leftDown)
    {
        ImGuiIO& io = ImGui::GetIO();
        if (mouse) io.AddMousePosEvent(mouse->x, mouse->y);
        if (leftDown) io.AddMouseButtonEvent(ImGuiMouseButton_Left, *leftDown);
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
        ImGui::Begin("doc");
        sel.NoteContentClick();
        ImGui::End();
        ImGui::Render();                                             // draw data discarded -- no backend
    };

    frame(std::nullopt, std::nullopt);                               // warm-up: "doc" exists
    frame(ImVec2(pos.x + 150.0f, pos.y + 100.0f), true);             // A: press in the content
    CHECK(sel.epoch == 2);
    frame(std::nullopt, false);                                      // release
    frame(ImVec2(pos.x + 150.0f, pos.y + 4.0f), true);               // B: press on the TITLE BAR
    CHECK(sel.epoch == 2);
    frame(std::nullopt, false);                                      // release
    frame(std::nullopt, std::nullopt);                               // C: no click
    CHECK(sel.epoch == 2);
}
