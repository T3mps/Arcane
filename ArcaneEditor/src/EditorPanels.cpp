#include "EditorPanels.hpp"
#include "ComponentCatalog.hpp"
#include "ConsoleBuffer.hpp"
#include "DiagnosticStore.hpp"   // MatchesDiagnosticFilter, reused for the console's own text search
#include "EditorFonts.hpp"
#include "EditorWidgets.hpp"
#include "EntityList.hpp"
#include "IconsLucide.h"
#include "InspectorFields.hpp"
#include "InspectorMeta.hpp"
#include "InspectorView.hpp"
#include "PlayMode.hpp"
#include "SelectionContext.hpp"

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Edit/EntityOps.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Scene/Components.hpp>   // Arcane::Identity (the rename target)
#include <Arcane/Sim/RunLoop.hpp>

#include <Astra/Registry/Registry.hpp>

#include <imgui.h>
#include <imgui_internal.h>   // DockBuilder* (:3765) + ImGuiDockNode::LocalFlags
                              // (:2037) -- the docking layout, none of it public.
                              // The grid's table internals left with the widget
                              // layer; the Outliner's own table below is all
                              // public imgui.h API.

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Arcane::Editor
{
    void BeginDockSpace(Arcane::CommandStack& undo, MenuRequests& requests,
                        bool sceneDirty, bool playing,
                        bool buildingModule, bool hasGameModule,
                        const RecentSelection* recents)
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

        // Editor menu bar. File's project/scene/material items all land in `requests`;
        // File -> Exit and Edit's Cut/Copy/Paste are still placeholders. Edit's
        // Undo/Redo drive the CommandStack; Preferences is a leaf item that will open a
        // settings window later.
        if (ImGui::BeginMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                requests.fileMenuOpen = true;
                if (ImGui::MenuItem("Open Project...")) requests.openProject = true;

                // Open Recent: the Hub's shared list, already filtered to what
                // THIS editor's ABI can open (see RecentProjects.hpp). Greyed
                // when there is nothing at all to show, rather than opening onto
                // an empty submenu.
                const bool anyRecents =
                    recents && (!recents->visible.empty() || recents->hiddenForAbi > 0);
                if (ImGui::BeginMenu("Open Recent", anyRecents))
                {
                    for (const RecentProject& r : recents->visible)
                    {
                        if (ImGui::MenuItem(r.name.c_str()))
                            requests.openRecentPath = r.path;
                        // Full path on hover. The name alone cannot separate two
                        // projects that share a folder name, which is exactly why
                        // Unreal puts the absolute path in this tooltip
                        // (FRecentProjectsMenu::MakeMenu).
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", r.path.c_str());
                    }

                    // Never leave the list silently short: the Hub shows every
                    // project across every engine version, so without this line
                    // an ABI-filtered absence is indistinguishable from a broken
                    // recents list. Disabled -- it is a statement, not an action.
                    if (recents->hiddenForAbi > 0)
                    {
                        if (!recents->visible.empty())
                            ImGui::Separator();
                        char hidden[128];
                        std::snprintf(hidden, sizeof(hidden),
                                      "%zu project%s hidden (built for another engine version)",
                                      recents->hiddenForAbi,
                                      recents->hiddenForAbi == 1 ? "" : "s");
                        ImGui::BeginDisabled();
                        ImGui::MenuItem(hidden);
                        ImGui::EndDisabled();
                    }
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("New Scene", "Ctrl+N")) requests.newScene = true;
                if (ImGui::MenuItem("Open Scene...", "Ctrl+O")) requests.openScene = true;
                ImGui::Separator();
                if (ImGui::MenuItem("New Material...")) requests.newMaterial = true;
                if (ImGui::MenuItem("Open Material...")) requests.openMaterial = true;
                ImGui::Separator();
                // Disabled during Play: the authored scene is the pre-Play snapshot,
                // and the live registry is play-time mutation that PlaySession::Stop
                // exists to discard, so saving it would persist garbage. Both items
                // carry the same tooltip because IsItemHovered names the LAST
                // submitted item -- one call after the pair would explain the greying
                // of "Save Scene As..." only, and "Save Scene" (the item carrying the
                // Ctrl+S hint) is the one users reach for.
                if (ImGui::MenuItem(sceneDirty ? "Save Scene *" : "Save Scene",
                                    "Ctrl+S", false, !playing))
                    requests.saveScene = true;
                if (playing && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Stop play mode to save the scene");
                if (ImGui::MenuItem("Save Scene As...", nullptr, false, !playing))
                    requests.saveSceneAs = true;
                if (playing && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Stop play mode to save the scene");
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
            if (ImGui::BeginMenu("Build"))
            {
                // Disabled during Play -- the UE model: the level editor
                // refuses a recompile while PIE runs (vendored source,
                // LevelEditorActions.cpp:1360-1374) and hot reload defers a
                // finished compile until play ends (HotReload.cpp:1272).
                // Mid-Play reload is a later opt-in, desk-verified first.
                // Also disabled while a build is already running (one at a
                // time -- the Runner refuses too, this just says so up front)
                // and when the open project has no gameModule to rebuild.
                const bool canRebuild = !playing && !buildingModule && hasGameModule;
                if (ImGui::MenuItem(buildingModule ? "Rebuilding..." : "Rebuild Game Module",
                                    nullptr, false, canRebuild))
                    requests.rebuildModule = true;
                if (!canRebuild && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip(playing        ? "Stop to rebuild"
                                      : buildingModule ? "A rebuild is already running (see Console)"
                                                       : "This project has no game module");
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
        // Inspector and Material share the right node as TABS: the scene's
        // selection and the active material's parameters are the same kind of
        // surface (the thing you are editing, in detail), and the host focuses
        // whichever one matches the active center tab. Inspector first, so a
        // fresh layout opens on it.
        ImGui::DockBuilderDockWindow("Inspector", rightId);
        ImGui::DockBuilderDockWindow("Material",  rightId);
        ImGui::DockBuilderDockWindow("Assets",    bottomId);   // Assets tab first...
        ImGui::DockBuilderDockWindow("Console",   bottomId);   // ...then Console...
        ImGui::DockBuilderDockWindow("Problems",  bottomId);   // ...then Problems, tabbed together
        ImGui::DockBuilderDockWindow("Viewport",  central);
        ImGui::DockBuilderFinish(dockspaceId);
    }

    void SelectDockTab(const char* windowName)
    {
        // FindWindowByName rather than a stored handle: the panel windows are
        // created by their own Begin, and this may run before the first one.
        ImGuiWindow* w = ImGui::FindWindowByName(windowName);
        if (!w || !w->DockNode || !w->DockNode->TabBar)
            return;
        // A docked window's tab id IS window->TabId -- imgui.cpp:19614 and
        // :19620 both select a tab by exactly that value.
        //
        // TabBarQueueFocus's ImGuiTabItem* overload (imgui_widgets.cpp:
        // 10379-10382) is the one that works here: the const char* overload
        // ASSERTS `(tab_bar->Flags & ImGuiTabBarFlags_DockNode) == 0`
        // (imgui_widgets.cpp:10386, "Only supported for manual/explicit tab
        // bars"), so naming the tab would fire an assert on every call.
        //
        // It parks NextSelectedTabId, which TabBarLayout applies on the next
        // frame -- deliberately a REQUEST, not a write: it loses to the node's
        // own newly-added-tab rule (imgui.cpp:19617-19620) rather than fighting
        // it, and it never touches g.NavWindow, so the center document keeps
        // both focus and its tab.
        if (ImGuiTabItem* tab = ImGui::TabBarFindTabByID(w->DockNode->TabBar, w->TabId))
            ImGui::TabBarQueueFocus(w->DockNode->TabBar, tab);
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
        // NoWindowMenuButton kills the tab-bar arrow menu (the one offering
        // "Hide tab bar") on EVERY node under this dockspace: DockSpace() flags
        // become the root's SharedFlags each frame, and SharedFlagsInheritMask_
        // is ~0 so children inherit them (imgui.cpp:18914/20202) -- no per-node
        // flags, no layout reset needed.
        ImGui::DockSpace(dockspaceId, ImVec2(0, 0), ImGuiDockNodeFlags_NoWindowMenuButton);

        // SCRUB the old lock: NoTabBar/NoCloseButton are SAVED dock flags, so
        // every imgui.ini written while the lock existed still carries them --
        // simply not re-applying changes nothing on machines with a layout.
        if (ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockspaceId))
            central->LocalFlags &= ~(ImGuiDockNodeFlags_NoTabBar
                                   | ImGuiDockNodeFlags_NoCloseButton
                                   | ImGuiDockNodeFlags_NoDockingOverMe
                                   | ImGuiDockNodeFlags_NoUndocking);

        // SCRUB HiddenTabBar the same way, on every node under the dockspace: a
        // tab bar hidden through the now-removed arrow menu in an earlier session
        // is a SAVED flag too, and with the menu gone there would be no way back.
        // Floating windows are separate root nodes, not under this dockspace, so
        // their single-window look is untouched.
        if (ImGuiDockNode* root = ImGui::DockBuilderGetNode(dockspaceId))
        {
            std::vector<ImGuiDockNode*> stack{ root };
            while (!stack.empty())
            {
                ImGuiDockNode* n = stack.back();
                stack.pop_back();
                n->LocalFlags &= ~ImGuiDockNodeFlags_HiddenTabBar;
                if (n->ChildNodes[0]) stack.push_back(n->ChildNodes[0]);
                if (n->ChildNodes[1]) stack.push_back(n->ChildNodes[1]);
            }
        }

        ImGui::End();
    }

    bool DrawSimTimeToolbar(PlaySession& play, Arcane::Runtime& runtime,
                            const Arcane::PluginVTable* plugin,
                            PlayLaunchMode& mode, uint64_t logoTex)
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

        // -- CENTER: transport, Unity-style toggles: [Play|mode] Pause Step. Center the
        // group within the full toolbar width, placed on the button row -- but never
        // behind the left cluster (clamp on narrow windows). Undo/Redo moved to the Edit
        // menu; the transform-gizmo tools moved to the Viewport overlay.
        //
        // The play-mode chevron is FUSED onto Play as one split control rather than
        // standing alone: it configures Play, but sitting after Step it read as Step's.
        // Same shape Unreal uses for its play-mode dropdown.
        //
        // kTransportGap overrides ImGui's default ItemSpacing.x (8) for this group only.
        // At 8 the buttons read as four separate objects instead of one instrument; the
        // group is an instrument. Everything else on the row keeps the global spacing,
        // which is why this is a scoped PushStyleVar and not a style change.
        constexpr float kTransportGap = 2.0f;
        constexpr float kCaretPadX    = 2.0f;   // the caret half is slimmer than a full icon button
        auto btnW = [&](const char* icon)
        { return ImGui::CalcTextSize(icon).x + st.FramePadding.x * 2.0f; };
        // The split's two halves overlap by one border so the shared edge is a single
        // 1px line, not two stacked -- hence the -FrameBorderSize, mirrored in the draw
        // below. Miss it here and the group centers a pixel off.
        const float caretW  = ImGui::CalcTextSize(ICON_LC_CHEVRON_DOWN).x + kCaretPadX * 2.0f;
        const float splitW  = btnW(ICON_LC_PLAY) + caretW - st.FrameBorderSize;
        const float transportW = splitW + btnW(ICON_LC_PAUSE) + btnW(ICON_LC_STEP_FORWARD)
                               + kTransportGap * 2.0f;
        const float centerStart = lineStartX + (fullContentW - transportW) * 0.5f;
        ImGui::SetCursorPos(ImVec2(std::max(centerStart, leftX + 12.0f), rowY));

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(kTransportGap, st.ItemSpacing.y));

        // Play/Stop toggle: the glyph SWAPS to a stop square while playing (user
        // ruling 2026-08-10, reversing the icon-font arc's tint-only Unity model)
        // and stays tinted, so the lit square reads unambiguously as "this stops".
        // The swap/tint is PIE-ONLY: a SeparateWindow launch is fire-and-forget
        // (the editor takes no lock on the child and does not track it), so it
        // never lights this button -- Stop would have nothing of its own to
        // restore for a launch it never tracked.
        bool launchStandaloneRequested = false;
        const bool playing = play.IsPlaying();
        if (iconToggle(playing ? ICON_LC_SQUARE : ICON_LC_PLAY, "##sim_play",
                       playing, playing ? "Stop" : "Play"))
        {
            if (playing)
                play.Stop(runtime, plugin);
            else if (mode == PlayLaunchMode::SeparateWindow)
                launchStandaloneRequested = true;   // caller resolves + spawns; play/plugin untouched
            else
                play.Play(runtime, plugin);
        }

        // The split's second half. Zero spacing, then back up one border width so the
        // two frames share an edge instead of showing a 2px seam. It carries Play's lit
        // state so the pair reads as ONE control while playing -- an untinted caret
        // welded to a tinted Play would look like a separate button again, which is the
        // whole thing this layout is fixing.
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() - st.FrameBorderSize);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(kCaretPadX, st.FramePadding.y));
        if (playing) ImGui::PushStyleColor(ImGuiCol_Button,
                                           ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (iconBtn(ICON_LC_CHEVRON_DOWN, "##sim_playmode", "Play mode"))
            ImGui::OpenPopup("##play_mode");
        if (playing) ImGui::PopStyleColor();
        ImGui::PopStyleVar();

        // Play-mode dropdown (Task 6, runtime-host-fold arc): choose whether the Play
        // button this hangs off enters PIE ("In viewport", today's behavior) or spawns a
        // standalone ArcaneRuntime window on the active scene ("Separate window",
        // fire-and-forget). These two rows are also the future SERVER-SET seam: a
        // server-driven "how should this build play" directive lands here as a new
        // row, never as a separate UI-only concept bolted on elsewhere.
        if (ImGui::BeginPopup("##play_mode"))
        {
            // MarkIniSettingsDirty on change, as the shader editor's layout
            // handler does (ShaderEditorDocument.cpp): ImGui::Shutdown saves the
            // ini on a clean exit anyway, so without this the setting survives a
            // normal quit but is LOST if the editor is killed or crashes.
            if (ImGui::MenuItem("In viewport", nullptr, mode == PlayLaunchMode::Viewport))
            {
                mode = PlayLaunchMode::Viewport;
                ImGui::MarkIniSettingsDirty();
            }
            if (ImGui::MenuItem("Separate window", nullptr, mode == PlayLaunchMode::SeparateWindow))
            {
                mode = PlayLaunchMode::SeparateWindow;
                ImGui::MarkIniSettingsDirty();
            }
            ImGui::EndPopup();
        }

        // Re-fetch AFTER a possible Stop -- Runtime::RestoreRegistry destroys and
        // replaces m_impl->loop on Stop, so a reference taken before this dangles.
        // (Nothing between that Stop and here touches `loop`, so fetching here is safe.)
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

        ImGui::PopStyleVar();   // kTransportGap: the rest of the row keeps the global spacing

        ImGui::Dummy(ImVec2(0.0f, 3.0f + overhang));   // clear the logo's lower overhang too
        ImGui::Separator();
        return launchStandaloneRequested;
    }

    void DrawConsolePanel(ConsoleBuffer& console, ConsoleUiState& ui)
    {
        ImGui::Begin("Console");

        // Snapshot once: CollapseConsole holds pointers into its input, and a
        // worker thread can push (and therefore evict) mid-frame.
        const std::vector<ConsoleEntry> entries = console.Snapshot();

        std::size_t nInfo = 0, nWarn = 0, nErr = 0;
        for (const ConsoleEntry& e : entries)
        {
            if (e.level == Arcane::DiagSeverity::Error)        ++nErr;
            else if (e.level == Arcane::DiagSeverity::Warning) ++nWarn;
            else                                               ++nInfo;
        }

        if (ImGui::Button("Clear")) console.Clear();
        ImGui::SameLine();
        if (ImGui::Button("Copy"))
        {
            std::string all;
            for (const ConsoleEntry& e : entries) { all += e.message; all += '\n'; }
            ImGui::SetClipboardText(all.c_str());
        }
        ImGui::SameLine();
        ImGui::Checkbox("Collapse", &ui.collapse);
        ImGui::SameLine();
        ImGui::Checkbox("Scroll", &ui.autoScroll);
        ImGui::SameLine();
        ImGui::Checkbox("Wrap", &ui.wrap);

        ImGui::SameLine();
        ImGui::Text("|");
        ImGui::SameLine(); ImGui::Checkbox("##infoT", &ui.showInfo);
        ImGui::SameLine(); ImGui::Text(ICON_LC_INFO " %zu", nInfo);
        ImGui::SameLine(); ImGui::Checkbox("##warnT", &ui.showWarning);
        ImGui::SameLine(); ImGui::TextColored(ImVec4(0.95f, 0.77f, 0.30f, 1.0f),
                                              ICON_LC_TRIANGLE_ALERT " %zu", nWarn);
        ImGui::SameLine(); ImGui::Checkbox("##errT", &ui.showError);
        ImGui::SameLine(); ImGui::TextColored(ImVec4(0.90f, 0.35f, 0.35f, 1.0f),
                                              ICON_LC_CIRCLE_X " %zu", nErr);

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##consolesearch", "Search", ui.search, sizeof(ui.search));

        ImGui::Separator();
        ImGui::BeginChild("##consolerows");

        const auto visible = [&](const ConsoleEntry& e)
        {
            if (e.level == Arcane::DiagSeverity::Error   && !ui.showError)   return false;
            if (e.level == Arcane::DiagSeverity::Warning && !ui.showWarning) return false;
            if (e.level == Arcane::DiagSeverity::Info    && !ui.showInfo)    return false;
            Arcane::Diagnostic probe;               // reuse the one filter definition
            probe.severity = e.level;
            probe.message  = e.message;
            return MatchesDiagnosticFilter(probe, Arcane::DiagSeverity::Info, ui.search);
        };

        // Wall-clock hh:mm:ss from the stored epoch millis. Local time: this is a
        // human reading their own session, not a log correlated across machines.
        const auto clockText = [](std::uint64_t ms)
        {
            const std::time_t secs = static_cast<std::time_t>(ms / 1000);
            std::tm tm{};
#if defined(_WIN32)
            localtime_s(&tm, &secs);
#else
            localtime_r(&secs, &tm);
#endif
            char buf[16] = {};
            std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
            return std::string(buf);
        };

        const auto drawRow = [&](const ConsoleEntry& e, std::size_t count)
        {
            ImVec4 col(0.80f, 0.80f, 0.80f, 1.0f);
            if (e.level == Arcane::DiagSeverity::Error)        col = ImVec4(0.90f, 0.35f, 0.35f, 1.0f);
            else if (e.level == Arcane::DiagSeverity::Warning) col = ImVec4(0.95f, 0.77f, 0.30f, 1.0f);

            // Timestamp and category are dimmed prefixes so the message stays the
            // thing the eye lands on.
            ImGui::TextDisabled("%s", clockText(e.timestampMs).c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%-8s", e.category.c_str());
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Text, col);
            if (ui.wrap) ImGui::PushTextWrapPos(0.0f);
            if (count > 1) ImGui::Text("%s  (x%zu)", e.message.c_str(), count);
            else           ImGui::TextUnformatted(e.message.c_str());
            if (ui.wrap) ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        };

        if (ui.collapse)
        {
            for (const CollapsedRow& r : CollapseConsole(entries))
                if (r.first && visible(*r.first)) drawRow(*r.first, r.count);
        }
        else
        {
            for (const ConsoleEntry& e : entries)
                if (visible(e)) drawRow(e, 1);
        }

        if (ui.autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
        ImGui::End();
    }

    ViewportPanelResult DrawViewportPanel(uint64_t textureId, uint32_t texW, uint32_t texH,
                                          bool& gizmoEnabled, Arcane::GizmoMode& mode,
                                          Arcane::GizmoSpace& space, bool showToolOverlay)
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
        // One-frame edge: this window just became the visible tab (the scene
        // was selected in the center node, or an asset document that was
        // covering it closed). imgui.cpp:9236-9240 returns window->Appearing,
        // raised for the docked case at imgui.cpp:7905-7907. The host turns it
        // into "focus the Inspector" -- see EditorApp::SyncCenterTabFocus.
        r.appearing = ImGui::IsWindowAppearing();
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
        // Hidden in Play mode (showToolOverlay=false): the game owns the viewport
        // there, and with the overlay gone overlayHovered stays false, so clicks
        // in that corner fall through to the game like anywhere else.
        bool overlayHovered = false;
        if (showToolOverlay)
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

        // `current` is Identity::name RAW -- never Edit::DisplayName, which
        // substitutes "Entity <id>" for an empty name (EntityOps.cpp:49-55).
        // Seeding that fallback made a no-edit commit on an empty-named entity
        // write "Entity 7" into the component; seeding the raw (possibly empty)
        // name keeps Escape and no-edit commits true no-ops.
        void BeginRename(OutlinerState& st, Astra::Entity e, const std::string& current)
        {
            st.renameTarget = e;
            st.renameBuf = current;
            st.renameFocusPending = true;
            // Starting a NEW rename abandons any deferred one. The user has
            // moved on; landing a parked rename afterwards would apply a name
            // they already replaced (or applied to an entity they have since
            // stopped editing) out of nowhere, one frame late.
            st.pendingRename = Astra::Entity::Invalid();
            st.pendingRenameName.clear();
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

        // A rename the commit site below could not land because another
        // transaction was open. Retried BEFORE any row is built, so this
        // frame's rows already show the new name. EVERY result but Deferred
        // consumes the slot: Renamed landed it, NoChange means someone else got
        // there first, and Invalid means the entity is gone or lost its
        // Identity -- retrying any of those forever would be a slow leak that
        // could also fire long after the user moved on.
        if (state.pendingRename.IsValid())
        {
            bool consumed = true;
            if (binding.editMode)
            {
                consumed = Arcane::Edit::RenameWithUndo(undo, registry, state.pendingRename,
                                                        state.pendingRenameName)
                           != Arcane::Edit::RenameResult::Deferred;
            }
            else
            {
                // Play started between the commit frame and this one. Applied
                // WITHOUT undo bracketing and consumed either way, exactly like
                // the commit site's own play-mode branch below: an entry
                // recorded now would let a later Ctrl+Z write play-time bytes
                // over the registry Stop restored. Holding the slot until Stop
                // instead would land a rename the user typed a whole play
                // session ago.
                Arcane::Edit::RenameEntity(registry, state.pendingRename,
                                           state.pendingRenameName);
            }
            if (consumed)
            {
                state.pendingRename = Astra::Entity::Invalid();
                state.pendingRenameName.clear();
            }
        }

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
            {
                // Rename edits an EXISTING Identity -- Edit::RenameEntity
                // refuses when there is none and never mints one
                // (EntityOps.cpp:180-186). An entity without one is a runtime
                // spawn with no durable identity, so F2 does nothing rather
                // than opening a box whose commit could not land.
                if (const Arcane::Identity* info =
                        registry.GetComponent<Arcane::Identity>(sel.Primary()))
                    BeginRename(state, sel.Primary(), info->name);
            }
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
                    InputTextString("##rename", &state.renameBuf,
                                    ImGuiInputTextFlags_AutoSelectAll);
                    // Commit on deactivate; the equality guard below IS the
                    // cancel path. Escape reverts the buffer to its
                    // activation-time text BEFORE deactivating
                    // (imgui_widgets.cpp:5212 raises revert_edit, :5300-5308
                    // writes TextToRevertTo back), so a cancelled rename
                    // arrives here equal to its seed and the guard drops it.
                    // Enter (:5180) and click-away deactivate the same way, so
                    // telling the three apart needs neither EnterReturnsTrue
                    // nor the global IsKeyPressed(Escape) query this replaced
                    // -- that query answered "was Escape pressed anywhere",
                    // not "did THIS box cancel".
                    if (ImGui::IsItemDeactivated())
                    {
                        const Astra::Entity e = row.entity;
                        // Re-read rather than trust the frame that opened the
                        // box: an undo/redo between the two can change or
                        // remove the component.
                        const Arcane::Identity* info =
                            registry.GetComponent<Arcane::Identity>(e);
                        if (info && state.renameBuf != info->name)
                        {
                            if (binding.editMode)
                            {
                                // ONE ComponentEditCommand in its OWN
                                // transaction -- the same shape as an Inspector
                                // field edit, not the whole-registry memento the
                                // structural edits in this panel use.
                                //
                                // This can fire in the same frame as a gizmo
                                // press or an Inspector field activation, so the
                                // stack may already be busy. RenameWithUndo then
                                // returns Deferred and mutates NOTHING: it
                                // refuses to join, because a joined rename rides
                                // the owner's Commit/Cancel and Cancel discards
                                // pending snapshots without reverting
                                // (CommandStack.cpp:75-82) -- which would leave
                                // the rename applied and permanently
                                // un-undoable. Park it and retry at the top of
                                // the next frame instead.
                                if (Arcane::Edit::RenameWithUndo(undo, registry, e, state.renameBuf)
                                    == Arcane::Edit::RenameResult::Deferred)
                                {
                                    state.pendingRename     = e;
                                    state.pendingRenameName = state.renameBuf;
                                }
                            }
                            else
                            {
                                // Play mode: applied WITHOUT undo bracketing,
                                // matching the Inspector's field visitor, which
                                // leaves its stack pointer null while Play runs
                                // (an entry recorded now would let a later
                                // Ctrl+Z write play-time bytes over the registry
                                // Stop restored). Deliberate change: this used
                                // to be refused outright and silently.
                                //
                                // Reached only when Play STARTS with a rename
                                // box already open -- all three entry points
                                // still require Edit mode to open one -- so it
                                // is the finish-what-you-typed path, not a
                                // play-time rename affordance.
                                Arcane::Edit::RenameEntity(registry, e, state.renameBuf);
                            }
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
                            // Gated on Identity like the other two entry
                            // points (see the F2 site); without one the click
                            // stays a plain select.
                            const Arcane::Identity* info =
                                registry.GetComponent<Arcane::Identity>(row.entity);
                            const bool slowSecond = binding.editMode && info != nullptr
                                && sel.Count() == 1 && sel.Primary() == row.entity
                                && state.lastClicked == row.entity
                                && (now - state.lastClickTime) > ImGui::GetIO().MouseDoubleClickTime
                                && (now - state.lastClickTime) < 1.2;
                            if (slowSecond)
                                BeginRename(state, row.entity, info->name);
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
                                    [&] { created = Arcane::Edit::CreateEntityInScene(registry, parent);
                                          return created.IsValid(); }))
                            {
                                state.collapsed.erase(
                                    static_cast<std::uint64_t>(parent.GetValue()));
                                sel.Select(created);
                            }
                        }
                        // Disabled rather than hidden without an Identity, so
                        // the refusal is visible before the click -- the same
                        // treatment the structural items get above. ForTooltip's
                        // default mouse flags include AllowWhenDisabled
                        // (imgui.cpp:1587, not overridden by this editor), which
                        // is what lets the explanation reach a greyed item.
                        const Arcane::Identity* rowInfo =
                            registry.GetComponent<Arcane::Identity>(row.entity);
                        if (ImGui::MenuItem("Rename", "F2", false, rowInfo != nullptr))
                            BeginRename(state, row.entity, rowInfo->name);
                        if (rowInfo == nullptr
                            && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                            ImGui::SetTooltip("Runtime entity: no Identity component to rename");
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
                        [&] { created = Arcane::Edit::CreateEntityInScene(registry,
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
        // ---------------------------------------------------------------------
        // Row rhythm (UE's Details rows read visibly tighter than ImGui's own
        // theme defaults).
        // ---------------------------------------------------------------------

        // Starting values for the vertical-rhythm tuning knobs used below --
        // NOT an applied tightening yet. Both equal ImGui's own stock style
        // defaults (FramePadding = (4,3) at imgui.cpp:1531, ItemSpacing =
        // (8,4) at imgui.cpp:1534) and nothing else in this editor modifies
        // style, so the push at the loop site moves ZERO pixels as authored
        // today. Per the spec's tune-at-desk flow, these constants are where
        // a human pass narrows the rhythm once the layout is on screen. Only
        // .y is a tuning target; the push site keeps the live style's .x so
        // horizontal spacing elsewhere in the panel (search box, buttons) is
        // untouched by a change scoped to vertical rhythm.
        constexpr float kInspectorFramePaddingY = 3.0f;
        constexpr float kInspectorItemSpacingY  = 4.0f;
    }

    void DrawInspectorPanel(Astra::Registry& registry, const SelectionContext& sel,
                            Arcane::CommandStack& undo, const SceneEditBinding& binding,
                            const Arcane::Project* project, InspectorState& state,
                            const InspectorServices* services)
    {
        // FIRST local, so it destructs LAST -- see EditGesture::ScopeGuard.
        const EditGesture::ScopeGuard gestureGuard{ &undo, state.gesture };

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

        // Search, UE's Details-panel shape (SDetailsViewBase.cpp:1016 --
        // OnFilterTextChanged -> FilterView). Filters components AND fields live.
        // UE's SSearchBox ships a built-in clear "X" (SSearchBox.cpp:149-161,
        // OnClearSearch) rather than requiring select-all-delete, and shows no
        // "no matches" text anywhere in PropertyEditor -- both mirrored below.
        // The button only claims layout space when there is something to
        // clear, so an empty query keeps the box at the full -FLT_MIN width it
        // would have if the button did not exist.
        const bool hasQuery = state.searchBuffer[0] != '\0';
        if (hasQuery)
        {
            const float clearButtonWidth = ImGui::CalcTextSize(ICON_LC_X).x
                                          + ImGui::GetStyle().FramePadding.x * 2.0f
                                          + ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetNextItemWidth(-clearButtonWidth);
        }
        else
        {
            ImGui::SetNextItemWidth(-FLT_MIN);
        }
        ImGui::InputTextWithHint("##inspector_search", ICON_LC_SEARCH " Filter",
                                 state.searchBuffer, sizeof(state.searchBuffer));
        if (hasQuery)
        {
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_LC_X))
                state.searchBuffer[0] = '\0';
        }
        // INVARIANT: `searchBuffer` is written ONLY by a widget that takes
        // ActiveId this same frame -- typing (already owned it from the prior
        // frame) or a click on the clear button above (takes it fresh, same
        // as clicking into the box). Either way, whatever field widget was
        // previously active deactivates THIS frame, so EndGesture() closes
        // state.gesture's open transaction before the next frame can redraw
        // under the new query and make that field vanish. A clear path that does
        // NOT move ActiveId (Escape, clear-on-selection-change) would skip that
        // close; the ScopeGuard closes it on the way out of this panel instead, so
        // the leak is contained rather than permanent -- see the Outliner's
        // hasRow sweep in DrawOutlinerPanel, which drops a renameTarget that
        // stopped being drawn, for the same hazard class. (Cited by NAME on
        // purpose: the line number this used to carry had already rotted.)
        const std::string_view query(state.searchBuffer);

        // Hoisted once per frame; see the Outliner's copy. Add/Remove Component
        // are structural (whole-registry memento), so they refuse during Play AND
        // inside an open field/gizmo gesture -- both now visible as disabled.
        const bool canEditStructure = CanEditStructure(undo, binding);

        // Removal is DEFERRED past the loop: Edit::RemoveComponent moves the
        // entity to a different archetype, which dangles every ci.data pointer
        // in the vector being iterated. Descriptor pointers themselves are
        // stable (they live in ComponentRegistry's fixed array).
        const Astra::ComponentDescriptor* pendingRemove = nullptr;

        // Section order: Identity first, Transform second, everything else
        // in registry order (matches UE's Details layout). ComponentInfo is
        // three raw pointers -- trivially copyable, and the pointers it holds
        // (descriptor into ComponentRegistry's fixed array, per the comment
        // above; data into the current archetype chunk, untouched before this
        // sort runs) stay valid across the reorder. stable_sort, not sort:
        // ties (rank 2, the overwhelming majority of components) must keep
        // their registry order, exactly like the loop did before this sort
        // existed. A null ci.meta ranks with the catch-all (2) rather than
        // being filtered here -- the loop body below already `continue`s on
        // null descriptor/meta, so leaving it in the vector is harmless and
        // this comparator never dereferences meta itself.
        std::vector<Astra::Registry::ComponentInfo> components = registry.InspectEntity(primary);
        std::stable_sort(components.begin(), components.end(),
            [](const Astra::Registry::ComponentInfo& a, const Astra::Registry::ComponentInfo& b)
            {
                const std::string_view an = a.meta ? a.meta->typeName : std::string_view{};
                const std::string_view bn = b.meta ? b.meta->typeName : std::string_view{};
                return InspectorSectionRank(an) < InspectorSectionRank(bn);
            });

        // One rhythm region for the whole per-component sweep, popped
        // unconditionally right after the loop closes. Both header types
        // fold FramePadding.y into their own frame height (TreeNodeBehavior,
        // imgui_widgets.cpp:6886-6888,6898): frame_height = label_size.y +
        // padding.y * 2, where padding.y IS style.FramePadding.y for the
        // framed CollapsingHeader, and is ImMin(CurrLineTextBaseOffset,
        // style.FramePadding.y) for the unframed category TreeNodeEx -- a
        // lower FramePadding.y can only shrink or hold that clamp, never grow
        // it, so the push still reaches both.
        //
        // ItemSpacing.y does NOT reach field-grid rows, though, despite
        // ItemSize adding it to the cursor advance (imgui.cpp:12130): that
        // same call subtracts it back out of CursorMaxPos.y one line later
        // (imgui.cpp:12132), and a table row's height is read from
        // CursorMaxPos.y + RowCellPaddingY at cell-close (TableEndCell,
        // imgui_tables.cpp:2268) -- CellPadding.y, untouched by this arc, is
        // what actually governs inner-row rhythm. What the ItemSpacing.y
        // push DOES reach: the header rows themselves (drawn outside any
        // table, where ItemSize's effect stands uncancelled) and the gaps
        // between sections. If a future desk pass wants the grid rows
        // themselves tighter too, CellPadding.y is the knob to add here, not
        // this one.
        // .x is carried over from the live style; only .y is overridden.
        //
        // Balanced on every path: this is OUTSIDE the loop, so none of the
        // loop's own `continue`s (component filter above, the category-field
        // sweep inside it) can skip the pop below -- they only skip to the
        // next `ci`, and the pop runs once the loop itself is done.
        //
        // Category indent is untouched: the open category's children are
        // indented by TreePushOverrideID's call to Indent() (imgui_widgets.cpp:
        // 7233-7239), which advances window->DC.Indent.x by g.Style.IndentSpacing
        // (imgui.cpp:12246) -- a third style var, distinct from the two pushed
        // here.
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
            ImVec2(ImGui::GetStyle().FramePadding.x, kInspectorFramePaddingY));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
            ImVec2(ImGui::GetStyle().ItemSpacing.x, kInspectorItemSpacingY));
        for (const Astra::Registry::ComponentInfo& ci : components)
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
            // Derived/runtime-owned state is never authored -- but as of task 5
            // this DISPLAY gate no longer matches the Add Component catalog or
            // Remove Component below: those two consult IsStructureLocked
            // (ComponentCatalog.hpp), which also covers Arcane::Identity, so
            // that identity renders its own section here (name editable, id
            // view-only) while staying un-addable and un-removable. The three
            // sites used to share one predicate by construction; splitting it
            // was the whole point of task 5, so read each site's predicate
            // rather than assuming parity.
            if (IsHiddenInInspector(typeName))
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

            // Friendly header, raw type in the tooltip: the label should read as
            // words, but the C++ type name is what you search the source for. The
            // hide-list check above and the PushID below both stay keyed on the
            // real type name -- only the drawn string changes. Built here rather
            // than after the PushID because the filter decision needs it.
            const std::string headerLabel = Arcane::Editor::DisplayNameForComponent(typeName);

            // A component survives the filter when its own name matches, or when
            // at least one of its fields does. Deciding BEFORE the header is drawn
            // is what makes a component with no matches disappear rather than
            // render as an empty section; the `continue` lands before the PushID
            // below so the ID stack stays balanced.
            //
            // An empty query matches every component here, so this whole field
            // sweep is skipped outright in the unfiltered case.
            bool componentVisible = Arcane::Editor::ComponentMatchesFilter(headerLabel, query);
            if (!componentVisible)
            {
                for (const Astra::FieldInfo& f : ci.meta->fields)
                {
                    // Mirrors BOTH skips the visitor applies, in the same order, so
                    // a field this sweep counts is a field the visitor will actually
                    // draw. visitFields is Astra's VisitFields (ComponentRegistry.hpp:314),
                    // which drops non-serializable fields before Visit() ever sees
                    // them; Visit() then drops Astra::Hidden fields itself (this
                    // file, ~line 1151). Missing either one lets a component whose
                    // only matching field fails that skip vote itself visible and
                    // draw an empty body.
                    if (!f.IsSerializable())
                        continue;
                    if (Arcane::Editor::FieldIsAttributeHidden(f))
                        continue;
                    if (Arcane::Editor::MatchesInspectorFilter(
                            headerLabel, Arcane::Editor::DisplayNameForField(f), f.name, query))
                    {
                        componentVisible = true;
                        break;
                    }
                }
            }
            if (!componentVisible)
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
            // Scoped tight around the header call only -- the band colors
            // must not leak into the tooltip/popup below, which read the
            // theme's own ImGuiCol_* set like every other popup in the editor.
            // The extra block IS that scope: HeaderBand pops at its closing
            // brace, which is why the header's answer is assigned out of it.
            bool open = false;
            {
                HeaderBand band;
                open = ImGui::CollapsingHeader(headerLabel.c_str(),
                                               ImGuiTreeNodeFlags_DefaultOpen);
            }
            // Safe to sit between the header and BeginPopupContextItem below,
            // which resolves its id from g.LastItemData: SetTooltip opens and
            // closes a window, and ImGui restores LastItemData in End()
            // (imgui.cpp:8849), so the popup still inherits the HEADER's id.
            // ForTooltip gates on stationary+delay (style.HoverFlagsForTooltipMouse,
            // imgui.h:1515) so scrolling past headers does not flicker a tooltip per
            // header.
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("%s", typeName.c_str());
            // Structure-locked components (identity + derived caches) have no context
            // actions at all, so the popup must not OPEN -- an empty popup frame reads
            // as a glitch. BeginPopupContextItem is self-balancing (imgui.cpp:13333-13344:
            // the window is pushed only when it returns true, and EndPopup runs only
            // inside the branch), so gating the whole block is safe.
            if (!Arcane::Editor::IsStructureLocked(typeName) && ImGui::BeginPopupContextItem())
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
                    // Built ONCE per component and re-driven per category:
                    // `activeCategory` is the only member that moves between
                    // the calls below, exactly as it was the only visitor
                    // member the category loop used to re-point. Positional
                    // init in declaration order -- see InspectorView.hpp.
                    ReflectedComponentArgs fieldArgs{
                        registry,
                        ci,
                        primary,
                        sel.Entities(),                     // fan-out targets
                        // Null while Play is running: BeginGestureIfActivated/EndGesture
                        // both early-return on a null stack, so gesture bracketing is
                        // fully inert (no Begin, no Commit/Cancel) against the live
                        // simulating registry.
                        binding.editMode ? &undo : nullptr,
                        project,
                        services,
                        state,
                        // Both outlive the drives below: headerLabel is this
                        // iteration's local and the query views InspectorState's
                        // buffer.
                        headerLabel,                        // componentDisplayName
                        std::string_view{},                 // activeCategory
                        query,                              // filterQuery
                    };

                    // Categories, UE's Details shape: the uncategorised fields
                    // ungrouped FIRST (UE's NoCategory fallback,
                    // DetailCategoryBuilderImpl.cpp:230), then each named
                    // category under its own collapsible sub-header. Named
                    // categories run in first-appearance order over declaration
                    // order, so the ordering is stable and a component still
                    // reads in the Inspector the way it reads in source -- the
                    // fields themselves are never reordered.
                    //
                    // A category is collected only when at least one of its
                    // fields will actually DRAW, which means reproducing every
                    // skip on the way to the widget: Astra's non-serializable
                    // drop then Astra::Hidden (both mirrored from the component
                    // sweep above, in the visitor's own order), plus the live
                    // filter. Counting a field the visitor then drops is what
                    // renders a header over an empty body -- the same defect one
                    // level up that the component sweep exists to avoid.
                    //
                    // The cheap `cat.empty()` rejection leads deliberately: it is
                    // the answer for EVERY field until components carry Category
                    // attributes, and it keeps the filter's display-name build
                    // off that path entirely.
                    std::vector<std::string_view> categories;
                    for (const Astra::FieldInfo& f : ci.meta->fields)
                    {
                        const std::string_view cat = Arcane::Editor::CategoryOfField(f);
                        if (cat.empty())
                            continue;
                        if (!f.IsSerializable())
                            continue;
                        if (Arcane::Editor::FieldIsAttributeHidden(f))
                            continue;
                        if (!Arcane::Editor::MatchesInspectorFilter(
                                headerLabel, Arcane::Editor::DisplayNameForField(f), f.name, query))
                            continue;
                        if (std::find(categories.begin(), categories.end(), cat) == categories.end())
                            categories.push_back(cat);
                    }

                    // Uncategorised pass -- `activeCategory` is empty by
                    // default. While no field carries a Category attribute this
                    // is the same drive over the same fields as before grouping
                    // existed.
                    //
                    // The grid is opened HERE rather than inside the visitor so
                    // the component header and the category sub-headers below
                    // stay full-width, exactly as UE lays them out; the visitor
                    // only ever emits rows. A FieldGrid that converts to false
                    // is ImGui culling the table, and then no row may be
                    // submitted -- the same "field stopped being drawn" shape
                    // as a collapsed header, which the ScopeGuard above already
                    // covers. The block scopes the grid closed before the
                    // category headers below, which must draw full-width.
                    {
                        FieldGrid grid{ "##fields", state.labelColWidth };
                        if (grid)
                            DrawReflectedComponent(fieldArgs);
                    }

                    for (const std::string_view cat : categories)
                    {
                        // The view is into the Category attribute's own literal
                        // and is NOT guaranteed NUL-terminated, so the name goes
                        // through the begin/end PushID overload and a "%.*s"
                        // label rather than a c_str(). The sub-header's id comes
                        // from that push, not from the drawn text.
                        ImGui::PushID(cat.data(), cat.data() + cat.size());
                        // Same scoped pair as the component header above --
                        // wraps only the call that reads Header/HeaderHovered/
                        // HeaderActive, not the grid content the `if` guards.
                        bool categoryOpen = false;
                        {
                            HeaderBand band;
                            categoryOpen = ImGui::TreeNodeEx("##category",
                                ImGuiTreeNodeFlags_DefaultOpen,
                                "%.*s", static_cast<int>(cat.size()), cat.data());
                        }
                        if (categoryOpen)
                        {
                            fieldArgs.activeCategory = cat;
                            // Its own grid, sharing the panel-wide label WIDTH
                            // (state.labelColWidth) with every uncategorized
                            // grid -- but the visual split does NOT line up
                            // across sections. This grid draws under
                            // TreePushOverrideID's own Indent() call
                            // (imgui_widgets.cpp:7233-7239 -- TreeNodeBehavior
                            // pushes it when the category is open), which
                            // advances the cursor by g.Style.IndentSpacing
                            // (imgui.cpp:12246; stock default 21.0f,
                            // imgui.cpp:1538) before this table opens, so a
                            // category grid's border sits one IndentSpacing
                            // right of the uncategorized grids' border.
                            // Whether that offset reads fine is a desk call --
                            // the spec locks category indent to the tree's
                            // own indent rather than fighting it back to 0.
                            {
                                FieldGrid grid{ "##fields", state.labelColWidth };
                                if (grid)
                                    DrawReflectedComponent(fieldArgs);
                            }
                            ImGui::TreePop();
                        }
                        ImGui::PopID();
                    }
                }
            }
            ImGui::PopID();
        }
        ImGui::PopStyleVar(2);

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
