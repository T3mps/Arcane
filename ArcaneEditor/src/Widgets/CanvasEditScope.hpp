#pragma once

// Arcane::Editor::CanvasCreateScope / CanvasDeleteScope -- the node-canvas
// create/delete bracket rule, named.
//
// THE RULE: ed::EndCreate() and ed::EndDelete() are called UNCONDITIONALLY,
// even on the frames where the matching Begin returned false.
//
// WHY, precisely: CreateItemAction::Begin() arms m_InActive even when it
// answers false (the idle frame), and a skipped End asserts on the NEXT frame's
// Begin. That is a crash you cannot see in one frame -- the recorded desk
// symptom was "frame 1 fine, frame 2 abort" -- which is exactly the shape of
// rule that should not be left to a comment. DeleteItemAction behaves the same
// way.
//
// This exists because the rule was written out as PROSE in four places across
// two files, each restating the reason, and one of the four cited another by
// line number. The precedent for turning that into a type is CanvasPopupScope
// next door, which was created for the same reason (a rule that was folklore,
// and had already been got wrong in writing). A destructor cannot be forgotten
// on the one path a reader did not check.
//
// Both types are bool-convertible: the value is what the Begin returned, which
// is also the honest "is a gesture live at all" answer -- CreateItemAction
// reports true for every frame of a drag and for the release frame, and false
// once it is over.
//
// Its own header rather than part of EditorWidgets: that vocabulary is imgui.h
// plus Astra::Range only, and pulling imgui_node_editor.h into it would couple
// every editor widget to the node editor (CanvasPopupScope.hpp:16-19, which
// makes the same refusal for the same reason). One file for the two types
// because they encode ONE rule, stated once above.

#include <imgui.h>
#include <imgui_node_editor.h>

namespace Arcane::Editor
{
    namespace ed = ax::NodeEditor;

    // The create bracket. `color`/`thickness` are ed::BeginCreate's own
    // defaults unless given; a canvas that draws its own in-flight wire passes
    // a transparent colour and the REAL thickness, so the library's hit test
    // still sizes the drag correctly while painting nothing.
    struct CanvasCreateScope
    {
        CanvasCreateScope()
            : m_active(ed::BeginCreate()) {}
        CanvasCreateScope(const ImVec4& color, float thickness)
            : m_active(ed::BeginCreate(color, thickness)) {}
        ~CanvasCreateScope() { ed::EndCreate(); }

        CanvasCreateScope(const CanvasCreateScope&)            = delete;
        CanvasCreateScope& operator=(const CanvasCreateScope&) = delete;

        explicit operator bool() const noexcept { return m_active; }

    private:
        bool m_active;
    };

    // The delete bracket. Same rule, same reason.
    struct CanvasDeleteScope
    {
        CanvasDeleteScope() : m_active(ed::BeginDelete()) {}
        ~CanvasDeleteScope() { ed::EndDelete(); }

        CanvasDeleteScope(const CanvasDeleteScope&)            = delete;
        CanvasDeleteScope& operator=(const CanvasDeleteScope&) = delete;

        explicit operator bool() const noexcept { return m_active; }

    private:
        bool m_active;
    };
}
