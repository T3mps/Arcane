#include "Settings/SettingsWindow.hpp"

#include "Widgets/EditorTheme.hpp"
#include "Widgets/IconsLucide.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace Arcane::Editor
{
    namespace
    {
        using Page = SettingsWindowState::FrameFacts::Page;

        // Valid only while SettingsWindowEnv::drawPage runs.
        thread_local SettingsRowContext* t_pageRow = nullptr;
        thread_local PropertyGrid*       t_pageGrid = nullptr;

        void Probe(SettingsWindowState& st, const std::string& key)
        {
            if (!st.grid.probe) return;
            const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
            (*st.grid.probe)[key] = ImVec2((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
        }

        const SettingsTreeNode* FindIn(const SettingsTreeNode& node, std::string_view path)
        {
            if (node.path == path) return &node;
            for (const SettingsTreeNode& c : node.children)
                if (const SettingsTreeNode* hit = FindIn(c, path)) return hit;
            return nullptr;
        }

        bool HasPage(const SettingsWindowEnv& env, std::string_view path)
        {
            for (const SettingsPageRef& p : env.pages)
                if (p.categoryPath == path) return true;
            return false;
        }

        bool IsModuleBranch(std::string_view path) { return path.starts_with("Game/") || path.starts_with("Plugins/"); }

        void DrawTreeNode(SettingsWindowState& st, const SettingsTreeNode& node)
        {
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                       ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
            if (node.children.empty()) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            if (node.path == st.selected) flags |= ImGuiTreeNodeFlags_Selected;
            const bool open = ImGui::TreeNodeEx(node.path.c_str(), flags, "%s", node.label.c_str());
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) st.selected = node.path;
            Probe(st, "tree:" + node.path);
            st.last.treePaths.push_back(node.path);
            if (open && !node.children.empty())
            {
                for (const SettingsTreeNode& child : node.children) DrawTreeNode(st, child);
                ImGui::TreePop();
            }
        }

        void DrawRows(SettingsRowContext& ctx, SettingsWindowState& st, const SettingsTreeNode& node,
                      const std::unordered_set<std::string>& visible)
        {
            // I9 / spec 6.2: Attr::Category -> CVarDesc::group is an in-page
            // sub-header. categoryPath children are tree nodes (and recurse
            // below); a group never becomes one.
            const auto drawGroup = [&](std::string_view group)
            {
                const std::string id = "##rows/" + node.path + "/" + std::string(group);
                PropertyGrid::Rows rows(ctx.grid, id.c_str());
                if (rows)
                    for (const std::string& name : node.cvars)
                    {
                        if (!visible.contains(name)) continue;
                        const CVarDescInfo* desc = st.model.Desc(name);
                        if (!desc || desc->group != group) continue;
                        const SettingRowResult r = DrawSettingRow(ctx, name);
                        if (!r.drawn) continue;
                        st.last.rows.push_back(name);
                        if (r.overridden) st.last.overridden.push_back(name);
                    }
            };
            drawGroup({});
            std::vector<std::string> groups;
            for (const std::string& name : node.cvars)
            {
                if (!visible.contains(name)) continue;
                const CVarDescInfo* desc = st.model.Desc(name);
                if (desc && !desc->group.empty() && std::find(groups.begin(), groups.end(), desc->group) == groups.end())
                    groups.push_back(desc->group);
            }
            for (const std::string& group : groups)
            {
                ImGui::PushID("group");
                const bool open = ctx.grid.SubSection(group);
                Probe(st, "group:" + node.path + "/" + group);
                st.last.groups.push_back(group);
                if (open)
                {
                    drawGroup(group);
                    ctx.grid.EndSubSection();
                }
                ImGui::PopID();
            }
            for (const SettingsTreeNode& child : node.children)
            {
                if (child.cvars.empty() && child.children.empty()) continue;   // a page-only child: no sub-header
                if (ctx.grid.SubSection(child.label))
                {
                    DrawRows(ctx, st, child, visible);
                    ctx.grid.EndSubSection();
                }
            }
        }

        void DrawToolbar(SettingsWindowState& st, const SettingsWindowEnv& env, Arcane::CommandStack& undo)
        {
            ImGui::BeginDisabled(!undo.CanUndo() || undo.InTransaction());
            if (ImGui::Button(ICON_LC_UNDO "##settings-undo")) undo.Undo();
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Undo %s (Ctrl+Z)", undo.UndoLabel());
            ImGui::SameLine();
            ImGui::BeginDisabled(!undo.CanRedo() || undo.InTransaction());
            if (ImGui::Button(ICON_LC_REDO "##settings-redo")) undo.Redo();
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Redo %s (Ctrl+Y)", undo.RedoLabel());
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22.0f);
            ImGui::InputTextWithHint("##settings-search", ICON_LC_SEARCH " Search settings", st.search, sizeof(st.search));
            Probe(st, "##settings-search");
            ImGui::SameLine();
            const char* filters[] = { "All", "Modified", "Overridden", "Project overrides" };
            const int filterCount = env.kind == SettingsWindowKind::Preferences ? 4 : 3;
            if (env.kind == SettingsWindowKind::Project && st.filter == SettingsModel::Filter::ProjectOverrides)
                st.filter = SettingsModel::Filter::All;
            int filter = static_cast<int>(st.filter);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
            if (ImGui::Combo("##settings-filter", &filter, filters, filterCount))
                st.filter = static_cast<SettingsModel::Filter>(filter);
            ImGui::SameLine();
            ImGui::Checkbox("Show advanced", &st.showAdvanced);
            ImGui::SetItemTooltip("Also list Dev and Hidden settings");
        }

        void DrawApplyBars(SettingsWindowState& st, const SettingsWindowEnv& env, const CVarRegistry& reg)
        {
            if (!env.tracker) return;
            const std::vector<std::string> restart = env.tracker->PendingRestart(reg);
            const std::vector<std::string> nextWorld = env.tracker->PendingNextWorld(reg);
            st.last.restartPending = restart.size();
            st.last.nextWorldPending = nextWorld.size();
            if (!restart.empty())
            {
                const bool one = restart.size() == 1;
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::kWarning);
                ImGui::Text(ICON_LC_POWER "  %zu setting%s need%s a restart", restart.size(), one ? "" : "s", one ? "s" : "");
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                {
                    std::string list;
                    for (const std::string& n : restart) { if (!list.empty()) list += '\n'; list += n; }
                    ImGui::SetTooltip("%s", list.c_str());
                }
                ImGui::SameLine();
                const bool blocked = !env.restartEditor || !env.restartBlockedReason.empty();
                ImGui::BeginDisabled(blocked);
                if (ImGui::SmallButton("Restart editor")) env.restartEditor();
                ImGui::EndDisabled();
                Probe(st, "Restart editor");
                if (!env.restartBlockedReason.empty()) ImGui::SetItemTooltip("%s", env.restartBlockedReason.c_str());
                else ImGui::SetItemTooltip("Reopen this project in a fresh editor so these settings take effect");
            }
            if (!nextWorld.empty())
            {
                const bool one = nextWorld.size() == 1;
                ImGui::TextDisabled(ICON_LC_ROTATE_CW "  %zu setting%s appl%s on the next Play or scene reopen",
                                    nextWorld.size(), one ? "" : "s", one ? "ies" : "y");
            }
        }

        void DrawPage(SettingsWindowState& st, const SettingsWindowEnv& env, CVarRegistry& reg, Arcane::CommandStack& undo,
                      const SettingsEditSink& sink, const SettingsTreeNode* node, bool unloaded,
                      const std::unordered_set<std::string>& visible)
        {
            if (unloaded)
            {
                st.last.page = Page::ModuleUnloaded;
                ImGui::TextUnformatted(ICON_LC_PLUG "  Module unloaded");
                ImGui::TextWrapped("The module that declares '%s' is not loaded right now. Its settings come back here "
                                   "when it reloads (a game module rebuild reloads it).", st.selected.c_str());
                return;
            }
            if (!node)
            {
                ImGui::TextDisabled("No settings match.");
                return;
            }
            std::string crumb;
            for (const char c : node->path) { if (c == '/') crumb += " > "; else crumb.push_back(c); }
            ImGui::TextDisabled("%s", crumb.c_str());
            PropertyGrid grid(st.grid);
            SettingsRowContext ctx{ .registry = reg, .window = env.kind, .grid = grid, .undo = undo, .gesture = st.gesture,
                                    .memo = st.memo, .sink = sink, .textDrafts = st.textDrafts, .assetRefs = env.assetRefs,
                                    .browsePath = env.browsePath, .projectOpen = env.projectOpen };
            st.last.page = Page::Rows;
            const bool custom = env.drawPage && HasPage(env, node->path);
            if (custom)
            {
                st.last.page = Page::Custom;
                t_pageRow = &ctx;
                t_pageGrid = &grid;
                env.drawPage(node->path);
                t_pageRow = nullptr;
                t_pageGrid = nullptr;
            }
            // Keyboard owns its keychord rows and Layout owns its two controls;
            // generic rows would repeat those controls below the custom page.
            // Font rows stay standard rows so edits retain row provenance and undo.
            const auto ownedByPage = [&](const std::string& name)
            {
                const CVarDescInfo* desc = st.model.Desc(name);
                return (desc && RowWidgetFor(*desc) == RowWidget::KeyChord) ||
                       (node->path == "Layout" &&
                        (name == "editor.layout.default" || name == "editor.layout.openPanelsAtStart"));
            };
            if (custom && std::any_of(node->cvars.begin(), node->cvars.end(), ownedByPage))
            {
                std::unordered_set<std::string> rows = visible;
                for (const std::string& name : node->cvars)
                    if (ownedByPage(name)) rows.erase(name);
                DrawRows(ctx, st, *node, rows);
            }
            else
                DrawRows(ctx, st, *node, visible);
            st.last.tooltip = ctx.lastTooltip;
            st.last.contextMenu = ctx.lastContextMenu;
        }

        void DrawBody(SettingsWindowState& st, const SettingsWindowEnv& env, CVarRegistry& reg, Arcane::CommandStack& undo)
        {
            const SettingScope scope = env.kind == SettingsWindowKind::Project ? SettingScope::Project
                                                                               : SettingScope::PreferencesMachine;
            if (st.model.BuiltRevision() != reg.Revision() || st.builtPages != env.pages || st.builtRoles != env.roles)
            {
                st.model.SetModuleRoles(env.roles);
                st.model.SetPages(env.pages);
                st.model.Rebuild(reg, scope);
                st.builtPages = env.pages;
                st.builtRoles = env.roles;
                if (env.tracker) env.tracker->Observe(reg);   // a reloaded module's Restart rows get their baseline
            }
            const SettingsEditSink sink = ArchiveSink(env.archive, env.now);

            // Window-local undo (spec s6.3): ours while focused and no text box owns the keys.
            if (st.focused && !ImGui::GetIO().WantTextInput && !undo.InTransaction())
            {
                if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) undo.Undo();
                else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y) ||
                         ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z)) undo.Redo();
            }

            DrawToolbar(st, env, undo);
            DrawApplyBars(st, env, reg);
            ImGui::Separator();

            const std::vector<std::string> visibleNames = st.model.Visible(reg, st.search, st.filter, st.showAdvanced);
            const SettingsTreeNode tree = st.model.Pruned(visibleNames);
            const std::unordered_set<std::string> visible(visibleNames.begin(), visibleNames.end());
            const SettingsTreeNode* node = st.selected.empty() ? nullptr : FindIn(tree, st.selected);
            bool unloaded = false;
            if (!node)
            {
                if (!st.selected.empty() && IsModuleBranch(st.selected) && !st.model.Find(st.selected))
                    unloaded = true;   // its module went away: keep the selection for the reload
                else if (!tree.children.empty())
                {
                    st.selected = tree.children.front().path;
                    node = &tree.children.front();
                }
            }

            if (ImGui::BeginChild("##settings-tree", ImVec2(ImGui::GetFontSize() * 16.0f, 0.0f),
                                  ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX))
                for (const SettingsTreeNode& root : tree.children) DrawTreeNode(st, root);
            ImGui::EndChild();
            ImGui::SameLine();
            if (ImGui::BeginChild("##settings-page", ImVec2(0.0f, 0.0f)))
                DrawPage(st, env, reg, undo, sink, node, unloaded, visible);
            ImGui::EndChild();
        }
    }

    Arcane::CommandStack& SettingsUndo(SettingsWindowState& st)
    {
        if (!st.undo)
            st.undo = std::make_unique<Arcane::CommandStack>(
                []() -> Astra::Registry& { throw std::logic_error("settings undo never touches the scene registry"); });
        return *st.undo;
    }

    void DrawSettingsWindow(SettingsWindowState& st, const SettingsWindowEnv& env, bool* open)
    {
        Arcane::CommandStack& undo = SettingsUndo(st);
        EditGesture::ScopeGuard guard{ &undo, st.gesture };   // FIRST local: closes an abandoned drag on every path
        st.last = {};
        const auto flush = [&] { if (env.archive) env.archive->Flush(env.writeRung); };
        if (open && !*open)
        {
            if (st.wasOpen) flush();   // closed from outside since the last frame
            st.wasOpen = false;
            st.focused = false;
            return;
        }
        st.wasOpen = true;
        CVarRegistry& reg = *env.registry;
        PropertyGrid(st.grid).CommitOrphans();   // once per frame, before the window Begins
        ImGui::SetNextWindowSize(ImVec2(1100.0f, 720.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        const bool visible = ImGui::Begin(env.title, open);
        st.focused = visible && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (visible) DrawBody(st, env, reg, undo);
        ImGui::End();
        if (open && !*open)   // the X this frame: write now (spec s6.3)
        {
            flush();
            st.wasOpen = false;
            st.focused = false;
        }
    }

    PropertyGrid& CurrentSettingsGrid()
    {
        IM_ASSERT(t_pageGrid && "CurrentSettingsGrid: only inside a settings page's draw function");
        return *t_pageGrid;
    }

    bool SettingsRow(std::string_view name)
    {
        if (!t_pageRow) return false;
        return DrawSettingRow(*t_pageRow, name).drawn;
    }

    void SetSettingsFontFamilies(const std::vector<EditorFontFamily>* families)
    {
        if (t_pageRow) t_pageRow->fontFamilies = families;
    }
}
