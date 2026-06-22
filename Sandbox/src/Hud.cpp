// Hud implementation (Task 8). See Hud.hpp for the section map + design notes.
//
// SCENE SWITCH / RESET ARE DEFERRED, NOT IMMEDIATE: Draw runs in the render phase
// (the host calls GamePlugin_DrawUI between ImGuiLayer BeginFrame/Render). A scene
// rebuild calls reg.Clear() + mints a fresh PhysicsResource, which must NOT happen
// mid-render. So the HUD requests a switch/reset through the SceneControl side channel
// (requestedScene / requestReset); SandboxApp::FixedUpdate consumes it next fixed step,
// on a clean registry -- the EXACT contract the SandboxSmokeTest already drives. The
// gravity slider rides the same path: SetGravityY then request a reset (the PhysicsWorld
// bakes gravity at construction, so a fresh world is the clean way to change it).

#include "Hud.hpp"
#include "SandboxApp.hpp"
#include "Scenes.hpp"

#include <Arcane/Scene/PhysicsSystem.hpp>   // PhysicsResource (live body/contact counts)

#include <Astra/Registry/Registry.hpp>

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <span>

namespace Arcane::Sandbox
{
    namespace
    {
        // Request a scene switch via the SceneControl side channel (consumed next
        // FixedUpdate on a clean registry). Idempotent; creates the resource if absent.
        void RequestScene(Astra::Registry& reg, int index)
        {
            SceneControl* ctrl = reg.GetResource<SceneControl>();
            if (!ctrl)
            {
                reg.SetResource(SceneControl{});
                ctrl = reg.GetResource<SceneControl>();
            }
            if (ctrl) ctrl->requestedScene = index;
        }

        // Request a rebuild of the current scene (same clean deferred path).
        void RequestReset(Astra::Registry& reg)
        {
            SceneControl* ctrl = reg.GetResource<SceneControl>();
            if (!ctrl)
            {
                reg.SetResource(SceneControl{});
                ctrl = reg.GetResource<SceneControl>();
            }
            if (ctrl) ctrl->requestReset = 1;
        }
    }

    void Hud::Draw(SandboxApp& app, Astra::Registry& reg)
    {
        const std::span<const SceneDef> scenes = SceneRegistry();
        const int sceneCount = static_cast<int>(scenes.size());
        const int current    = static_cast<int>(app.CurrentScene());

        ImGui::SetNextWindowSize(ImVec2(340.0f, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Sandbox"))
        {
            ImGui::End();
            return;
        }

        // ---- Scene ----------------------------------------------------------------
        if (ImGui::CollapsingHeader("Scene", ImGuiTreeNodeFlags_DefaultOpen))
        {
            const char* curName =
                (current >= 0 && current < sceneCount) ? scenes[current].name : "?";
            if (ImGui::BeginCombo("##scene", curName))
            {
                for (int i = 0; i < sceneCount; ++i)
                {
                    const bool selected = (i == current);
                    if (ImGui::Selectable(scenes[i].name, selected) && i != current)
                        RequestScene(reg, i);
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            if (ImGui::Button("Prev") && sceneCount > 0)
                RequestScene(reg, (current - 1 + sceneCount) % sceneCount);
            ImGui::SameLine();
            if (ImGui::Button("Next") && sceneCount > 0)
                RequestScene(reg, (current + 1) % sceneCount);
            ImGui::SameLine();
            if (ImGui::Button("Reset"))
                RequestReset(reg);
        }

        // ---- Sim ------------------------------------------------------------------
        if (ImGui::CollapsingHeader("Sim", ImGuiTreeNodeFlags_DefaultOpen))
        {
            bool paused = app.IsPaused();
            if (ImGui::Checkbox("Paused", &paused))
                app.SetPaused(paused);

            ImGui::SameLine();
            // Single-step is only meaningful while paused (a running sim steps anyway).
            ImGui::BeginDisabled(!paused);
            if (ImGui::Button("Step"))
                app.RequestSingleStep();
            ImGui::EndDisabled();

            float timeScale = app.TimeScale();
            if (ImGui::SliderFloat("Time scale", &timeScale, 0.05f, 4.0f, "%.2fx"))
                app.SetTimeScale(timeScale);

            // Gravity: PhysicsWorld bakes gravity at construction (no runtime setter),
            // so a change rebuilds the current scene with a fresh world at the new value.
            float gravityY = app.GravityY();
            if (ImGui::SliderFloat("Gravity Y", &gravityY, 0.0f, 2400.0f, "%.0f"))
            {
                app.SetGravityY(gravityY);
                RequestReset(reg);   // mint a fresh world at the new gravity (deferred)
            }
        }

        // ---- Spawn ----------------------------------------------------------------
        // Unified spawn section: one segmented shape-button row selects the active shape
        // (Box / Circle / Capsule / Polygon). Box/Circle/Capsule spawn on LMB-press over
        // empty space; Polygon enters vertex-collection mode (click to add, Enter/KP_Enter
        // to commit, Backspace to undo last, Esc to clear). The old separate Polygon
        // header is removed -- shape is the single knob.
        if (ImGui::CollapsingHeader("Spawn", ImGuiTreeNodeFlags_DefaultOpen))
        {
            Sandbox::SpawnConfig& cfg = app.SpawnConfigMut();

            // Segmented shape buttons: highlight the active shape with an accent color.
            static const char* kShapeNames[] = { "Box", "Circle", "Capsule", "Polygon" };
            static constexpr int kShapeCount = 4;
            const ImVec4 kAccent{0.95f, 0.55f, 0.25f, 1.0f};   // orange accent

            for (int i = 0; i < kShapeCount; ++i)
            {
                const bool active = (static_cast<int>(cfg.shape) == i);
                if (active)
                {
                    ImGui::PushStyleColor(ImGuiCol_Button,        kAccent);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{1.0f, 0.70f, 0.45f, 1.0f});
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4{0.80f, 0.40f, 0.15f, 1.0f});
                }
                if (ImGui::Button(kShapeNames[i]))
                    cfg.shape = static_cast<SpawnShape>(i);
                if (active)
                    ImGui::PopStyleColor(3);

                if (i < kShapeCount - 1)
                    ImGui::SameLine();
            }

            // Density applies to every shape.
            ImGui::SliderFloat("Density", &cfg.density, 0.1f, 10.0f, "%.1f");

            // Adaptive per-shape controls.
            if (cfg.shape == SpawnShape::Polygon)
            {
                const std::size_t pts = app.PolygonPointCount();
                ImGui::Text("Points: %zu", pts);

                ImGui::BeginDisabled(pts == 0);
                if (ImGui::Button("Clear"))
                    app.ClearPolygonPoints();
                ImGui::EndDisabled();

                ImGui::SameLine();
                ImGui::BeginDisabled(pts < 3);   // need >= 3 verts
                if (ImGui::Button("Spawn"))
                    app.RequestPolygonSpawn();
                ImGui::EndDisabled();

                ImGui::TextDisabled(
                    "Click to add a vertex - Enter spawn / Backspace undo / Esc clear");
            }
            else
            {
                ImGui::SliderFloat("Size", &cfg.size, 4.0f, 80.0f, "%.0f");
                ImGui::TextDisabled("Left-click empty space to spawn");
            }
        }

        // ---- Debug draw -----------------------------------------------------------
        if (ImGui::CollapsingHeader("Debug draw", ImGuiTreeNodeFlags_DefaultOpen))
        {
            SandboxDebugDraw& dbg = app.DebugOptionsMut();
            ImGui::Checkbox("Contacts", &dbg.drawContacts);
            ImGui::Checkbox("AABBs", &dbg.drawAabbs);
            ImGui::SliderFloat("Line thickness", &dbg.lineThickness, 1.0f, 4.0f, "%.1f");

            ImGui::Separator();

            // Rich per-body overlays (each with its scalar; disabled when its
            // flag is off so the slider only edits a live overlay).
            ImGui::Checkbox("Velocities", &dbg.drawVelocities);
            ImGui::BeginDisabled(!dbg.drawVelocities);
            ImGui::SliderFloat("Vel scale", &dbg.velocityScale, 0.02f, 0.5f, "%.2fs");
            ImGui::EndDisabled();

            ImGui::Checkbox("COM markers", &dbg.drawComMarkers);
            ImGui::BeginDisabled(!dbg.drawComMarkers);
            ImGui::SliderFloat("COM size", &dbg.comMarkerSize, 2.0f, 16.0f, "%.0f");
            ImGui::EndDisabled();

            ImGui::Checkbox("Orientation ticks", &dbg.drawOrientations);
            ImGui::BeginDisabled(!dbg.drawOrientations);
            ImGui::SliderFloat("Tick length", &dbg.orientationTickLen, 4.0f, 48.0f, "%.0f");
            ImGui::EndDisabled();
        }

        // ---- Stats ----------------------------------------------------------------
        if (ImGui::CollapsingHeader("Stats", ImGuiTreeNodeFlags_DefaultOpen))
        {
            std::uint32_t bodies   = 0;
            std::size_t   contacts = 0;
            if (const PhysicsResource* phys = reg.GetResource<PhysicsResource>();
                phys && phys->world)
            {
                bodies   = phys->world->Count();
                contacts = phys->world->ActiveContactCount();
            }

            const ImGuiIO& io = ImGui::GetIO();
            const float fps   = io.Framerate;
            const float stepMs = (fps > 0.0f) ? (1000.0f / fps) : 0.0f;

            ImGui::Text("Bodies:   %u", bodies);
            ImGui::Text("Contacts: %zu", contacts);
            ImGui::Text("FPS:      %.1f  (%.2f ms)", fps, stepMs);
        }

        ImGui::End();
    }
}
