#include <Arcane/Edit/CommandStack.hpp>

#include <Arcane/Edit/ComponentEditCommand.hpp>

#include <algorithm>
#include <utility>

namespace Arcane
{
    CommandStack::CommandStack(std::function<Astra::Registry&()> resolve, std::size_t maxDepth)
        : m_resolve(std::move(resolve)), m_maxDepth(maxDepth ? maxDepth : 1)
    {
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

        // Stamp the state this transaction produced. m_nextId is the same
        // monotonic source TransactionId::Begin draws from, so ids are unique
        // across BOTH uses and a committed state id can never collide with a
        // live transaction token.
        txn.id = m_nextId++;
        m_undo.push_back(std::move(txn));
        m_redo.clear();
        while (m_undo.size() > m_maxDepth)
            m_undo.pop_front();   // drop the oldest
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
        txn.commands.push_back(std::move(command));
        // See Commit: same stamp-before-push rule, same shared m_nextId source.
        txn.id = m_nextId++;
        m_undo.push_back(std::move(txn));
        m_redo.clear();
        while (m_undo.size() > m_maxDepth)
            m_undo.pop_front();
    }

    void CommandStack::Undo()
    {
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
        if (m_redo.empty())
            return;
        Transaction txn = std::move(m_redo.back());
        m_redo.pop_back();
        for (auto& c : txn.commands)
            c->Redo();       // forward order
        m_undo.push_back(std::move(txn));
    }

    const char* CommandStack::UndoLabel() const noexcept
    {
        return m_undo.empty() ? "" : m_undo.back().label.c_str();
    }
    const char* CommandStack::RedoLabel() const noexcept
    {
        return m_redo.empty() ? "" : m_redo.back().label.c_str();
    }

    void CommandStack::Clear() noexcept
    {
        m_undo.clear();
        m_redo.clear();
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
        // mapped here: entries the depth cap evicted took their touched lists
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
        if (savedStateId == 0)
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
