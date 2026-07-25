#pragma once

// Arcane/Edit: whole-registry memento for STRUCTURAL edits (create/delete/
// reparent/add/remove component/hide/rename). Field edits keep the
// fine-grained ComponentEditCommand path; structure uses this because binary
// registry restore resurrects EXACT entity ids -- so delete-undo, create-redo,
// and every later stack entry that references an entity by id stay valid
// (the spec's id-resurrection risk, resolved at planning).
//
// Snapshot/restore are injected seams: the editor binds
// Runtime::SnapshotRegistry / Runtime::RestoreRegistry (restore REPLACES the
// registry object -- the CommandStack's resolve-every-call design already
// survives that, see CommandStack's ctor contract); tests bind
// Registry::Save / Registry::Load over a local slot.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Edit/Command.hpp>
#include <Arcane/Util/FunctionRef.hpp>

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace Arcane
{
    class CommandStack;

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // std members on a dll-exported class: benign under /MD
#endif
    class ARCANE_API RegistryStateCommand final : public ICommand
    {
    public:
        using SnapshotFn = std::function<std::vector<std::byte>()>;       // empty = failed
        using RestoreFn  = std::function<bool(std::span<const std::byte>)>;

        // `before` is the registry state BEFORE the (already applied) edit.
        RegistryStateCommand(std::string label, SnapshotFn snapshot,
                             RestoreFn restore, std::vector<std::byte> before);

        // First Undo captures the CURRENT state as the redo target, then
        // restores `before`. A failed capture warns and still restores
        // (undo works; redo becomes a warned no-op).
        void Undo() override;
        void Redo() override;
        const char* Label() const override;

    private:
        std::string            m_label;
        SnapshotFn             m_snapshot;
        RestoreFn              m_restore;
        std::vector<std::byte> m_before;
        std::vector<std::byte> m_after;   // captured on first Undo
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

    // The one structural-edit entry point: snapshot -> mutate() -> push.
    // Refuses (false, nothing runs) when the before-snapshot fails -- a
    // structural edit without undo coverage must not happen silently.
    // Skips the push (edit stands, no undo step) when mutate() reports no
    // change, so no-op edits never pollute the history. Returns mutate()'s
    // result.
    ARCANE_API bool ApplyRegistryMutation(CommandStack& stack, std::string label,
                                          const RegistryStateCommand::SnapshotFn& snapshot,
                                          const RegistryStateCommand::RestoreFn& restore,
                                          FunctionRef<bool()> mutate);
}
