#include "EditorPanels.hpp"
#include "ConsoleBuffer.hpp"
#include "EntityList.hpp"
#include "InspectorFields.hpp"
#include "SelectionContext.hpp"

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

    void DrawSimTimeToolbar(Arcane::RunLoop& loop)
    {
        ImGui::Begin("Sim");
        const bool paused = loop.IsPaused();
        if (ImGui::Button(paused ? "Play" : "Pause")) loop.SetPaused(!paused);
        ImGui::SameLine();
        if (ImGui::Button("Step")) loop.RequestSingleStep();
        ImGui::SameLine();
        float scale = static_cast<float>(loop.TimeScale());
        if (ImGui::SliderFloat("time-scale", &scale, 0.0f, 2.0f, "%.2f"))
            loop.SetTimeScale(scale);
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
        struct ImGuiFieldVisitor : Astra::IFieldVisitor
        {
            bool IsWriting() const noexcept override { return true; }
            void Visit(const Astra::FieldInfo& f, void* instance) override
            {
                ImGui::PushID(static_cast<int>(f.nameHash));
                const std::string label(f.name);
                switch (Grimoire::ClassifyField(f))
                {
                    case Grimoire::FieldKind::Bool:
                    {
                        bool v = f.Get<bool>(instance);
                        if (ImGui::Checkbox(label.c_str(), &v)) Grimoire::ApplyBoolEdit(f, instance, v);
                        break;
                    }
                    case Grimoire::FieldKind::Int32:
                    {
                        int v = f.Get<int32_t>(instance);
                        if (ImGui::DragInt(label.c_str(), &v)) Grimoire::ApplyIntEdit(f, instance, v);
                        break;
                    }
                    case Grimoire::FieldKind::Float:
                    {
                        float v = f.Get<float>(instance);
                        if (ImGui::DragFloat(label.c_str(), &v, 0.1f)) Grimoire::ApplyFloatEdit(f, instance, v);
                        break;
                    }
                    case Grimoire::FieldKind::Vec2:
                    {
                        glm::vec2 v = f.Get<glm::vec2>(instance);
                        if (ImGui::DragFloat2(label.c_str(), &v.x, 0.1f))
                            if (glm::vec2* p = f.GetPtr<glm::vec2>(instance)) *p = v;
                        break;
                    }
                    case Grimoire::FieldKind::Vec3:
                    {
                        glm::vec3 v = f.Get<glm::vec3>(instance);
                        if (ImGui::DragFloat3(label.c_str(), &v.x, 0.1f))
                            if (glm::vec3* p = f.GetPtr<glm::vec3>(instance)) *p = v;
                        break;
                    }
                    case Grimoire::FieldKind::ReadOnly:
                    default:
                        ImGui::BeginDisabled();
                        ImGui::Text("%s (unsupported)", label.c_str());
                        ImGui::EndDisabled();
                        break;
                }
                ImGui::PopID();
            }
        };
    }

    void DrawInspectorPanel(Astra::Registry& registry, const SelectionContext& sel)
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
                ci.descriptor->visitFields(ci.data, visitor);
            }
        }
        ImGui::End();
    }
}
