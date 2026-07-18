# Arcane Epic 04.2 — Render Interpolation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Draw physics bodies at `lerp(previous-step pose, current-step pose, RunLoop::Alpha())` so slow-motion (and any frame faster than the fixed step) renders smoothly instead of snapping.

**Architecture:** Capture each body's previous fixed-step pose at fixed-step cadence in `PhysicsSystem`, surface `RunLoop::Alpha()` through `RenderContext2D`, and interpolate in the two render consumers — the physics-debug overlay (`DrawPhysicsDebug`, what the Sandbox actually draws) and the ECS sprite path (`RenderSubmissionSystem`, the general engine path). Rotation interpolates on the shortest arc (decomposed angle, not matrix components). No `RunLoop`/time-model/determinism changes.

**Tech Stack:** C++23, Astra ECS, Manifold2D physics, glm, Catch2, premake5 / MSBuild (VS2026), `/MD` md-CRT Arcane workspace.

Spec: `docs/superpowers/specs/2026-07-17-arcane-0402-render-interpolation-design.md`.

## Global Constraints

- **Units MKS**, `/fp:precise` (no `/fp:fast`), UTF-8 no BOM, ASCII comments.
- **CPU-only tests** here — tag new cases `[render][interp]`, never `[gpu]`. The dev-loop gate is `ArcaneTests.exe ~[gpu]`.
- **Run the test exe from its output dir** for the full `~[gpu]` gate (plugin/font tests use CWD-relative paths). A filtered `[interp]` run is CWD-independent.
- **Commits are on hold.** All Epic-04 work (04.1, 04.3, and this) stays UNCOMMITTED on the held Aphelyon/Gacha tree until the user lifts the hold — then it lands as one commit. Each task below ends with a **Verify** checkpoint (build + tests green), NOT a `git commit`. Do not commit unless the user explicitly says so.
- **Byte-identical defaults:** every new field defaults to the no-op (alpha `0`, `interp` null, no `PreviousTransform`) so all existing callers/tests are unchanged.
- Build: `cd Arcane && msbuild Arcane.slnx /p:Configuration=Debug /m`. Regenerate (only when the file LIST changes): `cd Arcane && GenerateProjects.bat`.
- Output dir: `Arcane\bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe`.

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `Arcane/Arcane/src/Arcane/Scene/SceneResources.hpp` | modify | `RenderContext2D.alpha`; `InterpPose`, `PhysicsInterpBuffer`; `Lerp`, `AngleLerp` |
| `Arcane/Arcane/src/Arcane/Base/Runtime.cpp` | modify | `SetRenderContext` writes `Loop().Alpha()` into `RenderContext2D` |
| `Arcane/Arcane/src/Arcane/Scene/PhysicsSystem.hpp` | modify | capture previous world poses (Path A) + previous local pose (Path B) |
| `Arcane/Arcane/src/Arcane/Render/PhysicsDebugDraw.hpp` | modify | `opts.interp` / `opts.alpha` |
| `Arcane/Arcane/src/Arcane/Render/PhysicsDebugDraw.cpp` | modify | interpolate per-body pose in the draw loop |
| `Arcane/Arcane/src/Arcane/Scene/Components.hpp` | modify | `PreviousTransform` component + reflection |
| `Arcane/Arcane/src/Arcane/Scene/SceneModule.hpp` | modify | register `PreviousTransform` |
| `Arcane/Arcane/src/Arcane/Scene/RenderSystems.hpp` | modify | interpolate sprite pose (Path B) |
| `Arcane/Sandbox/src/SandboxApp.hpp` | modify | `PhysicsDebugRenderSystem` wires `opts.interp`/`opts.alpha` |
| `Arcane/Sandbox/src/SandboxApp.cpp` | modify | `RebuildScene` installs a fresh `PhysicsInterpBuffer` |
| `Arcane/Sandbox/src/Sandbox.cpp` | modify | `ReRegisterComponent<PreviousTransform>` |
| `Arcane/Tests/src/RenderInterpolationTest.cpp` | create | all `[interp]` unit tests |

---

## Task 1: Interpolation math, resource types, and `RenderContext2D.alpha`

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Scene/SceneResources.hpp`
- Create: `Arcane/Tests/src/RenderInterpolationTest.cpp`

**Interfaces:**
- Produces: `float Arcane::Lerp(float a, float b, float t)`; `float Arcane::AngleLerp(float a, float b, float t)`; `struct Arcane::InterpPose { glm::vec2 position; float angle; std::uint32_t generation; }`; `struct Arcane::PhysicsInterpBuffer { std::vector<InterpPose> prev; bool captured; }`; `RenderContext2D.alpha` (float, defaulted after `zoom`).

- [ ] **Step 1: Write the failing test.** Create `Arcane/Tests/src/RenderInterpolationTest.cpp`:

```cpp
// Epic 04.2 render interpolation: pure math (Lerp / shortest-arc AngleLerp),
// PhysicsSystem previous-pose capture, and the two render consumers
// (DrawPhysicsDebug overlay + RenderSubmissionSystem sprites) driven against a
// recording mock Batcher2D. CPU-only (tag [interp], never [gpu]).

#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Scene/SceneResources.hpp>

using Catch::Approx;

namespace
{
    constexpr float kPi = 3.14159265358979323846f;
}

TEST_CASE("Lerp is the standard affine blend", "[interp]")
{
    CHECK(Arcane::Lerp(0.0f, 10.0f, 0.0f) == Approx(0.0f));
    CHECK(Arcane::Lerp(0.0f, 10.0f, 1.0f) == Approx(10.0f));
    CHECK(Arcane::Lerp(2.0f, 6.0f, 0.5f) == Approx(4.0f));
}

TEST_CASE("AngleLerp takes the shortest arc across the pi wrap", "[interp]")
{
    // 350deg -> 10deg: shortest arc is +20deg through 0, NOT -340deg.
    const float a = 350.0f * kPi / 180.0f;
    const float b =  10.0f * kPi / 180.0f;
    const float mid = Arcane::AngleLerp(a, b, 0.5f);
    // Midpoint is 360deg == 0deg (mod 2pi). Compare via sin/cos to dodge the wrap.
    CHECK(std::sin(mid) == Approx(0.0f).margin(1e-5));
    CHECK(std::cos(mid) == Approx(1.0f).margin(1e-5));
}

TEST_CASE("AngleLerp endpoints and non-wrapping case", "[interp]")
{
    CHECK(Arcane::AngleLerp(0.3f, 1.1f, 0.0f) == Approx(0.3f));
    CHECK(Arcane::AngleLerp(0.3f, 1.1f, 1.0f) == Approx(1.1f));
    CHECK(Arcane::AngleLerp(0.2f, 0.8f, 0.5f) == Approx(0.5f)); // no wrap: plain midpoint
}
```

- [ ] **Step 2: Add the math helpers + resource types to `SceneResources.hpp`.** Add `#include <cmath>` and `#include <vector>` to the include block (it already has `<cstdint>`, `<glm/glm.hpp>`, `<unordered_map>`). Inside `namespace Arcane`, ABOVE `struct RenderContext2D`, add:

```cpp
    // ---- render interpolation (Epic 04.2) -----------------------------------
    // Blend a previous fixed-step pose toward the current one by RunLoop alpha so
    // slow-mo renders smoothly instead of snapping. Rotation MUST use AngleLerp
    // (shortest arc), not a matrix-component lerp.
    [[nodiscard]] inline float Lerp(float a, float b, float t) noexcept
    {
        return a + (b - a) * t;
    }

    // Shortest-arc angle interpolation (radians): wrap the delta into (-pi, pi]
    // before blending, so 350deg->10deg travels +20deg through 0, not -340deg.
    [[nodiscard]] inline float AngleLerp(float a, float b, float t) noexcept
    {
        constexpr float kPi  = 3.14159265358979323846f;
        constexpr float kTau = 2.0f * kPi;
        float d = std::fmod(b - a, kTau);
        if (d < -kPi)      d += kTau;
        else if (d >  kPi) d -= kTau;
        return a + d * t;
    }

    // One body's previous fixed-step pose. `generation` mirrors the body handle's
    // generation so a recycled SoA slot (stale prev) is rejected by the consumer.
    struct InterpPose
    {
        glm::vec2     position{0.0f, 0.0f};
        float         angle      = 0.0f;   // radians
        std::uint32_t generation = 0;      // 0 == dead slot (never matches a live handle)
    };

    // Per-body previous-pose buffer, indexed by PhysicsWorld body SLOT index (the
    // same space DrawPhysicsDebug iterates). Populated by PhysicsSystem before each
    // world.Step(); read by DrawPhysicsDebug. Transient runtime state (Registry::Save
    // excludes resources; the no-op Serialize satisfies Astra's HasSerializeMethod so
    // the vector member does not hit the trivially-copyable path).
    struct PhysicsInterpBuffer
    {
        std::vector<InterpPose> prev;
        bool                    captured = false;   // false until the first capture

        template<typename Archive>
        void Serialize(Archive& /*ar*/) {}
    };
```

- [ ] **Step 3: Add the `alpha` field to `RenderContext2D`.** In `struct RenderContext2D`, add after the `zoom` member (keep it LAST so existing 3-arg aggregate inits `RenderContext2D{batcher, offset, zoom}` still compile):

```cpp
        float            alpha = 0.0f;              // RunLoop::Alpha() in [0,1); host-set each frame
```

- [ ] **Step 4: Regenerate projects (new test file) and build.**

Run: `cd Arcane && GenerateProjects.bat`
Then: `cd Arcane && msbuild Arcane.slnx /p:Configuration=Debug /m`
Expected: build succeeds (ArcaneTests now includes `RenderInterpolationTest.cpp`).

- [ ] **Step 5: Run the tests and verify pass.**

Run: `Arcane\bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe "[interp]"`
Expected: 3 test cases pass.

- [ ] **Step 6: Verify no regression.**

Run (from the output dir): `cd Arcane\bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe ~[gpu]`
Expected: all pass (prior baseline count + the 3 new cases). Do NOT commit (hold).

---

## Task 2: Surface alpha through `Runtime::SetRenderContext`

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Base/Runtime.cpp:190-198`

**Interfaces:**
- Consumes: `RenderContext2D.alpha` (Task 1); `Runtime::Loop()` (existing, returns `RunLoop&`).
- Produces: every frame's `RenderContext2D` now carries `Loop().Alpha()`.

- [ ] **Step 1: Wire alpha in.** In `Runtime::SetRenderContext`, replace the resource construction:

```cpp
        m_impl->registry->SetResource<RenderContext2D>(
            RenderContext2D{batcher, m_impl->cameraOffset, m_impl->cameraZoom});
```

with (append the alpha read — `Alpha()` returns `double`, `RenderContext2D.alpha` is `float`):

```cpp
        // Epic 04.2: carry the render alpha so RenderSubmissionSystem +
        // DrawPhysicsDebug can interpolate poses between fixed steps. Runtime owns
        // the RunLoop, so this needs no plugin-ABI surface.
        m_impl->registry->SetResource<RenderContext2D>(
            RenderContext2D{batcher, m_impl->cameraOffset, m_impl->cameraZoom,
                            static_cast<float>(Loop().Alpha())});
```

- [ ] **Step 2: Build.**

Run: `cd Arcane && msbuild Arcane.slnx /p:Configuration=Debug /m`
Expected: build succeeds. (`Runtime.cpp` already includes `SceneResources.hpp`; `Loop()` is a member.)

- [ ] **Step 3: Verify no regression.**

Run (from the output dir): `cd Arcane\bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe ~[gpu]`
Expected: all pass. Do NOT commit (hold).

---

## Task 3: Capture previous world poses in `PhysicsSystem` (Path A)

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Scene/PhysicsSystem.hpp`
- Test: `Arcane/Tests/src/RenderInterpolationTest.cpp` (append)

**Interfaces:**
- Consumes: `PhysicsInterpBuffer`, `InterpPose` (Task 1); `PhysicsWorld::Count()/Alive(i)/PosSlot(i)/HandleOf(i)/GetAngle(h)` (existing); `BodyHandle.generation` (existing).
- Produces: after each stepped `PhysicsSystem` run, `PhysicsInterpBuffer.prev[slot]` holds the pre-step pose and `.captured == true`.

- [ ] **Step 1: Write the failing test** (append to `RenderInterpolationTest.cpp`). Add includes at the top of the file (below the existing includes):

```cpp
#include <memory>

#include <Manifold2D/Physics/PhysicsWorld.hpp>
#include <Manifold2D/Physics/PhysicsTypes.hpp>
#include <Manifold2D/Physics/Shapes.hpp>

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/TransformSystems.hpp>

#include <Astra/Registry/Registry.hpp>
```

Then add the test:

```cpp
TEST_CASE("PhysicsInterpBuffer captures the pre-step pose each fixed step", "[interp]")
{
    namespace P = Manifold2D::Physics;
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);
    Arcane::RegisterPhysicsComponents(reg);

    P::WorldDef wd; wd.gravityY = 10.0f;
    reg.SetResource(Arcane::PhysicsResource{
        std::make_unique<P::PhysicsWorld>(wd), {} });
    reg.SetResource(Arcane::PhysicsInterpBuffer{});   // opt in to capture

    // One dynamic circle free-falling from the origin.
    Astra::Entity e = reg.CreateEntity();
    Arcane::LocalTransform lt; lt.position = glm::vec2(0.0f, 0.0f);
    reg.AddComponent<Arcane::LocalTransform>(e, lt);
    reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});
    Arcane::RigidBody2D rb; rb.type = P::BodyType::Dynamic;
    reg.AddComponent<Arcane::RigidBody2D>(e, rb);
    Arcane::Collider2D col;
    { Arcane::Fixture fx; fx.kind = P::ShapeKind::Circle; fx.radius = 0.5f;
      col.fixtures.push_back(fx); }
    reg.AddComponent<Arcane::Collider2D>(e, col);
    reg.AddComponent<Arcane::PhysicsBodyRef>(e, Arcane::PhysicsBodyRef{});

    constexpr float kDt = 1.0f / 60.0f;

    // Step once: creates the body, captures prev (== the initial pose (0,0)), steps.
    Arcane::PhysicsSystem(kDt)(reg);
    const P::BodyHandle h = reg.GetComponent<Arcane::PhysicsBodyRef>(e)->handle;
    const P::PhysicsWorld& world = *reg.GetResource<Arcane::PhysicsResource>()->world;
    const P::Vec2 afterStep1 = world.Position(h);   // pose after step 1

    // Step again: prev must now hold the post-step-1 pose (the pre-step-2 state).
    Arcane::PhysicsSystem(kDt)(reg);

    const auto* buf = reg.GetResource<Arcane::PhysicsInterpBuffer>();
    REQUIRE(buf->captured);
    REQUIRE(h.index < buf->prev.size());
    const Arcane::InterpPose& pp = buf->prev[h.index];
    CHECK(pp.generation == h.generation);
    CHECK(pp.position.y == Approx(static_cast<float>(afterStep1.y)));
    CHECK(world.Position(h).y > pp.position.y);   // it kept falling after the capture
}
```

- [ ] **Step 2: Run to verify it fails.**

Run: `cd Arcane\bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "[interp]"`
Expected: build fails or the new case fails — `PhysicsSystem` does not populate the buffer yet. (Build first; the assertion fails because `buf->captured` is false / `prev` is empty.)

- [ ] **Step 3: Implement the capture.** In `PhysicsSystem.hpp`, add the include near the other scene includes:

```cpp
#include <Arcane/Scene/SceneResources.hpp>
```

Then, inside `operator()`, BETWEEN PASS 2 (CREATE/SYNC, the closing `}` of that block) and PASS 3 (`if (m_stepWorld) world.Step(...)`), insert:

```cpp
            // ------------------------------------------------------------------
            // PASS 2.5: CAPTURE PREVIOUS POSES (Epic 04.2 render interpolation).
            // Snapshot every live body's PRE-STEP world pose into PhysicsInterpBuffer
            // (opt-in resource; skipped if absent). Captured before Step so prev ==
            // the step-N-1 pose; multiple steps/frame leave prev = second-to-last.
            // Gated on m_stepWorld: a paused/mint-only pass does not step, so the
            // buffer (and the render alpha) stay frozen -> a frozen scene renders
            // static. Iterates the WORLD (not ECS) so world-direct joint/polygon
            // bodies with no entity are covered too.
            // ------------------------------------------------------------------
            if (m_stepWorld)
            {
                if (PhysicsInterpBuffer* interp = reg.GetResource<PhysicsInterpBuffer>())
                {
                    const std::uint32_t n = world.Count();
                    interp->prev.resize(n);
                    for (std::uint32_t i = 0; i < n; ++i)
                    {
                        if (world.Alive(i))
                        {
                            const Phys::BodyHandle h = world.HandleOf(i);
                            const Phys::Vec2       p = world.PosSlot(i);
                            interp->prev[i] = InterpPose{
                                glm::vec2(static_cast<float>(p.x), static_cast<float>(p.y)),
                                static_cast<float>(world.GetAngle(h)),
                                h.generation };
                        }
                        else
                        {
                            interp->prev[i].generation = 0;   // dead slot never matches
                        }
                    }
                    interp->captured = true;
                }
            }
```

- [ ] **Step 4: Build and run to verify pass.**

Run: `cd Arcane && msbuild Arcane.slnx /p:Configuration=Debug /m`
Then: `cd Arcane\bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "[interp]"`
Expected: all `[interp]` cases pass (4 now).

- [ ] **Step 5: Verify no regression.**

Run (from the output dir): `ArcaneTests.exe ~[gpu]`
Expected: all pass. Do NOT commit (hold).

---

## Task 4: Interpolate the physics-debug overlay (Path A consumer)

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Render/PhysicsDebugDraw.hpp`
- Modify: `Arcane/Arcane/src/Arcane/Render/PhysicsDebugDraw.cpp:261-395`
- Test: `Arcane/Tests/src/RenderInterpolationTest.cpp` (append)

**Interfaces:**
- Consumes: `PhysicsInterpBuffer`, `InterpPose`, `Lerp`, `AngleLerp` (Task 1); `PhysicsInterpBuffer.prev` populated by Task 3.
- Produces: `PhysicsDebugDrawOptions.interp` (`const PhysicsInterpBuffer*`, default null) and `.alpha` (`float`, default 0); `DrawPhysicsDebug` draws each body at the interpolated pose when `interp` is set and the generation matches.

- [ ] **Step 1: Write the failing test** (append). It renders one dynamic circle through `DrawPhysicsDebug` into a recording mock batcher with a synthesized `prev` buffer; only the outline is enabled so the single `Circle` call is the body center. Add the include:

```cpp
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/PhysicsDebugDraw.hpp>
```

Add the mock batcher (anonymous namespace) and test:

```cpp
namespace
{
    // Recording Batcher2D: captures the last Circle center + count. All other
    // primitive overrides are no-ops (the tests disable every non-outline overlay).
    struct RecBatcher final : Arcane::Batcher2D
    {
        int       circleCalls = 0;
        glm::vec2 lastCircleCenter{0.0f, 0.0f};
        int       rectCalls = 0;
        float     lastRotation = 0.0f;
        glm::vec2 lastRectPos{0.0f, 0.0f};    // top-left origin of the last Rect/Quad
        glm::vec2 lastRectSize{0.0f, 0.0f};

        void Begin(nvrhi::ICommandList*, nvrhi::IFramebuffer*, uint32_t, uint32_t) override {}
        void SetLayer(uint16_t, uint16_t) override {}
        void Quad(glm::vec2 p, glm::vec2 sz, nvrhi::ITexture*, glm::vec2, glm::vec2,
                  glm::vec4, float rot) override
        { ++rectCalls; lastRotation = rot; lastRectPos = p; lastRectSize = sz; }
        void Glyph(glm::vec2, glm::vec2, nvrhi::ITexture*, glm::vec2, glm::vec2,
                   glm::vec4) override {}
        void Rect(glm::vec2 p, glm::vec2 sz, glm::vec4, float rot) override
        { ++rectCalls; lastRotation = rot; lastRectPos = p; lastRectSize = sz; }
        void Line(glm::vec2, glm::vec2, float, glm::vec4) override {}
        void Circle(glm::vec2 c, float, glm::vec4) override
        { ++circleCalls; lastCircleCenter = c; }
        void End() override {}
        void RemoveTexture(nvrhi::ITexture*) override {}
        Arcane::Batch2DStats Stats() const override { return {}; }

        // Center of the last Rect/Quad (Batcher2D quads are top-left origin).
        glm::vec2 lastRectCenter() const { return lastRectPos + lastRectSize * 0.5f; }
    };
}

TEST_CASE("DrawPhysicsDebug interpolates the body outline by alpha", "[interp]")
{
    namespace P = Manifold2D::Physics;
    P::WorldDef wd; P::PhysicsWorld world(wd);

    // One dynamic circle at (10, 0). No stepping -> velocity 0 (no velocity ray).
    P::BodyDef bd; bd.type = P::BodyType::Dynamic;
    bd.position = P::Vec2(P::Real(10), P::Real(0));
    bd.shape = P::MakeCircle(P::Real(0.5)); bd.density = P::Real(1);
    const P::BodyHandle h = world.AddBody(bd);

    // Synthesized previous pose at (0, 0), same generation as the live slot.
    Arcane::PhysicsInterpBuffer buf;
    buf.prev.resize(world.Count());
    buf.prev[h.index] = Arcane::InterpPose{ glm::vec2(0.0f, 0.0f), 0.0f, h.generation };
    buf.captured = true;

    Arcane::PhysicsDebugDrawOptions opts;
    opts.drawContacts = opts.drawAabbs = opts.drawVelocities = false;
    opts.drawComMarkers = opts.drawOrientations = false;   // isolate the outline
    opts.interp = &buf;
    opts.alpha  = 0.5f;                                     // halfway 0 -> 10

    RecBatcher rec;
    Arcane::DrawPhysicsDebug(world, rec, opts);

    REQUIRE(rec.circleCalls == 1);
    CHECK(rec.lastCircleCenter.x == Approx(5.0f));   // lerp(0, 10, 0.5) at identity zoom
    CHECK(rec.lastCircleCenter.y == Approx(0.0f));

    // Generation mismatch (stale slot) -> no interp, drawn at the current pose.
    buf.prev[h.index].generation = h.generation + 1u;
    RecBatcher rec2;
    Arcane::DrawPhysicsDebug(world, rec2, opts);
    CHECK(rec2.lastCircleCenter.x == Approx(10.0f));
}
```

- [ ] **Step 2: Run to verify it fails.**

Run: `cd Arcane\bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "[interp]"`
Expected: build fails (`opts.interp`/`opts.alpha` don't exist yet).

- [ ] **Step 3: Add the option fields.** In `PhysicsDebugDraw.hpp`, add the include below the existing render includes:

```cpp
#include <Arcane/Scene/SceneResources.hpp>   // PhysicsInterpBuffer + Lerp/AngleLerp (Epic 04.2)
```

At the END of `struct PhysicsDebugDrawOptions` (after `drawManifolds`), add:

```cpp
        // ---- render interpolation (Epic 04.2) -------------------------------
        // When `interp` is set (per-body previous-step poses from PhysicsSystem)
        // each body's outline / AABB / COM / orientation / velocity origin is drawn
        // at lerp(prev, current, alpha). Null -> current step pose (unchanged).
        // A per-body generation mismatch (recycled slot) falls back to current.
        const PhysicsInterpBuffer* interp = nullptr;
        float                      alpha  = 0.0f;   // RunLoop::Alpha() in [0,1)
```

- [ ] **Step 4: Interpolate in the draw loop.** In `PhysicsDebugDraw.cpp`, inside the per-body loop, replace the pose reads. Change:

```cpp
            const Shape&    s      = world.ShapeSlot(i);
            const Vec2      wpos   = world.PosSlot(i);
            const BodyType  btype  = world.TypeSlot(i);
            const bool      sensor = world.SensorSlot(i);
            const bool      awake  = world.AwakeSlot(i);
            const glm::vec2 spos   = ToScreen(wpos, off, zoom);
```

to:

```cpp
            const Shape&    s      = world.ShapeSlot(i);
            const BodyType  btype  = world.TypeSlot(i);
            const bool      sensor = world.SensorSlot(i);
            const bool      awake  = world.AwakeSlot(i);

            // Render pose (Epic 04.2): the live post-step pose, optionally blended
            // from the previous step's pose by opts.alpha for smooth slow-mo. The
            // generation gate rejects a recycled slot (stale prev). Computed ONCE and
            // reused for the outline, AABB, COM, orientation, and velocity origin.
            Vec2  wpos      = world.PosSlot(i);
            float bodyAngle = static_cast<float>(world.GetAngle(world.HandleOf(i)));
            if (opts.interp && opts.interp->captured
                && i < opts.interp->prev.size()
                && opts.interp->prev[i].generation == world.HandleOf(i).generation)
            {
                const InterpPose& pp = opts.interp->prev[i];
                wpos = Vec2(static_cast<Real>(Lerp(pp.position.x, static_cast<float>(wpos.x), opts.alpha)),
                            static_cast<Real>(Lerp(pp.position.y, static_cast<float>(wpos.y), opts.alpha)));
                bodyAngle = AngleLerp(pp.angle, bodyAngle, opts.alpha);
            }
            const glm::vec2 spos = ToScreen(wpos, off, zoom);
```

Then replace the three later per-body angle reads so they reuse `bodyAngle`. In the Capsule case change:

```cpp
                    const float angle = static_cast<float>(world.GetAngle(world.HandleOf(i)));
```
to:
```cpp
                    const float angle = bodyAngle;
```
In the Polygon case change the identical line the same way:
```cpp
                    const float angle = bodyAngle;
```
And in the rich-overlay block change:
```cpp
            const float     angle = static_cast<float>(world.GetAngle(world.HandleOf(i)));
```
to:
```cpp
            const float     angle = bodyAngle;
```

(These are the only three `world.GetAngle(world.HandleOf(i))` reads inside the per-body loop; `comW = ComWorldF(wpos, angle, ...)` now uses the interpolated `wpos` + `angle` automatically.)

- [ ] **Step 5: Build and run to verify pass.**

Run: `cd Arcane && msbuild Arcane.slnx /p:Configuration=Debug /m`
Then: `cd Arcane\bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "[interp]"`
Expected: all `[interp]` cases pass (5 now).

- [ ] **Step 6: Verify no regression.** The existing GPU debug-draw tests (`[gpu]`) pass null `interp` (default) so are unchanged; still run the CPU gate:

Run (from the output dir): `ArcaneTests.exe ~[gpu]`
Expected: all pass. Do NOT commit (hold).

---

## Task 5: Wire the overlay interpolation into the Sandbox

**Files:**
- Modify: `Arcane/Sandbox/src/SandboxApp.hpp` (`PhysicsDebugRenderSystem`)
- Modify: `Arcane/Sandbox/src/SandboxApp.cpp:104` (`RebuildScene`)

**Interfaces:**
- Consumes: `PhysicsDebugDrawOptions.interp`/`.alpha` (Task 4); `RenderContext2D.alpha` (Task 1/2); `PhysicsInterpBuffer` (Task 1).
- Produces: the Sandbox overlay renders interpolated; a fresh scene starts with an empty buffer.

- [ ] **Step 1: Wire the options in `PhysicsDebugRenderSystem`.** In `SandboxApp.hpp`, inside `PhysicsDebugRenderSystem::operator()`, AFTER the `if (const SandboxDebugDraw* dbg = ...) { ... } else { opts.drawContacts = true; }` block and BEFORE `DrawPhysicsDebug(*phys->world, *ctx->batcher, opts);`, add:

```cpp
            // Render interpolation (Epic 04.2): smooth the overlay between fixed steps
            // by the RunLoop alpha carried in RenderContext2D. The buffer is populated
            // by PhysicsSystem each step; absent / !captured -> DrawPhysicsDebug draws
            // at the current pose (no interp).
            opts.interp = reg.GetResource<PhysicsInterpBuffer>();
            opts.alpha  = ctx->alpha;
```

(`ctx` is the `RenderContext2D*` fetched at the top of the system; `PhysicsInterpBuffer` is visible via the existing `#include <Arcane/Scene/SceneResources.hpp>`.)

- [ ] **Step 2: Install a fresh buffer on scene (re)build.** In `SandboxApp.cpp`, in `RebuildScene`, immediately AFTER the `InstallFreshPhysicsResource(reg, m_gravityY, &m_scheduler);` line (step 2), add:

```cpp
        // 2b. Fresh render-interpolation buffer (Epic 04.2): drop the previous scene's
        //     per-body prev poses so the first frame of the new scene never lerps from a
        //     dead body. PhysicsSystem repopulates it on the first stepped frame.
        reg.SetResource<Arcane::PhysicsInterpBuffer>(Arcane::PhysicsInterpBuffer{});
```

- [ ] **Step 3: Build.**

Run: `cd Arcane && msbuild Arcane.slnx /p:Configuration=Debug /m`
Expected: build succeeds (Sandbox.dll relinks).

- [ ] **Step 4: Verify no regression.**

Run (from the output dir): `cd Arcane\bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe ~[gpu]`
Expected: all pass (the `[sandbox]` suite still green). Do NOT commit (hold).

- [ ] **Step 5: Desk-verify note.** Headless cannot judge smoothness. At the desk (GPU-driver crash hazard under Parsec — local session only): `Loom.exe`, open a scene, set time-scale ~0.1 → the outlines glide between steps instead of snapping; pause freezes them in place; single-step advances one step of motion per press.

---

## Task 6: `PreviousTransform` component (Path B scaffolding)

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Scene/Components.hpp`
- Modify: `Arcane/Arcane/src/Arcane/Scene/SceneModule.hpp:19-24`
- Modify: `Arcane/Sandbox/src/Sandbox.cpp:107-111`

**Interfaces:**
- Produces: `struct Arcane::PreviousTransform { glm::vec2 position; float rotation; }`, reflected + registered in `RegisterSceneComponents`.

- [ ] **Step 1: Add the component.** In `Components.hpp`, after `struct WorldTransform { ... };` (before the `SpriteShape` enum), add:

```cpp
    // PreviousTransform (Epic 04.2): an entity's LOCAL pose at the previous fixed
    // step, captured by PhysicsSystem write-back before it overwrites LocalTransform.
    // RenderSubmissionSystem draws at lerp(previous -> current, alpha) for smooth
    // slow-mo. Decomposed (position + angle) so rotation interpolates on the shortest
    // arc, NOT by lerping matrix components. Purely derived render state: an entity
    // opts into interpolation by carrying it; absent -> the sprite snaps to the latest
    // step (unchanged).
    struct PreviousTransform
    {
        glm::vec2 position{0.0f, 0.0f};
        float     rotation = 0.0f;          // radians
    };
```

- [ ] **Step 2: Reflect it.** In the reflection block at the bottom of `Components.hpp`, after the `WorldTransform` reflect block, add (both fields `Serializable(false)` — derived render state, like `WorldTransform::matrix`):

```cpp
    ASTRA_REFLECT_TYPE(PreviousTransform)
        ASTRA_REFLECT_FIELD(PreviousTransform, position)
            ASTRA_REFLECT_ATTR(Serializable, false)
        ASTRA_REFLECT_FIELD(PreviousTransform, rotation)
            ASTRA_REFLECT_ATTR(Serializable, false)
            ASTRA_REFLECT_ATTR(AngleFormat, Astra::AngleFormat::Unit::Radians)
    ASTRA_END_REFLECT_TYPE()
```

- [ ] **Step 3: Register it.** In `SceneModule.hpp`, in the `RegisterSceneComponents(Astra::ComponentRegistry& creg)` overload, add after the `WorldTransform` line:

```cpp
        creg.RegisterComponent<PreviousTransform>();
```

- [ ] **Step 4: Re-register it in the Sandbox plugin.** In `Sandbox.cpp` `GamePlugin_Init`, after `creg->ReRegisterComponent<Arcane::WorldTransform>();`, add:

```cpp
        creg->ReRegisterComponent<Arcane::PreviousTransform>();
```

- [ ] **Step 5: Build and verify no regression.**

Run: `cd Arcane && msbuild Arcane.slnx /p:Configuration=Debug /m`
Then (from the output dir): `cd Arcane\bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe ~[gpu]`
Expected: all pass (component registers; no entity carries it yet, so behavior is unchanged). Do NOT commit (hold).

---

## Task 7: Capture + consume `PreviousTransform` (Path B)

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Scene/PhysicsSystem.hpp` (write-back, PASS 4)
- Modify: `Arcane/Arcane/src/Arcane/Scene/RenderSystems.hpp:39-55`
- Test: `Arcane/Tests/src/RenderInterpolationTest.cpp` (append)

**Interfaces:**
- Consumes: `PreviousTransform` (Task 6); `RenderContext2D.alpha` (Task 1/2); `Lerp`/`AngleLerp` (Task 1).
- Produces: a sprite entity carrying `PreviousTransform` is drawn at the interpolated pose.

- [ ] **Step 1: Write the failing test** (append). Add the include (RenderSystems + the recording batcher already exists as `RecBatcher`):

```cpp
#include <Arcane/Scene/RenderSystems.hpp>
```

Add the test:

```cpp
TEST_CASE("RenderSubmissionSystem interpolates a sprite by PreviousTransform + alpha", "[interp]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);

    // Current world pose at x=10; previous local pose at x=0. Untextured Rect sprite.
    Astra::Entity e = reg.CreateEntity();
    Arcane::LocalTransform lt; lt.position = glm::vec2(10.0f, 0.0f);
    Arcane::WorldTransform wt; wt.matrix = lt.ToMatrix();
    reg.AddComponent<Arcane::WorldTransform>(e, wt);
    Arcane::SpriteRenderer sp; sp.size = glm::vec2(4.0f, 4.0f);
    reg.AddComponent<Arcane::SpriteRenderer>(e, sp);
    Arcane::PreviousTransform prev; prev.position = glm::vec2(0.0f, 0.0f); prev.rotation = 0.0f;
    reg.AddComponent<Arcane::PreviousTransform>(e, prev);

    RecBatcher rec;
    Arcane::RenderContext2D ctx{ &rec, glm::vec2(0.0f, 0.0f), 1.0f, 0.5f };  // alpha 0.5
    reg.SetResource<Arcane::RenderContext2D>(ctx);
    Arcane::RenderSubmissionSystem{}(reg);

    // Untextured -> Rect path (top-left origin). The quad is centered on the
    // interpolated screen position: center x = lerp(0, 10, 0.5) = 5 at identity zoom.
    REQUIRE(rec.rectCalls == 1);
    CHECK(rec.lastRectCenter().x == Approx(5.0f));
    CHECK(rec.lastRectCenter().y == Approx(0.0f));
    CHECK(rec.lastRotation == Approx(0.0f).margin(1e-5));
}

TEST_CASE("RenderSubmissionSystem interpolates sprite rotation on the shortest arc", "[interp]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);

    Astra::Entity e = reg.CreateEntity();
    Arcane::LocalTransform lt; lt.position = glm::vec2(0.0f, 0.0f);
    lt.rotation = 10.0f * kPi / 180.0f;            // current 10deg
    Arcane::WorldTransform wt; wt.matrix = lt.ToMatrix();
    reg.AddComponent<Arcane::WorldTransform>(e, wt);
    Arcane::SpriteRenderer sp; sp.size = glm::vec2(4.0f, 4.0f);
    reg.AddComponent<Arcane::SpriteRenderer>(e, sp);
    Arcane::PreviousTransform prev; prev.rotation = 350.0f * kPi / 180.0f;  // previous 350deg
    reg.AddComponent<Arcane::PreviousTransform>(e, prev);

    RecBatcher rec;
    reg.SetResource<Arcane::RenderContext2D>(
        Arcane::RenderContext2D{ &rec, glm::vec2(0.0f, 0.0f), 1.0f, 0.5f });
    Arcane::RenderSubmissionSystem{}(reg);

    REQUIRE(rec.rectCalls == 1);
    // Shortest arc 350 -> 10 midpoint is 0deg, NOT 180deg.
    CHECK(std::sin(rec.lastRotation) == Approx(0.0f).margin(1e-5));
    CHECK(std::cos(rec.lastRotation) == Approx(1.0f).margin(1e-5));
}
```

- [ ] **Step 2: Run to verify it fails.**

Run: `cd Arcane\bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "[interp]"`
Expected: the rotation case fails — `RenderSubmissionSystem` ignores `PreviousTransform` (draws current 10deg, not the 350->10 midpoint).

- [ ] **Step 3: Capture the previous local pose in the write-back.** In `PhysicsSystem.hpp` PASS 4, the write-back view lambda currently starts:

```cpp
                view.ForEach([&](Astra::Entity   /*entity*/,
                                 PhysicsBodyRef& ref,
                                 LocalTransform& lt,
                                 RigidBody2D&    rb)
                {
                    if (ref.handle == Phys::kInvalidBody) return;
                    if (!world.IsValid(ref.handle))          return;

                    const Phys::Vec2 pos = world.Position(ref.handle);
                    lt.position = glm::vec2(pos.x, pos.y);
                    lt.rotation = world.GetAngle(ref.handle);
```

Replace it with (name the entity, and stash the pose we are about to overwrite):

```cpp
                view.ForEach([&](Astra::Entity   entity,
                                 PhysicsBodyRef& ref,
                                 LocalTransform& lt,
                                 RigidBody2D&    rb)
                {
                    if (ref.handle == Phys::kInvalidBody) return;
                    if (!world.IsValid(ref.handle))          return;

                    // Render interpolation (Epic 04.2): stash the pose we are about to
                    // overwrite as this entity's PREVIOUS step pose (opt-in via the
                    // PreviousTransform component), so RenderSubmissionSystem can lerp
                    // prev -> current by alpha. Captured before the write below, so prev
                    // == the step-N-1 pose (multiple steps/frame leave prev = second-to-
                    // last, matching the physics pose buffer).
                    if (PreviousTransform* pt = reg.GetComponent<PreviousTransform>(entity))
                    {
                        pt->position = lt.position;
                        pt->rotation = lt.rotation;
                    }

                    const Phys::Vec2 pos = world.Position(ref.handle);
                    lt.position = glm::vec2(pos.x, pos.y);
                    lt.rotation = world.GetAngle(ref.handle);
```

- [ ] **Step 4: Consume it in `RenderSubmissionSystem`.** In `RenderSystems.hpp`, change the view lambda header + the pose locals. Replace:

```cpp
            view.ForEach([&](Astra::Entity, WorldTransform& world, SpriteRenderer& sprite)
            {
                const glm::mat3& m = world.matrix;
                const glm::vec2 worldPos(m[2].x, m[2].y);
                const glm::vec2 worldScale(glm::length(glm::vec2(m[0])),
                                           glm::length(glm::vec2(m[1])));
                const float worldRot = std::atan2(m[0].y, m[0].x);
```

with:

```cpp
            view.ForEach([&](Astra::Entity e, WorldTransform& world, SpriteRenderer& sprite)
            {
                const glm::mat3& m = world.matrix;
                glm::vec2       worldPos(m[2].x, m[2].y);
                const glm::vec2 worldScale(glm::length(glm::vec2(m[0])),
                                           glm::length(glm::vec2(m[1])));
                float worldRot = std::atan2(m[0].y, m[0].x);

                // Render interpolation (Epic 04.2): if the entity carries a
                // PreviousTransform (its prior fixed-step local pose, captured by
                // PhysicsSystem write-back), draw at lerp(prev -> current, alpha) for
                // smooth slow-mo. Rotation uses shortest-arc AngleLerp. Treats the
                // entity's local pose as its world pose -- exact for a flat / identity-
                // rooted physics entity (the case today). No PreviousTransform -> the
                // unchanged snap-to-step path.
                if (const PreviousTransform* prev = reg.GetComponent<PreviousTransform>(e))
                {
                    const float a = ctx->alpha;
                    worldPos = glm::vec2(Lerp(prev->position.x, worldPos.x, a),
                                         Lerp(prev->position.y, worldPos.y, a));
                    worldRot = AngleLerp(prev->rotation, worldRot, a);
                }
```

(`RenderSystems.hpp` already includes `Components.hpp` and `SceneResources.hpp`, so `PreviousTransform`, `Lerp`, and `AngleLerp` are in scope; the rest of the lambda already reads `worldPos`/`worldRot`.)

- [ ] **Step 5: Build and run to verify pass.**

Run: `cd Arcane && msbuild Arcane.slnx /p:Configuration=Debug /m`
Then: `cd Arcane\bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe "[interp]"`
Expected: all `[interp]` cases pass (7 now).

- [ ] **Step 6: Verify no regression.** The existing `SpriteRotationTest` sprites carry no `PreviousTransform` (unchanged path); still run the gate:

Run (from the output dir): `ArcaneTests.exe ~[gpu]`
Expected: all pass. Do NOT commit (hold).

---

## Task 8: Full-gate closeout + memory update

**Files:** none (verification + memory).

- [ ] **Step 1: Debug gate.**

Run (from the output dir): `cd Arcane\bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe ~[gpu]`
Expected: all pass (prior baseline `~[gpu]` count + the 7 new `[interp]` cases).

- [ ] **Step 2: Release build + gate** (determinism/NDEBUG parity):

Run: `cd Arcane && msbuild Arcane.slnx /p:Configuration=Release /m`
Then (from the Release output dir): `ArcaneTests.exe ~[gpu]`
Expected: all pass.

- [ ] **Step 3: Desk-verify (local session only; not headless).** `Loom.exe` → scene → time-scale ~0.1 → outlines + any sprites glide (not snap); pause freezes; single-step advances one step of motion. Confirm the physics-debug overlay outline still registers with the collider under pan/zoom at alpha 0 and mid-alpha.

- [ ] **Step 4: Update the running memory.** Append 04.2 completion to `project_arcane_next_milestone` (new `~[gpu]` count, the two interpolated paths, the contacts-stay-at-current-step decision, the flat-hierarchy assumption) and note the plan/spec paths. Leave the commit hold as-is unless the user lifts it.

---

## Self-Review

**Spec coverage** — every spec section maps to a task:
- RenderContext2D.alpha + Runtime wire → Tasks 1, 2. ✔
- PhysicsInterpBuffer (generation-safe) + capture-before-step → Tasks 1, 3. ✔
- DrawPhysicsDebug consume (pose only; contacts/broadphase at current) → Task 4. ✔
- Sandbox wiring + fresh buffer per scene → Task 5. ✔
- PreviousTransform + capture-in-write-back + RenderSubmission consume → Tasks 6, 7. ✔
- AngleLerp shortest-arc / Lerp shared helper → Task 1. ✔
- Pause / single-step semantics (m_stepWorld gate, frozen alpha) → Task 3 capture gate (tested indirectly; frozen == no capture). ✔
- Tests (AngleLerp, capture, both consumers, generation gate) → Tasks 1/3/4/7. ✔
- Desk-verify + gate → Tasks 5/8. ✔
- Out-of-scope items (contacts interp, non-physics capture, moving-parent) → explicitly not tasked. ✔

**Type consistency** — `PhysicsInterpBuffer.prev` / `.captured`, `InterpPose{position, angle, generation}`, `Lerp(float,float,float)`, `AngleLerp(float,float,float)`, `RenderContext2D{batcher, offset, zoom, alpha}`, `PhysicsDebugDrawOptions.interp/.alpha`, `PreviousTransform{position, rotation}` are used identically across Tasks 1–7. ✔

**Placeholder scan** — no TBD/TODO; every code step shows the full edit. ✔
