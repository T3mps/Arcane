#pragma once

// Arcane/Edit: undo/redo history. The undo unit is a Transaction of 1..N
// ComponentEditCommands (Unreal FTransaction model). Begin/SnapshotComponent
// (idempotent snapshot-on-first-touch)/Commit/Cancel groups a gesture into one
// step. ARCANE_API; Arcane Editor owns one and brackets its Inspector edits.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Edit/Command.hpp>

#include <Astra/Entity/Entity.hpp>

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Astra { class Registry; struct ComponentDescriptor; }

namespace Arcane
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // std::function/deque/vector/string members on a dll-exported class: benign under /MD (shared CRT heap)
#endif
    class ARCANE_API CommandStack
    {
    public:
        // `resolve` returns the CURRENT live registry each call (see
        // ComponentEditCommand's ctor comment) -- the stack never caches a
        // Registry& itself, so it survives Runtime::RestoreRegistry/ResetRegistry
        // swapping the registry object out from under it.
        explicit CommandStack(std::function<Astra::Registry&()> resolve, std::size_t maxDepth = 100);

        // Non-copyable: m_undo/m_redo hold move-only ICommand transactions, and
        // this class is dllexport'd -- MSVC eagerly instantiates implicit
        // special members for exported classes, so an implicit copy ctor would
        // hard-error trying to copy std::unique_ptr<ICommand>. Delete explicitly.
        CommandStack(const CommandStack&) = delete;
        CommandStack& operator=(const CommandStack&) = delete;

        // Transaction grouping.
        void Begin(std::string label);   // open a transaction (no-op if one is open)
        // Idempotent before-snapshot of (entity, descriptor) into the open
        // transaction. Call BEFORE the live edit mutates the component.
        void SnapshotComponent(Astra::Entity entity, const Astra::ComponentDescriptor* descriptor);
        void Commit();   // capture afters; push if any changed; clear redo; close
        void Cancel();   // discard the open transaction (no push, no revert)

        void Undo();
        void Redo();
        [[nodiscard]] bool CanUndo() const noexcept { return !m_undo.empty(); }
        [[nodiscard]] bool CanRedo() const noexcept { return !m_redo.empty(); }
        [[nodiscard]] const char* UndoLabel() const noexcept;
        [[nodiscard]] const char* RedoLabel() const noexcept;
        void Clear() noexcept;

    private:
        struct Transaction
        {
            std::string label;
            std::vector<std::unique_ptr<ICommand>> commands;
        };
        struct Pending
        {
            Astra::Entity                     entity;
            const Astra::ComponentDescriptor* descriptor;
            std::vector<std::byte>            before;
        };

        std::function<Astra::Registry&()> m_resolve;
        std::size_t                       m_maxDepth;

        std::deque<Transaction> m_undo;
        std::deque<Transaction> m_redo;

        bool                 m_open = false;
        std::string          m_openLabel;
        std::vector<Pending> m_pending;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

    // RAII form for single-scope edits (gizmo drag-commit, programmatic
    // multi-edit). Commits in the dtor unless Cancel() was called. NOT for the
    // Inspector -- its gesture spans frames, so it uses explicit Begin/Commit.
    class ARCANE_API ScopedTransaction
    {
    public:
        ScopedTransaction(CommandStack& stack, std::string label);
        ~ScopedTransaction();
        void Snapshot(Astra::Entity entity, const Astra::ComponentDescriptor* descriptor);
        void Cancel() noexcept { m_cancelled = true; }

        ScopedTransaction(const ScopedTransaction&) = delete;
        ScopedTransaction& operator=(const ScopedTransaction&) = delete;

    private:
        CommandStack& m_stack;
        bool          m_cancelled = false;
    };
}
