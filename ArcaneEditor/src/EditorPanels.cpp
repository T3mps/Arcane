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
#include <Arcane/Scene/Components.hpp>   // Arcane::Identity (the rename target)
#include <Arcane/Sim/RunLoop.hpp>

#include <Astra/Reflection/FieldVisitor.hpp>
#include <Astra/Registry/Registry.hpp>

#include <glm/glm.hpp>
#include <imgui.h>
#include <imgui_internal.h>   // DockBuilder* + ImGuiDockNode::LocalFlags (docking
                              // layout), ImGuiTable + TableSetColumnWidth (the
                              // Inspector grid's shared split), GetCurrentWindowRead

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
                // Upstream's own belt, spelled `data->Buf == str->c_str()` in
                // misc/cpp/imgui_stdlib.cpp -- the same pointer as data(), which is
                // what the caller below hands ImGui. It catches a user_data that is
                // not the string ImGui was given the buffer of: the assignment two
                // lines down would then point the live widget at an unrelated
                // buffer.
                IM_ASSERT(data->Buf == s->data());
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
            // Upstream's other belt (same file): CallbackResize is THIS helper's to
            // set, because it also supplies the callback that services it and `s` as
            // that callback's user data. A caller passing the flag is asking for a
            // resize hook it has no parameter to supply.
            IM_ASSERT((flags & ImGuiInputTextFlags_CallbackResize) == 0);
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

        // ---------------------------------------------------------------------
        // The two-column field grid (UE's Details-panel shape: label left in
        // one column, value right, one draggable split shared by every
        // section).
        // ---------------------------------------------------------------------

        // How much of the panel the label column takes when nothing has been
        // dragged yet. Only ever consulted once per session -- after that
        // InspectorState::labelColWidth is the authority.
        constexpr float kLabelColumnFraction = 0.4f;

        // Open one field region's grid. Returns false exactly when
        // ImGui::BeginTable did (culled/clipped host window), in which case the
        // caller must draw NO rows and must NOT call EndFieldGrid.
        //
        // WIDTH SYNC PROTOCOL. There is no ImGui API to bind two tables'
        // column widths, so InspectorState::labelColWidth is the shared
        // authority and each table is pushed to match it. The push has to
        // happen HERE -- after TableSetupColumn, before the first row -- for
        // two reasons, both from the vendored imgui_tables.cpp:
        //   - ImGui::TableSetColumnWidth asserts !IsLayoutLocked (:2343), and
        //     the first TableNextRow runs the layout (:1923-1924), which locks
        //     it (:1285).
        //   - TableSetupColumn's init width is applied ONLY while the table is
        //     initializing (:1693-1699 -> TableInitColumnDefaults :1637-1643),
        //     so it seeds a brand-new table and does nothing thereafter.
        //
        // Deciding whether to push or to ADOPT is what keeps a user drag from
        // being fought. A drag is not applied when the mouse moves: EndTable
        // records the pending width (:1531-1536) and the NEXT frame's
        // TableBegin applies it to WidthRequest via TableBeginApplyRequests
        // (:687-688), which is reached from TableBeginEx (:644) before
        // BeginTable returns. So by the time this runs, WidthRequest already
        // carries any user change.
        //
        // THE DISCRIMINATOR IS `LastResizedColumn`, NOT A WIDTH COMPARISON.
        // Comparing WidthRequest against WidthGiven looks like it should mean
        // "this split moved", and it is WRONG: imgui_internal.h:3126 says
        // outright that WidthGiven "may be > WidthRequest to honor minimum
        // width, may be < WidthRequest to honor shrinking columns down in
        // tight space" -- a legitimate PERMANENT divergence, not an event.
        // Two reachable ways to sit in it forever: WidthGiven is ImTrunc'd off
        // WidthRequest (:1054), so any fractional width diverges every frame;
        // and :1139 clamps WidthGiven by WidthMax, which a category grid's
        // TreeNode indent alone is enough to trigger. A table stuck in the
        // "moved" branch would adopt its own width over the shared one every
        // frame -- its push branch dead, so it could never follow another
        // section, and its stale number would clobber a real drag elsewhere.
        //
        // LastResizedColumn has exactly two writers, and both facts about
        // them are load-bearing.
        //
        // First, the ctor memset writes 0 -- ImGuiTable's ctor is
        // memset(this, 0, sizeof(*this)) (imgui_internal.h:3332), so 0 is
        // literally the value == 0 tests against -- but that write is never
        // OBSERVABLE by this code: a table whose pool slot was just
        // constructed also has RawData == NULL, which forces IsInitializing
        // = true (imgui_tables.cpp:577) and then :589 overwrites
        // LastResizedColumn to -1 as part of that init, all inside
        // BeginTableEx and before TableBeginApplyRequests runs (:644) and
        // before BeginTable returns to this call site. So `== 0` never fires
        // off the ctor's zero; it always means a real resize.
        //
        // Second, the :689 writer is UNCONDITIONAL, not resize-only: every
        // frame's instance-0 TableBegin runs `LastResizedColumn =
        // ResizedColumn` regardless of whether a resize happened this frame
        // (:687-691) -- ResizedColumn itself is reset to -1 at :691 right
        // after, so on a quiet frame this assigns the idle -1 right back.
        // That per-frame re-arm is what makes `LastResizedColumn == 0` an
        // EVENT rather than a latch: without it, a resize on column 0 once
        // would read as "still resized" on every later frame too.
        //
        // Everything else in the file only reads it. So once BeginTable has
        // returned, `LastResizedColumn == 0` means precisely "column 0 of THIS
        // table just had a queued resize applied this frame", which is the
        // event we want and nothing else.
        //
        // DECISION -- double-click auto-fit does NOT win. It is applied
        // through AutoFitSingleColumn (:695-699), which does not touch
        // LastResizedColumn, so it is not adopted and the shared width is
        // pushed back over it on the same frame. That is intended: ONE split
        // shared by every section is the feature, and a section auto-fitting
        // itself to a width of its own would break exactly that invariant.
        [[nodiscard]] bool BeginFieldGrid(InspectorState& state)
        {
            // Read BEFORE BeginTable: inside a table, "available" is a cell.
            // Truncated so the seed is idempotent under :1054's ImTrunc --
            // a fractional shared width would come back different from every
            // table it is pushed onto, which is noise nothing here needs.
            if (state.labelColWidth <= 0.0f)
            {
                const float avail = ImGui::GetContentRegionAvail().x;
                if (avail > 0.0f)
                    state.labelColWidth = ImTrunc(avail * kLabelColumnFraction);
            }
            // NoSavedSettings is passed explicitly even though a table inside a
            // child window inherits it anyway (:299-301): OUR float is the only
            // width authority, and an .ini-restored width would be a second one.
            if (!ImGui::BeginTable("##fields", 2,
                                   ImGuiTableFlags_Resizable |
                                   ImGuiTableFlags_NoSavedSettings |
                                   ImGuiTableFlags_NoBordersInBodyUntilResize))
                return false;
            // A <= 0 width here leaves the column auto-sized (:1640 stores -1
            // for it), which happens only on a frame where the panel had no
            // width to sample above; the sync below takes over once one exists.
            ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed,
                                    state.labelColWidth);
            ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

            ImGuiTable* table = ImGui::GetCurrentTable();
            ImGuiTableColumn& col = table->Columns[0];
            // The `> 0` on the request is not decoration: an auto-sized column
            // carries WidthRequest == -1 (:1640), which is what the degenerate
            // seeding path above leaves behind, and adopting it would hand the
            // whole panel a negative shared width.
            if (table->LastResizedColumn == 0 && col.WidthRequest > 0.0f)
            {
                state.labelColWidth = col.WidthRequest;   // the user moved THIS split
            }
            // Both guards keep TableSetColumnWidth away from a state it would
            // turn into a collapsed column rather than a no-op: it ImClamps to
            // at least MinColumnWidth (:2353), so pushing an unseeded 0 would
            // pin the column there; and WidthGiven == 0 is a table that has
            // never laid out (its column is memset in the constructor,
            // imgui_internal.h:3168-3170), where WidthMax is still 0 and the
            // same clamp (:2352) would collapse it -- there the init width
            // above is the seed. Otherwise this runs unconditionally: the
            // early-out at :2354 makes the steady-state push a no-op, and
            // TableSaveSettings returns immediately under NoSavedSettings
            // (:3772-3773), so a repeated push costs nothing.
            else if (state.labelColWidth > 0.0f && col.WidthGiven > 0.0f)
            {
                ImGui::TableSetColumnWidth(0, state.labelColWidth);
            }
            return true;
        }

        // Close a grid opened by BeginFieldGrid. Takes no state because the
        // whole width hand-off happens in BeginFieldGrid: a drag only reaches
        // WidthRequest at the NEXT frame's BeginTable (see above), so a
        // write-back here would read a width the drag has not landed in yet and
        // the following frame would push that stale number back over it.
        void EndFieldGrid()
        {
            ImGui::EndTable();
        }

        // One field row's label cell. Opens the row, writes the display name
        // into column 0, and leaves the cursor in column 1 with the next item
        // sized to fill it.
        //
        // Returns whether the LABEL is hovered. The label is its own ImGui item
        // now, so the row's tail tooltip -- which asks about the LAST item, i.e.
        // the value widget -- would never fire over the name; the caller ORs
        // this in. Asked here, while the label still IS the last item.
        //
        // `dimmed` is UE's disabled-label treatment for a field that cannot be
        // edited. The color push is exactly what ImGui::TextDisabled does
        // (imgui_widgets.cpp:316-322), spelled out so the text can go through
        // TextUnformatted rather than a format string.
        [[nodiscard]] bool FieldLabelCell(const std::string& label, bool dimmed)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            // The value cell holds a FRAMED widget on almost every row, whose
            // text sits FramePadding.y below the row top; bare text draws at
            // DC.CurrLineTextBaseOffset (imgui_widgets.cpp:177), which is 0 here
            // -- so without this the name rides high against its own value. A
            // table cannot fix it afterwards: TableEndCell only raises
            // RowTextBaseline for cells submitted LATER in the row
            // (imgui_tables.cpp:2273), and this is the first one.
            ImGui::AlignTextToFramePadding();
            if (dimmed)
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextUnformatted(label.c_str());
            if (dimmed)
                ImGui::PopStyleColor();
            const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
            ImGui::TableSetColumnIndex(1);
            // -FLT_MIN is ImGui's "fill the remaining width" spelling
            // (CalcItemWidth resolves a negative width against
            // GetContentRegionAvail, imgui.cpp:12319-12323), and inside a cell
            // that region IS the cell.
            ImGui::SetNextItemWidth(-FLT_MIN);
            return hovered;
        }

        // ---------------------------------------------------------------------
        // Axis color bars (UE's Details-panel treatment for vector components:
        // X red, Y green, Z blue on the left edge of each component's frame).
        // ---------------------------------------------------------------------

        // Sampled off the UE reference screenshot -- deliberately muted, unlike
        // the saturated primaries ImGui's own component markers use
        // (GDefaultRgbaColorMarkers is 240/20/20, 20/240/20, 20/20/240 --
        // imgui_widgets.cpp:2257-2260).
        constexpr ImU32 kAxisBarColors[3] = {
            IM_COL32(196,  64,  54, 255),   // X
            IM_COL32( 96, 166,  58, 255),   // Y
            IM_COL32( 58, 122, 196, 255),   // Z
        };

        // How wide the strip is, in pixels. Matches ImGuiStyle::ColorMarkerSize's
        // own default (imgui.cpp:1564), which is the width the vendored marker
        // renderer would have used.
        constexpr float kAxisBarWidth = 3.0f;

        // Paint the axis strip over the left edge of the item just submitted.
        // `component` indexes kAxisBarColors; an index past the palette draws
        // nothing, so a row wider than three components degrades quietly rather
        // than reading out of bounds.
        //
        // An OVERLAY on purpose: it runs AFTER the widget, so it pushes no style
        // and cannot move layout. It sits flush on the frame's corners because
        // nothing in this editor overrides ImGuiStyle::FrameRounding, whose
        // default is 0.0f (imgui.cpp:1532); a rounded frame would instead want
        // the vendored RenderColorComponentMarker (imgui.cpp:4089-4096), which
        // rounds -- but that one is reachable only from DragScalar/SliderScalar
        // (imgui_widgets.cpp:2791, :3384) and so cannot serve the multi-select
        // text boxes, which need the same strip.
        void DrawAxisBar(int component)
        {
            if (component < 0 || component >= IM_ARRAYSIZE(kAxisBarColors))
                return;
            // A window that is skipping items submitted nothing: both DragScalar
            // (imgui_widgets.cpp:2721-2723) and InputTextEx (:4708-4710) return
            // on that flag BEFORE ItemAdd, so g.LastItemData still describes some
            // EARLIER item and a bar taken from its rect would be painted onto
            // that one.
            if (ImGui::GetCurrentWindowRead()->SkipItems)
                return;
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRectFilled(
                min, ImVec2(min.x + kAxisBarWidth, max.y), kAxisBarColors[component]);
        }

        // ---------------------------------------------------------------------
        // Header bands (UE's Details treatment for CollapsingHeader/TreeNodeEx:
        // a muted dark band in place of the theme's default bright-blue
        // ImGuiCol_Header). Beside the axis palette per Section 2's rule that
        // inspector style constants live in one place.
        // ---------------------------------------------------------------------

        // Sampled off the UE reference screenshot, same as kAxisBarColors --
        // desk call, not measured off UE pixels. Hover/active step up in
        // lightness so the row still visibly responds to input.
        constexpr ImU32 kHeaderBandColor        = IM_COL32(48, 48, 52, 255);
        constexpr ImU32 kHeaderBandHoveredColor = IM_COL32(58, 58, 64, 255);
        constexpr ImU32 kHeaderBandActiveColor  = IM_COL32(66, 66, 73, 255);

        // Push/pop as a matched pair so every call site pushes and pops the
        // same 3 colors, rather than trusting three inline pushes (and three
        // inline pops) to stay in sync at each of the two header call sites.
        // Both CollapsingHeader (Framed) and TreeNodeEx (unframed) resolve
        // their background from this same triple, picked by hover/held state
        // (imgui_widgets.cpp:7102 framed, :7123 unframed), so one push covers
        // either caller.
        void PushHeaderBandColors()
        {
            ImGui::PushStyleColor(ImGuiCol_Header, kHeaderBandColor);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kHeaderBandHoveredColor);
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, kHeaderBandActiveColor);
        }

        void PopHeaderBandColors()
        {
            ImGui::PopStyleColor(3);
        }

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

        // The component drags for a single-selection Vec2/Vec3 row, spelled out
        // rather than calling ImGui::DragFloat2/3.
        //
        // WHY NOT DragFloat2/3: the bar needs each component's OWN frame rect,
        // and DragScalarN submits its components internally -- by the time it
        // returns, EndGroup has overwritten g.LastItemData.Rect with the group's
        // bounding box (imgui.cpp:12483), which is the only rect the caller can
        // see. Recovering the components from that would mean re-deriving
        // PushMultiItemsWidths' split arithmetic (imgui.cpp:12283-12291) out
        // here, against an internal layout detail no API contract holds still.
        //
        // This IS DragScalarN's body (imgui_widgets.cpp:2814-2849) specialised to
        // float with no bounds, with the bar added, the trailing
        // visible-label block (:2840-2845) dropped -- already dead for these
        // callers, whose labels are the "##name" hidden-id form -- and the
        // `flags` parameter dropped along with it, which also drops the
        // ImGuiSliderFlags_ColorMarkers branch it gates (:2831-2832): none of
        // these callers pass flags, so that branch was already unreachable
        // here too. An ImGui upgrader re-diffing this against the vendored
        // body should expect both omissions, not just the label block:
        // FindRenderedTextEnd stops at the leading "##" (imgui.cpp:3918) and
        // returns the string start, so :2841's
        // `label != label_end` is false. Everything ids and undo depend on is
        // therefore unchanged: the same BeginGroup/EndGroup, the same
        // PushID(label) + PushID(i) nesting over the same "" child labels, so
        // each component keeps the exact ImGui id DragFloat2/3 gave it, and the
        // caller's BeginGestureIfActivated/EndGesture still read the id EndGroup
        // forwards out of the group (imgui.cpp:12477-12482) exactly as before.
        [[nodiscard]] bool AxisDragFloatN(const char* label, float* v, int count, float speed)
        {
            // DragScalarN's own guard (:2816-2818), kept in the same place and
            // for the same reason: it returns before the group opens, so there
            // is nothing to unwind.
            if (ImGui::GetCurrentWindowRead()->SkipItems)
                return false;
            bool changed = false;
            ImGui::BeginGroup();
            ImGui::PushID(label);
            ImGui::PushMultiItemsWidths(count, ImGui::CalcItemWidth());
            for (int i = 0; i < count; ++i)
            {
                ImGui::PushID(i);
                if (i > 0)
                    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
                // `speed` is the only argument these callers ever varied; every
                // other one is DragFloat's default, and DragFloat's defaults ARE
                // DragFloat2/3's defaults (imgui.h:687-689), so each component
                // behaves exactly as it did inside the combined widget.
                // Written as an if rather than |= only to keep the assignment
                // bool-typed; like DragScalarN's |= it does not short-circuit,
                // so every component is always submitted.
                if (ImGui::DragFloat("", &v[i], speed))
                    changed = true;
                DrawAxisBar(i);
                ImGui::PopID();
                ImGui::PopItemWidth();
            }
            ImGui::PopID();
            ImGui::EndGroup();
            return changed;
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
            // Sprite-asset arc, Task 4: texture-drop auto-mint on a Sprite-typed
            // AssetRef field. Wired UNCONDITIONALLY today -- EditorAppFrame.cpp
            // passes &m_inspectorServices on every DrawInspectorPanel call, and
            // EditorApp::Init sets mintSpriteForTexture unconditionally (not
            // gated on Play/Edit) -- so in the shipping app this is never
            // actually null. The null check in the AssetRef arm below is
            // defensive, for a caller that does not wire InspectorServices at
            // all (DrawInspectorPanel's `services` parameter defaults to
            // nullptr).
            //
            // Play-mode note: `stack` above IS gated (null while Play runs,
            // binding.editMode -> nullptr), but `services` is not, so a
            // texture drop during Play still mints/reuses the .arcsprite and
            // calls ApplyGuidImmediate, which applies the Guid edit via its
            // unconditional ForEachTarget even with stack == nullptr (it only
            // skips opening the ScopedTransaction and taking the per-target
            // Snapshot -- both live inside the same `if (stack)` block,
            // EditorPanels.cpp:1797-1802). A Play-mode texture drop
            // therefore mints the file and writes the Guid with NO undo step
            // -- exactly the no-undo-in-Play behavior every other AssetRef
            // drop already has; this branch adds a minted file as a
            // consequence, not a new undo hole.
            const InspectorServices*          services = nullptr;

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
            // The string row's activation-time cancel reference and the id of
            // the row that latched it, both parked in the same persistent
            // InspectorState for the same reason -- see the String arm below
            // and InspectorState in EditorPanels.hpp. Wired unconditionally by
            // DrawInspectorPanel (they describe ImGui cancel semantics, not
            // undo, so they matter while Play runs too); the arm still falls
            // back to a per-frame seed if either is null.
            std::string*                      stringSeed = nullptr;
            ImGuiID*                          stringSeedItem = nullptr;

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
                    // transaction still open, return None (CommandStack.cpp:16-17),
                    // and A's later Commit(None) would no-op -- leaving A's
                    // transaction open forever holding stale `before` bytes. Close
                    // A's here so its edit lands, then open ours. A's own EndGesture
                    // still runs later this frame, but the slot below now names B as
                    // the owner, so EndGesture's ownership guard makes it return --
                    // it does NOT hand B's LIVE token to Commit/Cancel.
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
                    // through AxisDragFloatN, where EndGroup forwards the live component
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
            //
            // `idSeed` is an id scope only -- it keeps the N boxes' ids off the
            // row's other items -- and is NOT drawn: the display name lives in
            // the grid's label column now, so the trailing SameLine'd text this
            // row used to end on is gone with it.
            //
            // `axisColors` asks for the X/Y/Z strip on each box. It is a
            // parameter rather than `count > 1` because the two are not the same
            // question: this row also serves plain scalars, and a future
            // two-box row that is NOT a vector (a min/max pair, say) would be
            // silently painted red/green by that shortcut.
            int MultiScalarRow(const char* idSeed, int count, const double* vals,
                               const Arcane::Editor::FieldMixedMask& mask, bool integral,
                               bool axisColors, double& outValue)
            {
                int committed = -1;
                ImGui::BeginGroup();
                ImGui::PushID(idSeed);
                // Fills the value cell: the caller's SetNextItemWidth(-FLT_MIN)
                // is still pending here (nothing between it and this call
                // submits an item, and PushMultiItemsWidths consumes the flag
                // itself at imgui.cpp:12292), so CalcItemWidth resolves to the
                // cell's full width and the boxes split THAT.
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
                    // After the commit block, not before it: everything above
                    // reads g.LastItemData (IsItemDeactivatedAfterEdit), and
                    // putting the decoration last means no reader has to
                    // re-establish that a draw-list call left that state alone.
                    if (axisColors)
                        DrawAxisBar(i);
                    ImGui::PopID();
                    ImGui::PopItemWidth();
                }
                ImGui::PopID();
                ImGui::EndGroup();
                return committed;
            }

            void EndGesture()
            {
                if (!stack) return;
                // ONLY THE WIDGET THAT OPENED THE GESTURE MAY CLOSE IT. This runs for
                // every row, and a row that never opened a gesture still reports its
                // own deactivation: click into a string box, then press on a drag
                // drawn ABOVE it, and in ONE frame the drag activates (parking its
                // LIVE token in *gestureTxn) while the string box, submitted after
                // it, reports IsItemDeactivated -- imgui.cpp:6559 answers from
                // DeactivatedItemData.ID, which is the string box's own id. Without
                // this guard that click-without-typing would Cancel the drag's
                // transaction, and Cancel discards WITHOUT reverting
                // (CommandStack.cpp:75-82), so the whole drag stays applied and
                // permanently un-undoable.
                //
                // GetItemID() is g.LastItemData.ID (imgui.cpp:6676-6680), the same
                // value BeginGestureIfActivated recorded at activation, for every
                // shape this panel draws:
                //   - bare DragFloat/DragInt/Checkbox: ItemAdd stamps it on both
                //     frames (imgui.cpp:11979).
                //   - ctrl+click temp input: the id comes from the same window+label
                //     (imgui_widgets.cpp:2727, :4728) and the temp input skips
                //     ItemAdd (:4791-4792), so the drag's own id stands.
                //   - AxisDragFloatN and MultiScalarRow (groups): EndGroup re-points
                //     LastItemData.ID at the group's live ActiveId, ELSE at the child
                //     that deactivated inside it (imgui.cpp:12477-12482) -- so the
                //     activation frame yields the child that took ActiveId, and the
                //     deactivation frame that same child.
                //     One such frame is tabbing .x -> .y inside ONE group, where
                //     those two branches disagree (the live-ActiveId one wins and
                //     forwards .y while .x deactivates); any ActiveId handoff
                //     between siblings of one group does the same. But such a
                //     frame is also an
                //     activation: BeginGestureIfActivated has already committed .x's
                //     gesture and parked .y here, so this compares .y against .y.
                //     Unchanged by this guard -- it is what that path already did.
                if (ImGui::GetItemID() != *gestureItem)
                    return;
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
                // "this panel has no widget for that type"; a field can be
                // either, both, or neither, and the two dim different halves of
                // the row: this one disables the value WIDGET, that one has no
                // widget to disable and greys its value TEXT. Both grey the
                // label, via the flag handed to FieldLabelCell.
                const bool readOnly = Arcane::Editor::FieldIsReadOnly(f);
                // Classified once, above the switch: the label cell needs the
                // answer before the switch that used to ask for it.
                const Arcane::Editor::FieldKind kind = Arcane::Editor::ClassifyField(f);
                // Every widget below draws with its visible label HIDDEN -- the
                // display name is its own item in column 0 now, so a widget
                // still drawing its own would double it. "##", not "###":
                // "##" hides the text while leaving the id seeded by the rest of
                // the string, "###" would reseed the hash (ImHashStr,
                // imgui.cpp:2557). rawName, not label, so the id follows the C++
                // identifier rather than prose that a display-name tweak moves.
                const std::string widgetId = "##" + rawName;
                // The label cell OPENS THE ROW, so it has to run before anything
                // in the value cell -- and deliberately before BeginDisabled: a
                // disabled scope also multiplies alpha (imgui.cpp:8899-8900),
                // which on top of the grey below would dim the name twice.
                const bool labelHovered =
                    FieldLabelCell(label, readOnly || kind == Arcane::Editor::FieldKind::ReadOnly);
                if (readOnly)
                    ImGui::BeginDisabled();

                // Does this row want the tooltip below? An arm whose LAST item is
                // not the thing the user points at has to answer for itself while
                // its own widget is still the last item -- only the asset-ref arm
                // is in that position, and it fills this in. Left unset, the tail
                // asks about the last item, which is that row's own content.
                std::optional<bool> hovered;

                switch (kind)
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
                        bool changed = ImGui::Checkbox(widgetId.c_str(), &v);
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
                            if (MultiScalarRow(widgetId.c_str(), 1, &cur, MixedFor(f),
                                               /*integral*/ true, /*axisColors*/ false,
                                               out) >= 0)
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
                        bool changed = RangedDragInt(f, widgetId.c_str(), &v);
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
                            if (MultiScalarRow(widgetId.c_str(), 1, &cur, MixedFor(f),
                                               /*integral*/ false, /*axisColors*/ false,
                                               out) >= 0)
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
                        bool changed = RangedDragFloat(f, widgetId.c_str(), &v, 0.1f);
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
                            const int c = MultiScalarRow(widgetId.c_str(), 2, cur, MixedFor(f),
                                                         /*integral*/ false, /*axisColors*/ true,
                                                         out);
                            if (c >= 0)
                            {
                                const float fv = static_cast<float>(out);
                                ApplyImmediate(rawName, instance, [&](void* d)
                                               { if (glm::vec2* p = f.GetPtr<glm::vec2>(d)) (*p)[c] = fv; });
                            }
                            break;
                        }
                        bool changed = AxisDragFloatN(widgetId.c_str(), &v.x, 2, 0.1f);
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
                            const int c = MultiScalarRow(widgetId.c_str(), 3, cur, MixedFor(f),
                                                         /*integral*/ false, /*axisColors*/ true,
                                                         out);
                            if (c >= 0)
                            {
                                const float fv = static_cast<float>(out);
                                ApplyImmediate(rawName, instance, [&](void* d)
                                               { if (glm::vec3* p = f.GetPtr<glm::vec3>(d)) (*p)[c] = fv; });
                            }
                            break;
                        }
                        bool changed = AxisDragFloatN(widgetId.c_str(), &v.x, 3, 0.1f);
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
                        // browser drags; an "x" clears an EDITABLE one (see below).
                        // Kind filter is inferred from the field name
                        // (AssetKindFilterForFieldName).
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
                        // This row may end on the clear button rather than on the
                        // asset button, so the tail below would ask about THAT and
                        // the identifier tooltip would never appear over the
                        // asset. Asked here, while the button IS the last item.
                        hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
                        // `!readOnly` GUARDS THE DROP, and the BeginDisabled wrap
                        // above does NOT: drag-drop acceptance never consults
                        // ImGuiItemFlags_Disabled. BeginDragDropTarget tests only
                        // DragDropActive, the item's ImGuiItemStatusFlags_HoveredRect,
                        // and the hovered window (imgui.cpp:15823-15834), and ItemAdd
                        // stamps HoveredRect from a plain IsMouseHoveringRect
                        // regardless of the disabled flag (imgui.cpp:12070-12071);
                        // AcceptDragDropPayload checks the payload type and the target
                        // rect, nothing about being disabled (imgui.cpp:15860-15905).
                        // So a drag from the Asset Browser onto the disabled
                        // Identity::id row landed here and fanned that asset's Guid
                        // into the durable identity of every selected entity --
                        // AssetKindFilterForFieldName("id") returns -1, which accepts
                        // ANY kind. Read-only means read-only on every path in.
                        if (!readOnly && ImGui::BeginDragDropTarget())
                        {
                            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(Arcane::Editor::kAssetDragType))
                            {
                                const auto* payload = static_cast<const Arcane::Editor::AssetDragPayload*>(p->Data);
                                if (kindFilter < 0 || static_cast<int>(payload->kind) == kindFilter)
                                    ApplyGuidImmediate(rawName, f, instance, payload->guid);
                                // Sprite-asset arc, Task 4: dropping a TEXTURE on a sprite-typed
                                // field auto-mints (or reuses) the wrapping .arcsprite -- Unity's
                                // drop-and-go on UE's explicit-asset storage (sprite-asset spec,
                                // Section 3). The mint runs OUTSIDE ApplyGuidImmediate's
                                // ScopedTransaction (see MintOrReuseSpriteForTexture,
                                // EditorAppProject.cpp), so undo covers only the Guid edit --
                                // undoing this drop does NOT delete the minted file, same as any
                                // created asset outliving a later edit to a field referencing it.
                                else if (kindFilter == static_cast<int>(Arcane::Editor::AssetKind::Sprite)
                                        && payload->kind == Arcane::Editor::AssetKind::Texture
                                        && services && services->mintSpriteForTexture)
                                {
                                    if (const Arcane::Guid minted = services->mintSpriteForTexture(payload->guid);
                                        minted.IsValid())
                                        ApplyGuidImmediate(rawName, f, instance, minted);
                                }
                            }
                            ImGui::EndDragDropTarget();
                        }
                        // Offered when MIXED too: "clear all of them" is a
                        // meaningful action even though the primary may be nil.
                        //
                        // NOT offered on a read-only ref, where it used to render
                        // as a disabled "x" -- an affordance promising an action
                        // that is not merely unavailable right now but can never
                        // exist. Dropping it removes no reachable write: a
                        // disabled item is refused by ItemHoverable
                        // (imgui.cpp:5128-5134), so ButtonBehavior could never
                        // press it. Note this is the OPPOSITE of the drop target
                        // above, which the disabled wrap does not gate at all --
                        // hence its own explicit `!readOnly`.
                        if (!readOnly && (v.IsValid() || refMixed))
                        {
                            ImGui::SameLine();
                            if (ImGui::SmallButton("x##assetclear"))
                                ApplyGuidImmediate(rawName, f, instance, Arcane::Guid::Nil());
                        }

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
                        const std::string shown = text;   // what this frame rendered
                        InputTextString(widgetId.c_str(), &text);
                        // LATCH THE CANCEL REFERENCE AT ACTIVATION. ImGui copies
                        // the buffer it was handed into TextToRevertTo when the
                        // box takes ActiveId (imgui_widgets.cpp:4865-4866) and
                        // writes exactly that back on Escape (:5300-5308) -- so
                        // the reference this guard needs is the ACTIVATION-time
                        // text, and `shown` is that text on this frame (ImGui
                        // writes into the buffer only on a change, and the
                        // activation frame has none yet).
                        //
                        // The per-frame seed this replaced was NOT that reference
                        // whenever the value moved externally mid-edit -- an
                        // Outliner rename committing a frame after this box was
                        // activated on the same entity. Escape then restored the
                        // OLD name into `text`, the guard saw text != seed, and
                        // the documented CANCEL path pushed an undo entry.
                        // Latched, seed == TextToRevertTo by construction, so a
                        // revert is a guaranteed equality no-op no matter what
                        // moved underneath; a real commit still differs and still
                        // writes.
                        //
                        // For a MIXED multi-selection the latched value is the
                        // blank the user saw at activation -- which is right:
                        // Escape restores that same blank, so an untouched mixed
                        // row still commits nothing.
                        if (ImGui::IsItemActivated() && stringSeed && stringSeedItem)
                        {
                            *stringSeed     = shown;
                            *stringSeedItem = ImGui::GetItemID();
                        }
                        // The latch is ONE slot shared by every string row, so
                        // use it only when it names THIS row -- click into row A,
                        // then click row B drawn above it and B re-latches in the
                        // same frame A reports its deactivation. Same ownership
                        // guard as EndGesture's.
                        const bool latched = stringSeed && stringSeedItem
                                          && *stringSeedItem == ImGui::GetItemID();
                        const std::string& seed = latched ? *stringSeed : shown;
                        // Commit on deactivation, guarded by that equality test --
                        // and the guard IS the cancel path, not a nicety. Escape
                        // also sets value_changed (imgui_widgets.cpp:5300-5309), so
                        // IsItemDeactivatedAfterEdit would report a revert as an
                        // edit; "it deactivated and ended up different from what it
                        // started as" is the honest predicate, and it also makes a
                        // plain click-in-click-out write nothing.
                        //
                        // ApplyImmediate, not a widget gesture: the commit is one
                        // discrete event, and it fans the typed text to every
                        // selected entity as ONE undo step.
                        if (ImGui::IsItemDeactivated() && text != seed)
                            ApplyImmediate(rawName, instance, [&](void* d)
                                           { Arcane::Editor::ApplyStringEdit(f, d, text); });
                        break;
                    }
                    case Arcane::Editor::FieldKind::ReadOnly:
                    default:
                        // A type this panel has no widget for. It keeps the grid's
                        // rhythm rather than breaking out of it as bare mid-list
                        // text: the name is in column 0 (greyed, like every other
                        // uneditable label) and only the word goes here. The C++
                        // type is deliberately absent -- the row's tooltip already
                        // carries the raw identifier, which is what you grep for.
                        //
                        // TextDisabled rather than this arm opening a
                        // BeginDisabled of its own: on a field that is ONLY
                        // unsupported (the common case) it is the same grey the
                        // label cell pushes, so the two halves of a dead row
                        // match. A field that is ALSO Astra::ReadOnly is still
                        // inside that wrap and so does get both treatments --
                        // TextDisabled's colour over the disabled alpha
                        // (imgui.cpp:8899-8900) -- which is accepted: it is the
                        // rarest row in the panel and the extra dimming is not
                        // wrong, merely darker than its label.
                        //
                        // AlignTextToFramePadding for the ROW HEIGHT, not the
                        // baseline (the label cell already handed this cell its
                        // baseline via RowTextBaseline): it is what keeps a
                        // widget-less row as tall as its neighbours instead of
                        // collapsing to a line of text.
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextDisabled("unsupported");
                        break;
                }
                EndGesture();
                if (readOnly)
                    ImGui::EndDisabled();

                // Every arm but the asset-ref one ends on the row's own content, so
                // asking about the LAST item is asking about the field: ImGui
                // restores g.LastItemData when a window closes (imgui.cpp:8849), so
                // neither the asset-pick popup nor the SetTooltip below can
                // retarget it. The asset-ref arm may end on its clear button
                // instead and has already answered above. Asked after EndGesture
                // for the same reason the gesture is bracketed at all -- nothing
                // may sit between a widget and the item-state reads that close its
                // transaction.
                //
                // ORed with the label cell's own answer: the display name is a
                // separate item in column 0 now, and pointing at it is pointing at
                // the field. (Before the grid, a numeric row's name was INSIDE the
                // widget's rect -- DragScalar builds total_bb from its label,
                // imgui_widgets.cpp:2734, and registers THAT at :2738 -- so the
                // single query covered both.)
                //
                // The raw identifier is always in the tooltip, so a friendly label
                // never costs the ability to grep for the field. ForTooltip adds a
                // stationary+delay gate (style.HoverFlagsForTooltipMouse, imgui.h:1515)
                // so sweeping the cursor down the panel does not flicker a tooltip per
                // row; that default already carries AllowWhenDisabled, so an
                // Astra::ReadOnly row still explains itself on hover.
                if (!hovered.has_value())
                    hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
                if (labelHovered || *hovered)
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
                            const Arcane::Project* project, InspectorState& state,
                            const InspectorServices* services)
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
        // state.gestureTxn before the next frame can redraw under the new
        // query and make that field vanish. A clear path that does NOT move
        // ActiveId (Escape, clear-on-selection-change) would skip that close;
        // GestureCloseGuard closes it on the way out of this panel instead, so
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
            PushHeaderBandColors();
            const bool open = ImGui::CollapsingHeader(headerLabel.c_str(),
                                                      ImGuiTreeNodeFlags_DefaultOpen);
            PopHeaderBandColors();
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
                    visitor.services   = services;
                    visitor.gestureTxn  = &state.gestureTxn;
                    visitor.gestureItem = &state.gestureItem;
                    // Wired even while Play runs (unlike `stack`): these carry
                    // the string row's Escape-cancel semantics, which are
                    // ImGui's, not the undo stack's.
                    visitor.stringSeed     = &state.stringEditSeed;
                    visitor.stringSeedItem = &state.stringEditItem;
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
                    // default. While no field carries a Category attribute this
                    // is the same drive over the same fields as before grouping
                    // existed.
                    //
                    // The grid is opened HERE rather than inside the visitor so
                    // the component header and the category sub-headers below
                    // stay full-width, exactly as UE lays them out; the visitor
                    // only ever emits rows. BeginFieldGrid returning false is
                    // ImGui culling the table, and then no row may be submitted
                    // -- the same "field stopped being drawn" shape as a
                    // collapsed header, which GestureCloseGuard already covers.
                    if (BeginFieldGrid(state))
                    {
                        ci.descriptor->visitFields(ci.data, visitor);
                        EndFieldGrid();
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
                        PushHeaderBandColors();
                        const bool categoryOpen = ImGui::TreeNodeEx("##category",
                            ImGuiTreeNodeFlags_DefaultOpen,
                            "%.*s", static_cast<int>(cat.size()), cat.data());
                        PopHeaderBandColors();
                        if (categoryOpen)
                        {
                            visitor.activeCategory = cat;
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
                            if (BeginFieldGrid(state))
                            {
                                ci.descriptor->visitFields(ci.data, visitor);
                                EndFieldGrid();
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
