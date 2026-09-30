#pragma once

// DocumentPageSelection (inspector filters spec 2026-09-29 s3/s6a): the
// selection half of a document whose Inspector page is the WHOLE document's
// (material, sprite, mesh): one key, selected when the document opens,
// re-selected by a click in its content -- the spec's one selection rule
// (open + click select; focus and tab switches never do). Header-only; ImGui
// calls only in NoteContentClick.

#include <imgui.h>
#include <imgui_internal.h>   // ImGuiWindow::InnerRect

#include <cstdint>
#include <string>
#include <string_view>

namespace Arcane::Editor
{
    struct DocumentPageSelection
    {
        std::string   key;         // "material" / "sprite" / "mesh"
        std::uint64_t epoch = 1;   // 1 = selected at open: the app's per-document epoch map starts at 0, so frame 1 is an event
        [[nodiscard]] std::string SelectionKey() const { return key; }
        [[nodiscard]] bool Resolves(std::string_view k) const { return k == key; }
        // Call between the document window's Begin/End, after its content: a
        // mouse click (left/right/middle) inside the window's INNER rect -- never
        // the title bar or a dock tab -- bumps the epoch. Tab switches and focus
        // never do (they are not clicks in the content).
        void NoteContentClick();
    };

    inline void DocumentPageSelection::NoteContentClick()
    {
        ImGuiWindow* w = ImGui::GetCurrentWindow();
        const ImGuiIO& io = ImGui::GetIO();
        const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)
                          || ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
        // ChildWindows: the snippet InputTextMultiline and the ##preview child
        // are real child windows; the node-editor canvas draws in the document
        // window itself (its BeginChild is commented out in the vendored
        // imgui-node-editor) and restores io.MousePos at ed::End, so by the
        // time this runs (after the content) the coordinates are screen-space.
        // AllowWhenBlockedByActiveItem: a press that ACTIVATES a widget (a node
        // drag, a text field) is still a click in the content.
        if (clicked && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)
            && w->InnerRect.Contains(io.MousePos))
            ++epoch;
    }
}
