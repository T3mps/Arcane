#include <Arcane/Edit/CommandStack.hpp>

#include <Arcane/Edit/ComponentEditCommand.hpp>

#include <algorithm>
#include <utility>

namespace Arcane
{
    CommandStack::CommandStack(std::function<Astra::Registry&()> resolve)
        : m_resolve(std::move(resolve))
    {
    }

    void CommandStack::SetLimits(UndoLimits limits)
    {
        m_limits = limits;
        if (m_limits.maxSteps == 0)
            m_limits.maxSteps = 1;   // the old ctor's clamp
    }

    // Oldest-first while over EITHER bound. The top is never evicted.
    // PayloadBytes is summed live: a RegistryStateCommand gains its redo blob
    // on first Undo, after it was pushed.
    void CommandStack::Evict()
    {
        const auto bytesOf = [](const Transaction& t)
        {
            std::uint64_t n = 0;
            for (const auto& c : t.commands) n += c->PayloadBytes();
            return n;
        };
        std::uint64_t total = 0;
        for (const Transaction& t : m_undo) total += bytesOf(t);
        while (m_undo.size() > 1 &&
               (m_undo.size() > m_limits.maxSteps || total > m_limits.byteBudget))
        {
            if (m_undo.front().affectsScene)
                m_evictedSceneId = m_undo.front().id;   // oldest-first: the last one popped is the newest
            total -= bytesOf(m_undo.front());
            m_undo.pop_front();
        }
    }

    TransactionId CommandStack::Begin(std::string label)
    {
        if (m_openId != TransactionId::None)
            return TransactionId::None;   // already open; keep the first, caller JOINS it
        m_openId = static_cast<TransactionId>(m_nextId++);
        m_openLabel = std::move(label);
        m_pending.clear();
        m_pendingTouched.clear();
        return m_openId;
    }

    void CommandStack::SnapshotComponent(Astra::Entity entity,
                                         const Astra::ComponentDescriptor* descriptor)
    {
        if (m_openId == TransactionId::None || !descriptor)
            return;
        // Idempotent: first touch of (entity, descriptor) snapshots; later ones no-op.
        for (const Pending& p : m_pending)
            if (p.entity == entity && p.descriptor == descriptor)
                return;
        m_pending.push_back(Pending{
            entity, descriptor,
            ComponentEditCommand::Snapshot(m_resolve(), entity, descriptor) });
    }

    void CommandStack::Commit(TransactionId owner)
    {
        // Not the owner (a joiner passes None; a committed consumer passes a
        // stale id) -> leave the open transaction to whoever owns it.
        if (owner == TransactionId::None || owner != m_openId)
            return;
        Transaction txn;
        txn.label = m_openLabel;
        for (Pending& p : m_pending)
        {
            std::vector<std::byte> after =
                ComponentEditCommand::Snapshot(m_resolve(), p.entity, p.descriptor);
            if (after == p.before)
                continue;   // unchanged -> drop
            // CHANGED snapshots only: an entity that was snapshotted but not
            // actually edited must not pick up an unsaved marker.
            txn.touched.push_back(p.entity);
            txn.commands.push_back(std::make_unique<ComponentEditCommand>(
                m_resolve, p.entity, p.descriptor,
                std::move(p.before), std::move(after), m_openLabel));
        }
        for (auto& c : m_pendingGeneric)
            txn.commands.push_back(std::move(c));
        txn.touched.insert(txn.touched.end(),
                           m_pendingTouched.begin(), m_pendingTouched.end());
        m_openId = TransactionId::None;
        m_pending.clear();
        m_pendingGeneric.clear();
        m_pendingTouched.clear();
        if (txn.commands.empty())
            return;   // nothing changed -> no history entry
        txn.affectsScene = std::any_of(txn.commands.begin(), txn.commands.end(),
                                       [](const std::unique_ptr<ICommand>& c) { return c->AffectsScene(); });

        // Stamp the state this transaction produced. m_nextId is the same
        // monotonic source TransactionId::Begin draws from, so ids are unique
        // across BOTH uses and a committed state id can never collide with a
        // live transaction token.
        txn.id = m_nextId++;
        m_undo.push_back(std::move(txn));
        m_redo.clear();
        Evict();
    }

    void CommandStack::Cancel(TransactionId owner)
    {
        if (owner == TransactionId::None || owner != m_openId)
            return;   // see Commit: only the owner may discard.
        m_openId = TransactionId::None;
        m_pending.clear();
        m_pendingGeneric.clear();
        m_pendingTouched.clear();
    }

    void CommandStack::Push(std::unique_ptr<ICommand> command,
                            std::span<const Astra::Entity> touched)
    {
        if (!command)
            return;
        if (m_openId != TransactionId::None)
        {
            m_pendingGeneric.push_back(std::move(command));
            m_pendingTouched.insert(m_pendingTouched.end(), touched.begin(), touched.end());
            return;
        }
        Transaction txn;
        txn.label = command->Label();
        txn.touched.assign(touched.begin(), touched.end());
        txn.affectsScene = command->AffectsScene();
        txn.commands.push_back(std::move(command));
        // See Commit: same stamp-before-push rule, same shared m_nextId source.
        txn.id = m_nextId++;
        m_undo.push_back(std::move(txn));
        m_redo.clear();
        Evict();
    }

    bool CommandStack::Expired(const Transaction& t) noexcept
    {
        return !t.commands.empty() &&
               std::all_of(t.commands.begin(), t.commands.end(),
                           [](const std::unique_ptr<ICommand>& c) { return c->IsExpired(); });
    }

    void CommandStack::DiscardExpired(std::deque<Transaction>& d) noexcept
    {
        while (!d.empty() && Expired(d.back()))
            d.pop_back();
    }

    const CommandStack::Transaction* CommandStack::TopLive(const std::deque<Transaction>& d) noexcept
    {
        for (auto it = d.rbegin(); it != d.rend(); ++it)
            if (!Expired(*it))
                return &*it;
        return nullptr;
    }

    bool CommandStack::CanUndo() const noexcept { return TopLive(m_undo) != nullptr; }
    bool CommandStack::CanRedo() const noexcept { return TopLive(m_redo) != nullptr; }

    void CommandStack::Undo()
    {
        DiscardExpired(m_undo);
        if (m_undo.empty())
            return;
        Transaction txn = std::move(m_undo.back());
        m_undo.pop_back();
        for (auto it = txn.commands.rbegin(); it != txn.commands.rend(); ++it)
            (*it)->Undo();   // reverse order
        m_redo.push_back(std::move(txn));
    }

    void CommandStack::Redo()
    {
        DiscardExpired(m_redo);
        if (m_redo.empty())
            return;
        Transaction txn = std::move(m_redo.back());
        m_redo.pop_back();
        for (auto& c : txn.commands)
            c->Redo();       // forward order
        m_undo.push_back(std::move(txn));
    }

    std::uint64_t CommandStack::SceneStateId() const noexcept
    {
        for (auto it = m_undo.rbegin(); it != m_undo.rend(); ++it)
            if (it->affectsScene)
                return it->id;
        // Eviction took every scene step: the scene is still in the state the
        // newest evicted one produced, so a save under N document edits keeps
        // reading clean and undo can never reach below it.
        return m_evictedSceneId;
    }

    const char* CommandStack::UndoLabel() const noexcept
    {
        const Transaction* t = TopLive(m_undo);
        return t ? t->label.c_str() : "";
    }
    const char* CommandStack::RedoLabel() const noexcept
    {
        const Transaction* t = TopLive(m_redo);
        return t ? t->label.c_str() : "";
    }

    void CommandStack::Clear(std::string reason)
    {
        m_clearedReason = std::move(reason);
        m_undo.clear();
        m_redo.clear();
        m_evictedSceneId = 0;
        m_openId = TransactionId::None;
        m_pending.clear();
        m_pendingGeneric.clear();
        m_pendingTouched.clear();
    }

    CommandStack::TouchedSince CommandStack::TouchedSinceState(std::uint64_t savedStateId) const
    {
        TouchedSince out;
        if (StateId() == savedStateId)
        {
            out.baselineFound = true;   // at the save point: nothing differs
            return out;
        }

        // Dedup while accumulating: several steps commonly touch one entity.
        auto add = [&out](const std::vector<Astra::Entity>& touched)
        {
            for (Astra::Entity e : touched)
                if (std::find(out.entities.begin(), out.entities.end(), e) == out.entities.end())
                    out.entities.push_back(e);
        };

        // Baseline BELOW the current state (the normal case): every undo entry
        // ABOVE it is the diff. savedStateId 0 is the empty-stack bottom, so
        // the whole stack is the diff -- with the StateId eviction caveat
        // mapped here: entries Evict() dropped took their touched lists
        // with them, so a 0-baseline diff can understate after 100+ steps.
        for (auto it = m_undo.rbegin(); it != m_undo.rend(); ++it)
        {
            if (it->id == savedStateId)
            {
                out.baselineFound = true;   // this entry PRODUCED the saved state
                return out;
            }
            add(it->touched);
        }
        // m_evictedSceneId is a reachable bottom too: only document steps
        // (touched empty) can have been evicted after it, so the diff above
        // it is complete.
        if (savedStateId == 0 || savedStateId == m_evictedSceneId)
        {
            out.baselineFound = true;
            return out;
        }

        // Baseline AHEAD of the current state (the user undid past the save
        // point): the redo entries up to AND INCLUDING the baseline's are the
        // diff -- the baseline entry's edit is IN the saved state and absent
        // from the current one. m_redo's back is the next-to-redo (Undo
        // push_back / Redo pop_back), so back-to-front walks forward in time.
        out.entities.clear();
        for (auto it = m_redo.rbegin(); it != m_redo.rend(); ++it)
        {
            add(it->touched);
            if (it->id == savedStateId)
            {
                out.baselineFound = true;
                return out;
            }
        }

        // Unreachable (evicted, or diverged past the save point): unknowable.
        out.entities.clear();
        out.baselineFound = false;
        return out;
    }

    // ---- ScopedTransaction --------------------------------------------------
    ScopedTransaction::ScopedTransaction(CommandStack& stack, std::string label)
        : m_stack(stack), m_id(stack.Begin(std::move(label)))
    {
    }
    ScopedTransaction::~ScopedTransaction()
    {
        // Both are no-ops when m_id is None (this scope joined an already-open
        // transaction) -- the owner closes it, so a nested scope never commits
        // or discards someone else's gesture.
        if (m_cancelled) m_stack.Cancel(m_id);
        else             m_stack.Commit(m_id);
    }
    void ScopedTransaction::Snapshot(Astra::Entity entity,
                                     const Astra::ComponentDescriptor* descriptor)
    {
        m_stack.SnapshotComponent(entity, descriptor);
    }
}
