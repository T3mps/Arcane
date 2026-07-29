// SandboxApp implementation. Owns the scene roster index + the clean-rebuild path
// (Clear + fresh PhysicsResource + builder) and pumps the SceneControl side channel each
// fixed step. The engine systems do the physics + render work; see SandboxApp.hpp for the
// design notes.

#include "SandboxApp.hpp"
#include "Hud.hpp"
#include "Scenes.hpp"

#include <Arcane/Base/Runtime.hpp>              // device + shaders bridge (inset canvas source)
#include <Arcane/Input/InputSnapshot.hpp>       // InputSnapshot (fed to the interaction layer)
#include <Manifold2D/Physics/PhysicsWorld.hpp>      // PhysicsWorld + WorldDef (fresh world per scene)
#include <Manifold2D/Physics/Solver/Solver.hpp>     // ContactConstraint (pick-nearest-contact)
#include <Arcane/Render/OffscreenCanvas.hpp>    // OffscreenCanvas (Minkowski inset target)
#include <Arcane/Render/ShaderLibrary.hpp>      // ShaderLibrary (OffscreenCanvas::Create arg)
#include <Arcane/Scene/PhysicsSystem.hpp>       // PhysicsResource + PhysicsSystem (sandbox-owned step)

#include <Astra/Registry/Registry.hpp>

#include <algorithm>                            // std::clamp
#include <cmath>                                // std::sqrt
#include <cstdlib>                              // std::getenv, std::strtol
#include <limits>                               // std::numeric_limits
#include <memory>
#include <span>

namespace Arcane::Sandbox
{
    namespace
    {
        // The sandbox fixed timestep (matches the 60 Hz RunLoop). The sandbox-owned
        // physics step (Task 8) scales THIS by the HUD time-scale per FixedUpdate.
        constexpr float kFixedDt = 1.0f / 60.0f;

        // Time-scale clamp: a huge scale in one discrete step would tunnel/explode the
        // solver; a tiny one is harmless. Keep it readable + stable.
        constexpr float kMinTimeScale = 0.05f;
        constexpr float kMaxTimeScale = 4.0f;

        // Install a FRESH PhysicsResource (new PhysicsWorld + empty entity<->body map),
        // replacing any existing one. SetResource overwrites the resource slot in place, so
        // the previous world (and all its bodies) is destroyed -- nothing leaks into the new
        // scene. Mirrors EnsurePhysicsResource in Sandbox.cpp but unconditionally replaces.
        void InstallFreshPhysicsResource(Astra::Registry& reg, float gravityY,
                                         Mosaic::IWorkScheduler* sched = nullptr)
        {
            Manifold2D::Physics::WorldDef wd;
            wd.gravityY = gravityY;   // MKS: caller-supplied (SandboxApp::m_gravityY, HUD-editable)
            auto world = std::make_unique<Manifold2D::Physics::PhysicsWorld>(wd);
            world->SetExecutor(sched);  // Phase D1: wire scheduler (null -> serial default)
            reg.SetResource(Arcane::PhysicsResource{ std::move(world), {} });
        }
    }

    // Defined here (not defaulted in the header) so unique_ptr<OffscreenCanvas> sees a
    // COMPLETE OffscreenCanvas (included above) when it generates the deleter.
    SandboxApp::SandboxApp()  = default;
    SandboxApp::~SandboxApp() = default;

    // Sim-time control forwards to the engine RunLoop (Epic 04). Null-safe: with no loop
    // wired (a CPU path that never drives frames) the sandbox reports running/real-time.
    void SandboxApp::SetPaused(bool p) noexcept
    {
        if (m_loop) m_loop->SetPaused(p);
    }

    bool SandboxApp::IsPaused() const noexcept
    {
        return m_loop && m_loop->IsPaused();
    }

    void SandboxApp::RequestSingleStep() noexcept
    {
        if (m_loop) m_loop->RequestSingleStep();
    }

    void SandboxApp::SetTimeScale(float s) noexcept
    {
        // RunLoop only clamps to >= 0; keep the sandbox's sane upper/lower bound so a huge
        // scale cannot flood a frame with fixed steps.
        if (m_loop) m_loop->SetTimeScale(std::clamp(s, kMinTimeScale, kMaxTimeScale));
    }

    float SandboxApp::TimeScale() const noexcept
    {
        return m_loop ? static_cast<float>(m_loop->TimeScale()) : 1.0f;
    }

    void SandboxApp::RebuildScene(Astra::Registry& reg, std::size_t index)
    {
        const std::span<const SceneDef> scenes = SceneRegistry();
        if (scenes.empty()) return;
        if (index >= scenes.size()) index %= scenes.size();   // wrap into range
        m_sceneIndex = index;

        // 1. Destroy ALL current scene entities. Registry::Clear() wipes entities +
        //    relationships + archetypes but LEAVES resources intact (RenderContext2D is set
        //    by the host each frame; PhysicsResource/SceneRoot are replaced below/by the
        //    builder). This is the cleanest reset since the sandbox owns every entity.
        reg.Clear();

        // 2. Mint a fresh PhysicsResource so the new scene's bodies start from an empty
        //    world + empty entity<->body map (no stale handles from the old scene).
        InstallFreshPhysicsResource(reg, m_gravityY, &m_scheduler);

        // 2b. Fresh render-interpolation buffer (Epic 04.2): drop the previous scene's
        //     per-body prev poses so the first frame of the new scene never lerps from a
        //     dead body. PhysicsSystem repopulates it on the first stepped frame.
        reg.SetResource<Arcane::PhysicsInterpBuffer>(Arcane::PhysicsInterpBuffer{});

        // 3. Run the target builder -- it repopulates entities and re-sets SceneRoot.
        scenes[m_sceneIndex].build(reg);

        // 4. Default the camera to the identity transform (offset (0,0), zoom 1). The
        //    scenes are authored directly in canvas-pixel space (world unit == canvas px,
        //    floors low on a 1280x720 canvas), so the identity already frames them on
        //    screen -- the same framing the smoke test relied on pre-camera. Pan/zoom
        //    INPUT (mouse drag + wheel) is Task 7; this task just makes the camera
        //    pipeline live + correct so a nonzero camera moves sprites + overlay together.
        m_camera = Camera{};

        // 5. Drop any active mouse grab: the old PhysicsWorld (and the grabbed body's
        //    handle) is gone -- a held grab must not carry into the fresh world.
        m_interaction.ClearGrab();

        // 6. Publish the new index for the SceneControl side channel + the HUD debug
        //    flags for the render-phase overlay (both survive Clear() as resources, but
        //    a fresh build re-establishes them so the very first frame is correct).
        PublishControl(reg);
        PublishDebug(reg);
    }

    void SandboxApp::PublishDebug(Astra::Registry& reg) const
    {
        SandboxDebugDraw* dbg = reg.GetResource<SandboxDebugDraw>();
        if (!dbg)
        {
            reg.SetResource(SandboxDebugDraw{});
            dbg = reg.GetResource<SandboxDebugDraw>();
        }
        if (!dbg) return;
        *dbg = m_debug;   // mirror the HUD-owned flags into the render-read resource
    }

    // ---- narrowphase inspector (subject model, always on) ----------------------

    void SandboxApp::ClearSubject() noexcept
    {
        m_subjectValid      = false;
        m_subjectFixture    = Manifold2D::Physics::kInvalidFixture;
        m_subjectBody       = Manifold2D::Physics::kInvalidBody;
        m_selectedIndex     = -1;
        m_hasSelectedPartner = false;
        m_selectedPartnerId = 0;
        m_stepIndex         = 0;
        m_inspectorPlay     = false;
        m_contacts.clear();   // keeps capacity (incl. per-contact trace vectors)
    }

    void SandboxApp::SetSubjectBody(Astra::Registry& reg, Manifold2D::Physics::BodyHandle bh)
    {
        namespace Phys = Manifold2D::Physics;
        PhysicsResource* phys = reg.GetResource<PhysicsResource>();
        if (!phys || !phys->world || !phys->world->IsValid(bh))
        {
            ClearSubject();
            return;
        }
        Phys::PhysicsWorld& world = *phys->world;

        // The subject is the body's PRIMARY fixture (fixture 0). The "specific fixture
        // under the cursor" is not cheaply queryable (no per-fixture point query exists),
        // so we pick fixture 0: exact for the common single-fixture body, an accepted
        // debug-tier approximation for a compound body (see the partner caveat below).
        const Phys::FixtureHandle fh = world.GetBodyFixture(bh, 0);
        if (!world.IsValid(fh))
        {
            ClearSubject();
            return;
        }

        // Grabbing the SAME body again is a no-op (keep selection/step stable); a
        // DIFFERENT body switches the subject and resets the selection + step state.
        if (m_subjectValid && m_subjectBody == bh && m_subjectFixture == fh)
            return;

        m_subjectFixture     = fh;
        m_subjectBody        = bh;
        m_subjectValid       = true;
        m_selectedIndex      = -1;     // re-resolved to the deepest in the next enumerate
        m_hasSelectedPartner = false;
        m_selectedPartnerId  = 0;
        m_stepIndex          = 0;
        m_inspectorPlay      = false;
    }

    const Manifold2D::Physics::NarrowphaseTrace& SandboxApp::SelectedTrace() const noexcept
    {
        if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_contacts.size()))
            return m_contacts[static_cast<std::size_t>(m_selectedIndex)].trace;
        return m_idleTrace;   // no contacts -> a Cleared/empty trace (fit handles it)
    }

    int SandboxApp::RecordedStepCount() const noexcept
    {
        // The slider scrubs whichever per-iteration vector is populated for the SELECTED
        // contact's kind: Epa -> epaSnapshots, Mpr -> mprSnapshots, SatPolygon -> satAxes.
        // Analytic kinds (CircleCircle / CircleVsPolygon / Capsule / Separated) record no
        // per-iteration series -> 0 (the HUD disables the slider with a note).
        using K = Manifold2D::Physics::NarrowphaseKind;
        const Manifold2D::Physics::NarrowphaseTrace& t = SelectedTrace();
        switch (t.kind)
        {
            case K::Epa:        return static_cast<int>(t.epaSnapshots.size());
            case K::Mpr:        return static_cast<int>(t.mprSnapshots.size());
            case K::SatPolygon: return static_cast<int>(t.satAxes.size());
            default:            return 0;
        }
    }

    void SandboxApp::SetStepIndex(int i) noexcept
    {
        const int n = RecordedStepCount();
        if (n <= 0) { m_stepIndex = 0; return; }
        m_stepIndex = std::clamp(i, 0, n - 1);
    }

    void SandboxApp::SetSelectedIndex(int i) noexcept
    {
        if (m_contacts.empty())
        {
            m_selectedIndex = -1;
            m_hasSelectedPartner = false;
            m_selectedPartnerId = 0;
            return;
        }
        m_selectedIndex      = std::clamp(i, 0, static_cast<int>(m_contacts.size()) - 1);
        m_hasSelectedPartner = true;
        m_selectedPartnerId  = m_contacts[static_cast<std::size_t>(m_selectedIndex)].partnerBodyId;
        SetStepIndex(0);   // a partner switch resets the step scrub
    }

    void SandboxApp::UpdateAndPublishInspector(Astra::Registry& reg)
    {
        namespace Phys = Manifold2D::Physics;

        m_contacts.clear();   // reuse storage (capacity, incl. per-contact trace vectors)

        // Re-validate the subject against the live world; a stale handle (recycled slot,
        // a removed body, or a scene rebuild) silently clears the subject. Then enumerate
        // ALL its active contacts. Cheap + pure (DebugCollide reads world, writes traces).
        PhysicsResource* phys = reg.GetResource<PhysicsResource>();
        if (m_subjectValid &&
            (!phys || !phys->world ||
             !phys->world->IsValid(m_subjectFixture) ||
             !phys->world->IsValid(m_subjectBody)))
        {
            ClearSubject();
        }

        if (m_subjectValid && phys && phys->world)
        {
            Phys::PhysicsWorld& world = *phys->world;
            const std::uint32_t subjectSlot = m_subjectBody.index;

            // Track the deepest contact (max penetration) so it can be the default focus.
            int   deepestIdx   = -1;
            float deepestDepth = -std::numeric_limits<float>::max();

            // Scan the last Step's ContactConstraints for constraints the subject's body
            // participates in (bodyA or bodyB == subject slot). The OTHER body is a contact
            // partner. ContactConstraint exposes body SLOTS (not fixture handles), so this
            // is BODY-LEVEL enumeration: for each partner body we re-collide the SUBJECT
            // fixture against the partner's PRIMARY fixture (GetBodyFixture(partner, 0)).
            // For the common single-fixture body this is exact; for a compound body it is
            // an accepted debug-tier approximation (partner fixture 0). A partner whose
            // DebugCollide returns pointCount == 0 (the subject fixture is not really
            // touching that partner -- e.g. a compound mismatch, or the constraint came
            // from a different fixture pair) is DROPPED.
            world.ForEachContactConstraint(
                [&](const Phys::ContactConstraint& cc)
                {
                    std::uint32_t partnerSlot = Phys::kInvalidSlot;
                    if (cc.bodyA == subjectSlot)      partnerSlot = cc.bodyB;
                    else if (cc.bodyB == subjectSlot) partnerSlot = cc.bodyA;
                    else                              return;   // not the subject's contact

                    // Skip tile-span virtual fixtures (no FixtureHandle to DebugCollide).
                    if (partnerSlot == Phys::kInvalidSlot) return;

                    const Phys::BodyHandle partnerH = world.HandleOf(partnerSlot);
                    const Phys::FixtureHandle pf = world.GetBodyFixture(partnerH, 0);
                    if (!world.IsValid(pf)) return;

                    // De-dup: a partner may appear in more than one constraint (e.g. two
                    // manifold passes). Re-collide ONCE per partner body.
                    for (const ContactView& existing : m_contacts)
                        if (existing.partnerBodyId == partnerH.index) return;

                    // Append + re-collide into the new entry's reused trace vectors.
                    m_contacts.emplace_back();
                    ContactView& cv = m_contacts.back();
                    cv.partnerBodyId = partnerH.index;
                    (void)world.DebugCollide(m_subjectFixture, pf, cv.trace);

                    // Drop a partner the subject fixture is not actually touching.
                    if (cv.trace.manifold.pointCount <= 0)
                    {
                        m_contacts.pop_back();
                        return;
                    }

                    // Track the deepest (max penetration = max separation; this engine
                    // stores manifold separation POSITIVE for penetration).
                    float depth = -std::numeric_limits<float>::max();
                    for (int pi = 0; pi < cv.trace.manifold.pointCount; ++pi)
                        depth = std::max(depth,
                                         static_cast<float>(cv.trace.manifold.points[pi].separation));
                    const int idx = static_cast<int>(m_contacts.size()) - 1;
                    if (depth > deepestDepth) { deepestDepth = depth; deepestIdx = idx; }
                });

            // ---- resolve the SELECTED contact (stable by partner id; deepest default) --
            if (m_contacts.empty())
            {
                m_selectedIndex      = -1;
                m_hasSelectedPartner = false;
                m_selectedPartnerId  = 0;
                m_stepIndex          = 0;
            }
            else
            {
                // Try to re-find the previously-selected partner by id (order can shift).
                // m_hasSelectedPartner gates this -- slot 0 is a valid partner id, so it
                // can't double as the "no selection" sentinel.
                int reSel = -1;
                if (m_hasSelectedPartner)
                {
                    for (int i = 0; i < static_cast<int>(m_contacts.size()); ++i)
                        if (m_contacts[static_cast<std::size_t>(i)].partnerBodyId == m_selectedPartnerId)
                        { reSel = i; break; }
                }
                // Fall back to the deepest when the prior partner is gone / none selected.
                m_selectedIndex      = (reSel >= 0) ? reSel : deepestIdx;
                m_hasSelectedPartner = true;
                m_selectedPartnerId  = m_contacts[static_cast<std::size_t>(m_selectedIndex)].partnerBodyId;
                SetStepIndex(m_stepIndex);   // re-clamp (the selected kind's count may change)
            }
        }

        // Publish the resource (create if absent) so PhysicsDebugRenderSystem can draw the
        // world overlay. Mirrors PublishDebug. hasSubject defaults false -> a fresh build
        // with no inspector use looks exactly as before. The render system reads the
        // published resource (it holds no SandboxApp handle), so the contacts are copied
        // into it below. That copy only runs while a subject is actively held, and N is the
        // small number of contacts a single body has, so the per-frame copy is a deliberate,
        // acceptable debug-tier cost.
        SandboxInspectorResource* insp = reg.GetResource<SandboxInspectorResource>();
        if (!insp)
        {
            reg.SetResource(SandboxInspectorResource{});
            insp = reg.GetResource<SandboxInspectorResource>();
        }
        if (!insp) return;
        insp->hasSubject    = m_subjectValid;
        insp->subject       = m_subjectFixture;
        insp->subjectBody   = m_subjectBody.index;
        insp->selectedIndex = m_selectedIndex;
        insp->stepIndex     = m_stepIndex;
        insp->contacts      = m_contacts;   // copy for the render system (m_contacts is our master)
    }

    Arcane::OffscreenCanvas* SandboxApp::EnsureInspectorCanvas(std::uint32_t w,
                                                               std::uint32_t h)
    {
        if (w == 0 || h == 0)
            return m_inspectorCanvas.get();

        // The device + ShaderLibrary live in the host (ArcaneRuntime); the Runtime render-
        // resources bridge exposes them. Null in a headless host (the CPU [sandbox]
        // tests never wire a Runtime in) -> no GPU resource is created.
        if (!m_runtime)
            return nullptr;
        nvrhi::IDevice*        device  = m_runtime->Device();
        Arcane::ShaderLibrary* shaders = m_runtime->Shaders();
        if (!device || !shaders)
            return nullptr;

        if (!m_inspectorCanvas)
        {
            m_inspectorCanvas = Arcane::OffscreenCanvas::Create(device, *shaders, w, h);
        }
        else
        {
            m_inspectorCanvas->Resize(w, h);   // no-op on an unchanged size
        }
        return m_inspectorCanvas.get();
    }

    void SandboxApp::PublishControl(Astra::Registry& reg) const
    {
        SceneControl* ctrl = reg.GetResource<SceneControl>();
        if (!ctrl)
        {
            reg.SetResource(SceneControl{});
            ctrl = reg.GetResource<SceneControl>();
        }
        if (!ctrl) return;
        ctrl->sceneCount   = static_cast<std::int32_t>(SceneRegistry().size());
        ctrl->currentScene = static_cast<std::int32_t>(m_sceneIndex);
    }

    void SandboxApp::BuildInitialScene(Astra::Registry& reg)
    {
        // ARCANE_SANDBOX_SCENE selects the initial scene headlessly (default 0).
        m_sceneIndex = 0;
        if (const char* s = std::getenv("ARCANE_SANDBOX_SCENE"))
        {
            const long v = std::strtol(s, nullptr, 10);
            if (v >= 0) m_sceneIndex = static_cast<std::size_t>(v);
        }
        RebuildScene(reg, m_sceneIndex);
    }

    void SandboxApp::SetScene(Astra::Registry& reg, std::uint32_t index)
    {
        RebuildScene(reg, static_cast<std::size_t>(index));
    }

    void SandboxApp::Reset(Astra::Registry& reg)
    {
        RebuildScene(reg, m_sceneIndex);
    }

    void SandboxApp::FixedUpdate(Astra::Registry& reg, double dt,
                                 const Arcane::InputSnapshot& /*input*/)
    {
        // The pause-gated SIM step (Epic 04). RunLoop -- which owns pause / single-step /
        // time-scale -- calls this ONLY for the fixed steps that actually run (0 while
        // paused; N per frame under time-scale), at the CANONICAL fixed dt. Time-scale
        // changes how MANY steps run, never the step size, so the step stays deterministic.
        // Everything else the sandbox does per frame -- scene-switch pump, interaction,
        // inspector, polygon authoring, mint-while-frozen -- moved to Update, which runs
        // every frame even while paused.
        PhysicsResource* phys = reg.GetResource<PhysicsResource>();
        if (!phys || !phys->world) return;

        PhysicsSystem physics(static_cast<float>(dt));   // CREATE/SYNC + Step(dt) + write-back
        physics(reg);
    }

    void SandboxApp::Update(Astra::Registry& reg, double /*dt*/, double /*alpha*/,
                            const Arcane::InputSnapshot& input)
    {
        // The EVERY-FRAME hook -- runs even while the sim is frozen (RunLoop gates only the
        // fixed phase). All authoring/interaction that must keep working while paused lives
        // here. RunLoop runs this AFTER the fixed phase, so a grab's drive velocity set here
        // integrates on the NEXT fixed step (a mouse-joint's inherent ~1-frame latency).

        // Pump the SceneControl side channel FIRST so a rebuild lands on a clean registry.
        // A reset takes precedence over a switch; both are one-shot (cleared on consume).
        if (SceneControl* ctrl = reg.GetResource<SceneControl>())
        {
            if (ctrl->requestReset != 0)
            {
                ctrl->requestReset = 0;
                ctrl->requestedScene = -1;   // a reset supersedes any pending switch
                Reset(reg);
            }
            else if (ctrl->requestedScene >= 0)
            {
                const std::uint32_t idx = static_cast<std::uint32_t>(ctrl->requestedScene);
                ctrl->requestedScene = -1;
                SetScene(reg, idx);
            }
        }

        // Keep the render-overlay resource in lockstep with the HUD-owned flags every frame
        // (a HUD checkbox edit between frames reaches PhysicsDebugRenderSystem).
        PublishDebug(reg);

        PhysicsResource* phys = reg.GetResource<PhysicsResource>();
        if (!phys || !phys->world)
        {
            UpdateAndPublishInspector(reg);   // still publish (a missing world clears the subject)
            m_interaction.ApplyWheelZoom(m_camera, input);
            return;
        }

        // The mouse-interaction layer (spawn / grab / drag / pan). Runs every frame
        // regardless of pause, so bodies can be arranged/spawned while the sim is frozen.
        m_interaction.Tick(reg, *phys->world, m_camera, input, kFixedDt);

        // Inspector subject: consume a grab-to-select request (raised when the interaction
        // grabbed a body this frame); grabbing empty space keeps the current subject.
        if (const Manifold2D::Physics::BodyHandle grabbed = m_interaction.TakeSubjectGrab();
            grabbed != Manifold2D::Physics::kInvalidBody)
            SetSubjectBody(reg, grabbed);

        // Inspector: re-validate the subject, enumerate ALL its active contacts (re-run the
        // narrowphase for each), and publish the inspector resource for the world overlay.
        // The ContactConstraint pool it scans is the last step's -- the valid post-Step window.
        UpdateAndPublishInspector(reg);

        // Mirror the in-progress polygon draft into the render-read resource so
        // PolygonDraftRenderSystem can draw the clicked vertices. Published ONLY when shape
        // == Polygon -- switching away hides stray markers while the points are retained.
        if (!reg.GetResource<PolygonDraftResource>())
            reg.SetResource(PolygonDraftResource{});
        if (m_interaction.IsPolygonMode())
            reg.GetResource<PolygonDraftResource>()->worldPoints = m_interaction.PolygonPoints();
        else
            reg.GetResource<PolygonDraftResource>()->worldPoints.clear();

        // Commit a HUD-requested polygon on the LIVE world (deferred out of the render phase).
        // World-direct -> it renders via DrawPhysicsDebug (no entity).
        if (m_requestPolygonSpawn)
        {
            m_requestPolygonSpawn = false;
            m_interaction.SpawnPolygon(*phys->world);
        }

        // While PAUSED the fixed phase (and its PhysicsSystem CREATE pass) does not run, so
        // mint here: materialize a body spawned this frame + write back its pose WITHOUT
        // stepping, so a spawned-while-frozen body appears at its spawn location. When
        // running, the fixed-phase step already did CREATE/SYNC + write-back.
        if (m_loop && m_loop->IsPaused())
        {
            PhysicsSystem mintOnly(kFixedDt, /*stepWorld=*/false);
            mintOnly(reg);
        }

        // Camera wheel-zoom: consumed once per frame here (matches the wheel's per-frame
        // sampling cadence). The camera is pushed to the engine by the plugin right after this.
        m_interaction.ApplyWheelZoom(m_camera, input);
    }

    void SandboxApp::DrawUI(Astra::Registry& reg)
    {
        Hud::Draw(*this, reg);
    }
}
