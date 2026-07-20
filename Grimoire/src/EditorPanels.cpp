#include "EditorPanels.hpp"
#include "ConsoleBuffer.hpp"
#include "EntityList.hpp"
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
                            Arcane::CommandStack& undo)
    {
        ImGui::Begin("Sim");
        if (play.IsPlaying())
        {
            if (ImGui::Button("Stop")) play.Stop(runtime, plugin);
        }
        else
        {
            // Entering Play clears the undo history: play-time mutation is
            // discarded on Stop (PlaySession restores the pre-Play snapshot),
            // so those edits must not linger as undoable Edit-mode steps.
            if (ImGui::Button("Play")) { play.Play(runtime, plugin); undo.Clear(); }
        }
        // Re-fetch AFTER a possible Stop -- Runtime::RestoreRegistry destroys and
        // replaces m_impl->loop on Stop, so a reference taken before this point
        // (e.g. at the call site) would dangle for the rest of this frame.
        Arcane::RunLoop& loop = runtime.Loop();
        ImGui::SameLine();
        if (ImGui::Button(loop.IsPaused() ? "Resume" : "Pause")) loop.SetPaused(!loop.IsPaused());
        ImGui::SameLine();
        if (ImGui::Button("Step")) loop.RequestSingleStep();
        ImGui::SameLine();
        float scale = static_cast<float>(loop.TimeScale());
        if (ImGui::SliderFloat("time-scale", &scale, 0.0f, 2.0f, "%.2f"))
            loop.SetTimeScale(scale);

        ImGui::SameLine();
        ImGui::BeginDisabled(!undo.CanUndo());
        if (ImGui::Button("Undo")) undo.Undo();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && undo.CanUndo()) ImGui::SetTooltip("Undo %s", undo.UndoLabel());
        ImGui::SameLine();
        ImGui::BeginDisabled(!undo.CanRedo());
        if (ImGui::Button("Redo")) undo.Redo();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && undo.CanRedo()) ImGui::SetTooltip("Redo %s", undo.RedoLabel());

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
                            Arcane::CommandStack& undo)
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
                visitor.stack      = &undo;
                visitor.entity     = sel.selected;
                visitor.descriptor = ci.descriptor;
                visitor.typeName   = typeName;
                ci.descriptor->visitFields(ci.data, visitor);
            }
        }
        ImGui::End();
    }
}
