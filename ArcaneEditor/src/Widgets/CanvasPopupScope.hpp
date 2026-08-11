#pragma once

// Arcane::Editor::CanvasPopupScope -- the node-canvas popup rule, named.
//
// imgui-node-editor draws inside a TRANSFORMED coordinate space (pan and zoom),
// and an ImGui popup positions itself in screen space. Opening one from inside a
// canvas without leaving that space strands or misplaces it. ed::Suspend() exits
// the canvas's space and ed::Resume() re-enters it, which is why every canvas
// popup must be bracketed.
//
// This exists because the rule was FOLKLORE and was already got wrong in writing:
// a comment at the ConstColor node asserted that "popups cannot open inside the
// canvas" while three working Suspend/Resume brackets sat above it in the same
// file. One named type with the reason attached is cheaper than the next person
// re-deriving it wrongly.
//
// Its own header rather than part of EditorWidgets: that vocabulary includes only
// imgui.h plus Astra::Range, and pulling imgui_node_editor.h into it would couple
// every editor widget to the node editor.

#include <imgui_node_editor.h>

namespace Arcane::Editor
{
    namespace ed = ax::NodeEditor;

    struct CanvasPopupScope
    {
        CanvasPopupScope()  { ed::Suspend(); }
        ~CanvasPopupScope() { ed::Resume();  }
        CanvasPopupScope(const CanvasPopupScope&)            = delete;
        CanvasPopupScope& operator=(const CanvasPopupScope&) = delete;
    };
}
