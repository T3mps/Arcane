#include "Settings/ShortcutsPage.hpp"

#include "Settings/SettingsEdit.hpp"   // RungLabel
#include "Settings/SettingsHost.hpp"
#include "Widgets/EditorTheme.hpp"
#include "Widgets/IconsLucide.h"

#include <Arcane/Config/CVarDecl.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Input/KeyLayout.hpp>

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <utility>

namespace Arcane::Editor
{
    ARC_CVAR(cvar_settingsKeysConflictsOnly, "editor.settings.keysConflictsOnly", bool, false,
             .flags = ::Arcane::CVarFlags::Dev | ::Arcane::CVarFlags::Hidden,
             .audience = ::Arcane::Audience::Editor,
             .scope = ::Arcane::SettingScope::PreferencesMachine,
             .help = "Automation: the Keyboard page opens with Conflicts only ticked (headless desk captures).");

    namespace
    {
        std::string Lower(std::string_view s)
        {
            std::string out(s);
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return out;
        }

        std::string CVarNameOf(std::string_view id) { return "editor.keys." + std::string(id); }

        // The rung that holds `id`'s effective binding.
        Arcane::SetBy WinningRung(const EditorActions& actions, std::string_view id)
        {
            const std::optional<Arcane::CVarExplain> e = actions.Registry().Explain(CVarNameOf(id));
            return e ? e->setBy : Arcane::SetBy::Default;
        }

        // RefusedWeaker still RECORDS the EditorUser value beneath the
        // stronger rung, so it is archived like an applied write.
        Arcane::SetResult WriteChord(EditorActions& actions, std::string_view id, std::string_view text, const ShortcutWriteSink& sink)
        {
            Arcane::CVarRegistry& reg = actions.Registry();
            const Arcane::CVarHandle h = actions.HandleOf(id);
            if (h.IsStale()) return Arcane::SetResult::Stale;
            const Arcane::SetResult r = reg.Set(h, Arcane::CVarValue::String(std::string(text)), Arcane::SetBy::EditorUser,
                                                "editor", Arcane::CVarContext::Editor);
            reg.Publish();
            actions.RefreshBindings();
            if ((r == Arcane::SetResult::Applied || r == Arcane::SetResult::RefusedWeaker) && sink)
                sink(Arcane::SetBy::EditorUser, CVarNameOf(id));
            return r;
        }

        // Why a write did not take effect (`what`: "Ctrl+K" or "The clear"); empty when it did.
        std::string WriteRefusal(const EditorActions& actions, std::string_view id, Arcane::SetResult r, const std::string& what)
        {
            switch (r)
            {
            case Arcane::SetResult::RefusedWeaker:
                return what + " is saved for all projects, but the " + RungLabel(WinningRung(actions, id))
                     + " binding still wins: use Clear override on the row.";
            case Arcane::SetResult::Stale:        return what + " was not saved: the action's setting is no longer registered.";
            case Arcane::SetResult::TypeMismatch: return what + " was not saved: the action's setting is not a text setting.";
            case Arcane::SetResult::Denied:       return what + " was not saved: the settings registry denied the write.";
            case Arcane::SetResult::Applied:      break;
            }
            return {};
        }

        void StopListening(EditorActions& actions, ShortcutsPageState& state)
        {
            state.listeningId.clear();
            actions.SetListening(false);
        }
    }

    std::vector<ShortcutRow> BuildShortcutRows(const EditorActions& actions, std::string_view query, bool conflictsOnly)
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
            if (conflictsOnly && !row.conflict) continue;
            row.tooltip = row.conflict ? actions.ConflictTooltip(id) : "editor.keys." + row.id;
            if (const Arcane::SetBy winner = WinningRung(actions, id); winner > Arcane::SetBy::EditorUser)
                row.overriddenBy = winner;
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
        {
            const Arcane::SetResult r = WriteChord(actions, id, "", sink);
            state.refusal = WriteRefusal(actions, id, r, "The clear");
            return r == Arcane::SetResult::Applied ? ListenOutcome::Cleared : ListenOutcome::Refused;
        }
        const KeyChord current = actions.ChordOf(id).value_or(KeyChord{});
        const KeyType wanted = current.Bound() ? current.type : actions.DefaultChordOf(id).value_or(KeyChord{}).type;
        const KeyChord chosen = wanted == KeyType::Physical ? cap->physical : lab;
        if (const std::optional<std::string> why = ReservedChordReason(chosen))
        {
            state.refusal = DisplayKeyChord(chosen) + " is reserved: " + *why;
            return ListenOutcome::Refused;
        }
        const Arcane::SetResult r = WriteChord(actions, id, FormatKeyChord(chosen), sink);
        state.refusal = WriteRefusal(actions, id, r, DisplayKeyChord(chosen));
        return r == Arcane::SetResult::Applied ? ListenOutcome::Bound : ListenOutcome::Refused;
    }

    void EndListenIfPageHidden(EditorActions& actions, ShortcutsPageState& state, int frame)
    {
        if (!state.listeningId.empty() && state.drawnFrame != frame) StopListening(actions, state);
    }

    bool SetActionChord(EditorActions& actions, std::string_view id, const KeyChord& chord, const ShortcutWriteSink& sink)
    {
        return WriteChord(actions, id, FormatKeyChord(chord), sink) == Arcane::SetResult::Applied;
    }

    bool FlipKeyType(EditorActions& actions, std::string_view id, const ShortcutWriteSink& sink)
    {
        const std::optional<KeyChord> c = actions.ChordOf(id);
        if (!c || !c->Bound()) return false;
        const KeyChord flipped = ToKeyType(*c, c->type == KeyType::Labelled ? KeyType::Physical : KeyType::Labelled);
        if (flipped == *c) return false;   // no key maps on this layout
        return SetActionChord(actions, id, flipped, sink);
    }

    bool ClearShortcutOverride(EditorActions& actions, std::string_view id, const ShortcutWriteSink& sink)
    {
        const Arcane::SetBy winner = WinningRung(actions, id);
        const Arcane::CVarHandle h = actions.HandleOf(id);
        if (winner <= Arcane::SetBy::EditorUser || h.IsStale()) return false;
        Arcane::CVarRegistry& reg = actions.Registry();
        if (!reg.ClearRung(h, winner)) return false;   // the registry held no record there: nothing cleared, nothing to archive
        reg.Publish();
        actions.RefreshBindings();
        if (sink) sink(winner, CVarNameOf(id));
        return true;
    }

    void ResetAllShortcuts(EditorActions& actions, const ShortcutWriteSink& sink)
    {
        // Drop the records rather than writing the default text, so the user
        // files stop pinning a binding the engine may later change. The sink
        // is what makes that stick: the exit-time WriteCVarArchive never
        // erases a key, so only the archive queue's WriteCVarRungArchive takes
        // the old binding out of editor.json (EditorUser) and out of the
        // project's user file (User; S4-GATE: a User value silently shadowed
        // the page).
        Arcane::CVarRegistry& reg = actions.Registry();
        std::vector<std::pair<Arcane::SetBy, std::string>> cleared;
        for (std::string_view id : actions.Ids())
            if (const Arcane::CVarHandle h = actions.HandleOf(id); !h.IsStale())
            {
                const std::string name = CVarNameOf(id);
                if (reg.RungValue(name, Arcane::SetBy::User).has_value())
                {
                    (void)reg.ClearRung(h, Arcane::SetBy::User);
                    cleared.emplace_back(Arcane::SetBy::User, name);
                }
                (void)reg.ClearRung(h, Arcane::SetBy::EditorUser);
                cleared.emplace_back(Arcane::SetBy::EditorUser, name);
            }
        reg.Publish();
        actions.RefreshBindings();
        if (sink)
            for (const auto& [rung, name] : cleared) sink(rung, name);
    }

    void DrawShortcutsPage(void* user)
    {
        ShortcutsPageState& st = *static_cast<ShortcutsPageState*>(user);
        EditorActions& actions = EditorActions::Get();
        if (st.drawnFrame < 0) st.conflictsOnly = st.conflictsOnly || cvar_settingsKeysConflictsOnly.Get();   // the first draw
        st.drawnFrame = ImGui::GetFrameCount();
        const ShortcutWriteSink archive = [](Arcane::SetBy rung, const std::string& cvar)
        {
            if (rung == Arcane::SetBy::EditorUser || rung == Arcane::SetBy::User) NoteSettingEdited(rung, cvar);   // the archived rungs
        };
        (void)FeedListen(actions, st, archive);

        const ImGuiStyle& style = ImGui::GetStyle();
        const float trailing = ImGui::CalcTextSize("Reset All").x + style.FramePadding.x * 2.0f
                             + ImGui::GetFrameHeight() + ImGui::CalcTextSize("Conflicts only").x
                             + style.ItemInnerSpacing.x + style.ItemSpacing.x * 2.0f;
        ImGui::SetNextItemWidth(-trailing);
        ImGui::InputTextWithHint("##shortcutsearch", ICON_LC_SEARCH " Search actions, contexts or keys", st.search, sizeof st.search);
        ImGui::SameLine();
        ImGui::Checkbox("Conflicts only", &st.conflictsOnly);
        ImGui::SetItemTooltip("Show only the actions whose shortcut another action in an overlapping context also uses.");
        ImGui::SameLine();
        if (ImGui::Button("Reset All"))
        {
            StopListening(actions, st);
            st.refusal.clear();
            ResetAllShortcuts(actions, archive);
        }
        ImGui::SetItemTooltip("Every action back to its default: clears the shortcuts saved for all projects (%s) and "
                              "this project's overrides (%s). Command-line and console values stay; clear those per row.",
                              RungLabel(Arcane::SetBy::EditorUser), RungLabel(Arcane::SetBy::User));
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
        for (const ShortcutRow& row : BuildShortcutRows(actions, st.search, st.conflictsOnly))
        {
            ImGui::PushID(row.id.c_str());
            ImGui::TableNextRow();
            if (row.conflict) ImGui::PushStyleColor(ImGuiCol_Text, Theme::kError);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(row.action.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", row.tooltip.c_str());
            if (row.overriddenBy != Arcane::SetBy::Default)   // the generic rows' marker, compact for the Action column
            {
                // Icons, not the generic rows' full sentence: "Overridden by
                // Command line" clipped the Action column at 1080p (S4-GATE capture).
                ImGui::SameLine();
                ImGui::TextColored(Theme::kAmber, ICON_LC_LAYERS);
                ImGui::SetItemTooltip("Overridden by %s: this binding wins over the one saved for all projects", RungLabel(row.overriddenBy));
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_LC_ERASER "##clearoverride"))
                {
                    if (ClearShortcutOverride(actions, row.id, archive))
                        st.refusal.clear();
                    else
                        st.refusal = std::string("The ") + RungLabel(row.overriddenBy) + " binding of '" + row.action
                                   + "' was not cleared: the registry no longer holds it there.";
                }
                ImGui::SetItemTooltip("Clear override: remove the %s binding; the row shows what is left underneath", RungLabel(row.overriddenBy));
            }
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
