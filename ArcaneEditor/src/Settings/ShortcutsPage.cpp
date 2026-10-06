#include "Settings/ShortcutsPage.hpp"

#include "Settings/SettingsHost.hpp"
#include "Widgets/EditorTheme.hpp"
#include "Widgets/IconsLucide.h"

#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Input/KeyLayout.hpp>

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <utility>

namespace Arcane::Editor
{
    namespace
    {
        std::string Lower(std::string_view s)
        {
            std::string out(s);
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return out;
        }

        std::string CVarNameOf(std::string_view id) { return "editor.keys." + std::string(id); }

        bool WriteChord(EditorActions& actions, std::string_view id, std::string_view text, const ShortcutWriteSink& sink)
        {
            Arcane::CVarRegistry& reg = actions.Registry();
            const Arcane::CVarHandle h = actions.HandleOf(id);
            if (h.IsStale()) return false;
            const bool ok = reg.Set(h, Arcane::CVarValue::String(std::string(text)), Arcane::SetBy::EditorUser,
                                    "editor", Arcane::CVarContext::Editor) == Arcane::SetResult::Applied;
            reg.Publish();
            actions.RefreshBindings();
            if (ok && sink) sink(CVarNameOf(id));
            return ok;
        }

        void StopListening(EditorActions& actions, ShortcutsPageState& state)
        {
            state.listeningId.clear();
            actions.SetListening(false);
        }
    }

    std::vector<ShortcutRow> BuildShortcutRows(const EditorActions& actions, std::string_view query)
    {
        const std::string q = Lower(query);
        std::vector<ShortcutRow> rows;
        for (std::string_view id : actions.Ids())
        {
            const EditorActionDesc* d = actions.Desc(id);
            const KeyChord chord = actions.ChordOf(id).value_or(KeyChord{});
            const KeyChord def = actions.DefaultChordOf(id).value_or(KeyChord{});
            ShortcutRow row;
            row.id = std::string(id);
            row.action = std::string(d->displayName);
            row.context = ActionContextName(d->context);
            row.chord = DisplayKeyChord(chord);
            row.defaultChord = DisplayKeyChord(def);
            row.type = chord.Bound() ? chord.type : def.type;
            row.isDefault = chord == def;
            row.conflict = !actions.ConflictsOf(id).empty();
            row.tooltip = row.conflict ? actions.ConflictTooltip(id) : "editor.keys." + row.id;
            if (!q.empty())
            {
                const std::string hay = Lower(row.action + " " + row.id + " " + row.context + " " + row.chord + " " + FormatKeyChord(chord));
                if (hay.find(q) == std::string::npos) continue;
            }
            rows.push_back(std::move(row));
        }
        return rows;
    }

    ListenOutcome FeedListen(EditorActions& actions, ShortcutsPageState& state, const ShortcutWriteSink& sink)
    {
        if (state.listeningId.empty()) return ListenOutcome::Waiting;
        const std::optional<ChordCapture> cap = actions.CapturedChord();
        if (!cap) return ListenOutcome::Waiting;
        const std::string id = std::exchange(state.listeningId, std::string{});
        actions.SetListening(false);
        const KeyChord& lab = cap->labelled;
        const bool bare = !lab.ctrl && !lab.shift && !lab.alt && !lab.super;
        if (bare && lab.type == KeyType::Labelled && lab.key == Arcane::Keys::kEscape) return ListenOutcome::Cancelled;
        if (bare && lab.type == KeyType::Labelled && lab.key == Arcane::Keys::kBackspace)
            return WriteChord(actions, id, "", sink) ? ListenOutcome::Cleared : ListenOutcome::Cancelled;
        const KeyChord current = actions.ChordOf(id).value_or(KeyChord{});
        const KeyType wanted = current.Bound() ? current.type : actions.DefaultChordOf(id).value_or(KeyChord{}).type;
        const KeyChord chosen = wanted == KeyType::Physical ? cap->physical : lab;
        if (const std::optional<std::string> why = ReservedChordReason(chosen))
        {
            state.refusal = DisplayKeyChord(chosen) + " is reserved: " + *why;
            return ListenOutcome::Refused;
        }
        state.refusal.clear();
        return SetActionChord(actions, id, chosen, sink) ? ListenOutcome::Bound : ListenOutcome::Cancelled;
    }

    void EndListenIfPageHidden(EditorActions& actions, ShortcutsPageState& state, int frame)
    {
        if (!state.listeningId.empty() && state.drawnFrame != frame) StopListening(actions, state);
    }

    bool SetActionChord(EditorActions& actions, std::string_view id, const KeyChord& chord, const ShortcutWriteSink& sink)
    {
        return WriteChord(actions, id, FormatKeyChord(chord), sink);
    }

    bool FlipKeyType(EditorActions& actions, std::string_view id, const ShortcutWriteSink& sink)
    {
        const std::optional<KeyChord> c = actions.ChordOf(id);
        if (!c || !c->Bound()) return false;
        const KeyChord flipped = ToKeyType(*c, c->type == KeyType::Labelled ? KeyType::Physical : KeyType::Labelled);
        if (flipped == *c) return false;   // no key maps on this layout
        return SetActionChord(actions, id, flipped, sink);
    }

    void ResetAllShortcuts(EditorActions& actions, const ShortcutWriteSink& sink)
    {
        // Drop the EditorUser record rather than writing the default text, so
        // the user file stops pinning a binding the engine may later change.
        // The sink is what makes that stick: the exit-time WriteCVarArchive
        // never erases a key, so only the archive queue's
        // WriteCVarRungArchive takes the old binding out of editor.json.
        Arcane::CVarRegistry& reg = actions.Registry();
        std::vector<std::string> cleared;
        for (std::string_view id : actions.Ids())
            if (const Arcane::CVarHandle h = actions.HandleOf(id); !h.IsStale())
            {
                (void)reg.ClearRung(h, Arcane::SetBy::EditorUser);
                cleared.push_back(CVarNameOf(id));
            }
        reg.Publish();
        actions.RefreshBindings();
        if (sink)
            for (const std::string& name : cleared) sink(name);
    }

    void DrawShortcutsPage(void* user)
    {
        ShortcutsPageState& st = *static_cast<ShortcutsPageState*>(user);
        EditorActions& actions = EditorActions::Get();
        st.drawnFrame = ImGui::GetFrameCount();
        const ShortcutWriteSink archive = [](const std::string& cvar) { NoteSettingEdited(Arcane::SetBy::EditorUser, cvar); };
        (void)FeedListen(actions, st, archive);

        ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Reset All").x - ImGui::GetStyle().FramePadding.x * 4.0f);
        ImGui::InputTextWithHint("##shortcutsearch", ICON_LC_SEARCH " Search actions, contexts or keys", st.search, sizeof st.search);
        ImGui::SameLine();
        if (ImGui::Button("Reset All"))
        {
            StopListening(actions, st);
            st.refusal.clear();
            ResetAllShortcuts(actions, archive);
        }
        if (!st.refusal.empty()) ImGui::TextColored(Theme::kWarning, ICON_LC_TRIANGLE_ALERT " %s", st.refusal.c_str());

        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
        if (!ImGui::BeginTable("##shortcuts", 5, flags)) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Action");
        ImGui::TableSetupColumn("Context");
        ImGui::TableSetupColumn("Shortcut");
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Default");
        ImGui::TableHeadersRow();
        for (const ShortcutRow& row : BuildShortcutRows(actions, st.search))
        {
            ImGui::PushID(row.id.c_str());
            ImGui::TableNextRow();
            if (row.conflict) ImGui::PushStyleColor(ImGuiCol_Text, Theme::kError);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(row.action.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", row.tooltip.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(row.context.c_str());
            ImGui::TableNextColumn();
            const bool listening = st.listeningId == row.id;
            const std::string cell = listening ? std::string("Press a key (Esc cancels, Backspace clears)")
                                               : (row.chord.empty() ? std::string("(unbound)") : row.chord);
            if (ImGui::Button(cell.c_str(), ImVec2(-FLT_MIN, 0.0f)))
            {
                st.listeningId = row.id;
                st.refusal.clear();
                actions.SetListening(true);
            }
            if (row.conflict && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", row.tooltip.c_str());
            ImGui::TableNextColumn();
            if (ImGui::SmallButton(row.type == KeyType::Physical ? "Physical" : "Labelled")) (void)FlipKeyType(actions, row.id, archive);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", row.type == KeyType::Physical ? "Matched by the key's position (fly keys, tool row)"
                                                                      : "Matched by the key's label on your layout");
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", row.defaultChord.c_str());
            if (row.conflict) ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}
