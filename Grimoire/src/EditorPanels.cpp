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

#include <cstdio>
#include <string>

namespace Grimoire
{
    void BeginDockSpace()
    {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::SetNextWindowViewport(vp->ID);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("GrimoireDockHost", nullptr, flags);
        ImGui::PopStyleVar(3);
        ImGui::DockSpace(ImGui::GetID("GrimoireDockSpace"), ImVec2(0, 0), ImGuiDockNodeFlags_None);
        ImGui::End();
    }

    void DrawSimTimeToolbar(PlaySession& play, Arcane::Runtime& runtime,
                            const Arcane::PluginVTable* plugin,
                            Arcane::CommandStack& undo,
                            Arcane::GizmoMode& mode, Arcane::GizmoSpace& space)
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

        ImGui::Begin("Sim");

        // Full toolbar content width + left edge, captured before any widget, so the
        // Play/Pause/Step transport can be centered later. Unity's layout: transform
        // tools on the LEFT, the transport CENTERED.
        const float fullContentW = ImGui::GetContentRegionAvail().x;
        const float lineStartX   = ImGui::GetCursorPosX();

        // -- LEFT: transform-gizmo mode (Translate/Rotate/Scale) + space + undo/redo. --
        // T/R/S mirror the W/E/R keybinds in GrimoireApp's MainLoop input block.
        if (iconToggle(ICON_LC_MOVE, "##giz_t", mode == Arcane::GizmoMode::Translate, "Translate (W)"))
            mode = Arcane::GizmoMode::Translate;
        ImGui::SameLine();
        if (iconToggle(ICON_LC_ROTATE_3D, "##giz_r", mode == Arcane::GizmoMode::Rotate, "Rotate (E)"))
            mode = Arcane::GizmoMode::Rotate;
        ImGui::SameLine();
        if (iconToggle(ICON_LC_SCALE_3D, "##giz_s", mode == Arcane::GizmoMode::Scale, "Scale (R)"))
            mode = Arcane::GizmoMode::Scale;
        ImGui::SameLine();
        {
            bool local = (space == Arcane::GizmoSpace::Local);
            if (iconBtn(local ? ICON_LC_BOX : ICON_LC_GLOBE, "##giz_space",
                        local ? "Local space" : "World space"))
                space = local ? Arcane::GizmoSpace::World : Arcane::GizmoSpace::Local;
        }
        ImGui::SameLine(); ImGui::TextUnformatted("|"); ImGui::SameLine();
        ImGui::BeginDisabled(!undo.CanUndo());
        {
            const std::string undoTip = undo.CanUndo()
                ? (std::string("Undo ") + undo.UndoLabel())
                : std::string("Undo");
            if (iconBtn(ICON_LC_UNDO, "##sim_undo", undoTip.c_str())) undo.Undo();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!undo.CanRedo());
        {
            const std::string redoTip = undo.CanRedo()
                ? (std::string("Redo ") + undo.RedoLabel())
                : std::string("Redo");
            if (iconBtn(ICON_LC_REDO, "##sim_redo", redoTip.c_str())) undo.Redo();
        }
        ImGui::EndDisabled();

        // -- CENTER: transport (Play / Pause / Step), Unity-style toggles. --
        // Center the trio within the full toolbar width; if the left tools already
        // reach past the centered start, the transport just follows them via SameLine.
        const ImGuiStyle& st = ImGui::GetStyle();
        auto btnW = [&](const char* icon)
        { return ImGui::CalcTextSize(icon).x + st.FramePadding.x * 2.0f; };
        const float transportW = btnW(ICON_LC_PLAY) + btnW(ICON_LC_PAUSE)
                               + btnW(ICON_LC_STEP_FORWARD) + st.ItemSpacing.x * 2.0f;
        const float centerStart = lineStartX + (fullContentW - transportW) * 0.5f;
        ImGui::SameLine();
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
        // (Undo/redo/gizmo above never touch `loop`, so fetching here is safe.)
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

    ViewportPanelResult DrawViewportPanel(uint64_t textureId, uint32_t texW, uint32_t texH)
    {
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

        // Capture a left-click that lands inside the image, in viewport-local px
        // (origin = image top-left). GrimoireApp unprojects it through the plugin
        // camera and drives the entity pick; alt cycles the stacked candidates.
        if (r.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
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
