#include <Arcane/Edit/RegistryStateCommand.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Edit/CommandStack.hpp>

#include <memory>
#include <utility>

namespace Arcane
{
    RegistryStateCommand::RegistryStateCommand(std::string label,
                                               SnapshotFn snapshot,
                                               RestoreFn restore,
                                               std::vector<std::byte> before)
        : m_label(std::move(label))
        , m_snapshot(std::move(snapshot))
        , m_restore(std::move(restore))
        , m_before(std::move(before))
    {
    }

    void RegistryStateCommand::Undo()
    {
        if (m_after.empty())
        {
            m_after = m_snapshot();
            if (m_after.empty())
                ARC_WARN("'{}': redo-state capture failed -- undo proceeds, "
                         "redo will be unavailable", m_label);
        }
        if (!m_restore(m_before))
            ARC_WARN("'{}': registry restore failed on undo", m_label);
    }

    void RegistryStateCommand::Redo()
    {
        if (m_after.empty())
        {
            ARC_WARN("'{}': no redo state captured -- redo skipped", m_label);
            return;
        }
        if (!m_restore(m_after))
            ARC_WARN("'{}': registry restore failed on redo", m_label);
    }

    const char* RegistryStateCommand::Label() const { return m_label.c_str(); }

    bool ApplyRegistryMutation(CommandStack& stack, std::string label,
                               const RegistryStateCommand::SnapshotFn& snapshot,
                               const RegistryStateCommand::RestoreFn& restore,
                               FunctionRef<bool()> mutate)
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
        stack.Push(std::make_unique<RegistryStateCommand>(
            std::move(label), snapshot, restore, std::move(before)));
        return true;
    }
}
