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

    bool ShouldClosePopup(const Slots& s, std::uint32_t popupId,
                          bool open, bool hasPendingCommit) noexcept
    {
        if (s.txn == Arcane::TransactionId::None && !hasPendingCommit)
            return false;          // nothing parked
        if (s.item != popupId)
            return false;          // not ours -- see the ownership guard note
        return !open;
    }
}

#include <imgui.h>
#include <imgui_internal.h>   // GetActiveID (internal-only, imgui_internal.h:3532);
                              // EditorPanels.cpp already pairs both includes (:23-24)

#include <type_traits>
#include <utility>

static_assert(std::is_same_v<ImGuiID, unsigned int> && sizeof(ImGuiID) == 4,
              "EditGesture's pure core mirrors ImGuiID as uint32_t");

namespace Arcane::Editor::EditGesture
{
    void ClosePending(Arcane::CommandStack& stack, GestureState& st)
    {
        if (st.pendingCommit)
        {
            st.pendingCommit();
            st.pendingCommit = nullptr;
        }
        stack.Commit(st.slots.txn);   // no-op on None (joined gesture)
        st.slots = {};
    }

    void BeginOnActivate(Arcane::CommandStack* stack, GestureState& st,
                         Arcane::FunctionRef<std::string()> label,
                         Arcane::FunctionRef<std::function<void()>()> onOpened)
    {
        if (!stack || !ImGui::IsItemActivated())
            return;
        if (ShouldCloseStaleOnActivate(st.slots, static_cast<bool>(st.pendingCommit)))
            ClosePending(*stack, st);
        st.slots.txn  = stack->Begin(label());
        st.slots.item = ImGui::GetItemID();
        st.pendingCommit = onOpened();
    }

    void EndOnDeactivate(Arcane::CommandStack* stack, GestureState& st)
    {
        if (!stack)
            return;
        switch (EvaluateEnd(st.slots, ImGui::GetItemID(),
                            ImGui::IsItemDeactivatedAfterEdit(),
                            ImGui::IsItemDeactivated()))
        {
            case EndAction::Commit:
                if (st.pendingCommit)
                {
                    st.pendingCommit();
                    st.pendingCommit = nullptr;
                }
                stack->Commit(st.slots.txn);
                break;
            case EndAction::Cancel:
                st.pendingCommit = nullptr;
                stack->Cancel(st.slots.txn);
                break;
            case EndAction::None:
                return;
        }
        st.slots = {};
    }

    void BeginOnPopupOpen(Arcane::CommandStack* stack, GestureState& st,
                          std::uint32_t popupId,
                          Arcane::FunctionRef<std::string()> label,
                          Arcane::FunctionRef<std::function<void()>()> onOpened)
    {
        if (!stack || popupId == 0)
            return;
        if (!ImGui::IsPopupOpen(static_cast<ImGuiID>(popupId), ImGuiPopupFlags_None))
            return;
        if (st.slots.item == popupId)
            return;                     // ours, already live -- every frame after the first
        if (ShouldCloseStaleOnActivate(st.slots, static_cast<bool>(st.pendingCommit)))
            ClosePending(*stack, st);
        st.slots.txn  = stack->Begin(label());
        st.slots.item = popupId;
        st.pendingCommit = onOpened();
    }

    void EndOnPopupClose(Arcane::CommandStack* stack, GestureState& st,
                         std::uint32_t popupId)
    {
        if (!stack)
            return;
        if (ShouldClosePopup(st.slots, popupId,
                             ImGui::IsPopupOpen(static_cast<ImGuiID>(popupId),
                                                ImGuiPopupFlags_None),
                             static_cast<bool>(st.pendingCommit)))
            ClosePending(*stack, st);
    }

    ScopeGuard::~ScopeGuard()
    {
        if (!stack)
            return;
        if (ShouldCloseAbandoned(st.slots, ImGui::GetActiveID(),
                                 static_cast<bool>(st.pendingCommit)))
            ClosePending(*stack, st);
    }
}
