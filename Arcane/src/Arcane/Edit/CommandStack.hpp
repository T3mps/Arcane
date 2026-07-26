#pragma once

// Arcane/Edit: undo/redo history. The undo unit is a Transaction of 1..N
// ComponentEditCommands (Unreal FTransaction model). Begin/SnapshotComponent
// (idempotent snapshot-on-first-touch)/Commit/Cancel groups a gesture into one
// step. ARCANE_API; Arcane Editor owns one and brackets its Inspector edits.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Edit/Command.hpp>

#include <Astra/Entity/Entity.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Astra { class Registry; struct ComponentDescriptor; }

namespace Arcane
{
    // Ownership token for ONE open transaction, minted by CommandStack::Begin.
    // Monotonic per stack; `None` owns nothing. Only the call that actually
    // opened a transaction receives its live id, and Commit/Cancel ignore any
    // other id -- see Begin for why that has to be enforced rather than trusted.
    enum class TransactionId : std::uint64_t { None = 0 };

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
        //
        // Opens a transaction and returns its OWNER TOKEN. When one is already
        // open the FIRST is kept and this returns TransactionId::None -- the
        // caller has JOINED it: its snapshots ride along and are committed (or
        // discarded) by the real owner, so joining never loses an edit.
        //
        // Ownership is a checked token rather than a convention because several
        // INDEPENDENT input consumers share one stack -- a gizmo drag, an
        // Inspector field gesture (both spanning frames), and the Inspector's
        // single-shot immediate edits -- and ImGui fires two of them in ONE frame
        // routinely: pressing a gizmo handle clears the ActiveId of a text box
        // holding uncommitted text, so that box's deactivate-after-edit lands in
        // the same frame as the press. Under the previous `void Begin` +
        // unconditional Commit, the single-shot path's Commit closed the GIZMO's
        // transaction; the remainder of the drag then mutated Transforms against
        // a closed stack and the mouse-up Commit no-opped, making the whole drag
        // silently un-undoable. A monotonic token also makes a STALE owner inert:
        // a consumer that already committed cannot reach into whatever
        // transaction happens to be open now, which a bool "I opened it" flag
        // would happily do.
        [[nodiscard]] TransactionId Begin(std::string label);
        // Idempotent before-snapshot of (entity, descriptor) into the open
        // transaction. Call BEFORE the live edit mutates the component.
        void SnapshotComponent(Astra::Entity entity, const Astra::ComponentDescriptor* descriptor);
        // Both no-op unless `owner` is the currently-open transaction's token, so
        // a joiner (None) and a stale owner are inert instead of clobbering the
        // consumer that owns the stack now.
        void Commit(TransactionId owner);   // capture afters; push if any changed; clear redo; close
        void Cancel(TransactionId owner);   // discard the open transaction (no push, no revert)

        // Push an ALREADY-APPLIED generic command (the ICommand contract: the
        // live edit happened, the command only reverses/replays). Joins the open
        // transaction when one is open (committed/cancelled with it), otherwise
        // becomes its own one-command undo step labeled by cmd->Label(). This is
        // the non-component edit path -- material param edits, and later graph
        // edits, share the ONE undo history through it.
        void Push(std::unique_ptr<ICommand> command);

        void Undo();
        void Redo();
        [[nodiscard]] bool CanUndo() const noexcept { return !m_undo.empty(); }
        [[nodiscard]] bool CanRedo() const noexcept { return !m_redo.empty(); }
        // Structural mementos refuse to run inside an open gesture (Cancel
        // would discard their undo coverage without reverting the edit --
        // see ApplyRegistryMutation).
        [[nodiscard]] bool InTransaction() const noexcept { return m_openId != TransactionId::None; }
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

        // m_openId doubles as the "is one open" flag (None = closed); m_nextId
        // only ever increases, so a committed token can never be re-minted.
        TransactionId        m_openId = TransactionId::None;
        std::uint64_t        m_nextId = 1;
        std::string          m_openLabel;
        std::vector<Pending> m_pending;
        std::vector<std::unique_ptr<ICommand>> m_pendingGeneric;   // Push while open
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

    // RAII form for single-scope edits (gizmo drag-commit, programmatic
    // multi-edit, the Inspector's single-shot immediate edits). Commits in the
    // dtor unless Cancel() was called -- but ONLY if it opened the transaction:
    // constructed inside a live gesture it JOINS, and then neither commits nor
    // cancels, leaving that to the owner. NOT for a gesture that spans frames
    // (an Inspector field drag): the token has to outlive the scope, so those
    // use explicit Begin/Commit with the token parked in persistent state.
    class ARCANE_API ScopedTransaction
    {
    public:
        ScopedTransaction(CommandStack& stack, std::string label);
        ~ScopedTransaction();
        void Snapshot(Astra::Entity entity, const Astra::ComponentDescriptor* descriptor);
        void Cancel() noexcept { m_cancelled = true; }
        // False when this scope joined an already-open transaction rather than
        // opening its own (so the dtor will leave the stack alone).
        [[nodiscard]] bool OwnsTransaction() const noexcept { return m_id != TransactionId::None; }

        ScopedTransaction(const ScopedTransaction&) = delete;
        ScopedTransaction& operator=(const ScopedTransaction&) = delete;

    private:
        CommandStack& m_stack;
        TransactionId m_id = TransactionId::None;
        bool          m_cancelled = false;
    };
}
