#include "EditorPanels.hpp"
#include "AssetBrowser.hpp"
#include "ConsoleBuffer.hpp"
#include "EditorFonts.hpp"
#include "EntityList.hpp"
#include "IconsLucide.h"
#include "InspectorFields.hpp"
#include "PlayMode.hpp"
#include "SelectionContext.hpp"

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Edit/EntityOps.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Sim/RunLoop.hpp>

#include <Astra/Reflection/FieldVisitor.hpp>
#include <Astra/Registry/Registry.hpp>

#include <glm/glm.hpp>
#include <imgui.h>
#include <imgui_internal.h>   // DockBuilder* + ImGuiDockNode::LocalFlags (docking layout)

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>

namespace Arcane::Editor
{
    void BeginDockSpace(Arcane::CommandStack& undo, MenuRequests& requests)
    {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::SetNextWindowViewport(vp->ID);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
            ImGuiWindowFlags_MenuBar;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("EditorDockHost", nullptr, flags);
        ImGui::PopStyleVar(3);

        // Editor menu bar. File items + Edit's Cut/Copy/Paste are placeholders for now;
        // Edit's Undo/Redo drive the CommandStack; Preferences is a leaf item that will
        // open a settings window later.
        if (ImGui::BeginMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("Open Project...")) requests.openProject = true;
                ImGui::Separator();
                ImGui::MenuItem("New Scene");
                ImGui::MenuItem("Open Scene...");
                ImGui::Separator();
                if (ImGui::MenuItem("New Material...")) requests.newMaterial = true;
                if (ImGui::MenuItem("Open Material...")) requests.openMaterial = true;
                ImGui::Separator();
                ImGui::MenuItem("Save Scene");
                ImGui::MenuItem("Save Scene As...");
                ImGui::Separator();
                ImGui::MenuItem("Exit");
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Edit"))
            {
                // Undo/Redo share the CommandStack with the Ctrl+Z / Ctrl+Y shortcuts
                // (handled in the app input loop); the shortcut text here is display-only.
                const bool canUndo = undo.CanUndo();
                const bool canRedo = undo.CanRedo();
                const std::string undoLabel = canUndo ? (std::string("Undo ") + undo.UndoLabel())
                                                      : std::string("Undo");
                const std::string redoLabel = canRedo ? (std::string("Redo ") + undo.RedoLabel())
                                                      : std::string("Redo");
                if (ImGui::MenuItem(undoLabel.c_str(), "Ctrl+Z", false, canUndo)) undo.Undo();
                if (ImGui::MenuItem(redoLabel.c_str(), "Ctrl+Y", false, canRedo)) undo.Redo();
                ImGui::Separator();
                ImGui::MenuItem("Cut");
                ImGui::MenuItem("Copy");
                ImGui::MenuItem("Paste");
                ImGui::EndMenu();
            }
            // Leaf item (no submenu): will open a preferences window later.
            if (ImGui::MenuItem("Preferences")) { /* TODO: open preferences window */ }
            ImGui::EndMenuBar();
        }
        // Host window is left OPEN: the caller draws the fixed toolbar strip
        // (DrawSimTimeToolbar) into it, then closes it via EndDockSpace().
    }

    // Build the standard editor layout once (when there is no saved .ini node yet): the
    // Viewport takes the central node; the tool panels are split off around it, so the
    // Viewport's size is whatever those panels leave. Names must match each panel's
    // ImGui::Begin() title.
    static void BuildDefaultLayout(ImGuiID dockspaceId)
    {
        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

        ImGuiID central = dockspaceId;
        const ImGuiID leftId   = ImGui::DockBuilderSplitNode(central, ImGuiDir_Left,  0.18f, nullptr, &central);
        const ImGuiID rightId  = ImGui::DockBuilderSplitNode(central, ImGuiDir_Right, 0.22f, nullptr, &central);
        const ImGuiID bottomId = ImGui::DockBuilderSplitNode(central, ImGuiDir_Down,  0.25f, nullptr, &central);

        ImGui::DockBuilderDockWindow("Outliner", leftId);
        ImGui::DockBuilderDockWindow("Inspector", rightId);
        ImGui::DockBuilderDockWindow("Assets",    bottomId);   // Assets tab first...
        ImGui::DockBuilderDockWindow("Console",   bottomId);   // ...then Console
        ImGui::DockBuilderDockWindow("Viewport",  central);
        ImGui::DockBuilderFinish(dockspaceId);
    }

    void EndDockSpace()
    {
        const ImGuiID dockspaceId = ImGui::GetID("EditorDockSpace");

        // First run (no saved .ini layout): arrange the default editor layout.
        if (ImGui::DockBuilderGetNode(dockspaceId) == nullptr)
            BuildDefaultLayout(dockspaceId);

        // Emit the dockspace into the still-open host window. Anything drawn between
        // BeginDockSpace and here is a fixed strip above it.
        //
        // The central node is NOT locked anymore: the Viewport is a normal tab
        // (movable, dockable-over) and editor documents open as tabs beside it
        // -- the UE/Unity shape where asset editors share the main area with
        // the scene.
        ImGui::DockSpace(dockspaceId, ImVec2(0, 0), ImGuiDockNodeFlags_None);

        // SCRUB the old lock: NoTabBar/NoCloseButton are SAVED dock flags, so
        // every imgui.ini written while the lock existed still carries them --
        // simply not re-applying changes nothing on machines with a layout.
        if (ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockspaceId))
            central->LocalFlags &= ~(ImGuiDockNodeFlags_NoTabBar
                                   | ImGuiDockNodeFlags_NoCloseButton
                                   | ImGuiDockNodeFlags_NoDockingOverMe
                                   | ImGuiDockNodeFlags_NoUndocking);

        ImGui::End();
    }

    void DrawSimTimeToolbar(PlaySession& play, Arcane::Runtime& runtime,
                            const Arcane::PluginVTable* plugin, uint64_t logoTex)
    {
        // Icon button with a hover tooltip (icons need discoverable labels).
        // `id` is an ImGui ID-only suffix (e.g. "##sim_playstop") appended to the
        // glyph so that two buttons showing the SAME icon (e.g. Play and Resume
        // both render ICON_LC_PLAY) never hash to the same widget ID -- without
        // a unique id, ImGui::Button(icon) collides on the label hash and the
        // button drawn first steals the shared ActiveId out from under the one
        // drawn second, making the second button unclickable.
        auto iconBtn = [](const char* icon, const char* id, const char* tip) -> bool
        {
            const std::string label = std::string(icon) + id;
            const bool clicked = ImGui::Button(label.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            return clicked;
        };
        // Toggle-style icon button: tinted background when active (gizmo T/R/S).
        auto iconToggle = [](const char* icon, const char* id, bool active, const char* tip) -> bool
        {
            if (active) ImGui::PushStyleColor(ImGuiCol_Button,
                                              ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            const std::string label = std::string(icon) + id;
            const bool clicked = ImGui::Button(label.c_str());
            if (active) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            return clicked;
        };

        // Fixed transport strip drawn into the CURRENT window (the dockspace host -- call
        // between BeginDockSpace and EndDockSpace). Not its own window: no tab, cannot be
        // docked or moved. The logo mark is a touch TALLER than the transport buttons (a
        // small brand mark reads better with a few more pixels), vertically centered on the
        // button row -- so the top/bottom padding also absorbs its overhang above/below.
        const ImGuiStyle& st = ImGui::GetStyle();
        const float btnH     = ImGui::GetFrameHeight();
        const float logoH    = (logoTex != 0) ? std::floor(btnH * 1.35f) : btnH;
        const float overhang = (logoH - btnH) * 0.5f;   // logo extends this far above/below the row
        ImGui::Dummy(ImVec2(0.0f, 3.0f + overhang));

        // Measure the full strip at the button-row start so the transport centers in the
        // whole width (not the width left of the logo).
        const float fullContentW = ImGui::GetContentRegionAvail().x;
        const float lineStartX   = ImGui::GetCursorPosX();
        const float rowY         = ImGui::GetCursorPosY();

        // -- LEFT cluster (Unity-style branding): [pad] [logo] [wordmark], inset from the
        // window edge and vertically centered on the button row. Absolute SetCursorPos so it
        // never perturbs the transport's own placement. leftX tracks the running right edge.
        const float leftPad = 8.0f;
        float leftX = lineStartX + leftPad;
        if (logoTex != 0)
        {
            ImGui::SetCursorPos(ImVec2(leftX, rowY - overhang));
            ImGui::Image((ImTextureID)logoTex, ImVec2(logoH, logoH));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Arcane");
            leftX += logoH + 8.0f;   // gap between logo and wordmark
        }
        if (ImFont* brand = GetEditorFonts().brand)
        {
            // "Arcane" wordmark in the display face, a touch shorter than the logo, centered
            // on the same row. PushFont(font, size) renders it at a display size (ImGui 1.92).
            const float brandSize = std::floor(logoH * 0.80f);
            ImGui::PushFont(brand, brandSize);
            const ImVec2 sz = ImGui::CalcTextSize("Arcane");
            ImGui::SetCursorPos(ImVec2(leftX, rowY + (btnH - sz.y) * 0.5f));
            ImGui::TextUnformatted("Arcane");
            ImGui::PopFont();
            leftX += sz.x;
        }

        // -- CENTER: transport (Play / Pause / Step), Unity-style toggles. --
        // Center the trio within the full toolbar width, placed on the button row -- but
        // never behind the left cluster (clamp on narrow windows). Undo/Redo moved to the
        // Edit menu; the transform-gizmo tools moved to the Viewport overlay.
        auto btnW = [&](const char* icon)
        { return ImGui::CalcTextSize(icon).x + st.FramePadding.x * 2.0f; };
        const float transportW = btnW(ICON_LC_PLAY) + btnW(ICON_LC_PAUSE)
                               + btnW(ICON_LC_STEP_FORWARD) + st.ItemSpacing.x * 2.0f;
        const float centerStart = lineStartX + (fullContentW - transportW) * 0.5f;
        ImGui::SetCursorPos(ImVec2(std::max(centerStart, leftX + 12.0f), rowY));

        // Play/Stop toggle (Unity-style): ALWAYS the play glyph, tinted while playing;
        // clicking the lit (playing) button stops. No icon swap.
        const bool playing = play.IsPlaying();
        if (iconToggle(ICON_LC_PLAY, "##sim_play", playing, playing ? "Stop" : "Play"))
        {
            if (playing) play.Stop(runtime, plugin);
            else         play.Play(runtime, plugin);
        }
        // Re-fetch AFTER a possible Stop -- Runtime::RestoreRegistry destroys and
        // replaces m_impl->loop on Stop, so a reference taken before this dangles.
        // (Nothing above touches `loop`, so fetching here is safe.)
        Arcane::RunLoop& loop = runtime.Loop();
        ImGui::SameLine();
        // Pause/Resume toggle (Unity-style): ALWAYS the pause glyph, tinted while paused.
        // Disabled OUTSIDE Play: the loop's pause IS the Edit-mode sim freeze
        // (PlaySession::Stop pauses it, Play unpauses it), so toggling it in Edit would
        // run the sim with no active Play session and no way to Stop. Pause only within
        // Play; tint only while actually playing so it never looks "armed" in Edit.
        ImGui::BeginDisabled(!play.IsPlaying());
        if (iconToggle(ICON_LC_PAUSE, "##sim_pause", play.IsPlaying() && loop.IsPaused(),
                       loop.IsPaused() ? "Resume" : "Pause"))
            loop.SetPaused(!loop.IsPaused());
        ImGui::EndDisabled();
        ImGui::SameLine();
        // Step: momentary; disabled outside Play (single-step is meaningless in Edit),
        // matching Unity's greyed-out Step.
        ImGui::BeginDisabled(!play.IsPlaying());
        if (iconBtn(ICON_LC_STEP_FORWARD, "##sim_step", "Step")) loop.RequestSingleStep();
        ImGui::EndDisabled();

        ImGui::Dummy(ImVec2(0.0f, 3.0f + overhang));   // clear the logo's lower overhang too
        ImGui::Separator();
    }

    void DrawConsolePanel(const ConsoleBuffer& console)
    {
        ImGui::Begin("Console");
        console.ForEach([](const std::string& line)
        {
            ImGui::TextUnformatted(line.c_str());
        });
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);   // autoscroll while pinned to bottom
        ImGui::End();
    }

    ViewportPanelResult DrawViewportPanel(uint64_t textureId, uint32_t texW, uint32_t texH,
                                          bool& gizmoEnabled, Arcane::GizmoMode& mode,
                                          Arcane::GizmoSpace& space)
    {
        // Stateless icon-button helpers (mirrors the toolbar's).
        auto iconBtn = [](const char* icon, const char* id, const char* tip) -> bool
        {
            const bool clicked = ImGui::Button((std::string(icon) + id).c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            return clicked;
        };
        auto iconToggle = [](const char* icon, const char* id, bool active, const char* tip) -> bool
        {
            if (active) ImGui::PushStyleColor(ImGuiCol_Button,
                                              ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            const bool clicked = ImGui::Button((std::string(icon) + id).c_str());
            if (active) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            return clicked;
        };

        ViewportPanelResult r;
        ImGui::Begin("Viewport");
        r.dockId = static_cast<unsigned int>(ImGui::GetWindowDockID());
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        r.desiredW = avail.x > 0 ? static_cast<uint32_t>(avail.x) : 1;
        r.desiredH = avail.y > 0 ? static_cast<uint32_t>(avail.y) : 1;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        if (textureId != 0 && texW > 0 && texH > 0)
            ImGui::Image((ImTextureID)textureId, ImVec2((float)texW, (float)texH));
        r.imageRect = ViewportRect{ origin.x, origin.y, (float)texW, (float)texH };
        r.hovered = ImGui::IsWindowHovered();
        r.focused = ImGui::IsWindowFocused();

        // UE5-style transform-tool overlay at the top-right of the viewport image:
        // Select (no gizmo) / Move / Rotate / Scale + Local-World. Drawn over the
        // image; a click on it changes the tool and must NOT also pick an entity.
        bool overlayHovered = false;
        {
            const ImGuiStyle& st = ImGui::GetStyle();
            auto bw = [&](const char* ic){ return ImGui::CalcTextSize(ic).x + st.FramePadding.x * 2.0f; };
            const float totalW = bw(ICON_LC_MOUSE_POINTER_2) + bw(ICON_LC_MOVE) + bw(ICON_LC_ROTATE_3D)
                               + bw(ICON_LC_SCALE_3D) + bw(ICON_LC_BOX) + st.ItemSpacing.x * 4.0f;
            const float pad = 8.0f;
            ImGui::SetCursorScreenPos(ImVec2(origin.x + (float)texW - totalW - pad, origin.y + pad));
            ImGui::BeginGroup();
            if (iconToggle(ICON_LC_MOUSE_POINTER_2, "##tool_sel", !gizmoEnabled, "Select (Q)"))
                gizmoEnabled = false;
            ImGui::SameLine();
            if (iconToggle(ICON_LC_MOVE_3D, "##tool_t", gizmoEnabled && mode == Arcane::GizmoMode::Translate, "Move (W)"))
            { gizmoEnabled = true; mode = Arcane::GizmoMode::Translate; }
            ImGui::SameLine();
            if (iconToggle(ICON_LC_ROTATE_3D, "##tool_r", gizmoEnabled && mode == Arcane::GizmoMode::Rotate, "Rotate (E)"))
            { gizmoEnabled = true; mode = Arcane::GizmoMode::Rotate; }
            ImGui::SameLine();
            if (iconToggle(ICON_LC_SCALE_3D, "##tool_s", gizmoEnabled && mode == Arcane::GizmoMode::Scale, "Scale (R)"))
            { gizmoEnabled = true; mode = Arcane::GizmoMode::Scale; }
            ImGui::SameLine();
            {
                const bool local = (space == Arcane::GizmoSpace::Local);
                if (iconBtn(local ? ICON_LC_BOX : ICON_LC_GLOBE, "##tool_space",
                            local ? "Local space" : "World space"))
                    space = local ? Arcane::GizmoSpace::World : Arcane::GizmoSpace::Local;
            }
            ImGui::EndGroup();
            overlayHovered = ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        }

        // Capture a left-click inside the image, in viewport-local px (origin = image
        // top-left), UNLESS it landed on the tool overlay above. EditorApp unprojects
        // it through the plugin camera and drives the entity pick.
        if (r.hovered && !overlayHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            const ImVec2 m = ImGui::GetMousePos();
            const float lx = m.x - origin.x, ly = m.y - origin.y;
            if (lx >= 0 && ly >= 0 && lx < (float)texW && ly < (float)texH)
            {
                r.clicked = true;
                r.altHeld = ImGui::GetIO().KeyAlt;
                r.clickLocalX = lx; r.clickLocalY = ly;
            }
        }
        ImGui::End();
        return r;
    }

    namespace
    {
        bool ApplyStructural(Arcane::CommandStack& undo, const OutlinerBinding& b,
                             std::string label, Arcane::FunctionRef<bool()> mutate)
        {
            if (!b.editMode)
                return false;
            return Arcane::ApplyRegistryMutation(undo, std::move(label),
                                                 b.snapshot, b.restore, mutate);
        }

        void BeginRename(OutlinerState& st, Astra::Entity e, const std::string& current)
        {
            st.renameTarget = e;
            std::snprintf(st.renameBuf, sizeof(st.renameBuf), "%s", current.c_str());
            st.renameFocusPending = true;
        }

        void DeleteSelection(Astra::Registry& registry, SelectionContext& sel,
                             Arcane::CommandStack& undo, const OutlinerBinding& binding)
        {
            const std::vector<Astra::Entity> doomed = sel.Entities();   // copy: sel mutates after
            if (doomed.empty())
                return;
            if (ApplyStructural(undo, binding, "Delete",
                    [&] { return Arcane::Edit::DeleteEntities(registry, doomed) > 0; }))
                sel.Clear();
        }
    }

    void DrawOutlinerPanel(Astra::Registry& registry, SelectionContext& sel,
                           Arcane::CommandStack& undo, const OutlinerBinding& binding,
                           OutlinerState& state)
    {
        ImGui::Begin("Outliner");

        // A structural undo/redo can destroy the rename target out from
        // under the InputText; drop the stale target or `renaming` wedges
        // shut (and F2/Delete with it).
        if (state.renameTarget.IsValid() && !registry.IsValid(state.renameTarget))
            state.renameTarget = Astra::Entity::Invalid();

        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##outliner_search", ICON_LC_SEARCH " Filter",
                                 state.search, sizeof(state.search));

        const std::vector<OutlinerRow> rows =
            BuildOutlinerRows(registry, state.search, state.sort, state.collapsed);

        const bool renaming = state.renameTarget.IsValid();
        const bool windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
        // Shortcuts must not fire while any text field owns the keyboard
        // (e.g. the search box above) -- else Delete/F2 hijack typing.
        if (binding.editMode && windowFocused && !renaming && !ImGui::GetIO().WantTextInput)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_F2, false) && sel.Count() == 1)
                BeginRename(state, sel.Primary(),
                            Arcane::Edit::DisplayName(registry, sel.Primary()));
            if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && sel.HasSelection())
                DeleteSelection(registry, sel, undo, binding);
        }

        const float footerH = ImGui::GetFrameHeightWithSpacing();
        const ImGuiTableFlags tflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
                                     | ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate;
        if (ImGui::BeginTable("##outliner_rows", 3, tflags, ImVec2(0.0f, -footerH)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(ICON_LC_EYE,
                ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed,
                ImGui::GetFrameHeight());
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_DefaultSort);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::CalcTextSize("Post Process").x * 1.4f);
            ImGui::TableHeadersRow();

            // Header sort -> state.sort; rows were built with LAST frame's
            // sort (one-frame lag, rebuilt every frame anyway).
            if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs())
            {
                state.sort = OutlinerSort{};
                if (specs->SpecsCount > 0)
                {
                    const ImGuiTableColumnSortSpecs& s = specs->Specs[0];
                    state.sort.column = (s.ColumnIndex == 2) ? OutlinerSort::Column::Type
                                                             : OutlinerSort::Column::Label;
                    state.sort.ascending = s.SortDirection != ImGuiSortDirection_Descending;
                }
            }

            for (const OutlinerRow& row : rows)
            {
                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(row.entity.GetValue()));

                // -- column 0: the eye --------------------------------------
                ImGui::TableSetColumnIndex(0);
                {
                    const char* icon = row.hidden ? ICON_LC_EYE_OFF : ICON_LC_EYE;
                    if (row.hidden)
                        ImGui::PushStyleColor(ImGuiCol_Text,
                            ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    if (binding.editMode)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                        if (ImGui::SmallButton(icon))
                        {
                            const Astra::Entity e = row.entity;
                            const bool hide = !row.hidden;
                            ApplyStructural(undo, binding, hide ? "Hide" : "Show",
                                [&] { return Arcane::Edit::SetHiddenRecursive(registry, e, hide) > 0; });
                        }
                        ImGui::PopStyleColor();
                    }
                    else
                        ImGui::TextUnformatted(icon);
                    if (row.hidden)
                        ImGui::PopStyleColor();
                }

                // -- column 1: tree arrow + label (or inline rename) --------
                ImGui::TableSetColumnIndex(1);
                if (state.renameTarget == row.entity)
                {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (state.renameFocusPending)
                    {
                        ImGui::SetKeyboardFocusHere();
                        state.renameFocusPending = false;
                    }
                    bool commit = ImGui::InputText("##rename", state.renameBuf,
                        sizeof(state.renameBuf),
                        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                    if (ImGui::IsItemDeactivated())
                    {
                        commit = commit || !ImGui::IsKeyPressed(ImGuiKey_Escape);
                        if (commit)
                        {
                            const Astra::Entity e = row.entity;
                            ApplyStructural(undo, binding, "Rename",
                                [&] { return Arcane::Edit::RenameEntity(registry, e, state.renameBuf); });
                        }
                        state.renameTarget = Astra::Entity::Invalid();
                    }
                }
                else
                {
                    const float indent = row.depth * ImGui::GetStyle().IndentSpacing;
                    if (indent > 0.0f)
                        ImGui::Indent(indent);

                    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth
                                             | ImGuiTreeNodeFlags_OpenOnArrow
                                             | ImGuiTreeNodeFlags_NoTreePushOnOpen;
                    if (!row.hasChildren)
                        flags |= ImGuiTreeNodeFlags_Leaf;
                    if (sel.Contains(row.entity))
                        flags |= ImGuiTreeNodeFlags_Selected;

                    const std::uint64_t value = static_cast<std::uint64_t>(row.entity.GetValue());
                    const bool open = !state.collapsed.contains(value);
                    ImGui::SetNextItemOpen(open, ImGuiCond_Always);
                    if (row.dimmed)
                        ImGui::PushStyleColor(ImGuiCol_Text,
                            ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    const bool nowOpen = ImGui::TreeNodeEx(row.label.c_str(), flags);
                    if (row.dimmed)
                        ImGui::PopStyleColor();
                    if (row.hasChildren && nowOpen != open)
                    {
                        if (nowOpen) state.collapsed.erase(value);
                        else         state.collapsed.insert(value);
                    }

                    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)
                        && !ImGui::IsItemToggledOpen())
                    {
                        const double now = ImGui::GetTime();
                        const bool ctrl = ImGui::GetIO().KeyCtrl;
                        const bool shift = ImGui::GetIO().KeyShift;
                        if (ctrl)
                            sel.Toggle(row.entity);
                        else if (shift && sel.HasSelection())
                            sel.AddRange(RowRange(rows, sel.Primary(), row.entity), row.entity);
                        else
                        {
                            // Slow second click on the sole-selected row = rename.
                            const bool slowSecond = binding.editMode
                                && sel.Count() == 1 && sel.Primary() == row.entity
                                && state.lastClicked == row.entity
                                && (now - state.lastClickTime) > ImGui::GetIO().MouseDoubleClickTime
                                && (now - state.lastClickTime) < 1.2;
                            if (slowSecond)
                                BeginRename(state, row.entity, row.label);
                            else
                                sel.Select(row.entity);
                        }
                        state.lastClicked = row.entity;
                        state.lastClickTime = now;
                    }

                    if (indent > 0.0f)
                        ImGui::Unindent(indent);
                }

                // -- column 2: type -----------------------------------------
                ImGui::TableSetColumnIndex(2);
                if (row.dimmed)
                    ImGui::TextDisabled("%s", row.type.c_str());
                else
                    ImGui::TextUnformatted(row.type.c_str());

                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        std::size_t total = 0;
        for (Astra::Entity e : registry.GetEntityManager())
        {
            (void)e;
            ++total;
        }
        ImGui::Text("%zu entities (%zu selected)", total, sel.Count());

        ImGui::End();
    }

    namespace
    {
        // Renders one widget per reflected field and applies edits through the
        // pure InspectorFields writers (kept ImGui-free so the write-back is unit-
        // testable). Unsupported/compound types (e.g. glm::mat3, enums) render
        // disabled text -- never crash, never silently misinterpret bytes.
        //
        // Each field's edit gesture is bracketed into the undo stack: the first
        // frame a widget activates (ImGui::IsItemActivated(), BEFORE the edit
        // applies) opens a transaction and snapshots the owning component's
        // pre-edit bytes; the widget's release closes it -- Commit() if the value
        // actually changed (IsItemDeactivatedAfterEdit), Cancel() on a pure click
        // (IsItemDeactivated with no edit) so a no-op click never leaks an empty
        // undo step.
        struct ImGuiFieldVisitor : Astra::IFieldVisitor
        {
            Arcane::CommandStack*             stack = nullptr;
            Astra::Entity                     entity{};
            const Astra::ComponentDescriptor* descriptor = nullptr;
            std::string                       typeName;
            const Arcane::Project*            project = nullptr;   // asset-ref resolve/pick; may be null

            bool IsWriting() const noexcept override { return true; }

            void BeginGestureIfActivated(const std::string& field)
            {
                if (stack && ImGui::IsItemActivated())
                {
                    stack->Begin("Edit " + typeName + "." + field);
                    stack->SnapshotComponent(entity, descriptor);
                }
            }

            void EndGesture()
            {
                if (!stack) return;
                if (ImGui::IsItemDeactivatedAfterEdit()) stack->Commit();
                else if (ImGui::IsItemDeactivated())     stack->Cancel();
            }

            // Single-shot edit (asset drop / popup pick / clear): no widget gesture
            // to bracket, so the whole transaction happens in one call. Commit()
            // closes the stack before the shared EndGesture epilogue runs -- its
            // Commit/Cancel on a closed stack are no-ops.
            void ApplyGuidImmediate(const std::string& field, const Astra::FieldInfo& f,
                                    void* instance, const Arcane::Guid& v)
            {
                if (stack)
                {
                    stack->Begin("Edit " + typeName + "." + field);
                    stack->SnapshotComponent(entity, descriptor);
                }
                Arcane::Editor::ApplyGuidEdit(f, instance, v);
                if (stack) stack->Commit();
            }

            void Visit(const Astra::FieldInfo& f, void* instance) override
            {
                ImGui::PushID(static_cast<int>(f.nameHash));
                const std::string label(f.name);
                switch (Arcane::Editor::ClassifyField(f))
                {
                    case Arcane::Editor::FieldKind::Bool:
                    {
                        bool v = f.Get<bool>(instance);
                        bool changed = ImGui::Checkbox(label.c_str(), &v);
                        BeginGestureIfActivated(label);
                        if (changed) Arcane::Editor::ApplyBoolEdit(f, instance, v);
                        break;
                    }
                    case Arcane::Editor::FieldKind::Int32:
                    {
                        int v = f.Get<int32_t>(instance);
                        bool changed = ImGui::DragInt(label.c_str(), &v);
                        BeginGestureIfActivated(label);
                        if (changed) Arcane::Editor::ApplyIntEdit(f, instance, v);
                        break;
                    }
                    case Arcane::Editor::FieldKind::Float:
                    {
                        float v = f.Get<float>(instance);
                        bool changed = ImGui::DragFloat(label.c_str(), &v, 0.1f);
                        BeginGestureIfActivated(label);
                        if (changed) Arcane::Editor::ApplyFloatEdit(f, instance, v);
                        break;
                    }
                    case Arcane::Editor::FieldKind::Vec2:
                    {
                        glm::vec2 v = f.Get<glm::vec2>(instance);
                        bool changed = ImGui::DragFloat2(label.c_str(), &v.x, 0.1f);
                        BeginGestureIfActivated(label);
                        if (changed) if (glm::vec2* p = f.GetPtr<glm::vec2>(instance)) *p = v;
                        break;
                    }
                    case Arcane::Editor::FieldKind::Vec3:
                    {
                        glm::vec3 v = f.Get<glm::vec3>(instance);
                        bool changed = ImGui::DragFloat3(label.c_str(), &v.x, 0.1f);
                        BeginGestureIfActivated(label);
                        if (changed) if (glm::vec3* p = f.GetPtr<glm::vec3>(instance)) *p = v;
                        break;
                    }
                    case Arcane::Editor::FieldKind::AssetRef:
                    {
                        // Asset-reference (Guid) field: button shows the resolved
                        // mount path (or raw guid), opens a pick popup, and accepts
                        // browser drags; "x" clears. Kind filter is inferred from
                        // the field name (AssetKindFilterForFieldName).
                        const Arcane::Guid v = f.Get<Arcane::Guid>(instance);
                        const int kindFilter = Arcane::Editor::AssetKindFilterForFieldName(label);

                        std::string display = "(none)";
                        if (v.IsValid())
                        {
                            display = v.ToString();
                            if (project)
                                if (const auto mount = project->Registry().Resolve(v))
                                    display = *mount;
                        }

                        if (ImGui::Button((display + "##assetref").c_str()))
                            ImGui::OpenPopup("##assetpick");
                        if (ImGui::BeginDragDropTarget())
                        {
                            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(Arcane::Editor::kAssetDragType))
                            {
                                const auto* payload = static_cast<const Arcane::Editor::AssetDragPayload*>(p->Data);
                                if (kindFilter < 0 || static_cast<int>(payload->kind) == kindFilter)
                                    ApplyGuidImmediate(label, f, instance, payload->guid);
                            }
                            ImGui::EndDragDropTarget();
                        }
                        if (v.IsValid())
                        {
                            ImGui::SameLine();
                            if (ImGui::SmallButton("x##assetclear"))
                                ApplyGuidImmediate(label, f, instance, Arcane::Guid::Nil());
                        }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(label.c_str());

                        if (ImGui::BeginPopup("##assetpick"))
                        {
                            if (!project)
                            {
                                ImGui::TextDisabled("no project open");
                            }
                            else
                            {
                                // Type-to-filter (one popup is open at a time,
                                // so a function-local buffer serves them all).
                                static char s_pickSearch[64] = {};
                                if (ImGui::IsWindowAppearing())
                                {
                                    s_pickSearch[0] = '\0';
                                    ImGui::SetKeyboardFocusHere();
                                }
                                ImGui::InputTextWithHint("##assetsearch", "Search...",
                                                         s_pickSearch, sizeof(s_pickSearch));
                                ImGui::Separator();
                                if (ImGui::Selectable("(none)"))
                                    ApplyGuidImmediate(label, f, instance, Arcane::Guid::Nil());
                                for (const Arcane::Editor::AssetEntry& e :
                                     Arcane::Editor::BuildAssetEntries(project->Registry()))
                                {
                                    if (!Arcane::Editor::MatchesFilter(e, kindFilter, s_pickSearch))
                                        continue;
                                    if (ImGui::Selectable((e.name + "##" + e.mountPath).c_str(), e.guid == v))
                                        ApplyGuidImmediate(label, f, instance, e.guid);
                                }
                            }
                            ImGui::EndPopup();
                        }
                        break;
                    }
                    case Arcane::Editor::FieldKind::ReadOnly:
                    default:
                        ImGui::BeginDisabled();
                        ImGui::Text("%s (unsupported)", label.c_str());
                        ImGui::EndDisabled();
                        break;
                }
                EndGesture();
                ImGui::PopID();
            }
        };
    }

    void DrawInspectorPanel(Astra::Registry& registry, const SelectionContext& sel,
                            Arcane::CommandStack& undo, bool editMode,
                            const Arcane::Project* project)
    {
        ImGui::Begin("Inspector");
        if (!sel.HasSelection())
        {
            ImGui::TextDisabled("No selection");
            ImGui::End();
            return;
        }
        for (const Astra::Registry::ComponentInfo& ci : registry.InspectEntity(sel.Primary()))
        {
            if (!ci.descriptor || !ci.descriptor->visitFields || !ci.data) continue;
            // TypeMeta::typeName is a std::string_view into a substring of a larger
            // compile-time literal (__FUNCSIG__/__PRETTY_FUNCTION__) -- NOT
            // guaranteed NUL-terminated, so it is copied into a std::string before
            // handing a `const char*` to ImGui.
            const std::string typeName = ci.meta ? std::string(ci.meta->typeName) : std::string("<unreflected>");
            // Derived transform state is never authored: WorldTransform is
            // recomputed by TransformPropagationSystem every frame and
            // PreviousTransform is the physics-capture interpolation pose.
            // Editing either would be stomped next frame -- the Inspector shows
            // only the authored Transform (the UE/Unity convention: relative/
            // local is the editable truth, world is display-only derived data).
            if (typeName == "Arcane::WorldTransform" || typeName == "Arcane::PreviousTransform")
                continue;
            if (ImGui::CollapsingHeader(typeName.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGuiFieldVisitor visitor;
                // Null while Play is running: BeginGestureIfActivated/EndGesture both
                // early-return on a null stack, so gesture bracketing is fully inert
                // (no Begin, no Commit/Cancel) against the live simulating registry.
                visitor.stack      = editMode ? &undo : nullptr;
                visitor.entity     = sel.Primary();
                visitor.descriptor = ci.descriptor;
                visitor.typeName   = typeName;
                visitor.project    = project;
                ci.descriptor->visitFields(ci.data, visitor);
            }
        }
        ImGui::End();
    }
}
