#include "EditorPanels.hpp"
#include "AssetBrowser.hpp"
#include "ComponentCatalog.hpp"
#include "ConsoleBuffer.hpp"
#include "EditorFonts.hpp"
#include "EntityList.hpp"
#include "IconsLucide.h"
#include "InspectorFields.hpp"
#include "InspectorMeta.hpp"
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
#include <string_view>
#include <utility>
#include <vector>

namespace Arcane::Editor
{
    void BeginDockSpace(Arcane::CommandStack& undo, MenuRequests& requests,
                        bool sceneDirty, bool playing)
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
                if (ImGui::MenuItem("Open Project...")) requests.openProject = true;
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

        // std::string-backed InputText. misc/cpp/imgui_stdlib is NOT vendored, so
        // this inlines its CallbackResize pattern: ImGui tells the callback how
        // long the text is about to be, the string resizes to fit, and the
        // callback hands back the (possibly reallocated) data() pointer.
        //
        // Declared here rather than beside the Inspector's other small helpers so
        // both panels in this file can reach it.
        int StringResizeCallback(ImGuiInputTextCallbackData* data)
        {
            if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
            {
                std::string* s = static_cast<std::string*>(data->UserData);
                s->resize(static_cast<std::size_t>(data->BufTextLen));
                data->Buf = s->data();
            }
            return 0;
        }

        // While the widget is ACTIVE, ImGui edits its own copy of the text and
        // ignores the buffer passed in -- it re-reads that buffer only when
        // WantReloadUserBuf is set (imgui_widgets.cpp:4834-4849, restated at
        // :5417-5419). Call sites may therefore pass a per-frame local reseeded
        // from live data every frame without fighting the user's typing.
        //
        // capacity() + 1 is BufSize's own C++ spelling (imgui.h:2772); the +1 is
        // the terminator slot past capacity(). It cannot under-report the room
        // available, because ImGui's ONLY write into the buffer runs the resize
        // callback above first and then copies into the pointer that callback
        // returned (imgui_widgets.cpp:5423-5447) -- the string is grown to fit
        // before any byte lands in it.
        bool InputTextString(const char* label, std::string* s, ImGuiInputTextFlags flags = 0)
        {
            flags |= ImGuiInputTextFlags_CallbackResize;
            return ImGui::InputText(label, s->data(), s->capacity() + 1, flags,
                                    StringResizeCallback, s);
        }

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
                                    [&] { created = Arcane::Edit::CreateEntityInScene(registry, parent);
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
        // Astra::Range is authored in double; ImGui's drags take float and int.
        // Clamping through double BEFORE the narrowing cast is the point:
        // converting an out-of-range double to a narrower type is UB, and a Range
        // holds whatever the author typed at the ASTRA_REFLECT_ATTR site.
        [[nodiscard]] float ToFloatClamped(double d) noexcept
        {
            return static_cast<float>(std::clamp(d, static_cast<double>(-FLT_MAX),
                                                    static_cast<double>(FLT_MAX)));
        }

        [[nodiscard]] int ToInt32Clamped(double d) noexcept
        {
            const double lo = static_cast<double>(std::numeric_limits<int32_t>::min());
            const double hi = static_cast<double>(std::numeric_limits<int32_t>::max());
            return static_cast<int>(std::clamp(d, lo, hi));
        }

        // Range::step is the author's drag increment and DEFAULTS TO 0
        // (Attribute.hpp:77), which means "unspecified" -- not "frozen". A zero
        // therefore keeps the speed the widget used before ranges were read at
        // all; handed to ImGui it would instead be REPLACED by a speed derived
        // from the bounds (imgui_widgets.cpp:2546), changing the feel of every
        // range-annotated-but-unstepped field as a side effect of clamping it.
        [[nodiscard]] float DragSpeedFor(const Astra::Range& r, float fallbackSpeed) noexcept
        {
            return r.step > 0.0 ? ToFloatClamped(r.step) : fallbackSpeed;
        }

        // The authored Range as a [lo, hi] pair, or nullopt when the field has no
        // Range or the authored one does not actually bind.
        //
        // "Binds" is ImGui's own rule for the drags below, so the typed paths that
        // call this agree with them: min < max, or a NON-ZERO min == max
        // (imgui_widgets.cpp:2540, minus its ClampZeroRange term -- these calls do
        // not pass that flag). imgui.h:687's "if v_min >= v_max we have no bound"
        // is only the header's doc comment; the implementation pins Range(5, 5) at
        // 5 and leaves just min > max and Range(0, 0) unbounded.
        [[nodiscard]] std::optional<std::pair<double, double>>
        BindingRange(const Astra::FieldInfo& f)
        {
            const std::optional<Astra::Range> r = Arcane::Editor::RangeOfField(f);
            if (!r)
                return std::nullopt;
            if (r->min < r->max || (r->min == r->max && r->min != 0.0))
                return std::make_pair(r->min, r->max);
            return std::nullopt;
        }

        // Drags that honour the field's Astra::Range when it carries one, and are
        // otherwise the exact call these sites made before ranges were read.
        //
        // ClampOnInput is what makes the bound real. Dragging clamps on its own,
        // but Ctrl+click text entry into the same widget is clamped ONLY under
        // this flag (imgui_widgets.cpp:2783 -> :2703-2706), so without it a typed
        // value passes the bounds untouched. Deliberately NOT AlwaysClamp, which
        // is ClampOnInput|ClampZeroRange (imgui.h:2034) and would change which
        // degenerate ranges bind -- see BindingRange above.
        //
        // The format strings are spelled out only because `flags` sits after them
        // in the signature; both are the header's own defaults (imgui.h:687/692),
        // so nothing about how a value reads changes.
        [[nodiscard]] bool RangedDragFloat(const Astra::FieldInfo& f, const char* label,
                                           float* v, float fallbackSpeed)
        {
            if (const std::optional<Astra::Range> r = Arcane::Editor::RangeOfField(f))
                return ImGui::DragFloat(label, v, DragSpeedFor(*r, fallbackSpeed),
                                        ToFloatClamped(r->min), ToFloatClamped(r->max),
                                        "%.3f", ImGuiSliderFlags_ClampOnInput);
            return ImGui::DragFloat(label, v, fallbackSpeed);
        }

        [[nodiscard]] bool RangedDragInt(const Astra::FieldInfo& f, const char* label, int* v)
        {
            if (const std::optional<Astra::Range> r = Arcane::Editor::RangeOfField(f))
                // 1.0f is DragInt's own default speed (imgui.h:692), passed
                // explicitly because the bounded overload leaves no way to omit it.
                return ImGui::DragInt(label, v, DragSpeedFor(*r, 1.0f),
                                      ToInt32Clamped(r->min), ToInt32Clamped(r->max),
                                      "%d", ImGuiSliderFlags_ClampOnInput);
            return ImGui::DragInt(label, v);
        }

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

            // The Inspector's live search, set per component before the visit.
            // `componentDisplayName` is the header's prose name, which is part of
            // what MatchesInspectorFilter tests: a hit on it shows EVERY field in
            // this component. `query` views the panel's persistent
            // InspectorState buffer, which outlives the visitor; empty matches
            // everything, so the unfiltered case needs no branch of its own.
            std::string                       componentDisplayName;
            std::string_view                  query;
            // Which category group this drive is rendering. Astra's VisitFields
            // always walks every field, so a visitor that must draw one group at
            // a time has to select here; the panel re-drives it once per group.
            // EMPTY IS THE UNCATEGORISED PASS, not "draw everything":
            // CategoryOfField returns empty for an unannotated field, so the two
            // compare equal and only those fields draw.
            std::string_view                  activeCategory{};
            // The in-flight gesture's CommandStack ownership token, owned by the
            // panel's persistent InspectorState -- the visitor is rebuilt every
            // frame but the gesture it brackets is not. Never null when `stack` is
            // non-null.
            Arcane::TransactionId*            gestureTxn = nullptr;
            // ImGui id of the widget that opened that gesture, parked in the same
            // persistent state for the same reason -- it is what CloseAbandonedGesture
            // below compares ActiveId against. Never null when `stack` is non-null.
            ImGuiID*                          gestureItem = nullptr;

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
                    // Remember WHICH widget owns the gesture, so the panel can tell a
                    // live one from an abandoned one. IsItemActivated() above is true
                    // exactly when g.ActiveId == g.LastItemData.ID (imgui.cpp:6549),
                    // so this is ActiveId -- including through a ctrl+click text entry
                    // (TempInputText re-derives the id from the same window+label) and
                    // through DragFloat2/3, where EndGroup forwards the live component
                    // id into LastItemData (imgui.cpp:12480).
                    *gestureItem = ImGui::GetItemID();
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
                *gestureTxn  = Arcane::TransactionId::None;   // spent
                *gestureItem = 0;                             // ...and unowned
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
                // Group selector: this drive renders exactly one category, so a
                // field belonging to any other one is another drive's business.
                // Leaves from the UNPUSHED scope, like the two skips below.
                if (Arcane::Editor::CategoryOfField(f) != activeCategory)
                    return;

                // Astra::Hidden -- the FIELD ATTRIBUTE, "do not show this
                // property". Nothing to do with Arcane::Hidden, the marker
                // component that makes render submission skip an entity; the
                // names collide, the meanings do not. Returning BEFORE the PushID
                // below is what keeps the ID stack balanced without an early-out
                // inside the scope.
                if (Arcane::Editor::FieldIsAttributeHidden(f))
                    return;

                // Two names on purpose. `label` is prose for the row; `rawName` is
                // the C++ identifier and stays the input to everything the user
                // does not read as prose -- the undo description and the
                // asset-kind heuristic, both of which would change meaning if
                // handed a display name.
                //
                // Built ABOVE the PushID because the search below needs `label` to
                // decide, and its skip has to leave from the UNPUSHED scope for the
                // same reason the Hidden check above does.
                const std::string label = Arcane::Editor::DisplayNameForField(f);
                const std::string rawName(f.name);
                // Both names are searchable: a user who knows the source can type
                // `sortingLayer`, one who does not can type `sorting`. The
                // component-name-hit rule (a match on the header shows every field)
                // lives inside the predicate, not here.
                if (!Arcane::Editor::MatchesInspectorFilter(componentDisplayName, label,
                                                            rawName, query))
                    return;

                ImGui::PushID(static_cast<int>(f.nameHash));
                // Astra::ReadOnly -- the field is still SHOWN, it just cannot be
                // edited. Distinct from FieldKind::ReadOnly below, which means
                // "this panel has no widget for that type"; a field can be either,
                // both, or neither. When it is both, the two BeginDisabled pairs
                // nest, which ImGui supports and does not double-dim: the inner
                // Begin sees the flag already set and skips the alpha change
                // (imgui.cpp:8893-8906).
                const bool readOnly = Arcane::Editor::FieldIsReadOnly(f);
                if (readOnly)
                    ImGui::BeginDisabled();

                // Does this row want the tooltip below? An arm whose LAST item is
                // not the thing the user points at has to answer for itself while
                // its own widget is still the last item -- only the asset-ref arm
                // is in that position, and it fills this in. Left unset, the tail
                // asks about the last item, which is that row's own content.
                std::optional<bool> hovered;

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
                        BeginGestureIfActivated(rawName, instance);
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
                                // The field's Range binds here too. This row is a
                                // raw text box rather than a drag, so ImGui clamps
                                // nothing for it -- and an out-of-range value typed
                                // into a multi-selection reaches the same
                                // narrowing casts downstream as one typed into the
                                // single-selection drag.
                                if (const auto bound = BindingRange(f))
                                    out = std::clamp(out, bound->first, bound->second);
                                // Then the cast's own domain: converting an
                                // out-of-range double to int32 is UB, and the box
                                // accepts arbitrary digits. Two clamps rather than
                                // one intersected pair because an authored range
                                // lying entirely outside int32 would invert the
                                // intersection and break std::clamp's lo <= hi
                                // precondition.
                                const double lo = static_cast<double>(std::numeric_limits<int32_t>::min());
                                const double hi = static_cast<double>(std::numeric_limits<int32_t>::max());
                                const int32_t iv = static_cast<int32_t>(std::clamp(out, lo, hi));
                                ApplyImmediate(rawName, instance, [&](void* d)
                                               { Arcane::Editor::ApplyIntEdit(f, d, iv); });
                            }
                            break;
                        }
                        bool changed = RangedDragInt(f, label.c_str(), &v);
                        BeginGestureIfActivated(rawName, instance);
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
                                // Same rule as the Int32 row above and as the drag
                                // one branch down: a Range bounds the typed value,
                                // because nothing in a plain text box does.
                                if (const auto bound = BindingRange(f))
                                    out = std::clamp(out, bound->first, bound->second);
                                const float fv = static_cast<float>(out);
                                ApplyImmediate(rawName, instance, [&](void* d)
                                               { Arcane::Editor::ApplyFloatEdit(f, d, fv); });
                            }
                            break;
                        }
                        bool changed = RangedDragFloat(f, label.c_str(), &v, 0.1f);
                        BeginGestureIfActivated(rawName, instance);
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
                                ApplyImmediate(rawName, instance, [&](void* d)
                                               { if (glm::vec2* p = f.GetPtr<glm::vec2>(d)) (*p)[c] = fv; });
                            }
                            break;
                        }
                        bool changed = ImGui::DragFloat2(label.c_str(), &v.x, 0.1f);
                        BeginGestureIfActivated(rawName, instance);
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
                                ApplyImmediate(rawName, instance, [&](void* d)
                                               { if (glm::vec3* p = f.GetPtr<glm::vec3>(d)) (*p)[c] = fv; });
                            }
                            break;
                        }
                        bool changed = ImGui::DragFloat3(label.c_str(), &v.x, 0.1f);
                        BeginGestureIfActivated(rawName, instance);
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
                        // rawName, NOT the display label: this heuristic reads the
                        // C++ identifier, which is what its documented contract
                        // (AssetBrowser.hpp) is written against.
                        const int kindFilter = Arcane::Editor::AssetKindFilterForFieldName(rawName);

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
                        // This row ends with a SameLine'd name, not with its own
                        // widget, so the tail below would ask about that text and
                        // the identifier tooltip would never appear over the
                        // button. Asked here, while the button IS the last item.
                        hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
                        if (ImGui::BeginDragDropTarget())
                        {
                            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(Arcane::Editor::kAssetDragType))
                            {
                                const auto* payload = static_cast<const Arcane::Editor::AssetDragPayload*>(p->Data);
                                if (kindFilter < 0 || static_cast<int>(payload->kind) == kindFilter)
                                    ApplyGuidImmediate(rawName, f, instance, payload->guid);
                            }
                            ImGui::EndDragDropTarget();
                        }
                        // Offered when MIXED too: "clear all of them" is a
                        // meaningful action even though the primary may be nil.
                        if (v.IsValid() || refMixed)
                        {
                            ImGui::SameLine();
                            if (ImGui::SmallButton("x##assetclear"))
                                ApplyGuidImmediate(rawName, f, instance, Arcane::Guid::Nil());
                        }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(label.c_str());
                        // The name counts as part of the row: DragScalar registers
                        // a rect that spans its own label (imgui_widgets.cpp:2734
                        // builds total_bb from it, :2738 registers THAT), so
                        // hovering the name already explains the field on every
                        // numeric row.
                        hovered = *hovered || ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);

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
                                    ApplyGuidImmediate(rawName, f, instance, Arcane::Guid::Nil());
                                for (const Arcane::Editor::AssetEntry& e :
                                     Arcane::Editor::BuildAssetEntries(project->Registry()))
                                {
                                    if (!Arcane::Editor::MatchesFilter(e, kindFilter, s_pickSearch))
                                        continue;
                                    if (ImGui::Selectable((e.name + "##" + e.mountPath).c_str(), e.guid == v))
                                        ApplyGuidImmediate(rawName, f, instance, e.guid);
                                }
                            }
                            ImGui::EndPopup();
                        }
                        break;
                    }
                    case Arcane::Editor::FieldKind::String:
                    {
                        const std::string* live = f.GetPtr<std::string>(instance);
                        // Blank when the selection disagrees -- the same "unset
                        // means multiple differing values" rule the numeric rows
                        // above use.
                        const bool strMixed = Multi() && MixedFor(f).Any();
                        // Reseeded from the component every frame while the box is
                        // idle; once it is active ImGui's own state owns the text
                        // and this local is ignored (see InputTextString), so the
                        // per-frame seed cannot stomp what the user is typing.
                        std::string text = (strMixed || !live) ? std::string() : *live;
                        const std::string seed = text;
                        InputTextString(label.c_str(), &text);
                        // Commit on deactivation, guarded by an equality test --
                        // and that guard IS the cancel path, not a nicety. Escape
                        // restores the activation-time text AND sets value_changed
                        // (imgui_widgets.cpp:5300-5309), so
                        // IsItemDeactivatedAfterEdit reports a revert as an edit;
                        // "it deactivated and ended up different" is the honest
                        // predicate, and it also makes a plain click-in-click-out
                        // write nothing.
                        //
                        // ApplyImmediate, not a widget gesture: the commit is one
                        // discrete event, and it fans the typed text to every
                        // selected entity as ONE undo step. A mixed row left alone
                        // still commits nothing, because its seed is the blank it
                        // renders.
                        if (ImGui::IsItemDeactivated() && text != seed)
                            ApplyImmediate(rawName, instance, [&](void* d)
                                           { Arcane::Editor::ApplyStringEdit(f, d, text); });
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
                if (readOnly)
                    ImGui::EndDisabled();

                // Every arm but the asset-ref one ends on the row's own content, so
                // asking about the LAST item is asking about the field: ImGui
                // restores g.LastItemData when a window closes (imgui.cpp:8849), so
                // neither the asset-pick popup nor the SetTooltip below can
                // retarget it. The asset-ref arm ends on a trailing name instead
                // and has already answered above. Asked after EndGesture for the
                // same reason the gesture is bracketed at all -- nothing may sit
                // between a widget and the item-state reads that close its
                // transaction.
                //
                // The raw identifier is always in the tooltip, so a friendly label
                // never costs the ability to grep for the field. ForTooltip adds a
                // stationary+delay gate (style.HoverFlagsForTooltipMouse, imgui.h:1515)
                // so sweeping the cursor down the panel does not flicker a tooltip per
                // row; that default already carries AllowWhenDisabled, so an
                // Astra::ReadOnly row still explains itself on hover.
                if (!hovered.has_value())
                    hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
                if (*hovered)
                {
                    const std::string_view tip = Arcane::Editor::TooltipOfField(f);
                    if (tip.empty())
                        ImGui::SetTooltip("%s", rawName.c_str());
                    else
                        ImGui::SetTooltip("%.*s\n\n%s", static_cast<int>(tip.size()),
                                          tip.data(), rawName.c_str());
                }
                ImGui::PopID();
            }
        };

        // Close a field-edit gesture whose widget stopped being drawn.
        //
        // BeginGestureIfActivated opens the transaction and EndGesture closes it,
        // but EndGesture reads item state -- so it can only run while that widget
        // is still being SUBMITTED. Anything that stops it being submitted while
        // it owns the gesture strands the transaction open: its component header
        // (or a category sub-header) collapsing, the search query hiding the
        // field, the selection turning multi (which swaps the drag for text
        // boxes), the Inspector window collapsing or its tab going to the
        // background. That is not cosmetic -- CanEditStructure is
        // `!undo.InTransaction()`, so Add/Remove Component stay dead until some
        // LATER gesture force-closes the orphan and silently absorbs its stale
        // snapshots into an unrelated undo step.
        //
        // The collapse case is reachable, not theoretical: a ctrl+click text
        // entry on a DragFloat leaves g.ActiveIdAllowOverlap = !io.MouseDown[0]
        // (imgui_widgets.cpp:5004) and nothing resets it, so with the mouse up
        // the header above is still hoverable -- that flag is exactly what the
        // hover gate tests (imgui.cpp:5091) -- and a click over its arrow presses
        // on MouseDown (imgui_widgets.cpp:7012) and flips is_open in the SAME
        // frame (:7045/:7075), before the field would have been drawn. A
        // mouse-HELD drag is NOT reachable this way: it never sets
        // ActiveIdAllowOverlap, so the same gate rejects every other item.
        //
        // COMMIT, not Cancel, for both kinds of orphan:
        //   - mid-drag: the edits are already applied and the user watched them
        //     happen. Cancel drops the transaction WITHOUT reverting
        //     (CommandStack.cpp:75-82), which would leave them applied and
        //     permanently un-undoable -- the same hazard ApplyRegistryMutation
        //     refuses a structural memento over.
        //   - ctrl+click text entry: nothing was applied at all (a temp input
        //     writes only on submit). Commit re-snapshots, drops every component
        //     whose bytes match, and returns before pushing a step or clearing
        //     redo when none differ (CommandStack.cpp:61-62) -- so here Commit
        //     lands exactly where Cancel would.
        void CloseAbandonedGesture(Arcane::CommandStack& undo, InspectorState& state)
        {
            // None also covers the gesture that JOINED a gizmo drag: that drag
            // owns the transaction and closes it on mouse-up.
            if (state.gestureTxn == Arcane::TransactionId::None)
                return;
            // Healthy for exactly as long as the owning widget still holds
            // ActiveId. ImGui hands ActiveId over within ONE frame on a
            // click-through between two fields or into the search box, but the
            // field losing it is still submitted that frame, so its own
            // EndGesture has already run and cleared the token above.
            if (ImGui::GetActiveID() == state.gestureItem)
                return;
            undo.Commit(state.gestureTxn);
            state.gestureTxn  = Arcane::TransactionId::None;
            state.gestureItem = 0;
        }

        // Runs CloseAbandonedGesture on EVERY exit from DrawInspectorPanel. RAII
        // rather than a call before each ImGui::End(): the defect being fixed IS
        // a missed close path, and the panel already has an early return (no
        // selection) that is one of them. Declared as the panel's FIRST local so
        // it destructs LAST -- after ImGui::End(), on both paths, and on frames
        // where ImGui::Begin returned false (collapsed or background tab), where
        // every widget inside bails before ItemAdd on window->SkipItems (e.g.
        // imgui_widgets.cpp:2722) and so cannot report its own deactivation.
        struct GestureCloseGuard
        {
            Arcane::CommandStack& undo;
            InspectorState&       state;
            ~GestureCloseGuard() { CloseAbandonedGesture(undo, state); }
        };
    }

    void DrawInspectorPanel(Astra::Registry& registry, const SelectionContext& sel,
                            Arcane::CommandStack& undo, const SceneEditBinding& binding,
                            const Arcane::Project* project, InspectorState& state)
    {
        // FIRST local, so it destructs LAST -- see GestureCloseGuard.
        const GestureCloseGuard gestureGuard{ undo, state };

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
        // clear, so an empty query keeps the exact same -FLT_MIN full-width
        // box every other InputText in this panel uses.
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
        // state.gestureTxn before the next frame can redraw under the new
        // query and make that field vanish. A clear path that does NOT move
        // ActiveId (Escape, clear-on-selection-change) would skip that close;
        // GestureCloseGuard closes it on the way out of this panel instead, so
        // the leak is contained rather than permanent -- see the Outliner's
        // renameTarget guard (~line 537) for the same hazard class.
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
            const bool open = ImGui::CollapsingHeader(headerLabel.c_str(),
                                                      ImGuiTreeNodeFlags_DefaultOpen);
            // Safe to sit between the header and BeginPopupContextItem below,
            // which resolves its id from g.LastItemData: SetTooltip opens and
            // closes a window, and ImGui restores LastItemData in End()
            // (imgui.cpp:8849), so the popup still inherits the HEADER's id.
            // ForTooltip gates on stationary+delay (style.HoverFlagsForTooltipMouse,
            // imgui.h:1515) so scrolling past headers does not flicker a tooltip per
            // header.
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("%s", typeName.c_str());
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
                    visitor.gestureTxn  = &state.gestureTxn;
                    visitor.gestureItem = &state.gestureItem;
                    visitor.registry   = &registry;
                    visitor.selection  = &sel.Entities();
                    // Both outlive the visitor: headerLabel is this iteration's
                    // local and the query views InspectorState's buffer.
                    visitor.componentDisplayName = headerLabel;
                    visitor.query                = query;

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
                    // default. Nothing is pushed or drawn around this call, so
                    // while no field carries a Category attribute it is the same
                    // drive, over the same fields, under the same ImGui ids as
                    // before grouping existed.
                    ci.descriptor->visitFields(ci.data, visitor);

                    for (const std::string_view cat : categories)
                    {
                        // The view is into the Category attribute's own literal
                        // and is NOT guaranteed NUL-terminated, so the name goes
                        // through the begin/end PushID overload and a "%.*s"
                        // label rather than a c_str(). The sub-header's id comes
                        // from that push, not from the drawn text.
                        ImGui::PushID(cat.data(), cat.data() + cat.size());
                        if (ImGui::TreeNodeEx("##category", ImGuiTreeNodeFlags_DefaultOpen,
                                              "%.*s", static_cast<int>(cat.size()), cat.data()))
                        {
                            visitor.activeCategory = cat;
                            ci.descriptor->visitFields(ci.data, visitor);
                            ImGui::TreePop();
                        }
                        ImGui::PopID();
                    }
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
