// Settings arc S4 (spec s7.2): the node editor's built-in keys are off on every
// editor canvas -- EditorActions owns F, Delete and the clipboard there.
#include <catch2/catch_test_macros.hpp>
#include "Widgets/GraphCanvasStyle.hpp"
#include <imgui.h>
#include <imgui_node_editor.h>

namespace ed = ax::NodeEditor;

TEST_CASE("Graph canvases: ApplyGraphCanvasStyle switches the vendored shortcuts off", "[shortcuts]")
{
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ed::Config cfg;
    cfg.SettingsFile = nullptr;
    ed::EditorContext* canvas = ed::CreateEditor(&cfg);
    ed::SetCurrentEditor(canvas);
    REQUIRE(ed::AreShortcutsEnabled());
    Arcane::Editor::ApplyGraphCanvasStyle(Arcane::Editor::GraphCanvasStyleDesc{});
    CHECK_FALSE(ed::AreShortcutsEnabled());
    ed::SetCurrentEditor(nullptr);
    ed::DestroyEditor(canvas);
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);
}
