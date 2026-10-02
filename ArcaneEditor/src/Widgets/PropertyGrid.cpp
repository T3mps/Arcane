#include "Widgets/PropertyGrid.hpp"
#include "Widgets/ColorPickerPopup.hpp"
#include "Widgets/EditorTheme.hpp"
#include "Widgets/IconsLucide.h"   // ICON_LC_ROTATE_CCW (the reset slot)
#include <imgui_internal.h>   // ClearActiveID (numeric-row Escape cancel)

#include <cfloat>
#include <string>
#include <utility>

namespace Arcane::Editor
{
    void PropertyGrid::ProbeItem(const char* label)
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

    bool PropertyGrid::Section(const char* label, bool defaultOpen, const std::function<void()>& trailing)
    {
        bool open = false;
        {
            HeaderBand band;   // popped before the control: it reads the theme's own colours
            ImGuiTreeNodeFlags flags = defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0;
            if (trailing) flags |= ImGuiTreeNodeFlags_AllowOverlap;
            open = ImGui::CollapsingHeader(label, flags);
        }
        if (!trailing || ImGui::GetCurrentWindowRead()->SkipItems)
            return open;
        const unsigned int key = ImGui::GetItemID();
        const float headerRight = ImGui::GetItemRectMax().x - ImGui::GetStyle().FramePadding.x;
        const auto it = m_state.trailingWidths.find(key);
        const float width = it != m_state.trailingWidths.end() ? it->second : 0.0f;
        // SameLine(offset) keeps the header's line without SetCursorPos (no
        // boundary-extension assert); the control's own ItemSize ends the line.
        ImGui::SameLine(headerRight - width - ImGui::GetWindowPos().x + ImGui::GetScrollX());
        ImGui::PushID(label);
        ImGui::BeginGroup();
        trailing();
        ImGui::EndGroup();
        ImGui::PopID();
        m_state.trailingWidths[key] = ImGui::GetItemRectSize().x;
        return open;
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
        BeginPlainRow();
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
        // An unchanged value is never refused.
        auto refusal = [&] { return (validate && draft.text != current) ? validate(draft.text) : std::nullopt; };
        {
            // A refused value draws red (outline + text) while typing AND while
            // held after a refused Enter -- the document rename box's look (D4).
            // Styled from the text as it enters this frame (the edit lands inside
            // the call), so the look trails a keystroke by one frame; the logic
            // below reads the fresh reason.
            RefusedFieldStyle refused(refusal().has_value());
            InputTextString("##value", &draft.text, ImGuiInputTextFlags_AutoSelectAll);
        }
        // Still the last item: the reason shows on hover whenever the value is
        // refused, active or held (the Input Actions rename box's rule).
        const std::optional<std::string> reason = refusal();
        RefusedFieldTooltip(reason);
        ImGui::EndDisabled();
        ProbeItem(label);
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

    void PropertyGrid::BeginPlainRow()
    {
        m_events = {};
        IM_ASSERT(!m_hasDecor && "SetNextRowDecor: this row type takes no decoration");
        m_hasDecor = false;
    }

    void PropertyGrid::BeginValueCell(const char* label, bool dimmed)
    {
        m_events = {};
        const RowDecor decor = m_hasDecor ? std::move(m_decor) : RowDecor{};
        m_decor = RowDecor{};   // one-shot: a lead's captures die with its row
        m_hasDecor = false;
        IM_ASSERT(!(decor.overridden && decor.reset) && "RowDecor: override and reset are mutually exclusive");
        if (decor.overridden)
        {
            // FieldLabelCell's shape (see its comments: AlignTextToFramePadding,
            // -FLT_MIN) with the override checkbox ahead of the name.
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::PushID(label);
            if (ImGui::Checkbox("##override", decor.overridden))
                m_events.overrideToggled = true;
            ImGui::SetItemTooltip("%s", *decor.overridden
                ? "Overridden -- untick to inherit the parent's value"
                : "Inherited -- tick to override");   // drafting pick, 9.28
            if (m_state.probe) ProbeItem((std::string(label) + "#override").c_str());
            ImGui::PopID();
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            (void)FieldLabelText(label, dimmed);
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
        }
        else
            (void)FieldLabelCell(label, dimmed);
        ImGui::PushID(label);
        if (decor.lead)
        {
            // FIRST in the cell. Its items consume the -FLT_MIN the label cell
            // set (ItemAdd clears NextItemData), so the value's width is
            // re-stated after it unless the reset slot below states its own.
            decor.lead();
            ImGui::SameLine();
            if (!decor.reset)
                ImGui::SetNextItemWidth(-FLT_MIN);
        }
        if (decor.reset)
        {
            // BEFORE the value (R3): place reset at the cell's right edge, then
            // return to the cell start so the value stays LastItemData.
            const float resetW = ImGui::GetFrameHeight();
            const float cellX = ImGui::GetCursorPosX();
            if (decor.resetActive)
            {
                ImGui::SetCursorPosX(cellX + ImGui::GetContentRegionAvail().x - resetW);
                if (ImGui::Button(ICON_LC_ROTATE_CCW "##reset", ImVec2(resetW, 0.0f)))
                    m_events.resetClicked = true;
                ImGui::SetItemTooltip("Reset to default");
                if (m_state.probe) ProbeItem((std::string(label) + "#reset").c_str());
                ImGui::SameLine();
                ImGui::SetCursorPosX(cellX);
            }
            ImGui::SetNextItemWidth(-(resetW + ImGui::GetStyle().ItemSpacing.x));   // reserved either way
        }
        m_valueDisabled = decor.overridden && !*decor.overridden;
        if (m_valueDisabled)
            ImGui::BeginDisabled();
    }

    void PropertyGrid::EndValueCell(const char* label)
    {
        if (m_valueDisabled)
        {
            ImGui::EndDisabled();
            m_valueDisabled = false;
        }
        ProbeItem(label);   // the value widget is still LastItemData
        ImGui::PopID();
    }

    bool PropertyGrid::CheckboxRow(const char* label, bool& value)
    {
        BeginValueCell(label, false);
        const bool changed = ImGui::Checkbox("##value", &value);
        EndValueCell(label);
        return changed;
    }

    namespace
    {
        struct NumericResult { bool committed = false; bool cancelled = false; };

        // Shared drag/commit path for every numeric row (1-4 components). Runs
        // right after PushID(label); `draw` submits the value widget over the
        // `count`-long local copy, which is LastItemData when it returns.
        template <typename T, typename DrawFn>
        NumericResult NumericRow(PropertyGridState& state, T* value, int count, DrawFn&& draw)
        {
            IM_ASSERT(count >= 1 && count <= 4);
            const unsigned int key = ImGui::GetID("##value");
            auto [it, inserted] = state.numericDrafts.try_emplace(key, PropertyGridState::NumericDraft{});
            PropertyGridState::NumericDraft& draft = it->second;
            // Re-seed whenever THIS widget was not active on its last draw (the
            // draft's own flag, never IsAnyItemActive() -- see TextRow). While
            // active the external value is never consulted: pages mutate the
            // document only on commit, so there is no mid-gesture change to adopt.
            if (!draft.active)
            {
                draft.count = count;
                for (int i = 0; i < count; ++i) { draft.value[i] = static_cast<double>(value[i]); draft.seed[i] = draft.value[i]; }
            }
            T local[4]{};
            for (int i = 0; i < count; ++i) local[i] = static_cast<T>(draft.value[i]);
            draw(local);
            for (int i = 0; i < count; ++i)
            {
                draft.value[i] = static_cast<double>(local[i]);
                value[i] = local[i];                         // write-through: the caller follows the gesture
            }
            draft.active = ImGui::IsItemActive();            // grouped rows: EndGroup forwarded the live id
            // Escape mid-DRAG (button held; text mode has no button down and
            // InputText reverts on its own): restore the seed, end the gesture.
            if (draft.active && ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            {
                for (int i = 0; i < count; ++i) value[i] = static_cast<T>(draft.seed[i]);
                ImGui::ClearActiveID();
                state.numericDrafts.erase(it);
                return { false, true };
            }
            NumericResult r;
            if (ImGui::IsItemDeactivatedAfterEdit())
                for (int i = 0; i < count; ++i)
                    r.committed = r.committed || draft.value[i] != draft.seed[i];   // released ON the seed = no undo step
            // The map holds IN-FLIGHT gestures only: a row whose widget is not
            // active after this draw (gesture over, edited or not, or never
            // started -- try_emplace above made a draft for every drawn row)
            // drops its draft, so the next activation seeds from live data.
            if (!draft.active)
                state.numericDrafts.erase(it);
            return r;
        }
    }

    bool PropertyGrid::IntRow(const char* label, int& value,
                              const std::optional<Astra::Range>& range, const char* format)
    {
        BeginValueCell(label, false);
        const NumericResult r = NumericRow(m_state, &value, 1, [&](int* v)
        {
            if (range)
                (void)RangedDragInt("##value", v, range, format);
            else
            {
                // InputInt spelled out (imgui_widgets.cpp: InputScalar S32,
                // step 1, fast 100) so it can take `format`; same id, same
                // step buttons.
                const int step = 1, stepFast = 100;
                ImGui::InputScalar("##value", ImGuiDataType_S32, v, &step, &stepFast, format);
            }
        });
        m_events.cancelled = r.cancelled;
        EndValueCell(label);
        return r.committed;
    }

    bool PropertyGrid::FloatRow(const char* label, float& value, float speed,
                                const std::optional<Astra::Range>& range, const char* format)
    {
        BeginValueCell(label, false);
        const NumericResult r = NumericRow(m_state, &value, 1, [&](float* v)
        {
            if (range)
                (void)RangedDragFloat("##value", v, speed, range, format);
            else
                ImGui::DragFloat("##value", v, speed, 0.0f, 0.0f, format);
        });
        m_events.cancelled = r.cancelled;
        EndValueCell(label);
        return r.committed;
    }

    bool PropertyGrid::SliderRow(const char* label, float& value, float min, float max, const char* format)
    {
        BeginValueCell(label, false);
        const NumericResult r = NumericRow(m_state, &value, 1,
            [&](float* v) { ImGui::SliderFloat("##value", v, min, max, format); });
        m_events.cancelled = r.cancelled;
        EndValueCell(label);
        return r.committed;
    }

    bool PropertyGrid::VecRow(const char* label, float* v, int n, float speed,
                              const std::optional<Astra::Range>& range, const char* format)
    {
        IM_ASSERT(n >= 2 && n <= 4);
        BeginValueCell(label, false);
        const NumericResult r = NumericRow(m_state, v, n,
            [&](float* local) { (void)AxisDragFloatN("##value", local, n, speed, range, format); });
        m_events.cancelled = r.cancelled;
        EndValueCell(label);
        return r.committed;
    }

    bool PropertyGrid::ColorRow(const char* label, float linear[4], ImGuiID* popupIdOut, bool hdr)
    {
        BeginValueCell(label, false);
        const ImVec2 cell = ImGui::GetCursorScreenPos();   // the swatch's top-left: ColorValue draws it first
        ImGuiID popupId = 0;
        const NumericResult r = NumericRow(m_state, linear, 4, [&](float* local)
        {
            popupId = ColorValue("##value", local, m_state.colorOriginal, hdr).popupId;
        });
        m_events.cancelled = r.cancelled;
        bool committed = r.committed;
        if (ImGui::IsPopupOpen(popupId, ImGuiPopupFlags_None))
            m_state.colorPopupLive = popupId;
        else if (popupId != 0 && m_state.colorPopupLive == popupId)
        {
            m_state.colorPopupLive = 0;                      // the close frame: one commit per session
            for (int i = 0; i < 4; ++i)
                committed = committed || linear[i] != m_state.colorOriginal[i];
        }
        if (popupIdOut)
            *popupIdOut = popupId;
        if (m_state.probe)                                   // TEST SEAM: the swatch's centre
        {
            const float fh = ImGui::GetFrameHeight();
            (*m_state.probe)[std::string(label) + "#swatch"] = ImVec2(cell.x + fh * 0.5f, cell.y + fh * 0.5f);
        }
        EndValueCell(label);                                 // probes the boxes (still LastItemData)
        return committed;
    }

    int PropertyGrid::ComboRow(const char* label, const char* const* items, int count, int current)
    {
        BeginValueCell(label, false);
        int picked = -1;
        const char* preview = (current >= 0 && current < count) ? items[current] : "";
        if (ImGui::BeginCombo("##value", preview))
        {
            for (int i = 0; i < count; ++i)
                if (ImGui::Selectable(items[i], i == current) && i != current)
                    picked = i;
            ImGui::EndCombo();
        }
        EndValueCell(label);
        return picked;
    }

    void PropertyGrid::ReadOnlyRow(const char* label, std::string_view text)
    {
        // RowDecor::lead is the one decoration a read-only row takes.
        std::function<void()> lead;
        if (m_hasDecor)
        {
            IM_ASSERT(!m_decor.overridden && !m_decor.reset && "ReadOnlyRow: only RowDecor::lead applies");
            lead = std::move(m_decor.lead);
            m_decor = RowDecor{};
            m_hasDecor = false;
        }
        BeginPlainRow();
        (void)FieldLabelCell(label, true);
        ImGui::PushID(label);
        if (lead)
        {
            lead();
            ImGui::SameLine();
        }
        // Cut to the cell (node-page s4.1(e)); the full text is one hover away.
        const std::string shown = EllipsisToWidth(text, ImGui::GetContentRegionAvail().x);
        ImGui::TextDisabled("%s", shown.c_str());
        if (shown != text)
            ImGui::SetItemTooltip("%.*s", static_cast<int>(text.size()), text.data());
        ProbeItem(label);
        ImGui::PopID();
    }

    int PropertyGrid::ButtonRow(const char* label, const char* const* buttons, int count,
                                unsigned enabledMask)
    {
        BeginPlainRow();
        (void)FieldLabelCell(label, false);
        ImGui::PushID(label);
        int clicked = -1;
        for (int i = 0; i < count; ++i)
        {
            if (i > 0) ImGui::SameLine();
            ImGui::BeginDisabled(((enabledMask >> i) & 1u) == 0);
            if (ImGui::SmallButton(buttons[i])) clicked = i;
            if (m_state.probe) ProbeItem((std::string(label) + "#" + buttons[i]).c_str());   // TEST SEAM: each button's centre under "<label>#<text>"
            ImGui::EndDisabled();
        }
        ProbeItem(label);
        ImGui::PopID();
        return clicked;
    }

    void PropertyGrid::MeterRow(const char* label, float fraction01, const char* overlay)
    {
        BeginPlainRow();
        (void)FieldLabelCell(label, true);
        ImGui::PushID(label);
        ImGui::ProgressBar(fraction01 < 0.0f ? 0.0f : fraction01 > 1.0f ? 1.0f : fraction01,
                           ImVec2(-FLT_MIN, 0.0f), overlay);   // PlotHistogram = Theme::kAmber
        ProbeItem(label);
        ImGui::PopID();
    }
}
