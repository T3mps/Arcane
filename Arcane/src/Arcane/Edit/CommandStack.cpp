#include <Arcane/Edit/CommandStack.hpp>

#include <Arcane/Edit/ComponentEditCommand.hpp>

#include <utility>

namespace Arcane
{
    CommandStack::CommandStack(std::function<Astra::Registry&()> resolve, std::size_t maxDepth)
        : m_resolve(std::move(resolve)), m_maxDepth(maxDepth ? maxDepth : 1)
    {
    }

    void CommandStack::Begin(std::string label)
    {
        if (m_open)
            return;   // already open (exclusive gestures); keep the first
        m_open = true;
        m_openLabel = std::move(label);
        m_pending.clear();
    }

    void CommandStack::SnapshotComponent(Astra::Entity entity,
                                         const Astra::ComponentDescriptor* descriptor)
    {
        if (!m_open || !descriptor)
            return;
        // Idempotent: first touch of (entity, descriptor) snapshots; later ones no-op.
        for (const Pending& p : m_pending)
            if (p.entity == entity && p.descriptor == descriptor)
                return;
        m_pending.push_back(Pending{
            entity, descriptor,
            ComponentEditCommand::Snapshot(m_resolve(), entity, descriptor) });
    }

    void CommandStack::Commit()
    {
        if (!m_open)
            return;
        Transaction txn;
        txn.label = m_openLabel;
        for (Pending& p : m_pending)
        {
            std::vector<std::byte> after =
                ComponentEditCommand::Snapshot(m_resolve(), p.entity, p.descriptor);
            if (after == p.before)
                continue;   // unchanged -> drop
            txn.commands.push_back(std::make_unique<ComponentEditCommand>(
                m_resolve, p.entity, p.descriptor,
                std::move(p.before), std::move(after), m_openLabel));
        }
        m_open = false;
        m_pending.clear();
        if (txn.commands.empty())
            return;   // nothing changed -> no history entry

        m_undo.push_back(std::move(txn));
        m_redo.clear();
        while (m_undo.size() > m_maxDepth)
            m_undo.pop_front();   // drop the oldest
    }

    void CommandStack::Cancel()
    {
        m_open = false;
        m_pending.clear();
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
        m_open = false;
        m_pending.clear();
    }

    // ---- ScopedTransaction --------------------------------------------------
    ScopedTransaction::ScopedTransaction(CommandStack& stack, std::string label)
        : m_stack(stack)
    {
        m_stack.Begin(std::move(label));
    }
    ScopedTransaction::~ScopedTransaction()
    {
        if (m_cancelled) m_stack.Cancel();
        else             m_stack.Commit();
    }
    void ScopedTransaction::Snapshot(Astra::Entity entity,
                                     const Astra::ComponentDescriptor* descriptor)
    {
        m_stack.SnapshotComponent(entity, descriptor);
    }
}
