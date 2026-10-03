#include <Arcane/Edit/RegistryStateCommand.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Edit/CommandStack.hpp>

#include <memory>
#include <utility>

namespace Arcane
{
    RegistryStateCommand::RegistryStateCommand(std::string label, SnapshotFn snapshot,
                                               RestoreFn restore, UndoPayload before,
                                               CommandStack& stack)
        : m_label(std::move(label)), m_snapshot(std::move(snapshot)),
          m_restore(std::move(restore)), m_stack(&stack), m_before(std::move(before))
    {
    }

    void RegistryStateCommand::Undo()
    {
        if (m_after.Empty() && !m_redoLost)
        {
            std::vector<std::byte> now = m_snapshot();
            if (now.empty())
            {
                // Latch: retrying on a later Undo would capture the restored
                // BEFORE state and make redo "succeed" silently while
                // restoring the state the registry is already in.
                m_redoLost = true;
                ARC_WARN("'{}': redo-state capture failed -- undo proceeds, "
                         "redo will be unavailable", m_label);
            }
            else
            {
                m_after = m_stack->MakePayload(std::move(now));
            }
        }
        const auto before = m_before.Load();
        if (!before)
        {
            ARC_ERROR("'{}': undo payload unreadable at '{}' -- step skipped",
                      m_label, m_before.SpillPath().generic_string());
            return;
        }
        if (!m_restore(*before))
            ARC_WARN("'{}': registry restore failed on undo", m_label);
    }

    void RegistryStateCommand::Redo()
    {
        if (m_redoLost || m_after.Empty())
        {
            ARC_WARN("'{}': no redo state captured -- redo skipped", m_label);
            return;
        }
        const auto after = m_after.Load();
        if (!after)
        {
            ARC_ERROR("'{}': redo payload unreadable at '{}' -- step skipped",
                      m_label, m_after.SpillPath().generic_string());
            return;
        }
        if (!m_restore(*after))
            ARC_WARN("'{}': registry restore failed on redo", m_label);
    }

    const char* RegistryStateCommand::Label() const { return m_label.c_str(); }

    bool ApplyRegistryMutation(CommandStack& stack, std::string label,
                               const RegistryStateCommand::SnapshotFn& snapshot,
                               const RegistryStateCommand::RestoreFn& restore,
                               FunctionRef<bool()> mutate,
                               const std::vector<Astra::Entity>* touched)
    {
        // A structural memento pushed while a gesture is open would ride
        // Cancel()'s discard-without-revert path, stranding the (already
        // applied) edit with no undo coverage. Refuse before mutate() runs.
        if (stack.InTransaction())
        {
            ARC_WARN("'{}': structural edit inside an open undo gesture "
                     "refused -- Cancel would strand it without undo "
                     "coverage", label);
            return false;
        }
        std::vector<std::byte> before = snapshot();
        if (before.empty())
        {
            ARC_WARN("'{}': before-snapshot failed -- structural edit refused "
                     "(it would be un-undoable)", label);
            return false;
        }
        if (!mutate())
            return false;   // no-op edit: no undo step
        // Minted AFTER mutate(): a no-op edit never writes spill bytes.
        UndoPayload beforePayload = stack.MakePayload(std::move(before));
        // `touched` is read here, AFTER mutate() -- see the header: a create
        // appends its new entity ids from inside the mutate lambda.
        stack.Push(std::make_unique<RegistryStateCommand>(
                       std::move(label), snapshot, restore, std::move(beforePayload), stack),
                   touched ? std::span<const Astra::Entity>(*touched)
                           : std::span<const Astra::Entity>{});
        return true;
    }
}
