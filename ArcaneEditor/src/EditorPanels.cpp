#include "EditorPanels.hpp"
#include "AssetBrowser.hpp"
#include "ComponentCatalog.hpp"
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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <vector>

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
                r.ctrlHeld = ImGui::GetIO().KeyCtrl;
                r.clickLocalX = lx; r.clickLocalY = ly;
            }
        }
        ImGui::End();
        return r;
    }

    namespace
    {
        // Payload type tag for outliner entity drag/reparent; shared by the
        // source/target sites below so the string only ever appears once.
        constexpr const char* kOutlinerDragType = "ARC_OUTLINER_ENTITY";

        // Whether a structural (whole-registry memento) edit can run RIGHT NOW.
        // Two independent refusals, both of which used to be invisible in the UI:
        //   editMode      -- false during Play (a play-time structural edit would
        //                    be undone against the restored registry).
        //   InTransaction -- ApplyRegistryMutation refuses inside an open gesture
        //                    (Cancel discards pending commands WITHOUT reverting,
        //                    so a memento pushed mid-gesture could strand an
        //                    already-applied edit with no undo coverage).
        // The second one only ARC_WARN'd, which presents at the desk as "the
        // right-click menu stopped working". Every structural affordance disables
        // on this predicate so the refusal is visible before the click.
        [[nodiscard]] bool CanEditStructure(const Arcane::CommandStack& undo,
                                            const SceneEditBinding& b) noexcept
        {
            return b.editMode && !undo.InTransaction();
        }

        bool ApplyStructural(Arcane::CommandStack& undo, const SceneEditBinding& b,
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
                             Arcane::CommandStack& undo, const SceneEditBinding& binding)
        {
            const std::vector<Astra::Entity> doomed = sel.Entities();   // copy: sel mutates after
            if (doomed.empty())
                return;
            if (ApplyStructural(undo, binding, "Delete",
                    [&] { return Arcane::Edit::DeleteEntities(registry, doomed) > 0; }))
                sel.Clear();
        }

        // Popup id shared by the Inspector's "+ Add Component" button and the
        // Outliner row menu's "Add Component...".
        //
        // NOTE the ids at those two sites are NOT equal -- ImGui seeds a popup id
        // with the CURRENT WINDOW, so this literal resolves to a different id in
        // the Inspector than in the Outliner. (The previous comment here claimed
        // the opposite. Corrected 2026-07-26.) That is harmless: each panel opens
        // and draws its own popup at its own scope, and each is self-consistent.
        constexpr const char* kAddComponentPopup = "##addcomponent";

        // The searchable Add Component popup: draws the catalog, applies the
        // pick as ONE undo step over the whole selection. The caller opens it
        // with ImGui::OpenPopup(kAddComponentPopup) and then calls this every
        // frame at the same id-stack level.
        //
        // The function-local search buffer is justified by ONE POPUP BEING OPEN AT
        // A TIME (not by the ids being equal -- see above), same as the asset-ref
        // pick popup below.
        void DrawAddComponentPopup(Astra::Registry& registry,
                                   const std::vector<Astra::Entity>& selection,
                                   Arcane::CommandStack& undo,
                                   const SceneEditBinding& binding)
        {
            static char s_search[64] = {};
            const Astra::ComponentDescriptor* chosen = nullptr;
            // Evaluated ONCE for the whole popup: a popup left open when Play
            // starts used to keep its rows fully interactive and then silently
            // no-op in ApplyStructural.
            const bool canEdit = CanEditStructure(undo, binding);

            if (ImGui::BeginPopup(kAddComponentPopup))
            {
                if (ImGui::IsWindowAppearing())
                {
                    s_search[0] = '\0';
                    ImGui::SetKeyboardFocusHere();
                }
                ImGui::SetNextItemWidth(260.0f);
                ImGui::InputTextWithHint("##compsearch", "Search...",
                                         s_search, sizeof(s_search));
                ImGui::Separator();

                const std::vector<ComponentCatalogEntry> entries =
                    BuildComponentCatalog(registry, selection, s_search);
                if (entries.empty())
                {
                    ImGui::TextDisabled("no matching components");
                }
                else
                {
                    ImGui::BeginChild("##complist", ImVec2(260.0f, 260.0f));
                    for (const ComponentCatalogEntry& e : entries)
                    {
                        // missingCount == 0 means every selected entity already
                        // carries it, so the add would be a no-op. Shown
                        // disabled rather than hidden: "you already have this"
                        // reads better than a row that silently vanishes.
                        const bool addable = e.missingCount > 0 && canEdit;
                        if (!addable)
                            ImGui::BeginDisabled();
                        if (ImGui::Selectable(e.typeName.c_str()) && addable)
                            chosen = e.desc;
                        if (!addable)
                            ImGui::EndDisabled();
                    }
                    ImGui::EndChild();
                }

                if (chosen)
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }

            // Applied OUTSIDE the popup scope: the mutation invalidates the
            // catalog vector the loop above is still holding.
            if (chosen)
            {
                const Astra::ComponentDescriptor& desc = *chosen;
                ApplyStructural(undo, binding, "Add Component",
                    [&] { return Arcane::Edit::AddComponent(registry, selection, desc) > 0; });
            }
        }
    }

    void DrawOutlinerPanel(Astra::Registry& registry, SelectionContext& sel,
                           Arcane::CommandStack& undo, const SceneEditBinding& binding,
                           OutlinerState& state)
    {
        ImGui::Begin("Outliner");

        // Hoisted once per frame: every structural affordance in this panel keys
        // off it, and BeginDisabled/EndDisabled pairs must agree.
        const bool canEditStructure = CanEditStructure(undo, binding);

        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##outliner_search", ICON_LC_SEARCH " Filter",
                                 state.search, sizeof(state.search));

        const std::vector<OutlinerRow> rows =
            BuildOutlinerRows(registry, state.search, state.sort, state.collapsed);

        // The rename target can stop being drawable two ways: a structural
        // undo/redo destroys it, or it simply leaves the visible set (its
        // parent collapsed, or the filter excludes it). Either way no row
        // draws the InputText, so IsItemDeactivated never fires -- drop the
        // target or `renaming` wedges shut, taking F2 and Delete with it.
        if (state.renameTarget.IsValid())
        {
            bool hasRow = false;
            for (const OutlinerRow& r : rows)
                if (r.entity == state.renameTarget)
                {
                    hasRow = true;
                    break;
                }
            if (!hasRow)
                state.renameTarget = Astra::Entity::Invalid();
        }

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
        if (ImGui::BeginTable("##outliner_rows", 2, tflags, ImVec2(0.0f, -footerH)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(ICON_LC_EYE,
                ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed,
                ImGui::GetFrameHeight());
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_DefaultSort);
            ImGui::TableHeadersRow();

            // Header sort -> state.sort; rows were built with LAST frame's
            // sort (one-frame lag, rebuilt every frame anyway). Label is the
            // only sortable column, so any spec means sort-by-label.
            if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs())
            {
                state.sort = OutlinerSort{};
                if (specs->SpecsCount > 0)
                {
                    state.sort.column = OutlinerSort::Column::Label;
                    state.sort.ascending =
                        specs->Specs[0].SortDirection != ImGuiSortDirection_Descending;
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
                        {
                            // An anchor with no visible row (filtered out, or
                            // under a collapsed parent) yields an empty range;
                            // degrade to a plain select rather than moving the
                            // primary outside the selection.
                            const std::vector<Astra::Entity> range =
                                RowRange(rows, sel.Primary(), row.entity);
                            if (range.empty())
                                sel.Select(row.entity);
                            else
                                sel.AddRange(range, row.entity);
                        }
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

                    // Right-click selects (if outside the selection) then menus.
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)
                        && !sel.Contains(row.entity))
                        sel.Select(row.entity);
                    if (ImGui::BeginPopupContextItem("##row_ctx"))
                    {
                        // CanEditStructure, not just editMode: a memento refuses
                        // inside an open gesture too, and that refusal used to be an
                        // ARC_WARN only -- i.e. invisible, reading as "the menu
                        // stopped working".
                        if (!canEditStructure)
                            ImGui::BeginDisabled();
                        if (ImGui::MenuItem("New Child Entity"))
                        {
                            Astra::Entity created = Astra::Entity::Invalid();
                            const Astra::Entity parent = row.entity;
                            if (ApplyStructural(undo, binding, "Create Entity",
                                    [&] { created = Arcane::Edit::CreateEntity(registry, parent);
                                          return created.IsValid(); }))
                            {
                                state.collapsed.erase(
                                    static_cast<std::uint64_t>(parent.GetValue()));
                                sel.Select(created);
                            }
                        }
                        if (ImGui::MenuItem("Rename", "F2"))
                            BeginRename(state, row.entity, row.label);
                        // ImGui cannot open a popup from inside another popup's
                        // scope, so the request is latched and consumed at panel
                        // scope below (the standard deferred-OpenPopup pattern).
                        if (ImGui::MenuItem("Add Component..."))
                            state.addComponentPending = true;
                        if (ImGui::MenuItem("Delete", "Del"))
                        {
                            if (!sel.Contains(row.entity))
                                sel.Select(row.entity);
                            DeleteSelection(registry, sel, undo, binding);
                        }
                        if (!canEditStructure)
                            ImGui::EndDisabled();
                        ImGui::EndPopup();
                    }

                    // Reparent-by-drag is structural too, so it honours the same
                    // predicate rather than starting a drag that will refuse.
                    if (canEditStructure && ImGui::BeginDragDropSource())
                    {
                        ImGui::SetDragDropPayload(kOutlinerDragType,
                                                  &row.entity, sizeof(Astra::Entity));
                        ImGui::TextUnformatted(row.label.c_str());
                        ImGui::EndDragDropSource();
                    }
                    if (canEditStructure && ImGui::BeginDragDropTarget())
                    {
                        if (const ImGuiPayload* p =
                                ImGui::AcceptDragDropPayload(kOutlinerDragType))
                        {
                            Astra::Entity dragged;
                            std::memcpy(&dragged, p->Data, sizeof(dragged));
                            const std::vector<Astra::Entity> moving =
                                sel.Contains(dragged) ? sel.Entities()
                                                      : std::vector<Astra::Entity>{ dragged };
                            const Astra::Entity target = row.entity;
                            ApplyStructural(undo, binding, "Reparent",
                                [&] { return Arcane::Edit::Reparent(registry, moving, target) > 0; });
                        }
                        ImGui::EndDragDropTarget();
                    }

                    if (indent > 0.0f)
                        ImGui::Unindent(indent);
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        // Drop below the table = unparent to root. Only visible mid-drag, and
        // only for our own entity payload -- GetDragDropPayload() returns
        // non-null for ANY active drag (e.g. an asset-browser drag), which
        // used to show this strip for foreign payloads too.
        const ImGuiPayload* activeDrag = ImGui::GetDragDropPayload();
        if (canEditStructure && activeDrag != nullptr
            && activeDrag->IsDataType(kOutlinerDragType))
        {
            ImGui::Selectable("(drop here to unparent)", false,
                              ImGuiSelectableFlags_Disabled);
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* p =
                        ImGui::AcceptDragDropPayload(kOutlinerDragType))
                {
                    Astra::Entity dragged;
                    std::memcpy(&dragged, p->Data, sizeof(dragged));
                    const std::vector<Astra::Entity> moving =
                        sel.Contains(dragged) ? sel.Entities()
                                              : std::vector<Astra::Entity>{ dragged };
                    ApplyStructural(undo, binding, "Unparent",
                        [&] { return Arcane::Edit::Reparent(registry, moving,
                                                            Astra::Entity::Invalid()) > 0; });
                }
                ImGui::EndDragDropTarget();
            }
        }

        // BeginPopupContextWindow cannot serve this: the ScrollY rows table
        // opens a child window that covers the panel, and the window-hover
        // test behind that helper demands an EXACT window match against the
        // outer window. Detect the hover across the child hierarchy and open
        // the popup by hand.
        if (canEditStructure
            && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)
            && !ImGui::IsAnyItemHovered()
            && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
            ImGui::OpenPopup("##outliner_ctx");
        if (ImGui::BeginPopup("##outliner_ctx"))
        {
            if (ImGui::MenuItem("New Entity"))
            {
                Astra::Entity created = Astra::Entity::Invalid();
                if (ApplyStructural(undo, binding, "Create Entity",
                        [&] { created = Arcane::Edit::CreateEntity(registry,
                                            Astra::Entity::Invalid());
                              return created.IsValid(); }))
                    sel.Select(created);
            }
            ImGui::EndPopup();
        }

        // Latched by the row menu one step earlier -- see the comment there.
        // The right-clicked row is already in the selection (the row's
        // right-click handler selects it when it was outside), so the popup
        // operates on exactly what the user aimed at.
        if (state.addComponentPending)
        {
            state.addComponentPending = false;
            ImGui::OpenPopup(kAddComponentPopup);
        }
        DrawAddComponentPopup(registry, sel.Entities(), undo, binding);

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
            // The in-flight gesture's CommandStack ownership token, owned by the
            // panel's persistent InspectorState -- the visitor is rebuilt every
            // frame but the gesture it brackets is not. Never null when `stack` is
            // non-null.
            Arcane::TransactionId*            gestureTxn = nullptr;

            // Fan-out targets. `selection` includes the primary; entities lacking
            // this component are skipped (the panel only shows components the whole
            // selection shares, but selection and panel are a frame apart).
            Astra::Registry*                  registry = nullptr;
            const std::vector<Astra::Entity>* selection = nullptr;

            bool IsWriting() const noexcept override { return true; }

            // Run `fn(instanceOfThatEntity)` for every selected entity carrying
            // this component. Falls back to the primary's own instance when the
            // fan-out context is absent, so a field is never silently un-editable.
            template<typename Fn>
            void ForEachTarget(void* primaryInstance, Fn&& fn)
            {
                if (!registry || !selection || selection->empty())
                {
                    fn(entity, primaryInstance);
                    return;
                }
                for (Astra::Entity e : *selection)
                    if (void* data = registry->GetComponentByHash(e, descriptor->hash))
                        fn(e, data);
            }

            void BeginGestureIfActivated(const std::string& field, void* primaryInstance)
            {
                if (stack && ImGui::IsItemActivated())
                {
                    // Clicking field B while field A is mid-gesture moves ActiveId in
                    // ONE frame, and if B is drawn ABOVE A then B activates before A's
                    // EndGesture runs. Without this, B's Begin would see A's
                    // transaction still open, return None, and A's later Commit(None)
                    // would no-op -- leaving A's transaction open forever holding
                    // stale `before` bytes. Close A's here so its edit lands, then
                    // open ours; A's own EndGesture then passes a stale token, which
                    // the stack correctly ignores.
                    if (*gestureTxn != Arcane::TransactionId::None)
                        stack->Commit(*gestureTxn);
                    // Park the token for the deactivation frame (EndGesture). None
                    // means a gizmo drag already owns the stack -- this gesture then
                    // JOINS it: the snapshots below still land, and the drag's own
                    // Commit records them, so the edit is never lost.
                    *gestureTxn = stack->Begin("Edit " + typeName + "." + field);
                    // One Begin + N snapshots + one Commit = one undo step for the
                    // whole fan-out (CommandStack dedupes per (entity, descriptor)).
                    ForEachTarget(primaryInstance,
                                  [&](Astra::Entity e, void*) { stack->SnapshotComponent(e, descriptor); });
                }
            }

            // UE parity: a multi-selection gets NO drag widget and ignores every
            // non-committed change. Both belts are Unreal's, in
            // ComponentTransformDetails.cpp -- `.AllowSpin(SelectedObjects.Num()
            // == 1)` (:505/:551/:628) and, at :1248, "Ignore interactive changes
            // when we have more than one selected object". A single selection
            // keeps the drag exactly as before.
            [[nodiscard]] bool Multi() const noexcept
            { return selection && selection->size() > 1; }

            [[nodiscard]] Arcane::Editor::FieldMixedMask MixedFor(const Astra::FieldInfo& f) const
            {
                if (!registry || !selection || !descriptor)
                    return {};
                return Arcane::Editor::ComputeFieldMixed(*registry, *selection,
                                                         descriptor->hash, f);
            }

            // Single-shot fan-out for the multi-select path: the commit is one
            // discrete event (no widget gesture spanning frames to bracket), so
            // Begin + snapshot-all + apply + Commit happen in one call.
            //
            // ScopedTransaction, NOT a bare Begin/Commit pair: this can fire in the
            // same frame as a gizmo press (clicking a handle deactivates a text box
            // that still holds typed text), and an unconditional Commit here used to
            // close the DRAG's transaction -- the rest of the drag then ran against a
            // closed stack and lost its entire undo record. The scope commits only
            // what it opened; when it joins a live drag instead, the snapshots ride
            // along and the drag's own Commit records them.
            template<typename Fn>
            void ApplyImmediate(const std::string& field, void* primaryInstance, Fn&& apply)
            {
                std::optional<Arcane::ScopedTransaction> txn;
                if (stack)
                {
                    txn.emplace(*stack, "Edit " + typeName + "." + field);
                    ForEachTarget(primaryInstance,
                                  [&](Astra::Entity e, void*) { txn->Snapshot(e, descriptor); });
                }
                ForEachTarget(primaryInstance, [&](Astra::Entity, void* d) { apply(d); });
            }

            // One multi-select scalar row: `count` TEXT-ENTRY boxes (never a
            // drag), each rendered BLANK when that component differs across the
            // selection -- Unreal's "unset means multiple differing values"
            // (ComponentTransformDetails.cpp:1026). Returns the index of the
            // component the user COMMITTED this frame, or -1 for none; typing
            // alone writes nothing. Laid out like ImGui's own InputScalarN.
            //
            // DOUBLE, not float, and an `integral` flag: an int32 field used to
            // round-trip int32 -> float -> "%.3f" -> strtof -> int, which both
            // showed "7.000" in an integer box and TRUNCATED silently past 2^24
            // where float can no longer represent consecutive integers. double
            // represents every int32 and every float exactly, so this row is now
            // lossless for both kinds and `integral` picks the formatting.
            int MultiScalarRow(const std::string& label, int count, const double* vals,
                               const Arcane::Editor::FieldMixedMask& mask, bool integral,
                               double& outValue)
            {
                int committed = -1;
                ImGui::BeginGroup();
                ImGui::PushID(label.c_str());
                ImGui::PushMultiItemsWidths(count, ImGui::CalcItemWidth());
                for (int i = 0; i < count; ++i)
                {
                    ImGui::PushID(i);
                    if (i > 0)
                        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);

                    char buf[64];
                    if (mask.Test(i))
                        buf[0] = '\0';                       // differs: show nothing
                    else if (integral)
                        std::snprintf(buf, sizeof(buf), "%lld",
                                      static_cast<long long>(vals[i]));
                    else
                        std::snprintf(buf, sizeof(buf), "%.3f", vals[i]);

                    // Scientific notation is offered only for real numbers -- "1e3"
                    // in an integer box is not something to encourage.
                    const bool entered = ImGui::InputText(
                        "", buf, sizeof(buf),
                        ImGuiInputTextFlags_CharsDecimal |
                        (integral ? 0 : ImGuiInputTextFlags_CharsScientific) |
                        ImGuiInputTextFlags_EnterReturnsTrue);
                    // Enter, or focus lost after an edit: the two ways ImGui says
                    // "the user is done with this box".
                    if ((entered || ImGui::IsItemDeactivatedAfterEdit()) && buf[0] != '\0')
                    {
                        char* end = nullptr;
                        const double parsed = std::strtod(buf, &end);
                        if (end != buf)
                        {
                            outValue = parsed;
                            committed = i;
                        }
                    }
                    ImGui::PopID();
                    ImGui::PopItemWidth();
                }
                ImGui::PopID();
                if (!label.empty())
                {
                    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
                    ImGui::TextUnformatted(label.c_str());
                }
                ImGui::EndGroup();
                return committed;
            }

            void EndGesture()
            {
                if (!stack) return;
                // Both no-op on TransactionId::None, so a field that never opened a
                // gesture -- and a gesture that JOINED a gizmo drag -- leaves the
                // stack strictly alone here.
                if (ImGui::IsItemDeactivatedAfterEdit()) stack->Commit(*gestureTxn);
                else if (ImGui::IsItemDeactivated())     stack->Cancel(*gestureTxn);
                else return;
                *gestureTxn = Arcane::TransactionId::None;   // spent
            }

            // Single-shot edit (asset drop / popup pick / clear): no widget gesture
            // to bracket, so the whole transaction happens in one call. Scoped for
            // the same ownership reason as ApplyImmediate above.
            void ApplyGuidImmediate(const std::string& field, const Astra::FieldInfo& f,
                                    void* instance, const Arcane::Guid& v)
            {
                std::optional<Arcane::ScopedTransaction> txn;
                if (stack)
                {
                    txn.emplace(*stack, "Edit " + typeName + "." + field);
                    ForEachTarget(instance,
                                  [&](Astra::Entity e, void*) { txn->Snapshot(e, descriptor); });
                }
                ForEachTarget(instance, [&](Astra::Entity, void* d)
                              { Arcane::Editor::ApplyGuidEdit(f, d, v); });
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
                        // ImGui's native tri-state. ImGuiItemFlags_MixedValue is
                        // Checkbox-ONLY in our vendored ImGui (imgui_internal.h:984),
                        // which is why the numeric kinds below blank by hand.
                        // Clicking a mixed checkbox resolves the whole selection to
                        // one value -- ImGui's documented behaviour, and UE's.
                        const bool boolMixed = Multi() && MixedFor(f).Any();
                        if (boolMixed)
                            ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
                        bool changed = ImGui::Checkbox(label.c_str(), &v);
                        if (boolMixed)
                            ImGui::PopItemFlag();
                        BeginGestureIfActivated(label, instance);
                        if (changed)
                            ForEachTarget(instance, [&](Astra::Entity, void* d)
                                          { Arcane::Editor::ApplyBoolEdit(f, d, v); });
                        break;
                    }
                    case Arcane::Editor::FieldKind::Int32:
                    {
                        int v = f.Get<int32_t>(instance);
                        if (Multi())
                        {
                            const double cur = static_cast<double>(v);
                            double out = cur;
                            if (MultiScalarRow(label, 1, &cur, MixedFor(f), /*integral*/ true, out) >= 0)
                            {
                                // Clamp before the narrowing cast: converting an
                                // out-of-range double to int32 is UB, and the box
                                // accepts arbitrary digits.
                                const double lo = static_cast<double>(std::numeric_limits<int32_t>::min());
                                const double hi = static_cast<double>(std::numeric_limits<int32_t>::max());
                                const int32_t iv = static_cast<int32_t>(std::clamp(out, lo, hi));
                                ApplyImmediate(label, instance, [&](void* d)
                                               { Arcane::Editor::ApplyIntEdit(f, d, iv); });
                            }
                            break;
                        }
                        bool changed = ImGui::DragInt(label.c_str(), &v);
                        BeginGestureIfActivated(label, instance);
                        if (changed)
                            ForEachTarget(instance, [&](Astra::Entity, void* d)
                                          { Arcane::Editor::ApplyIntEdit(f, d, v); });
                        break;
                    }
                    case Arcane::Editor::FieldKind::Float:
                    {
                        float v = f.Get<float>(instance);
                        if (Multi())
                        {
                            const double cur = static_cast<double>(v);
                            double out = cur;
                            if (MultiScalarRow(label, 1, &cur, MixedFor(f), /*integral*/ false, out) >= 0)
                            {
                                const float fv = static_cast<float>(out);
                                ApplyImmediate(label, instance, [&](void* d)
                                               { Arcane::Editor::ApplyFloatEdit(f, d, fv); });
                            }
                            break;
                        }
                        bool changed = ImGui::DragFloat(label.c_str(), &v, 0.1f);
                        BeginGestureIfActivated(label, instance);
                        if (changed)
                            ForEachTarget(instance, [&](Astra::Entity, void* d)
                                          { Arcane::Editor::ApplyFloatEdit(f, d, v); });
                        break;
                    }
                    case Arcane::Editor::FieldKind::Vec2:
                    {
                        glm::vec2 v = f.Get<glm::vec2>(instance);
                        if (Multi())
                        {
                            const double cur[2]{ v.x, v.y };
                            double out = 0.0;
                            // Only the COMMITTED component is written, so typing
                            // into one blank axis leaves the others alone on every
                            // target -- the point of per-axis mixed values.
                            const int c = MultiScalarRow(label, 2, cur, MixedFor(f), /*integral*/ false, out);
                            if (c >= 0)
                            {
                                const float fv = static_cast<float>(out);
                                ApplyImmediate(label, instance, [&](void* d)
                                               { if (glm::vec2* p = f.GetPtr<glm::vec2>(d)) (*p)[c] = fv; });
                            }
                            break;
                        }
                        bool changed = ImGui::DragFloat2(label.c_str(), &v.x, 0.1f);
                        BeginGestureIfActivated(label, instance);
                        if (changed)
                            ForEachTarget(instance, [&](Astra::Entity, void* d)
                                          { if (glm::vec2* p = f.GetPtr<glm::vec2>(d)) *p = v; });
                        break;
                    }
                    case Arcane::Editor::FieldKind::Vec3:
                    {
                        glm::vec3 v = f.Get<glm::vec3>(instance);
                        if (Multi())
                        {
                            const double cur[3]{ v.x, v.y, v.z };
                            double out = 0.0;
                            const int c = MultiScalarRow(label, 3, cur, MixedFor(f), /*integral*/ false, out);
                            if (c >= 0)
                            {
                                const float fv = static_cast<float>(out);
                                ApplyImmediate(label, instance, [&](void* d)
                                               { if (glm::vec3* p = f.GetPtr<glm::vec3>(d)) (*p)[c] = fv; });
                            }
                            break;
                        }
                        bool changed = ImGui::DragFloat3(label.c_str(), &v.x, 0.1f);
                        BeginGestureIfActivated(label, instance);
                        if (changed)
                            ForEachTarget(instance, [&](Astra::Entity, void* d)
                                          { if (glm::vec3* p = f.GetPtr<glm::vec3>(d)) *p = v; });
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

                        // Mixed asset refs render BLANK, same rule as the numeric
                        // kinds. This was the one kind the "mixed shows blank" work
                        // missed: ComputeFieldMixed already handles AssetRef (width
                        // 1), but the result was never consulted here, so a
                        // multi-selection showed the PRIMARY's asset as if the whole
                        // selection shared it.
                        const bool refMixed = Multi() && MixedFor(f).Any();

                        std::string display = "(none)";
                        if (refMixed)
                        {
                            display = "--";   // differing values across the selection
                        }
                        else if (v.IsValid())
                        {
                            display = v.ToString();
                            if (project)
                                if (const auto mount = project->Registry().Resolve(v))
                                    display = *mount;
                        }

                        // "###", not "##": only "###" resets the id hash
                        // (ImHashStr, imgui.cpp:2557), so with "##" the button's id
                        // was seeded by `display` -- a MUTABLE string that changes
                        // the moment an asset is picked. The id then changed under
                        // ImGui mid-interaction, dropping the item's state.
                        if (ImGui::Button((display + "###assetref").c_str()))
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
                        // Offered when MIXED too: "clear all of them" is a
                        // meaningful action even though the primary may be nil.
                        if (v.IsValid() || refMixed)
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
                            Arcane::CommandStack& undo, const SceneEditBinding& binding,
                            const Arcane::Project* project, InspectorState& state)
    {
        ImGui::Begin("Inspector");
        if (!sel.HasSelection())
        {
            ImGui::TextDisabled("No selection");
            ImGui::End();
            return;
        }

        const Astra::Entity primary = sel.Primary();
        const std::string primaryName = Arcane::Edit::DisplayName(registry, primary);
        if (sel.Count() > 1)
            ImGui::Text("%s (+%zu)", primaryName.c_str(), sel.Count() - 1);
        else
            ImGui::TextUnformatted(primaryName.c_str());
        ImGui::Separator();

        // Hoisted once per frame; see the Outliner's copy. Add/Remove Component
        // are structural (whole-registry memento), so they refuse during Play AND
        // inside an open field/gizmo gesture -- both now visible as disabled.
        const bool canEditStructure = CanEditStructure(undo, binding);

        // Removal is DEFERRED past the loop: Edit::RemoveComponent moves the
        // entity to a different archetype, which dangles every ci.data pointer
        // in the vector being iterated. Descriptor pointers themselves are
        // stable (they live in ComponentRegistry's fixed array).
        const Astra::ComponentDescriptor* pendingRemove = nullptr;

        for (const Astra::Registry::ComponentInfo& ci : registry.InspectEntity(primary))
        {
            // An unreflected component has no name to show and no fields to
            // visit: visitFields is populated FROM TypeMeta at registration, so
            // meta != null implies visitFields != null.
            if (!ci.descriptor || !ci.meta)
                continue;
            // TypeMeta::typeName is a std::string_view into a substring of a larger
            // compile-time literal (__FUNCSIG__/__PRETTY_FUNCTION__) -- NOT
            // guaranteed NUL-terminated, so it is copied into a std::string before
            // handing a `const char*` to ImGui.
            const std::string typeName(ci.meta->typeName);
            // Derived/runtime-owned state is never authored. ONE hide-list, in
            // ComponentCatalog.hpp: the same predicate gates these sections, the
            // Add Component catalog, and Remove Component, so the three cannot
            // drift apart.
            if (IsSystemManagedComponent(typeName))
                continue;

            // Component-type INTERSECTION: editing a component only some of the
            // selection carries would silently edit a subset, so hide it entirely.
            // HasComponentByHash, NOT GetComponentByHash: an empty (tag) component
            // has no storage array, so the getter returns null even when present,
            // which used to make every tag component look unshared.
            bool sharedByAll = true;
            for (Astra::Entity e : sel.Entities())
            {
                if (e != primary && !registry.HasComponentByHash(e, ci.descriptor->hash))
                {
                    sharedByAll = false;
                    break;
                }
            }
            if (!sharedByAll)
                continue;

            // PER-COMPONENT ID SCOPE. Without it every id inside this section is
            // seeded only by the BARE field name (FieldInfo::nameHash), so two
            // components on one entity that share a field name collide --
            // SpriteRenderer::material and PostProcess::material resolved to ONE
            // "##assetpick" popup id, both BeginPopup calls returned true in the
            // same frame, and a pick landed on the wrong field. Scoped for the
            // header too so the context menu (which inherits the header's item id)
            // is distinct even for identically-named component types.
            //
            // Nothing between this push and its pop may `continue`: the branches
            // below are deliberately nested rather than early-outs so the ID stack
            // stays balanced on every path.
            ImGui::PushID(static_cast<int>(ci.descriptor->hash));
            const bool open = ImGui::CollapsingHeader(typeName.c_str(),
                                                      ImGuiTreeNodeFlags_DefaultOpen);
            // Header context menu. A null str_id makes the popup inherit the
            // HEADER's item id, so each component gets its own popup -- a shared
            // literal id would make every header open the same popup and the
            // first-drawn component would swallow the click.
            if (ImGui::BeginPopupContextItem())
            {
                if (!canEditStructure)
                    ImGui::BeginDisabled();
                if (ImGui::MenuItem("Remove Component"))
                    pendingRemove = ci.descriptor;
                if (!canEditStructure)
                    ImGui::EndDisabled();
                ImGui::EndPopup();
            }
            if (open)
            {
                // Tag components (Astra's is_empty optimization) have no storage
                // array, so ci.data is null even though the entity carries them.
                // They still get a header: that is what makes them visible at all,
                // and what makes them removable through the menu above.
                if (!ci.data)
                {
                    ImGui::TextDisabled("(tag component -- no fields)");
                }
                else
                {
                    ImGuiFieldVisitor visitor;
                    // Null while Play is running: BeginGestureIfActivated/EndGesture
                    // both early-return on a null stack, so gesture bracketing is
                    // fully inert (no Begin, no Commit/Cancel) against the live
                    // simulating registry.
                    visitor.stack      = binding.editMode ? &undo : nullptr;
                    visitor.entity     = primary;
                    visitor.descriptor = ci.descriptor;
                    visitor.typeName   = typeName;
                    visitor.project    = project;
                    visitor.gestureTxn = &state.gestureTxn;
                    visitor.registry   = &registry;
                    visitor.selection  = &sel.Entities();
                    ci.descriptor->visitFields(ci.data, visitor);
                }
            }
            ImGui::PopID();
        }

        if (pendingRemove)
        {
            // Copy the selection: ApplyStructural's mutate runs immediately and
            // the span must outlive it.
            const std::vector<Astra::Entity> targets = sel.Entities();
            const Astra::ComponentDescriptor& desc = *pendingRemove;
            ApplyStructural(undo, binding, "Remove Component",
                [&] { return Arcane::Edit::RemoveComponent(registry, targets, desc) > 0; });
        }

        // Bottom of the panel, full width -- the UE/Unity placement.
        ImGui::Separator();
        if (!canEditStructure)
            ImGui::BeginDisabled();
        if (ImGui::Button(ICON_LC_PLUS " Add Component", ImVec2(-FLT_MIN, 0.0f)))
            ImGui::OpenPopup(kAddComponentPopup);
        if (!canEditStructure)
            ImGui::EndDisabled();
        // Drawn unconditionally at window scope: BeginPopup is a no-op until
        // the button above (or a previous frame's click) opened it.
        DrawAddComponentPopup(registry, sel.Entities(), undo, binding);

        ImGui::End();
    }
}
