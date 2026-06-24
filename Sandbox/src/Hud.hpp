#pragma once

// Hud: the Sandbox ImGui control panel (Task 8).
//
// A focused, stateless unit: Hud::Draw(app, reg) issues the ImGui widgets for one
// "Sandbox" window and binds each control to SandboxApp state (and reads live stats
// from the PhysicsWorld in `reg`). The HUD does NOT own physics/scene logic -- it
// reads/writes SandboxApp's HUD-bound fields and lets SandboxApp APPLY them:
//
//   * Scene  -- a combo of SceneRegistry() names + Prev/Next/Reset -> app.SetScene /
//               app.Reset (driven through the SceneControl side channel so the rebuild
//               lands on a clean registry next FixedUpdate, never mid-step).
//   * Sim    -- Pause / Single-step / Time-scale -> app.SetPaused / RequestSingleStep /
//               SetTimeScale. Gravity slider -> app.SetGravityY + a scene reset (the
//               PhysicsWorld bakes gravity at construction; a reset mints a fresh world).
//   * Spawn  -- shape (box/circle) + size + density -> app.SpawnConfigMut() (the knobs
//               the Interaction reads at the next empty-space LMB-press).
//   * Debug  -- contacts / AABBs / line thickness checkboxes -> app.DebugOptionsMut()
//               (published into SandboxDebugDraw, read by PhysicsDebugRenderSystem).
//   * Stats  -- body count (world.Count()), contact count (world.ActiveContactCount()),
//               FPS + step ms (ImGui::GetIO().Framerate).
//
// Headless-safe: every call is a plain ImGui:: call, so a throwaway ImGui context (no
// device) drives Draw in the unit test (the gate is "no assert/crash + the bound state
// drives SandboxApp"). The plugin calls SandboxApp::DrawUI (-> Hud::Draw) from
// GamePlugin_DrawUI, which the host issues between ImGuiLayer BeginFrame and Render.

namespace Astra { class Registry; }

namespace Arcane::Sandbox
{
    class SandboxApp;

    struct Hud
    {
        // Draw the control panel for `app`, reading live stats from the PhysicsWorld
        // resource in `reg` and a pending scene switch/reset through SandboxApp's
        // public API. Static -- the HUD holds no state of its own.
        static void Draw(SandboxApp& app, Astra::Registry& reg);

        // Draw the Slice-B "Narrowphase Inspector" window (header + step control + the
        // Minkowski inset via the app's OffscreenCanvas). No-op unless a subject is set.
        // Called by Draw after the main "Sandbox" window. Separated so its render-device
        // path (OffscreenCanvas::Draw + ImGui::Image) is isolated from the main panel.
        static void DrawInspectorWindow(SandboxApp& app);
    };
}
