#include "EditGesture.hpp"

namespace Arcane::Editor::EditGesture
{
    EndAction EvaluateEnd(const Slots& s, std::uint32_t lastItemId,
                          bool deactivatedAfterEdit, bool deactivated) noexcept
    {
        if (lastItemId != s.item)  return EndAction::None;
        if (deactivatedAfterEdit)  return EndAction::Commit;
        if (deactivated)           return EndAction::Cancel;
        return EndAction::None;
    }

    bool ShouldCloseAbandoned(const Slots& s, std::uint32_t activeId,
                              bool hasPendingCommit) noexcept
    {
        if (s.txn == Arcane::TransactionId::None && !hasPendingCommit)
            return false;
        return activeId != s.item;
    }

    bool ShouldCloseStaleOnActivate(const Slots& s, bool hasPendingCommit) noexcept
    {
        return s.txn != Arcane::TransactionId::None || hasPendingCommit;
    }
}
