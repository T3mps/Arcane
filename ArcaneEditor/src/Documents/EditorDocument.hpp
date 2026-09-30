#pragma once

// EditorDocument: the thin per-asset editing unit (shader-editor Slice 5, spec
// Fold 3) -- open/dirty/save lifecycle over one GUID asset. Deliberately NOT a
// docking framework: each document draws its own ImGui window; DocumentHost
// owns the list, the unsaved-close confirm flow, and the asset-type -> factory
// routing. The shader editor is the first implementation.

#include "Panels/InspectorSource.hpp"

#include <Arcane/Guid.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace Arcane::Editor
{
    class EditorDocument : public InspectorSource
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

        // ---- Inspector source (inspector-ownership spec s3.2) -----------
        // A document that can SELECT opts in by overriding Page()/PageFor()/
        // SelectionKey()/RestoreSelection()/Resolves() and bumping
        // SelectionEpoch() on every selection change (the host's per-frame
        // poll turns the bump into InspectorHost::NotifySelected). The
        // defaults are "this document contributes no page and never drives
        // the Inspector" (MeshDocument today).
        std::string SourceName() const override { return Title(); }
        std::string_view Kind() const override { return {}; }
        InspectorPage* Page() override { return nullptr; }
        InspectorPage* PageFor(std::string_view) override { return nullptr; }
        std::string SelectionKey() const override { return {}; }
        bool RestoreSelection(std::string_view) override { return false; }
        bool Resolves(std::string_view) const override { return false; }
        // Monotonic; changes whenever the document's own selection changes.
        [[nodiscard]] virtual std::uint64_t SelectionEpoch() const { return 0; }
        // --select-in-document: select by a human path ("Player/Jump"). False
        // = the document has no such notion or the path did not resolve.
        virtual bool SelectByPath(std::string_view) { return false; }
    };
}
