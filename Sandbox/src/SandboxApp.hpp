#pragma once

// SandboxApp: owns the sandbox's per-run state (the current scene index) and the
// scene lifecycle -- build the initial scene, switch to another scene, and reset (rebuild)
// the current scene. The heavy lifting -- physics stepping, transform propagation, sprite +
// debug rendering -- is done by the engine systems the plugin registers in Init; SandboxApp
// just owns the scene roster index + the clean-rebuild path.
//
// SCENE SWITCH / RESET (Task 5):
//   Switching scenes must START CLEAN. The cleanest reset given the sandbox owns the entire
//   registry (every entity is scene content under SceneRoot) is:
//     1. reg.Clear()            -- destroy ALL entities (Clear leaves resources untouched).
//     2. replace PhysicsResource -- a FRESH PhysicsWorld + empty entity<->body map, so the
//                                   old world's bodies cannot leak into the new scene.
//     3. run the new scene builder -- it repopulates entities and re-sets SceneRoot.
//   Reset() rebuilds the CURRENT index the same way. Both clamp/wrap the index into
//   [0, SceneRegistry().size()). The actual UI/key that calls these is a later task (HUD =
//   Task 8); here they exist and are exercised by the smoke test through the SceneControl
//   side channel below.
//
// Also defines PhysicsDebugRenderSystem: a render-phase Astra system that pulls the live
// Batcher2D from the RenderContext2D resource (set by the host each frame via
// Runtime::SetRenderContext) and the PhysicsWorld from the PhysicsResource, then calls
// DrawPhysicsDebug to overlay collider outlines + contacts. It runs in the same render
// scheduler as RenderSubmissionSystem (registered AFTER it, so the overlay draws on top),
// which is exactly how the render phase is driven -- no bespoke render path.

#include "Camera.hpp"                       // the sandbox 2D pan + zoom view
#include "Interaction.hpp"                  // mouse spawn/drag/throw + pan/zoom (Task 7)

#include <Arcane/Jobs/TaskExecutor.hpp>     // ITaskExecutor (Phase D1 executor injection)
#include <Arcane/Jobs/ArcaneWorkScheduler.hpp> // bridges ITaskExecutor -> Mosaic::IWorkScheduler
#include <Manifold2D/Physics/Fixture.hpp>          // FixtureHandle, kInvalidFixture (subject + partner fixtures)
#include <Manifold2D/Physics/Narrowphase/NarrowphaseTrace.hpp> // NarrowphaseTrace (inspector recorder)
#include <Arcane/Render/Batcher2D.hpp>        // Batcher2D virtual interface (Circle call)
#include <Arcane/Render/PhysicsDebugDraw.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>   // PhysicsResource (owns the PhysicsWorld)
#include <Arcane/Scene/SceneResources.hpp>  // RenderContext2D (holds the live batcher)

#include <Astra/Registry/Registry.hpp>
#include <Astra/System/System.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

namespace Arcane { struct InputSnapshot; class OffscreenCanvas; class Runtime; class RunLoop; }

namespace Arcane::Sandbox
{
    // -------------------------------------------------------------------------
    // SceneControl: the sandbox-private scene-switch side channel (Registry resource).
    // -------------------------------------------------------------------------
    // Lives as a Registry resource (NOT part of the engine PluginVTable ABI -- the 7-entry
    // vtable is untouched). The plugin publishes sceneCount + currentScene; an external
    // driver (the HUD in Task 8, the smoke test today) sets requestedScene / requestReset
    // and the plugin consumes the request in GamePlugin_FixedUpdate (which RunLoop runs
    // BEFORE the engine fixedUpdate scheduler, so the rebuild lands on a clean registry,
    // never mid-PhysicsSystem).
    //
    // POD, <= 64 bytes => ResourceStorage keeps it inline (SBO). Identified purely by its
    // fully-qualified type name (Astra cross-module type identity), so the smoke test can
    // mirror an identically-named struct in its own module and share the same slot.
    struct SceneControl
    {
        std::int32_t sceneCount     = 0;   // plugin -> driver: SceneRegistry().size()
        std::int32_t currentScene   = 0;   // plugin -> driver: index after the last (re)build
        std::int32_t requestedScene = -1;  // driver -> plugin: switch to this index (-1 = none)
        std::int32_t requestReset   = 0;   // driver -> plugin: rebuild current scene (0/1)
    };

    // -------------------------------------------------------------------------
    // SandboxDebugDraw: the HUD-controlled debug-overlay flags (Registry resource).
    // -------------------------------------------------------------------------
    // The HUD (Task 8) toggles the PhysicsDebugDrawOptions flags; PhysicsDebugRenderSystem
    // (a SEPARATE Astra system with no handle to SandboxApp) reads them through this
    // resource each render frame. SandboxApp owns the authoritative copy and publishes
    // it into this resource every (re)build + whenever the HUD edits a flag. POD, small,
    // identified by its fully-qualified type name (Astra cross-module type identity), so
    // a headless test can mirror it if needed. Defaults match the Task-7 overlay
    // (contacts on, AABBs off): a fresh build with no HUD looks exactly as before.
    struct SandboxDebugDraw
    {
        bool  drawContacts  = true;
        // MKS: 0.03 m = 3 px at pixelsPerMeter=100, zoom 1 (the px-era apparent
        // size; the engine-side default is still the px-era 3.0 until the debug
        // test cluster converts).
        float contactMarkerSize = 0.03f;
        bool  drawAabbs     = false;
        float lineThickness = 1.0f;

        // Rich per-body overlays (mirror PhysicsDebugDrawOptions; defaults match it so
        // the showcase is informative out of the box). The HUD toggles these; the
        // render system copies them into the options each frame.
        bool  drawVelocities     = true;
        float velocityScale      = 0.15f;
        bool  drawComMarkers     = true;
        float comMarkerSize      = 0.05f;
        bool  drawOrientations   = true;
        float orientationTickLen = 0.18f;

        // ---- Slice A broadphase + manifold overlays (default OFF) -----------
        // Visualize the physics broadphases + contact manifolds. All four default
        // off so a fresh build looks exactly as before; the HUD toggles them and
        // the render system copies them into PhysicsDebugDrawOptions each frame.
        bool  drawFixtureTree    = false;  // mover DynamicTree leaves + pairs
        bool  drawStaticGrid     = false;  // static-body DynamicTree leaves
        bool  drawResidencyGrid  = false;  // dynamic/kinematic residency cells
        bool  drawManifolds      = false;  // per-point contact manifolds + normals
    };

    // -------------------------------------------------------------------------
    // ContactView: one of the subject fixture's active contacts (subject model).
    // -------------------------------------------------------------------------
    // The subject participates in 0..N contacts; each ContactView holds the partner
    // body id (the OTHER body in the constraint -- stable across frames, used to keep
    // the HUD's partner selection stable by id rather than index) and the re-collided
    // NarrowphaseTrace (subject fixture vs the partner's primary fixture). Traces carry
    // std::vectors; the enclosing resource is never serialized (no-op Serialize).
    struct ContactView
    {
        std::uint32_t                     partnerBodyId = 0;  // partner BodyHandle.index
        Manifold2D::Physics::NarrowphaseTrace trace{};            // subject-vs-partner re-run
    };

    // -------------------------------------------------------------------------
    // SandboxInspectorResource: the narrowphase-inspector publish channel (subject model).
    // -------------------------------------------------------------------------
    // SandboxApp owns the authoritative inspector state (the SUBJECT fixture + body, the
    // subject's enumerated contacts, the selected contact index, the step index); it
    // mirrors it into THIS Registry resource each FixedUpdate -- like SandboxDebugDraw.
    // PhysicsDebugRenderSystem (a separate system with no SandboxApp handle) reads it and
    // draws the WORLD-SPACE overlay (subject highlighted + ALL contacts, selected
    // emphasized) when a valid subject exists. The Minkowski INSET is drawn separately by
    // the HUD (it needs the OffscreenCanvas, which SandboxApp owns directly).
    //
    // Defaults to no-subject, so a fresh build is unchanged. The contacts vector carries
    // traces with std::vectors, so (like PolygonDraftResource) it provides a no-op
    // Serialize to satisfy Astra's HasSerializeMethod concept (Save excludes resources).
    struct SandboxInspectorResource
    {
        bool                           hasSubject  = false;   // a valid subject this frame
        Manifold2D::Physics::FixtureHandle subject     = Manifold2D::Physics::kInvalidFixture;
        std::uint32_t                  subjectBody = 0;       // subject BodyHandle.index
        std::vector<ContactView>       contacts;              // ALL of the subject's contacts
        int                            selectedIndex = -1;    // focused contact (-1 = none)
        int                            stepIndex     = 0;     // per-iteration snapshot index

        template<typename Archive>
        void Serialize(Archive& /*ar*/) {}   // no-op: transient per-frame inspector state
    };

    // Render-phase overlay: collider outlines + contacts on top of the sprites.
    // Reads-only w.r.t. ECS (the side effect is the external batcher submission),
    // matching RenderSubmissionSystem. No SystemTraits component dependencies are
    // declared because it touches no component storage -- it reads resources and
    // submits to the batcher. (An empty-traits system runs single-threaded, which is
    // required: Batcher2D is not thread-safe, same as RenderSubmissionSystem.)
    //
    // The overlay flags (contacts / AABBs / line thickness) come from the
    // SandboxDebugDraw resource the HUD controls -- NOT hardcoded -- so a HUD
    // checkbox toggle reaches DrawPhysicsDebug. The resource may be absent (a
    // headless host that never built one); then the Task-7 defaults apply.
    struct PhysicsDebugRenderSystem
    {
        void operator()(Astra::Registry& reg)
        {
            RenderContext2D* ctx = reg.GetResource<RenderContext2D>();
            if (!ctx || !ctx->batcher) return;

            PhysicsResource* phys = reg.GetResource<PhysicsResource>();
            if (!phys || !phys->world) return;

            PhysicsDebugDrawOptions opts;
            opts.cameraOffset = ctx->cameraOffset;   // same camera as the sprites...
            opts.zoom         = ctx->zoom;            // ...so the overlay lines up under pan + zoom.

            // HUD-controlled flags (default to the Task-7 overlay if no resource).
            if (const SandboxDebugDraw* dbg = reg.GetResource<SandboxDebugDraw>())
            {
                opts.drawContacts      = dbg->drawContacts;
                opts.contactMarkerSize = dbg->contactMarkerSize;
                opts.drawAabbs         = dbg->drawAabbs;
                opts.lineThickness     = dbg->lineThickness;
                opts.drawVelocities    = dbg->drawVelocities;
                opts.velocityScale     = dbg->velocityScale;
                opts.drawComMarkers    = dbg->drawComMarkers;
                opts.comMarkerSize     = dbg->comMarkerSize;
                opts.drawOrientations  = dbg->drawOrientations;
                opts.orientationTickLen = dbg->orientationTickLen;
                opts.drawFixtureTree   = dbg->drawFixtureTree;
                opts.drawStaticGrid    = dbg->drawStaticGrid;
                opts.drawResidencyGrid = dbg->drawResidencyGrid;
                opts.drawManifolds     = dbg->drawManifolds;
            }
            else
            {
                opts.drawContacts = true;
            }

            // Render interpolation (Epic 04.2): smooth the overlay between fixed steps
            // by the RunLoop alpha carried in RenderContext2D. The buffer is populated
            // by PhysicsSystem each step; absent / !captured -> DrawPhysicsDebug draws
            // at the current pose (no interp).
            opts.interp = reg.GetResource<PhysicsInterpBuffer>();
            opts.alpha  = ctx->alpha;

            DrawPhysicsDebug(*phys->world, *ctx->batcher, opts);

            // ---- Slice B: subject narrowphase WORLD overlay ---------------------
            // When the inspector has a valid SUBJECT (published by SandboxApp::FixedUpdate
            // into SandboxInspectorResource each step), highlight the subject shape and
            // draw EVERY one of its contacts' world-space internals on top of the base
            // overlay -- the SELECTED contact emphasized, the others dimmed. The Minkowski
            // inset is the HUD's job (it owns the OffscreenCanvas).
            if (const SandboxInspectorResource* insp =
                    reg.GetResource<SandboxInspectorResource>();
                insp && insp->hasSubject && !insp->contacts.empty())
            {
                // Each ContactView trace shares xfA/shapeA == the subject fixture, so the
                // selected contact's trace also carries the subject geometry to highlight.
                const int sel = (insp->selectedIndex >= 0 &&
                                 insp->selectedIndex < static_cast<int>(insp->contacts.size()))
                                    ? insp->selectedIndex : 0;
                for (int i = 0; i < static_cast<int>(insp->contacts.size()); ++i)
                {
                    const bool focused = (i == sel);
                    DrawNarrowphaseWorldOverlay(
                        insp->contacts[i].trace,
                        focused ? insp->stepIndex : -1,
                        *ctx->batcher, ctx->cameraOffset, ctx->zoom,
                        /*lineThickness=*/focused ? 1.6f : 1.0f,
                        /*emphasis=*/focused ? 1.0f : 0.4f);
                }
            }
        }
    };

    // Published each FixedUpdate from Interaction::PolygonPoints(); read in the
    // render phase to draw the in-progress polygon vertices. Empty when not in
    // polygon mode (no markers). Survives reg.Clear() as a resource.
    //
    // No-op serialization: this is a transient per-frame scratch resource
    // (re-published from Interaction::PolygonPoints every FixedUpdate). The
    // member Serialize satisfies Astra's HasSerializeMethod concept so the
    // compiler does not attempt trivially-copyable serialization on the vector
    // (mirrors the PhysicsResource pattern). Registry::Save excludes resources.
    struct PolygonDraftResource
    {
        std::vector<glm::vec2> worldPoints;   // WORLD space

        template<typename Archive>
        void Serialize(Archive& /*ar*/) {}    // no-op: transient per-frame scratch
    };

    // Render-phase system: draws a small fixed-pixel circle at each draft point,
    // projected with the SAME world*zoom+offset transform as the sprites + debug
    // overlay. Single-threaded (Batcher2D is not thread-safe), reads-only w.r.t.
    // ECS, exactly like PhysicsDebugRenderSystem. Absent resource -> draws nothing.
    struct PolygonDraftRenderSystem
    {
        void operator()(Astra::Registry& reg)
        {
            RenderContext2D* ctx = reg.GetResource<RenderContext2D>();
            if (!ctx || !ctx->batcher) return;
            const PolygonDraftResource* draft = reg.GetResource<PolygonDraftResource>();
            if (!draft || draft->worldPoints.empty()) return;

            constexpr float    kMarkerPx = 4.0f;   // screen-space px by design, not a world unit (MKS P6)
            constexpr glm::vec4 kMarkerColor{1.0f, 0.85f, 0.2f, 1.0f};   // amber
            for (const glm::vec2 wp : draft->worldPoints)
            {
                const glm::vec2 screen = wp * ctx->zoom + ctx->cameraOffset;
                ctx->batcher->Circle(screen, kMarkerPx, kMarkerColor);
            }
        }
    };

    // Owns the sandbox run state. One per loaded plugin image (file-local g_app).
    class SandboxApp
    {
    public:
        // Out-of-line dtor: m_inspectorCanvas is a unique_ptr<OffscreenCanvas> (an
        // incomplete type in this header). The destructor is defined in SandboxApp.cpp,
        // which includes OffscreenCanvas.hpp, so unique_ptr can delete a complete type.
        SandboxApp();
        ~SandboxApp();

        // Record the gravity the (re)built PhysicsResource worlds use. Called once by the
        // plugin in Init before BuildInitialScene so SetScene/Reset can mint fresh worlds.
        void Configure(float gravityY) noexcept { m_gravityY = gravityY; }

        // Phase D1: inject the task executor that each (re)built PhysicsWorld will use.
        // nullptr -> the world's owned serial default (deterministic). The engine
        // executor is wrapped by m_scheduler so the world sees a Mosaic::IWorkScheduler.
        void SetExecutor(Arcane::ITaskExecutor* exec) noexcept
        {
            m_scheduler.SetExecutor(exec);
        }

        // Build the current scene index into `reg` from scratch (fresh PhysicsResource +
        // builder). Called by GamePlugin_Init on a fresh boot (after the components are
        // registered). The scene index is recorded so SetScene/Reset can switch later.
        void BuildInitialScene(Astra::Registry& reg);

        // Switch to scene `index` (clamped/wrapped into [0, SceneRegistry().size())):
        // destroy all current entities + recreate a FRESH PhysicsResource, then run the
        // target builder. The old world's bodies do NOT leak into the new scene.
        void SetScene(Astra::Registry& reg, std::uint32_t index);

        // Rebuild the CURRENT scene index from scratch (same clean path as SetScene).
        void Reset(Astra::Registry& reg);

        // Per-frame hooks (Epic 04: engine RunLoop owns sim-time control).
        //
        // FixedUpdate is the PAUSE-GATED sim step: RunLoop calls it ONLY for the fixed
        // steps that actually run (0 while paused; N per frame scaled by time-scale), at
        // the canonical fixed dt. It does one thing -- drive the PhysicsSystem step. It is
        // the RunLoop plugin-fixed hook, so it runs BEFORE the engine fixedUpdate scheduler.
        //
        // Update is the EVERY-FRAME hook (runs even while paused): the SceneControl pump,
        // debug-flag publish, the mouse-interaction layer, the inspector, polygon authoring,
        // a mint-only pass while paused (so a body spawned while frozen still materializes),
        // and camera wheel-zoom. All the authoring/interaction that must keep working while
        // the sim is frozen lives here, NOT in the gated fixed phase. TransformPropagation
        // runs in the engine UPDATE scheduler (see Sandbox Init) so a frozen scene still
        // derives WorldTransform for the render phase.
        void FixedUpdate(Astra::Registry& reg, double dt, const Arcane::InputSnapshot& input);
        void Update(Astra::Registry& reg, double dt, double alpha, const Arcane::InputSnapshot& input);

        // Draw the ImGui control panel (Task 8). Called by GamePlugin_DrawUI (the host
        // calls it between ImGuiLayer BeginFrame/Render). Forwards to Hud::Draw. Defined
        // in SandboxApp.cpp (which includes Hud.hpp) so the header stays imgui-free.
        void DrawUI(Astra::Registry& reg);

        std::size_t CurrentScene() const noexcept { return m_sceneIndex; }

        // ---- HUD-bound sim controls (Epic 04) --------------------------------------
        // Sim-time control is OWNED BY the engine RunLoop; these forward to it so the HUD
        // drives the one engine clock, and RunLoop's fixed-phase gating applies uniformly.
        // The loop pointer is injected via SetLoop (production: Runtime::Loop(); tests: a
        // fixture-owned RunLoop). All null-safe: with no loop wired the sandbox reports
        // running/real-time (the CPU test path that never drives frames sees a live sim).

        // Injected by the host at load (Sandbox Init) / by the test fixture.
        void SetLoop(Arcane::RunLoop* loop) noexcept { m_loop = loop; }

        void SetPaused(bool p) noexcept;
        [[nodiscard]] bool IsPaused() const noexcept;

        // Request exactly ONE canonical fixed step on the next frame, then stay paused.
        void RequestSingleStep() noexcept;

        // Time-scale (1 = real time, 0.5 = half, 2 = double). Scales the sim clock, not the
        // step dt (the fixed step stays deterministic). Clamped to the sandbox's sane range
        // so a huge value cannot flood the frame with steps.
        void  SetTimeScale(float s) noexcept;
        [[nodiscard]] float TimeScale() const noexcept;

        // Gravity (world units/s^2, +Y down). PhysicsWorld has no runtime gravity setter
        // (gravity is baked at construction), so this only records the value; the HUD
        // pairs it with a scene reset (Reset(reg) / a SceneControl reset request), which
        // mints a FRESH world at the new gravity via the same clean-rebuild path
        // SetScene/Reset use. SetGravityY alone does not affect the live world.
        void  SetGravityY(float g) noexcept { m_gravityY = g; }
        [[nodiscard]] float GravityY() const noexcept { return m_gravityY; }

        // The spawn knobs the HUD edits (forwarded to the Interaction's SpawnConfig).
        [[nodiscard]] const SpawnConfig& SpawnConfig()    const noexcept { return m_interaction.SpawnCfg(); }
        [[nodiscard]] Sandbox::SpawnConfig& SpawnConfigMut()    noexcept { return m_interaction.SpawnCfg(); }

        // ---- POLYGON-CREATION MODE (shape == Polygon) ------------------------------
        // Polygon mode is derived from SpawnConfig.shape == Polygon (no separate bool).
        // The HUD reads PolygonPointCount and requests commits via RequestPolygonSpawn.
        // SetPolygonMode is a shim: true -> shape=Polygon, false -> shape=Box. Prefer
        // SpawnConfigMut().shape = SpawnShape::Polygon for direct control.
        void SetPolygonMode(bool on) noexcept { m_interaction.SetPolygonMode(on); }
        [[nodiscard]] bool IsPolygonMode() const noexcept { return m_interaction.IsPolygonMode(); }
        [[nodiscard]] std::size_t PolygonPointCount() const noexcept
        {
            return m_interaction.PolygonPoints().size();
        }
        void ClearPolygonPoints() noexcept { m_interaction.ClearPolygonPoints(); }
        void RequestPolygonSpawn() noexcept { m_requestPolygonSpawn = true; }

        // The debug-overlay flags the HUD toggles. SandboxApp owns the authoritative
        // copy; FixedUpdate publishes it into the SandboxDebugDraw resource each step so
        // PhysicsDebugRenderSystem (a separate system) reads the HUD's choices.
        [[nodiscard]] const SandboxDebugDraw& DebugOptions() const noexcept { return m_debug; }
        [[nodiscard]] SandboxDebugDraw&       DebugOptionsMut()     noexcept { return m_debug; }

        // ---- NARROWPHASE INSPECTOR (subject model, always on) ----------------------
        // The inspector subject is set as a SIDE EFFECT of the normal grab: when the
        // Interaction grabs a body to drag, SandboxApp resolves that body's fixture as the
        // subject (no enable toggle; dragging is unchanged). The subject PERSISTS after the
        // drag releases (study a shape at rest in a pile) until you grab another body, the
        // subject becomes invalid (re-validated each FixedUpdate), or you press "Clear
        // subject". Each FixedUpdate, if the subject is valid, SandboxApp enumerates ALL
        // active contacts the subject participates in (re-colliding the subject fixture
        // against each partner's primary fixture) and publishes a SandboxInspectorResource
        // for the world overlay + the HUD inset.

        // A valid subject is currently set (re-validated each FixedUpdate).
        [[nodiscard]] bool HasSubject() const noexcept { return m_subjectValid; }

        // The subject fixture handle (valid only when HasSubject()); the HUD shows its ids.
        [[nodiscard]] Manifold2D::Physics::FixtureHandle Subject() const noexcept { return m_subjectFixture; }
        [[nodiscard]] Manifold2D::Physics::BodyHandle    SubjectBody() const noexcept { return m_subjectBody; }

        // Set the subject to body `bh`'s primary fixture (fixture 0). No-op (clears the
        // subject) for a stale/fixtureless body. Called by FixedUpdate from the grab
        // request; also directly callable by tests. Re-enumerates contacts on the next
        // UpdateAndPublishInspector. Switching bodies resets the selection + step state.
        void SetSubjectBody(Astra::Registry& reg, Manifold2D::Physics::BodyHandle bh);

        // Drop the subject (the HUD "Clear subject" button / an invalid subject).
        void ClearSubject() noexcept;

        // The subject's enumerated contacts this frame (each a partner + re-run trace).
        // Empty when the subject is touching nothing. The HUD's partner selector lists them.
        [[nodiscard]] const std::vector<ContactView>& Contacts() const noexcept { return m_contacts; }

        // The SELECTED contact index (the focused contact: the inset + step + emphasis).
        // -1 when the subject has no contacts. Defaults to the DEEPEST contact; selecting
        // a different partner re-points the inset/step. SelectedTrace() is the focused
        // contact's trace (an idle/empty trace when there are no contacts).
        [[nodiscard]] int SelectedIndex() const noexcept { return m_selectedIndex; }
        void SetSelectedIndex(int i) noexcept;
        [[nodiscard]] const Manifold2D::Physics::NarrowphaseTrace& SelectedTrace() const noexcept;

        // The step index over the SELECTED contact's recorded iterations (the HUD slider
        // writes it; clamped to RecordedStepCount() for the selected trace's kind; 0 ->
        // analytic, slider disabled).
        [[nodiscard]] int  StepIndex() const noexcept { return m_stepIndex; }
        void SetStepIndex(int i) noexcept;
        [[nodiscard]] int  RecordedStepCount() const noexcept;

        // The inspector "Play" auto-advance flag (the HUD checkbox binds to it). Lives
        // here -- not a HUD-local static -- so it resets in ClearSubject() / on a partner
        // switch like the rest of the inspector state.
        [[nodiscard]] bool InspectorPlay() const noexcept { return m_inspectorPlay; }
        void SetInspectorPlay(bool on) noexcept { m_inspectorPlay = on; }

        // The inspector's Minkowski-inset OffscreenCanvas (lazily created on first use,
        // null until then / in a headless host with no device). The HUD draws into it.
        [[nodiscard]] Arcane::OffscreenCanvas* Inspector() noexcept { return m_inspectorCanvas.get(); }
        // Lazily create (or fetch) the inset canvas at `w`x`h` using the host's device +
        // shaders (Runtime render-resources bridge). Returns null in a headless host.
        Arcane::OffscreenCanvas* EnsureInspectorCanvas(std::uint32_t w, std::uint32_t h);

        // The sandbox 2D camera (pan + zoom). The plugin pushes it to the engine each
        // frame via Runtime::SetCamera so RenderSubmissionSystem + DrawPhysicsDebug
        // apply the SAME transform (sprites + overlay move together).
        const Camera& Cam() const noexcept { return m_camera; }
        Camera&       Cam()       noexcept { return m_camera; }

    private:
        // Tear the registry down to bare resources + mint a fresh PhysicsResource, then run
        // the scene `index` builder. Shared by BuildInitialScene/SetScene/Reset.
        void RebuildScene(Astra::Registry& reg, std::size_t index);

        // Publish sceneCount + currentScene into the SceneControl resource (idempotent;
        // creates it if absent). Called after every (re)build.
        void PublishControl(Astra::Registry& reg) const;

        // Publish the HUD debug flags into the SandboxDebugDraw resource so the
        // render-phase overlay system reads them. Idempotent; creates it if absent.
        void PublishDebug(Astra::Registry& reg) const;

        // Re-validate the subject, enumerate ALL its active contacts (re-colliding the
        // subject fixture against each partner's primary fixture into the reused
        // m_contacts traces), pick the deepest as the default focus (keeping the prior
        // selection stable by partner id), and publish the SandboxInspectorResource (for
        // the world overlay). A stale subject -> clears it. Idempotent; creates the
        // resource if absent. Called each FixedUpdate so the traces track the live bodies.
        void UpdateAndPublishInspector(Astra::Registry& reg);

        std::size_t            m_sceneIndex = 0;
        float                  m_gravityY   = 0.0f;
        // Persistent IWorkScheduler bridge for the injected task executor: outlives each
        // freshly-minted PhysicsWorld (the world stores a raw scheduler pointer). Re-pointed
        // by SetExecutor.
        Arcane::ArcaneWorkScheduler m_scheduler{};
        Camera      m_camera{};      // default identity; configured in RebuildScene
        Interaction m_interaction{}; // mouse spawn/drag/throw + pan/zoom (Task 7)

        // ---- sim controls (Epic 04) ------------------------------------------------
        // The engine RunLoop owns pause / single-step / time-scale; the sandbox forwards
        // the HUD to it via this pointer (Runtime::Loop() in production, a fixture-owned
        // RunLoop in tests). Null until SetLoop is called.
        Arcane::RunLoop* m_loop = nullptr;

        // One-shot HUD request: commit the in-progress polygon on the next frame
        // (deferred out of the render phase so the world AddBody never races the
        // render-phase DrawPhysicsDebug read). Consumed + cleared when handled.
        bool  m_requestPolygonSpawn = false;

        // ---- debug overlay flags (Task 8) ------------------------------------------
        SandboxDebugDraw m_debug{};   // contacts on / AABBs off by default (Task-7 look)

        // ---- narrowphase inspector (subject model, always on) ----------------------
        bool m_subjectValid = false;  // a valid subject is set (re-validated each step)
        Manifold2D::Physics::FixtureHandle m_subjectFixture = Manifold2D::Physics::kInvalidFixture;
        Manifold2D::Physics::BodyHandle    m_subjectBody    = Manifold2D::Physics::kInvalidBody;
        int  m_selectedIndex  = -1;   // focused contact index (-1 = subject has none)
        // Stable selection: re-resolve the focused contact by PARTNER ID across frames
        // (the contact list order can shift). m_hasSelectedPartner gates the lookup --
        // body slot 0 is a VALID partner id, so a bare `id != 0` sentinel would wrongly
        // treat a partner at slot 0 as "no selection". The bool keeps slot 0 selectable.
        bool m_hasSelectedPartner = false;
        std::uint32_t m_selectedPartnerId = 0;  // partner BodyHandle.index (valid iff the bool)
        int  m_stepIndex      = 0;    // per-iteration snapshot index (slider-driven)
        bool m_inspectorPlay  = false; // HUD "Play" auto-advance (reset on clear/switch)

        // Reused per-frame storage for the subject's enumerated contacts (each carries a
        // partner id + a re-run trace). m_contacts is .clear()'d + refilled each step;
        // the inner traces keep their vector capacity across frames (DebugCollide Clears
        // each before re-recording) -> no gratuitous per-frame heap churn after warmup.
        std::vector<ContactView> m_contacts;

        // An empty idle trace returned by SelectedTrace() when there are no contacts (so
        // the HUD inset has something safe to read; fit-to-bounds handles no geometry).
        Manifold2D::Physics::NarrowphaseTrace m_idleTrace{};

        // The Minkowski-inset render target. Lazily created on first use from the host's
        // device + ShaderLibrary (Runtime render-resources bridge); null in a headless
        // host (no device) so the CPU [sandbox] tests never touch a GPU. Owned here, torn
        // down with the app. Stored opaquely (unique_ptr<OffscreenCanvas>) so the header
        // stays nvrhi-light.
        std::unique_ptr<Arcane::OffscreenCanvas> m_inspectorCanvas;

        // The Runtime is the only path to the host's device + shaders (the OffscreenCanvas
        // factory needs both). Cached on first DrawUI/FixedUpdate via the engine context;
        // null in the CPU test harness (which never wires a Runtime in).
        Arcane::Runtime* m_runtime = nullptr;

    public:
        // Inject the engine Runtime (the device/shaders source for the inset canvas).
        // Called by the plugin once in Init; the CPU test harness never calls it, so
        // EnsureInspectorCanvas returns null there (no GPU touched).
        void SetRuntime(Arcane::Runtime* rt) noexcept { m_runtime = rt; }
    };
}
