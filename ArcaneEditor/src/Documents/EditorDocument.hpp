#pragma once

// EditorDocument: the thin per-asset editing unit (shader-editor Slice 5, spec
// Fold 3) -- open/dirty/save lifecycle over one GUID asset. Deliberately NOT a
// docking framework: each document draws its own ImGui window; DocumentHost
// owns the list, the unsaved-close confirm flow, and the asset-type -> factory
// routing. The shader editor is the first implementation.

#include <Arcane/Guid.hpp>

#include <string>

namespace Arcane::Editor
{
    class EditorDocument
    {
    public:
        virtual ~EditorDocument() = default;

        virtual const std::string& Title() const = 0;     // window title (stable, unique)
        virtual Arcane::Guid AssetGuid() const = 0;       // identity (nil for unsaved-new)
        virtual bool Dirty() const = 0;
        virtual bool Save() = 0;                          // false = save failed/refused

        // True when this document's window, or a child of it, held keyboard
        // focus at its last Draw. The document owns its own Begin/End, so it is
        // the only thing that can answer.
        //
        // Documents bind their own Ctrl+S with ImGui::Shortcut, which routes to
        // the focused window on its own and needs none of this. What needs it is
        // the APP: its scene-level Ctrl+S is a raw-scancode keybind that knows
        // nothing about ImGui's routing, so without a way to ask "is the user
        // inside a document?" one keypress would save the document AND the
        // scene. DocumentHost::FocusedDoc is that question.
        virtual bool WindowFocused() const { return false; }

        // Per-frame: advance async work (compiles, previews). dt in seconds.
        virtual void Tick(double dt) {}

        // Draw the document's ImGui window. Set `requestClose` true when the
        // user asked to close it (window X / shortcut); the HOST runs the
        // dirty-confirm flow -- documents never delete themselves.
        virtual void Draw(bool& requestClose) = 0;
    };
}
