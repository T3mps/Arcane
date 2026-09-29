#include "Widgets/PropertyGrid.hpp"
#include "Widgets/EditorTheme.hpp"
#include <imgui_internal.h>   // ClearActiveID (numeric-row Escape cancel)

#include <cfloat>

namespace Arcane::Editor
{
    void PropertyGrid::Probe(const char* label)
    {
        if (!m_state.probe) return;
        const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
        (*m_state.probe)[label] = ImVec2((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
    }

    bool PropertyGrid::Section(const char* label, bool defaultOpen)
    {
        // The band pops at return, so a tooltip/popup drawn after the header
        // reads the theme's own Header colours (EditorPanels' rule).
        HeaderBand band;
        return ImGui::CollapsingHeader(label, defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    }

    bool PropertyGrid::SubSection(std::string_view label, bool defaultOpen)
    {
        ImGui::PushID(label.data(), label.data() + label.size());
        bool open = false;
        {
            HeaderBand band;
            open = ImGui::TreeNodeEx("##subsection", defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0,
                                     "%.*s", static_cast<int>(label.size()), label.data());
        }
        if (!open) ImGui::PopID();
        return open;
    }

    void PropertyGrid::EndSubSection()
    {
        ImGui::TreePop();
        ImGui::PopID();
    }

    PropertyGrid::Rows::Rows(PropertyGrid& grid, const char* id)
        : m_grid(id, grid.m_state.labelColWidth) {}
    PropertyGrid::Rows::~Rows() = default;

    bool PropertyGrid::TextRow(const char* label, std::string_view current,
                               std::function<void(std::string)> commit, bool dimmed,
                               std::function<std::optional<std::string>(std::string_view)> validate)
    {
        using TextDraft = PropertyGridState::TextDraft;
        (void)FieldLabelCell(label, dimmed);
        ImGui::PushID(label);
        const unsigned int key = ImGui::GetID("##value");
        const int now = ImGui::GetFrameCount();
        auto [it, inserted] = m_state.textDrafts.try_emplace(
            key, TextDraft{ std::string(current), std::string(current), false, now, {} });
        TextDraft& draft = it->second;
        // A hold (Enter on a refused value) keeps the typed text while focus is
        // re-armed; it expires if the row vanished or focus never came back, so
        // a refused name never leaves stale text on screen.
        // The queued re-arm expires with it: a stale focusPending would grab focus
        // (select-all) unprompted the next time the row draws.
        if (draft.hold && (draft.lastFrame + 1 < now || now > draft.holdFrame + 3)) { draft.hold = false; draft.focusPending = false; }   // row vanished, or focus never came back
        // Re-seed from live data whenever THIS widget was not active on its
        // last draw, OR was not drawn last frame at all (page/source switched
        // while the box was active: CommitOrphans owns that edit, never this
        // row). Gated on the draft's own flags, not IsAnyItemActive(): on the
        // frame a click elsewhere deactivates the box, ImGui may already have
        // moved ActiveId, and a global check would re-seed BEFORE InputText
        // reports IsItemDeactivatedAfterEdit -- wiping the edit it commits.
        if (!inserted && !draft.hold && (!draft.active || draft.lastFrame + 1 < now) && draft.text != current)
        {
            draft.text.assign(current);
            draft.seed.assign(current);
            draft.active = false;
        }
        draft.lastFrame = now;
        draft.commit = commit;   // re-bound to THIS target every draw
        ImGui::BeginDisabled(dimmed);
        // Single-line property text selects all on activation (click or Tab/nav
        // into the box), the same rule as the document's inline rename box.
        if (draft.focusPending) { ImGui::SetKeyboardFocusHere(); draft.focusPending = false; }
        InputTextString("##value", &draft.text, ImGuiInputTextFlags_AutoSelectAll);
        // Still the last item: the refusal reason shows while typing (the
        // Input Actions rename box's rule). An unchanged value is never refused.
        const std::optional<std::string> reason = (validate && draft.text != current) ? validate(draft.text) : std::nullopt;
        if (reason && ImGui::IsItemActive()) ImGui::SetItemTooltip("%s", reason->c_str());
        ImGui::EndDisabled();
        Probe(label);
        draft.active = ImGui::IsItemActive();
        if (draft.active) draft.hold = false;   // focus came back: the hold has done its job
        bool committed = false;
        // Enter on a refused value keeps the text and re-arms the box on EVERY
        // deactivation, edited this activation or not (a second Enter after a
        // re-arm has no new edit): the rename box's IsItemDeactivated rule (D4).
        // A draft created this frame is CommitOrphans' flushed one (see below).
        if (reason && !inserted && ImGui::IsItemDeactivated()
            && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))
        {
            draft.hold = true; draft.holdFrame = now; draft.focusPending = true;   // keep text + re-arm
            ImGui::PopID();
            return false;
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            if (inserted)
            {
                // A deactivation reported on a draft created THIS frame belongs
                // to the draft CommitOrphans already flushed (ImGui re-applies its
                // deactivated buffer for up to one extra frame): ignore it.
                draft.text.assign(current);
            }
            else
            {
                if (reason) { m_state.textDrafts.erase(it); ImGui::PopID(); return false; }                                            // focus loss / Escape: revert, no commit
                std::string edited = draft.text;
                auto fn = std::move(draft.commit);
                m_state.textDrafts.erase(it);
                if (edited != current) { fn(std::move(edited)); committed = true; }
            }
        }
        ImGui::PopID();
        return committed;
    }

    void PropertyGrid::CommitOrphans()
    {
        using TextDraft = PropertyGridState::TextDraft;
        // ImGui clears an ActiveId that was not submitted last frame (NewFrame)
        // and reports the deactivation for at most one further frame; a row not
        // drawn in that window never sees it. Erase before calling: the commit
        // may re-enter the grid.
        const int now = ImGui::GetFrameCount();
        for (auto it = m_state.textDrafts.begin(); it != m_state.textDrafts.end();)
        {
            TextDraft& d = it->second;
            if (d.active && d.lastFrame < now - 1)
            {
                auto fn = std::move(d.commit);
                std::string text = std::move(d.text);
                const bool changed = text != d.seed;
                it = m_state.textDrafts.erase(it);
                if (changed && fn) fn(std::move(text));
            }
            else ++it;
        }
    }

    bool PropertyGrid::CheckboxRow(const char* label, bool& value)
    {
        (void)FieldLabelCell(label, false);
        ImGui::PushID(label);
        const bool changed = ImGui::Checkbox("##value", &value);
        Probe(label);
        ImGui::PopID();
        return changed;
    }

    namespace
    {
        // Shared drag/commit path for both numeric rows. Runs right after
        // PushID(label); the caller draws the label cell first and Probe()s
        // after (the widget is still LastItemData -- Probe adds no item).
        template <typename T, typename DrawFn>
        bool NumericRow(PropertyGridState& state, T& value, DrawFn&& draw)
        {
            const unsigned int key = ImGui::GetID("##value");
            auto [it, inserted] = state.numericDrafts.try_emplace(key, PropertyGridState::NumericDraft{});
            PropertyGridState::NumericDraft& draft = it->second;
            // Re-seed whenever THIS widget was not active on its last draw (the
            // draft's own flag, never IsAnyItemActive() -- see TextRow). While
            // active the external value is never consulted: pages mutate the
            // document only on commit, so there is no mid-gesture change to adopt.
            if (!draft.active) { draft.value = static_cast<double>(value); draft.seed = draft.value; }
            T local = static_cast<T>(draft.value);
            draw(local);
            draft.value = static_cast<double>(local);
            value = local;                                   // write-through: the caller's reference follows the gesture
            draft.active = ImGui::IsItemActive();
            // Escape mid-DRAG (button held; text mode has no button down and
            // InputText reverts on its own): restore the seed, end the gesture.
            if (draft.active && ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            {
                value = static_cast<T>(draft.seed);
                ImGui::ClearActiveID();
                state.numericDrafts.erase(it);
                return false;
            }
            bool committed = false;
            if (ImGui::IsItemDeactivatedAfterEdit())
                committed = draft.value != draft.seed;       // released ON the seed = no undo step
            // The map holds IN-FLIGHT gestures only: a row whose widget is not
            // active after this draw (gesture over, edited or not, or never
            // started -- try_emplace above made a draft for every drawn row)
            // drops its draft, so the next activation seeds from live data.
            if (!draft.active)
                state.numericDrafts.erase(it);
            return committed;
        }
    }

    bool PropertyGrid::IntRow(const char* label, int& value)
    {
        (void)FieldLabelCell(label, false);
        ImGui::PushID(label);
        const bool committed = NumericRow(m_state, value, [](int& v) { ImGui::InputInt("##value", &v); });
        Probe(label);
        ImGui::PopID();
        return committed;
    }

    bool PropertyGrid::FloatRow(const char* label, float& value, float speed)
    {
        (void)FieldLabelCell(label, false);
        ImGui::PushID(label);
        const bool committed = NumericRow(m_state, value,
            [speed](float& v) { ImGui::DragFloat("##value", &v, speed, 0.0f, 0.0f, "%.2f"); });
        Probe(label);
        ImGui::PopID();
        return committed;
    }

    int PropertyGrid::ComboRow(const char* label, const char* const* items, int count, int current)
    {
        (void)FieldLabelCell(label, false);
        ImGui::PushID(label);
        int picked = -1;
        const char* preview = (current >= 0 && current < count) ? items[current] : "";
        if (ImGui::BeginCombo("##value", preview))
        {
            for (int i = 0; i < count; ++i)
                if (ImGui::Selectable(items[i], i == current) && i != current)
                    picked = i;
            ImGui::EndCombo();
        }
        Probe(label);
        ImGui::PopID();
        return picked;
    }

    void PropertyGrid::ReadOnlyRow(const char* label, std::string_view text)
    {
        (void)FieldLabelCell(label, true);
        ImGui::PushID(label);
        ImGui::TextDisabled("%.*s", static_cast<int>(text.size()), text.data());
        Probe(label);
        ImGui::PopID();
    }

    int PropertyGrid::ButtonRow(const char* label, const char* const* buttons, int count,
                                unsigned enabledMask)
    {
        (void)FieldLabelCell(label, false);
        ImGui::PushID(label);
        int clicked = -1;
        for (int i = 0; i < count; ++i)
        {
            if (i > 0) ImGui::SameLine();
            ImGui::BeginDisabled(((enabledMask >> i) & 1u) == 0);
            if (ImGui::SmallButton(buttons[i])) clicked = i;
            if (m_state.probe) Probe((std::string(label) + "#" + buttons[i]).c_str());   // TEST SEAM: each button's centre under "<label>#<text>"
            ImGui::EndDisabled();
        }
        Probe(label);
        ImGui::PopID();
        return clicked;
    }

    void PropertyGrid::MeterRow(const char* label, float fraction01, const char* overlay)
    {
        (void)FieldLabelCell(label, true);
        ImGui::PushID(label);
        ImGui::ProgressBar(fraction01 < 0.0f ? 0.0f : fraction01 > 1.0f ? 1.0f : fraction01,
                           ImVec2(-FLT_MIN, 0.0f), overlay);   // PlotHistogram = Theme::kAmber
        Probe(label);
        ImGui::PopID();
    }
}
