#include "EditorPanels.hpp"
#include "ConsoleBuffer.hpp"
#include "EntityList.hpp"
#include "IconsLucide.h"
#include "InspectorFields.hpp"
#include "PlayMode.hpp"
#include "SelectionContext.hpp"

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Sim/RunLoop.hpp>

#include <Astra/Reflection/FieldVisitor.hpp>
#include <Astra/Registry/Registry.hpp>

#include <glm/glm.hpp>
#include <imgui.h>
#include <imgui_internal.h>   // DockBuilder* + ImGuiDockNode::LocalFlags (docking layout)

#include <cstdio>
#include <string>

namespace Grimoire
{
    void BeginDockSpace(Arcane::CommandStack& undo)
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
        ImGui::Begin("GrimoireDockHost", nullptr, flags);
        ImGui::PopStyleVar(3);

        // Editor menu bar. File items + Edit's Cut/Copy/Paste are placeholders for now;
        // Edit's Undo/Redo drive the CommandStack; Preferences is a leaf item that will
        // open a settings window later.
        if (ImGui::BeginMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                ImGui::MenuItem("New Scene");
                ImGui::MenuItem("Open Scene...");
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

        ImGui::DockBuilderDockWindow("Hierarchy", leftId);
        ImGui::DockBuilderDockWindow("Inspector", rightId);
        ImGui::DockBuilderDockWindow("Assets",    bottomId);   // Assets tab first...
        ImGui::DockBuilderDockWindow("Console",   bottomId);   // ...then Console
        ImGui::DockBuilderDockWindow("Viewport",  central);
        ImGui::DockBuilderFinish(dockspaceId);
    }

    void EndDockSpace()
    {
        const ImGuiID dockspaceId = ImGui::GetID("GrimoireDockSpace");

        // First run (no saved .ini layout): arrange the default editor layout.
        if (ImGui::DockBuilderGetNode(dockspaceId) == nullptr)
            BuildDefaultLayout(dockspaceId);

        // Emit the dockspace into the still-open host window. Anything drawn between
        // BeginDockSpace and here is a fixed strip above it.
        ImGui::DockSpace(dockspaceId, ImVec2(0, 0), ImGuiDockNodeFlags_None);

        // Lock the central node as the Viewport EVERY frame: no tab, not closeable, and
        // nothing else may dock into or undock it -- so the Viewport is always the centre
        // and its size is dictated purely by the panels split off around it.
        // (NoDockingOverMe/NoUndocking are runtime-only flags, not saved to the layout, so
        // they are re-applied here rather than trusted to persist.)
        if (ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockspaceId))
            central->LocalFlags |= ImGuiDockNodeFlags_NoTabBar
                                 | ImGuiDockNodeFlags_NoCloseButton
                                 | ImGuiDockNodeFlags_NoDockingOverMe
                                 | ImGuiDockNodeFlags_NoUndocking;

        ImGui::End();
    }

    void DrawSimTimeToolbar(PlaySession& play, Arcane::Runtime& runtime,
                            const Arcane::PluginVTable* plugin)
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
        // docked or moved. A little vertical padding sets it off from the menu bar above;
        // the trailing separator divides it from the dockspace below.
        ImGui::Dummy(ImVec2(0.0f, 3.0f));

        // -- CENTER: transport (Play / Pause / Step), Unity-style toggles. --
        // Center the trio within the full toolbar width. Undo/Redo moved to the Edit
        // menu; the transform-gizmo tools moved to the Viewport top-right overlay.
        const float fullContentW = ImGui::GetContentRegionAvail().x;
        const float lineStartX   = ImGui::GetCursorPosX();
        const ImGuiStyle& st = ImGui::GetStyle();
        auto btnW = [&](const char* icon)
        { return ImGui::CalcTextSize(icon).x + st.FramePadding.x * 2.0f; };
        const float transportW = btnW(ICON_LC_PLAY) + btnW(ICON_LC_PAUSE)
                               + btnW(ICON_LC_STEP_FORWARD) + st.ItemSpacing.x * 2.0f;
        const float centerStart = lineStartX + (fullContentW - transportW) * 0.5f;
        if (centerStart > ImGui::GetCursorPosX())
            ImGui::SetCursorPosX(centerStart);

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

        ImGui::Dummy(ImVec2(0.0f, 3.0f));
        ImGui::Separator();
    }

    void DrawAssetsPanel()
    {
        ImGui::Begin("Assets");
        // Placeholder: the asset browser lands here later.
        ImGui::TextDisabled("Assets browser -- coming soon");
        ImGui::End();
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
        // top-left), UNLESS it landed on the tool overlay above. GrimoireApp unprojects
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

    void DrawHierarchyPanel(Astra::Registry& registry, SelectionContext& sel)
    {
        ImGui::Begin("Hierarchy");
        for (Astra::Entity e : Grimoire::CollectEntities(registry))
        {
            char label[64];
            std::snprintf(label, sizeof(label), "Entity %u (v%u)",
                          (unsigned)e.GetID(), (unsigned)e.GetVersion());
            const bool isSel = sel.HasSelection() && sel.selected.GetValue() == e.GetValue();
            if (ImGui::Selectable(label, isSel))
                sel.Select(e);
        }
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

            void Visit(const Astra::FieldInfo& f, void* instance) override
            {
                ImGui::PushID(static_cast<int>(f.nameHash));
                const std::string label(f.name);
                switch (Grimoire::ClassifyField(f))
                {
                    case Grimoire::FieldKind::Bool:
                    {
                        bool v = f.Get<bool>(instance);
                        bool changed = ImGui::Checkbox(label.c_str(), &v);
                        BeginGestureIfActivated(label);
                        if (changed) Grimoire::ApplyBoolEdit(f, instance, v);
                        break;
                    }
                    case Grimoire::FieldKind::Int32:
                    {
                        int v = f.Get<int32_t>(instance);
                        bool changed = ImGui::DragInt(label.c_str(), &v);
                        BeginGestureIfActivated(label);
                        if (changed) Grimoire::ApplyIntEdit(f, instance, v);
                        break;
                    }
                    case Grimoire::FieldKind::Float:
                    {
                        float v = f.Get<float>(instance);
                        bool changed = ImGui::DragFloat(label.c_str(), &v, 0.1f);
                        BeginGestureIfActivated(label);
                        if (changed) Grimoire::ApplyFloatEdit(f, instance, v);
                        break;
                    }
                    case Grimoire::FieldKind::Vec2:
                    {
                        glm::vec2 v = f.Get<glm::vec2>(instance);
                        bool changed = ImGui::DragFloat2(label.c_str(), &v.x, 0.1f);
                        BeginGestureIfActivated(label);
                        if (changed) if (glm::vec2* p = f.GetPtr<glm::vec2>(instance)) *p = v;
                        break;
                    }
                    case Grimoire::FieldKind::Vec3:
                    {
                        glm::vec3 v = f.Get<glm::vec3>(instance);
                        bool changed = ImGui::DragFloat3(label.c_str(), &v.x, 0.1f);
                        BeginGestureIfActivated(label);
                        if (changed) if (glm::vec3* p = f.GetPtr<glm::vec3>(instance)) *p = v;
                        break;
                    }
                    case Grimoire::FieldKind::ReadOnly:
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
                            Arcane::CommandStack& undo, bool editMode)
    {
        ImGui::Begin("Inspector");
        if (!sel.HasSelection())
        {
            ImGui::TextDisabled("No selection");
            ImGui::End();
            return;
        }
        for (const Astra::Registry::ComponentInfo& ci : registry.InspectEntity(sel.selected))
        {
            if (!ci.descriptor || !ci.descriptor->visitFields || !ci.data) continue;
            // TypeMeta::typeName is a std::string_view into a substring of a larger
            // compile-time literal (__FUNCSIG__/__PRETTY_FUNCTION__) -- NOT
            // guaranteed NUL-terminated, so it is copied into a std::string before
            // handing a `const char*` to ImGui.
            const std::string typeName = ci.meta ? std::string(ci.meta->typeName) : std::string("<unreflected>");
            if (ImGui::CollapsingHeader(typeName.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGuiFieldVisitor visitor;
                // Null while Play is running: BeginGestureIfActivated/EndGesture both
                // early-return on a null stack, so gesture bracketing is fully inert
                // (no Begin, no Commit/Cancel) against the live simulating registry.
                visitor.stack      = editMode ? &undo : nullptr;
                visitor.entity     = sel.selected;
                visitor.descriptor = ci.descriptor;
                visitor.typeName   = typeName;
                ci.descriptor->visitFields(ci.data, visitor);
            }
        }
        ImGui::End();
    }
}
