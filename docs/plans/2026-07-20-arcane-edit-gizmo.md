# Transform Gizmo (2D TRS) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A screen-constant, procedurally-drawn 2D transform gizmo (translate/rotate/scale, Global/Local toggle, Ctrl-hold snap) for the Grimoire viewport, with an editor-free `ARCANE_API` core and drags bracketed into the undo `CommandStack` (one drag = one undo step).

**Architecture:** A stateless `Arcane::Gizmo` core (`Arcane/Edit/Gizmo.{hpp,cpp}`, `ARCANE_API`): `HitTest` (analytic screen-space handle picking), `ApplyDrag` (pure transform math from the drag start), and `Draw` (screen-constant geometry into a `Batcher2D`), operating on a `GizmoTransform` + `GizmoView`. Grimoire is the thin consumer that owns mode/space/drag state, brackets each drag into its `CommandStack`, and draws the gizmo in the Viewport. Core math (`HitTest`/`ApplyDrag`) is headlessly `[gizmo]`-testable; `Draw` + interactive feel are desk-verified.

**Tech Stack:** C++23, glm, Astra ECS (reflection + `CommandStack`), Dear ImGui (Grimoire toolbar/input), `Batcher2D`, Catch2. Spec: `docs/superpowers/specs/2026-07-20-arcane-edit-gizmo-design.md`.

## Global Constraints

- **`ARCANE_API`, editor-free:** the gizmo core lives in Arcane; Grimoire only consumes it. No editor state in Arcane. The core is **stateless** (pure functions over value inputs).
- **/MD everywhere; no `/fp:fast`; UTF-8 without BOM; ASCII comments.**
- **Headless tests are CPU-only** (tag `[gizmo]`, NO `[gpu]` tag) — pure value tests over `GizmoTransform`, no Registry, no graphics device.
- **Build (PowerShell, VS18 MSBuild):** `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "Arcane\Arcane.slnx" /t:<Target> /p:Configuration=Debug /m /nologo /v:minimal`. New files → run `& "Arcane\GenerateProjects.bat"` once first (premake globs `Arcane/**`). Targets: `ArcaneTests`, `Grimoire`.
- **Run headless tests** from the exe dir: `cd "Arcane\bin\Debug-windows-x86_64-md\ArcaneTests"` then `.\ArcaneTests.exe "[gizmo]"`.
- **Clang/clangd diagnostics in this workspace are NOISE** (wrong toolchain — "expected Clang 20", "Api.hpp not found"). The MSVC build is the sole source of truth.
- **Commits:** `type(scope): summary`, NO AI trailers.
- **Baseline:** `~[gpu]` 27809/336, `[grimoire]` 65/10 (must not drop); `[gizmo]` count grows with the new cases.
- **Namespace-qualified type names:** `Astra::TypeMeta::typeName` is namespace-qualified — a `LocalTransform` descriptor is matched by `"Arcane::LocalTransform"` (NOT bare).

---

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `Arcane/Arcane/src/Arcane/Edit/Gizmo.hpp` | Create | `ARCANE_API` enums + `GizmoTransform`/`GizmoView`/`GizmoSnap` + `HitTest`/`Draw`/`ApplyDrag` declarations. |
| `Arcane/Arcane/src/Arcane/Edit/Gizmo.cpp` | Create | Impl: world<->screen helpers, `ApplyDrag` (T1), `HitTest` (T2), `Draw` (T3). |
| `Arcane/Tests/src/GizmoTest.cpp` | Create | `[gizmo]` headless tests (ApplyDrag T1, HitTest T2). |
| `Arcane/Grimoire/src/GrimoireApp.hpp` | Modify | Gizmo mode/space/drag state members. |
| `Arcane/Grimoire/src/GrimoireApp.cpp` | Modify | Per-frame viewport interaction + drag->command bracketing + W/E/R keys + Draw hook. |
| `Arcane/Grimoire/src/EditorPanels.hpp` | Modify | Toolbar gizmo mode buttons + Global/Local toggle signature. |
| `Arcane/Grimoire/src/EditorPanels.cpp` | Modify | Toolbar controls impl. |

---

## Task 1: Gizmo types + view helpers + `ApplyDrag` (headless `[gizmo]`)

The stateless core interface + the transform math. Creates the full `Gizmo.hpp` interface (so later tasks have stable signatures) and implements the world<->screen helpers + `ApplyDrag` for all three modes. `HitTest`/`Draw` are declared here but defined in Tasks 2/3 (unreferenced declarations link fine).

**Files:** Create `Gizmo.hpp`, `Gizmo.cpp` (view helpers + `ApplyDrag`), `GizmoTest.cpp` (ApplyDrag cases).

**Interfaces:**
- Produces: the enums, `GizmoTransform`, `GizmoView`, `GizmoSnap`, and `GizmoTransform ApplyDrag(GizmoMode, GizmoSpace, GizmoAxis, const GizmoTransform&, const GizmoView&, glm::vec2 mouseStartScreen, glm::vec2 mouseCurScreen, const GizmoSnap&)`; plus the declarations of `HitTest` and `Draw` (defined later).

- [ ] **Step 0 (read-first):** Open `Arcane/Grimoire/src/EntityPick`/`PickBuffer`-adjacent viewport code and any existing `PickEntitiesAt` call to see the viewport's world<->screen convention (is `pixelsPerMeter` folded into zoom? is screen-Y flipped? is the origin the viewport center?). The `GizmoView` helpers below use a **centered, Y-flipped, `zoom*ppm`** convention; if the real viewport differs, adjust `WorldToScreen`/`ScreenToWorld` to match EXACTLY (a mismatch makes the gizmo misalign with the scene). Also confirm the repo's glm include convention from `Arcane/Arcane/src/Arcane/Scene/Components.hpp`.

- [ ] **Step 1: Write `Gizmo.hpp`.**

```cpp
#pragma once

// Arcane/Edit: 2D transform gizmo core (ARCANE_API, editor-free, STATELESS).
// Pure functions over value inputs: HitTest (which handle is under the cursor),
// ApplyDrag (new transform from the drag start), Draw (screen-constant geometry
// into a Batcher2D). Grimoire owns all interaction state and consumes these.

#include <Arcane/Base/Api.hpp>

#include <glm/glm.hpp>

namespace Arcane
{
    class Batcher2D;

    enum class GizmoMode  { Translate, Rotate, Scale };
    enum class GizmoSpace { World, Local };
    // Center = free-move (Translate) / uniform (Scale); in Rotate the ring is the
    // sole handle and is reported as Center (ApplyDrag ignores the axis there).
    enum class GizmoAxis  { None, X, Y, Center };

    // Decoupled from Scene so Edit/Gizmo has no Scene dependency; Grimoire maps
    // LocalTransform <-> this.
    struct GizmoTransform
    {
        glm::vec2 position{0.0f, 0.0f};
        float     rotation = 0.0f;      // radians
        glm::vec2 scale{1.0f, 1.0f};
    };

    // World<->screen for the viewport. Fill from the viewport camera (Grimoire).
    struct GizmoView
    {
        glm::vec2 cameraOffset{0.0f, 0.0f};   // world point at the viewport center
        float     zoom = 1.0f;
        float     pixelsPerMeter = 100.0f;
        glm::vec2 viewportOriginPx{0.0f, 0.0f};
        glm::vec2 viewportSizePx{0.0f, 0.0f};
    };

    struct GizmoSnap
    {
        bool  enabled = false;   // Ctrl held during the drag
        float translate = 0.5f;  // world units grid
        float rotationDeg = 15.0f;
        float scale = 0.1f;
    };

    // Which handle is under mouseScreen (None if off-gizmo). Center-priority.
    ARCANE_API GizmoAxis HitTest(GizmoMode mode, GizmoSpace space,
                                 const GizmoTransform& t, const GizmoView& view,
                                 glm::vec2 mouseScreen);

    // Screen-constant gizmo geometry for the current state; hovered/active brighten.
    ARCANE_API void Draw(Batcher2D& batcher, GizmoMode mode, GizmoSpace space,
                         const GizmoTransform& t, const GizmoView& view,
                         GizmoAxis hovered, GizmoAxis active);

    // New transform, computed from `start` (no accumulation drift).
    ARCANE_API GizmoTransform ApplyDrag(GizmoMode mode, GizmoSpace space, GizmoAxis axis,
                                        const GizmoTransform& start, const GizmoView& view,
                                        glm::vec2 mouseStartScreen, glm::vec2 mouseCurScreen,
                                        const GizmoSnap& snap);
}
```

- [ ] **Step 2: Write the failing test** — `GizmoTest.cpp`:

```cpp
// Arcane transform-gizmo core ([gizmo], CPU-only). Pure value tests over
// GizmoTransform -- no Registry, no graphics device.

#include <cmath>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/glm.hpp>

#include <Arcane/Edit/Gizmo.hpp>

using Catch::Matchers::WithinAbs;

namespace
{
    // Simple view: center (400,300), 100 px/world-unit, no camera offset, Y-flip.
    // worldToScreen(w) = (400 + w.x*100, 300 - w.y*100).
    Arcane::GizmoView MakeView()
    {
        Arcane::GizmoView v;
        v.cameraOffset = glm::vec2(0.0f, 0.0f);
        v.zoom = 1.0f;
        v.pixelsPerMeter = 100.0f;
        v.viewportOriginPx = glm::vec2(0.0f, 0.0f);
        v.viewportSizePx = glm::vec2(800.0f, 600.0f);
        return v;
    }
}

TEST_CASE("Gizmo ApplyDrag: translate world axis + center", "[gizmo]")
{
    const Arcane::GizmoView v = MakeView();
    Arcane::GizmoTransform start;                 // pos (0,0), rot 0, scale (1,1)
    const Arcane::GizmoSnap noSnap;               // enabled=false

    // Drag X: mouse (400,300)->(450,300) == world (0,0)->(0.5,0).
    Arcane::GizmoTransform rx = Arcane::ApplyDrag(
        Arcane::GizmoMode::Translate, Arcane::GizmoSpace::World, Arcane::GizmoAxis::X,
        start, v, glm::vec2(400, 300), glm::vec2(450, 300), noSnap);
    CHECK_THAT(rx.position.x, WithinAbs(0.5f, 1e-4f));
    CHECK_THAT(rx.position.y, WithinAbs(0.0f, 1e-4f));

    // Center: mouse (400,300)->(450,250) == world (0,0)->(0.5,0.5).
    Arcane::GizmoTransform rc = Arcane::ApplyDrag(
        Arcane::GizmoMode::Translate, Arcane::GizmoSpace::World, Arcane::GizmoAxis::Center,
        start, v, glm::vec2(400, 300), glm::vec2(450, 250), noSnap);
    CHECK_THAT(rc.position.x, WithinAbs(0.5f, 1e-4f));
    CHECK_THAT(rc.position.y, WithinAbs(0.5f, 1e-4f));
}

TEST_CASE("Gizmo ApplyDrag: translate local axis rotates the direction", "[gizmo]")
{
    const Arcane::GizmoView v = MakeView();
    Arcane::GizmoTransform start;
    start.rotation = 3.14159265f * 0.5f;          // 90deg -> local X points +Y
    const Arcane::GizmoSnap noSnap;

    // Drag world delta (0.5, 0.3); local-X = (0,1) so only the Y component projects.
    Arcane::GizmoTransform r = Arcane::ApplyDrag(
        Arcane::GizmoMode::Translate, Arcane::GizmoSpace::Local, Arcane::GizmoAxis::X,
        start, v, glm::vec2(400, 300), glm::vec2(450, 270), noSnap);
    CHECK_THAT(r.position.x, WithinAbs(0.0f, 1e-4f));
    CHECK_THAT(r.position.y, WithinAbs(0.3f, 1e-4f));
}

TEST_CASE("Gizmo ApplyDrag: rotate delta-angle + snap", "[gizmo]")
{
    const Arcane::GizmoView v = MakeView();
    Arcane::GizmoTransform start;                 // rotation 0, pivot (0,0)
    Arcane::GizmoSnap noSnap;

    // Mouse from world (1,0) [angle 0] to (0,1) [angle +90deg].
    Arcane::GizmoTransform r = Arcane::ApplyDrag(
        Arcane::GizmoMode::Rotate, Arcane::GizmoSpace::World, Arcane::GizmoAxis::Center,
        start, v, glm::vec2(500, 300), glm::vec2(400, 200), noSnap);
    CHECK_THAT(r.rotation, WithinAbs(3.14159265f * 0.5f, 1e-3f));

    // Snap 15deg: rotate ~20deg -> 15deg. cos/sin(20deg)=(0.9397,0.3420).
    Arcane::GizmoSnap snap; snap.enabled = true; snap.rotationDeg = 15.0f;
    Arcane::GizmoTransform rs = Arcane::ApplyDrag(
        Arcane::GizmoMode::Rotate, Arcane::GizmoSpace::World, Arcane::GizmoAxis::Center,
        start, v, glm::vec2(500, 300), glm::vec2(400 + 93.97f, 300 - 34.20f), snap);
    CHECK_THAT(rs.rotation, WithinAbs(3.14159265f / 12.0f, 1e-3f));   // 15deg
}

TEST_CASE("Gizmo ApplyDrag: scale ratio, uniform, clamp, snap", "[gizmo]")
{
    const Arcane::GizmoView v = MakeView();
    Arcane::GizmoTransform start;                 // scale (1,1), rot 0
    Arcane::GizmoSnap noSnap;

    // Axis X: world (1,0)->(2,0) => factor 2 on x only.
    Arcane::GizmoTransform rx = Arcane::ApplyDrag(
        Arcane::GizmoMode::Scale, Arcane::GizmoSpace::Local, Arcane::GizmoAxis::X,
        start, v, glm::vec2(500, 300), glm::vec2(600, 300), noSnap);
    CHECK_THAT(rx.scale.x, WithinAbs(2.0f, 1e-4f));
    CHECK_THAT(rx.scale.y, WithinAbs(1.0f, 1e-4f));

    // Center uniform: |world| 1 -> 2 => (2,2).
    Arcane::GizmoTransform rc = Arcane::ApplyDrag(
        Arcane::GizmoMode::Scale, Arcane::GizmoSpace::Local, Arcane::GizmoAxis::Center,
        start, v, glm::vec2(500, 300), glm::vec2(400, 100), noSnap);
    CHECK_THAT(rc.scale.x, WithinAbs(2.0f, 1e-4f));
    CHECK_THAT(rc.scale.y, WithinAbs(2.0f, 1e-4f));

    // Clamp: dragging the axis point onto the pivot => factor 0 => clamped > 0.
    Arcane::GizmoTransform rz = Arcane::ApplyDrag(
        Arcane::GizmoMode::Scale, Arcane::GizmoSpace::Local, Arcane::GizmoAxis::X,
        start, v, glm::vec2(500, 300), glm::vec2(400, 300), noSnap);
    CHECK(rz.scale.x > 0.0f);

    // Snap 0.1: factor 1.37 -> 1.4.
    Arcane::GizmoSnap snap; snap.enabled = true; snap.scale = 0.1f;
    Arcane::GizmoTransform rsn = Arcane::ApplyDrag(
        Arcane::GizmoMode::Scale, Arcane::GizmoSpace::Local, Arcane::GizmoAxis::X,
        start, v, glm::vec2(500, 300), glm::vec2(400 + 137.0f, 300), snap);
    CHECK_THAT(rsn.scale.x, WithinAbs(1.4f, 1e-4f));
}
```

- [ ] **Step 3: Run it, verify it fails** (no `ApplyDrag`). Regenerate first (new files): `& "Arcane\GenerateProjects.bat"`, build `ArcaneTests`, then from the exe dir `.\ArcaneTests.exe "[gizmo]"`. Expected: link/compile error.

- [ ] **Step 4: Write `Gizmo.cpp` (view helpers + `ApplyDrag` + math helpers; leave `HitTest`/`Draw` for Tasks 2/3 — do NOT define them yet).**

```cpp
#include <Arcane/Edit/Gizmo.hpp>

#include <cmath>

namespace Arcane
{
    namespace
    {
        constexpr float kEps      = 1e-6f;
        constexpr float kMinScale = 0.01f;

        glm::vec2 WorldToScreen(const GizmoView& v, glm::vec2 world)
        {
            const float s = v.zoom * v.pixelsPerMeter;
            const glm::vec2 c = v.viewportOriginPx + v.viewportSizePx * 0.5f;
            const glm::vec2 d = (world - v.cameraOffset) * s;
            return glm::vec2(c.x + d.x, c.y - d.y);   // screen-Y down
        }

        glm::vec2 ScreenToWorld(const GizmoView& v, glm::vec2 screen)
        {
            const float s = v.zoom * v.pixelsPerMeter;
            const glm::vec2 c = v.viewportOriginPx + v.viewportSizePx * 0.5f;
            const glm::vec2 d(screen.x - c.x, -(screen.y - c.y));
            return v.cameraOffset + (s > kEps ? d / s : glm::vec2(0.0f));
        }

        glm::vec2 AxisDirWorld(GizmoAxis axis)
        {
            return axis == GizmoAxis::Y ? glm::vec2(0.0f, 1.0f) : glm::vec2(1.0f, 0.0f);
        }

        glm::vec2 AxisDirLocal(float rot, GizmoAxis axis)
        {
            const float c = std::cos(rot), s = std::sin(rot);
            return axis == GizmoAxis::Y ? glm::vec2(-s, c) : glm::vec2(c, s);
        }

        // Translate uses space; scale is always local.
        glm::vec2 AxisDir(GizmoSpace space, float rot, GizmoAxis axis)
        {
            return space == GizmoSpace::Local ? AxisDirLocal(rot, axis) : AxisDirWorld(axis);
        }

        float SnapScalar(float v, float step)
        {
            return step > kEps ? std::round(v / step) * step : v;
        }
    }

    GizmoTransform ApplyDrag(GizmoMode mode, GizmoSpace space, GizmoAxis axis,
                             const GizmoTransform& start, const GizmoView& view,
                             glm::vec2 mouseStartScreen, glm::vec2 mouseCurScreen,
                             const GizmoSnap& snap)
    {
        GizmoTransform r = start;
        const glm::vec2 pStart = ScreenToWorld(view, mouseStartScreen);
        const glm::vec2 pCur   = ScreenToWorld(view, mouseCurScreen);
        const glm::vec2 pivot  = start.position;

        switch (mode)
        {
            case GizmoMode::Translate:
            {
                const glm::vec2 delta = pCur - pStart;
                if (axis == GizmoAxis::Center)
                {
                    r.position = start.position + delta;
                    if (snap.enabled)
                    {
                        r.position.x = SnapScalar(r.position.x, snap.translate);
                        r.position.y = SnapScalar(r.position.y, snap.translate);
                    }
                }
                else
                {
                    const glm::vec2 dir = AxisDir(space, start.rotation, axis);
                    r.position = start.position + glm::dot(delta, dir) * dir;
                    if (snap.enabled)   // snap only the moved axis component
                    {
                        if (axis == GizmoAxis::X) r.position.x = SnapScalar(r.position.x, snap.translate);
                        else                      r.position.y = SnapScalar(r.position.y, snap.translate);
                    }
                }
                break;
            }
            case GizmoMode::Rotate:
            {
                const glm::vec2 d0 = pStart - pivot;
                const glm::vec2 d1 = pCur - pivot;
                const float a0 = std::atan2(d0.y, d0.x);
                const float a1 = std::atan2(d1.y, d1.x);
                r.rotation = start.rotation + (a1 - a0);
                if (snap.enabled)
                {
                    const float step = snap.rotationDeg * 3.14159265358979323846f / 180.0f;
                    r.rotation = SnapScalar(r.rotation, step);
                }
                break;
            }
            case GizmoMode::Scale:
            {
                if (axis == GizmoAxis::Center)
                {
                    const float l0 = glm::length(pStart - pivot);
                    const float l1 = glm::length(pCur - pivot);
                    const float f = (l0 > kEps) ? (l1 / l0) : 1.0f;
                    r.scale = start.scale * f;
                }
                else
                {
                    const glm::vec2 dir = AxisDirLocal(start.rotation, axis);   // scale is local
                    const float d0 = glm::dot(pStart - pivot, dir);
                    const float d1 = glm::dot(pCur - pivot, dir);
                    const float f = (std::fabs(d0) > kEps) ? (d1 / d0) : 1.0f;
                    if (axis == GizmoAxis::X) r.scale.x = start.scale.x * f;
                    else                      r.scale.y = start.scale.y * f;
                }
                if (snap.enabled)
                {
                    r.scale.x = SnapScalar(r.scale.x, snap.scale);
                    r.scale.y = SnapScalar(r.scale.y, snap.scale);
                }
                r.scale.x = std::max(r.scale.x, kMinScale);
                r.scale.y = std::max(r.scale.y, kMinScale);
                break;
            }
        }
        return r;
    }
}
```

- [ ] **Step 5: Regenerate + build + run, verify PASS.** `& "Arcane\GenerateProjects.bat"`, build `ArcaneTests`, `.\ArcaneTests.exe "[gizmo]"`. Expected: 4 cases pass.

- [ ] **Step 6: Commit** — `feat(arcane): Gizmo core -- transform math (ApplyDrag) + view helpers`.

---

## Task 2: `HitTest` — analytic handle picking (headless `[gizmo]`)

Screen-space handle picking. Consumes the T1 types + view helpers.

**Files:** Modify `Gizmo.cpp` (add `HitTest` + its screen-projection helpers). Modify `GizmoTest.cpp` (append HitTest cases).

**Interfaces:**
- Consumes: T1 `GizmoView`, `GizmoTransform`, enums.
- Produces: `GizmoAxis HitTest(...)` (already declared in `Gizmo.hpp`).

- [ ] **Step 1: Write the failing tests** (append to `GizmoTest.cpp`):

```cpp
TEST_CASE("Gizmo HitTest: translate axes, center, and miss", "[gizmo]")
{
    const Arcane::GizmoView v = MakeView();
    Arcane::GizmoTransform t;                      // pivot screen (400,300)

    // On the +X arrow (pivot .. pivot+~80px right).
    CHECK(Arcane::HitTest(Arcane::GizmoMode::Translate, Arcane::GizmoSpace::World, t, v,
                          glm::vec2(450, 300)) == Arcane::GizmoAxis::X);
    // On the +Y arrow (screen-up = -Y screen).
    CHECK(Arcane::HitTest(Arcane::GizmoMode::Translate, Arcane::GizmoSpace::World, t, v,
                          glm::vec2(400, 260)) == Arcane::GizmoAxis::Y);
    // On the center handle (priority over axes).
    CHECK(Arcane::HitTest(Arcane::GizmoMode::Translate, Arcane::GizmoSpace::World, t, v,
                          glm::vec2(401, 301)) == Arcane::GizmoAxis::Center);
    // Off-gizmo.
    CHECK(Arcane::HitTest(Arcane::GizmoMode::Translate, Arcane::GizmoSpace::World, t, v,
                          glm::vec2(700, 300)) == Arcane::GizmoAxis::None);
}

TEST_CASE("Gizmo HitTest: rotate ring band, scale axes", "[gizmo]")
{
    const Arcane::GizmoView v = MakeView();
    Arcane::GizmoTransform t;                      // pivot screen (400,300)

    // Rotate: ring radius ~64px -> a point ~64px from pivot hits (reported Center).
    CHECK(Arcane::HitTest(Arcane::GizmoMode::Rotate, Arcane::GizmoSpace::World, t, v,
                          glm::vec2(464, 300)) == Arcane::GizmoAxis::Center);
    // Rotate: near the pivot (inside the ring) misses.
    CHECK(Arcane::HitTest(Arcane::GizmoMode::Rotate, Arcane::GizmoSpace::World, t, v,
                          glm::vec2(410, 300)) == Arcane::GizmoAxis::None);
    // Scale: on the +X handle.
    CHECK(Arcane::HitTest(Arcane::GizmoMode::Scale, Arcane::GizmoSpace::Local, t, v,
                          glm::vec2(450, 300)) == Arcane::GizmoAxis::X);
}
```

- [ ] **Step 2: Run headless, verify fail** (`.\ArcaneTests.exe "[gizmo]"`): link error (no `HitTest`).

- [ ] **Step 3: Add `HitTest` to `Gizmo.cpp`** (inside `namespace Arcane`, after `ApplyDrag`; add the pixel constants + a segment-distance helper to the anonymous namespace):

```cpp
    // --- add to the anonymous namespace ---
    constexpr float kAxisLenPx    = 80.0f;   // arrow / scale-handle length
    constexpr float kHitThreshPx  = 8.0f;    // axis segment pick radius
    constexpr float kCenterHalfPx = 8.0f;    // center box half-extent
    constexpr float kRingRadiusPx = 64.0f;   // rotate ring radius
    constexpr float kRingBandPx   = 8.0f;    // rotate ring pick band

    // Screen-space unit direction of a world axis at the pivot (handles Y-flip).
    glm::vec2 AxisDirScreen(const GizmoView& v, glm::vec2 pivotWorld, glm::vec2 dirWorld)
    {
        const glm::vec2 a = WorldToScreen(v, pivotWorld);
        const glm::vec2 b = WorldToScreen(v, pivotWorld + dirWorld);
        const glm::vec2 d = b - a;
        const float len = glm::length(d);
        return len > kEps ? d / len : glm::vec2(1.0f, 0.0f);
    }

    float DistToSegment(glm::vec2 p, glm::vec2 a, glm::vec2 b)
    {
        const glm::vec2 ab = b - a;
        const float len2 = glm::dot(ab, ab);
        const float u = len2 > kEps ? glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
        return glm::length(p - (a + u * ab));
    }
```

```cpp
    GizmoAxis HitTest(GizmoMode mode, GizmoSpace space,
                      const GizmoTransform& t, const GizmoView& view, glm::vec2 mouseScreen)
    {
        const glm::vec2 pivot = WorldToScreen(view, t.position);

        if (mode == GizmoMode::Rotate)
        {
            const float d = glm::length(mouseScreen - pivot);
            return std::fabs(d - kRingRadiusPx) <= kRingBandPx ? GizmoAxis::Center : GizmoAxis::None;
        }

        // Center handle wins on overlap.
        if (std::fabs(mouseScreen.x - pivot.x) <= kCenterHalfPx &&
            std::fabs(mouseScreen.y - pivot.y) <= kCenterHalfPx)
            return GizmoAxis::Center;

        // Scale axes are always local; translate axes follow `space`.
        const GizmoSpace axisSpace = (mode == GizmoMode::Scale) ? GizmoSpace::Local : space;
        const glm::vec2 dirX = AxisDirScreen(view, t.position, AxisDir(axisSpace, t.rotation, GizmoAxis::X));
        const glm::vec2 dirY = AxisDirScreen(view, t.position, AxisDir(axisSpace, t.rotation, GizmoAxis::Y));

        if (DistToSegment(mouseScreen, pivot, pivot + dirX * kAxisLenPx) <= kHitThreshPx) return GizmoAxis::X;
        if (DistToSegment(mouseScreen, pivot, pivot + dirY * kAxisLenPx) <= kHitThreshPx) return GizmoAxis::Y;
        return GizmoAxis::None;
    }
```

(`glm::clamp` needs `#include <glm/common.hpp>` — confirm at read-first whether `<glm/glm.hpp>` already provides it; add the include to `Gizmo.cpp` if the build complains.)

- [ ] **Step 4: Build + run, verify PASS.** Build `ArcaneTests`, `.\ArcaneTests.exe "[gizmo]"`. Expected: all `[gizmo]` cases pass (T1's 4 + T2's 2).

- [ ] **Step 5: Commit** — `feat(arcane): Gizmo HitTest -- analytic screen-space handle picking`.

---

## Task 3: `Draw` — screen-constant gizmo geometry (build-verified; desk visuals)

Emit the gizmo geometry into a `Batcher2D`. Not headless-testable (needs a real batcher/device), so verified by a clean build here + desk visuals in Task 5.

**Files:** Modify `Gizmo.cpp` (add `Draw`).

**Interfaces:**
- Consumes: T1 types + helpers, `Arcane::Batcher2D`.
- Produces: `void Draw(...)` (already declared in `Gizmo.hpp`).

- [ ] **Step 0 (read-first):** Open `Arcane/Arcane/src/Arcane/Render/Batcher2D.hpp` and confirm the EXACT draw API: the line call (signature + thickness + color type — `glm::vec4`? a `Color`?), the filled-quad/rect call, and the SDF-circle/ring call. Confirm whether a filled-triangle primitive exists (for arrowheads); if not, draw arrowheads as a short thick segment or two triangles built from quads. Note the exact names — the calls below are the intended shape and MUST be adapted to the real API.

- [ ] **Step 1: Add `Draw` to `Gizmo.cpp`.** Compute screen-space handle endpoints (reusing `WorldToScreen`, `AxisDirScreen`, and the T2 pixel constants), then emit lines + heads + ring + boxes with per-handle color (brightened when `hovered`/`active` matches). Structure:

```cpp
    namespace
    {
        // X=red, Y=green, Center=yellow; brightened when hovered/active.
        glm::vec4 AxisColor(GizmoAxis axis, GizmoAxis hovered, GizmoAxis active)
        {
            glm::vec4 base(0.85f, 0.2f, 0.2f, 1.0f);            // X
            if (axis == GizmoAxis::Y)      base = glm::vec4(0.2f, 0.85f, 0.2f, 1.0f);
            if (axis == GizmoAxis::Center) base = glm::vec4(0.9f, 0.85f, 0.2f, 1.0f);
            const bool hot = (axis == hovered) || (axis == active);
            return hot ? glm::vec4(glm::min(base.x * 1.4f, 1.0f),
                                   glm::min(base.y * 1.4f, 1.0f),
                                   glm::min(base.z * 1.4f, 1.0f), 1.0f)
                       : base;
        }
    }

    void Draw(Batcher2D& batcher, GizmoMode mode, GizmoSpace space,
              const GizmoTransform& t, const GizmoView& view,
              GizmoAxis hovered, GizmoAxis active)
    {
        const glm::vec2 pivot = WorldToScreen(view, t.position);
        const GizmoSpace axisSpace = (mode == GizmoMode::Scale) ? GizmoSpace::Local : space;
        const glm::vec2 dirX = AxisDirScreen(view, t.position, AxisDir(axisSpace, t.rotation, GizmoAxis::X));
        const glm::vec2 dirY = AxisDirScreen(view, t.position, AxisDir(axisSpace, t.rotation, GizmoAxis::Y));

        if (mode == GizmoMode::Rotate)
        {
            // Ring (SDF circle outline) -- use the real Batcher2D ring/circle call.
            // batcher.<DrawCircleOutline>(pivot, kRingRadiusPx, thickness, AxisColor(Center, hovered, active));
            return;
        }

        // Axis X + Y: shaft line + head (arrow for Translate, box for Scale).
        const glm::vec2 tipX = pivot + dirX * kAxisLenPx;
        const glm::vec2 tipY = pivot + dirY * kAxisLenPx;
        // batcher.<DrawLine>(pivot, tipX, thickness, AxisColor(X, hovered, active));
        // batcher.<DrawLine>(pivot, tipY, thickness, AxisColor(Y, hovered, active));
        // if Translate: arrowheads (filled triangle / short thick segment) at tipX/tipY.
        // if Scale: small filled box at tipX/tipY (AxisColor(X/Y,...)).
        // Center box at pivot (half kCenterHalfPx), AxisColor(Center, hovered, active).
    }
```

Fill in the commented `batcher.<...>` lines with the real `Batcher2D` calls confirmed at Step 0 (color type, thickness, primitive names). Keep the geometry math above (endpoints, colors, screen-constant sizes) as-is.

- [ ] **Step 2: Regenerate + build, verify clean.** `& "Arcane\GenerateProjects.bat"`, build `ArcaneTests` (the Arcane lib compiles `Draw`); expect zero errors. `.\ArcaneTests.exe "[gizmo]"` still green (unchanged count).

- [ ] **Step 3: Commit** — `feat(arcane): Gizmo Draw -- screen-constant procedural handle geometry`.

---

## Task 4: Grimoire integration — viewport interaction + drag->command + keys + toolbar

Grimoire owns the gizmo state, drives the per-frame interaction in the Viewport, brackets each drag into the `CommandStack`, and exposes mode/space controls. Not headless-testable (ImGui + GPU viewport); verified by a clean build + desk.

**Files:** Modify `GrimoireApp.{hpp,cpp}`, `EditorPanels.{hpp,cpp}`.

**Interfaces:**
- Consumes: `Arcane::Gizmo` (`HitTest`/`Draw`/`ApplyDrag`), `Arcane::CommandStack` (`Begin`/`SnapshotComponent`/`Commit`/`Cancel`), `Grimoire::SelectionContext`.

- [ ] **Step 0 (read-first):** Re-read `Arcane/Grimoire/src/GrimoireApp.cpp` around the Viewport draw + the input block + the existing click-pick call, and `EditorPanels.cpp`'s `DrawSimTimeToolbar`. Confirm: (a) how the viewport camera (offset/zoom) is stored and how the click-pick builds its `view` — map that SAME data into a `GizmoView` (origin/size from the viewport image rect; ppm from the world scale) so the gizmo aligns with the scene; (b) the `InputSnapshot` mouse API (screen position, button down/pressed/released) + the viewport-hover + ImGui-mouse-capture accessors (reuse what click-pick + T3 keybinds used); (c) where the viewport's world-space `Batcher2D` pass is, to call `Gizmo::Draw` after the scene; (d) that W/E/R don't collide with viewport camera keys (fall back to 1/2/3 if they do); (e) how the Inspector resolves the `LocalTransform` `ComponentDescriptor` (via `InspectEntity`, matching `meta->typeName == "Arcane::LocalTransform"`).

- [ ] **Step 1: Gizmo state in `GrimoireApp.hpp`.** Add `#include <Arcane/Edit/Gizmo.hpp>` and, beside `m_gizmoMode` area (near `m_selection`/`m_undo`):

```cpp
        // Transform-gizmo state (Edit-mode; drives the Viewport handles).
        Arcane::GizmoMode  m_gizmoMode  = Arcane::GizmoMode::Translate;
        Arcane::GizmoSpace m_gizmoSpace = Arcane::GizmoSpace::World;
        struct GizmoDrag
        {
            bool                  active = false;
            Arcane::GizmoAxis     axis   = Arcane::GizmoAxis::None;
            Arcane::GizmoTransform start;
            glm::vec2             mouseStartScreen{0.0f, 0.0f};
        } m_gizmoDrag;
```

- [ ] **Step 2: Per-frame interaction in `GrimoireApp.cpp`** (in the Viewport handling, Edit-mode + has-selection + viewport-hovered + not-ImGui-mouse-captured). Map `LocalTransform` <-> `GizmoTransform`, build the `GizmoView` from the viewport camera (Step 0), then:

```cpp
    // -- inside the viewport block; `reg`, `view`, `mouseScreen` per Step 0 --
    if (!m_play.IsPlaying() && m_selection.HasSelection() && viewportHovered && !imguiWantsMouse)
    {
        const Astra::Entity sel = m_selection.selected;
        Arcane::LocalTransform* lt = reg.GetComponent<Arcane::LocalTransform>(sel);
        if (lt)
        {
            Arcane::GizmoTransform gt{ lt->position, lt->rotation, lt->scale };

            if (!m_gizmoDrag.active)
            {
                const Arcane::GizmoAxis hovered =
                    Arcane::HitTest(m_gizmoMode, m_gizmoSpace, gt, view, mouseScreen);

                if (hovered != Arcane::GizmoAxis::None && mousePressedLeft)
                {
                    // Resolve the LocalTransform descriptor (Step 0e) and open the txn.
                    const Astra::ComponentDescriptor* desc = /* InspectEntity match "Arcane::LocalTransform" */;
                    m_undo->Begin("Gizmo");
                    m_undo->SnapshotComponent(sel, desc);
                    m_gizmoDrag = { true, hovered, gt, mouseScreen };
                }
                // draw uses `hovered` for the highlight
                Arcane::Draw(viewportBatcher, m_gizmoMode, m_gizmoSpace, gt, view,
                             hovered, Arcane::GizmoAxis::None);
            }
            else
            {
                Arcane::GizmoSnap snap;
                snap.enabled = ctrlHeld;   // reuse the T3 Ctrl detection
                const Arcane::GizmoTransform nt = Arcane::ApplyDrag(
                    m_gizmoMode, m_gizmoSpace, m_gizmoDrag.axis, m_gizmoDrag.start, view,
                    m_gizmoDrag.mouseStartScreen, mouseScreen, snap);
                lt->position = nt.position; lt->rotation = nt.rotation; lt->scale = nt.scale;

                Arcane::Draw(viewportBatcher, m_gizmoMode, m_gizmoSpace, nt, view,
                             m_gizmoDrag.axis, m_gizmoDrag.axis);

                if (mouseReleasedLeft)
                {
                    m_undo->Commit();          // no-move drag self-drops (after==before)
                    m_gizmoDrag = {};
                }
            }
        }
    }
```

Adapt the placeholder tokens (`reg`, `view`, `mouseScreen`, `viewportHovered`, `imguiWantsMouse`, `mousePressedLeft`, `mouseReleasedLeft`, `ctrlHeld`, `viewportBatcher`, the descriptor resolve) to the real APIs found at Step 0. If `desc` is null, skip the Begin/Snapshot (no drag).

- [ ] **Step 3: W/E/R mode keys in the input block** (edge-triggered, same gating/edge-detection as the T3 undo keys — Edit-mode, viewport focus, not-capturing-keyboard). Use W=Translate, E=Rotate, R=Scale (or 1/2/3 if W/E/R collide per Step 0d):

```cpp
        if (!m_play.IsPlaying() && viewportFocused && !imguiWantsKeyboard)
        {
            if (keyPressed_W) m_gizmoMode = Arcane::GizmoMode::Translate;
            if (keyPressed_E) m_gizmoMode = Arcane::GizmoMode::Rotate;
            if (keyPressed_R) m_gizmoMode = Arcane::GizmoMode::Scale;
        }
```

(Use the same member-persisted edge detection the T3 keybinds use; add `m_prev*` flags as needed.)

- [ ] **Step 4: Toolbar controls.** In `EditorPanels.cpp` `DrawSimTimeToolbar`, after the Undo/Redo buttons, add mode buttons + a Global/Local toggle driving refs passed in. Extend the signature to take `Arcane::GizmoMode& mode, Arcane::GizmoSpace& space` (hpp + cpp + the call site in `GrimoireApp.cpp`):

```cpp
        ImGui::SameLine(); ImGui::TextUnformatted("|"); ImGui::SameLine();
        if (ImGui::RadioButton("T", mode == Arcane::GizmoMode::Translate)) mode = Arcane::GizmoMode::Translate;
        ImGui::SameLine();
        if (ImGui::RadioButton("R", mode == Arcane::GizmoMode::Rotate))    mode = Arcane::GizmoMode::Rotate;
        ImGui::SameLine();
        if (ImGui::RadioButton("S", mode == Arcane::GizmoMode::Scale))     mode = Arcane::GizmoMode::Scale;
        ImGui::SameLine();
        {
            bool local = (space == Arcane::GizmoSpace::Local);
            if (ImGui::Checkbox("Local", &local))
                space = local ? Arcane::GizmoSpace::Local : Arcane::GizmoSpace::World;
        }
```

Add `#include <Arcane/Edit/Gizmo.hpp>` to `EditorPanels.hpp`; pass `m_gizmoMode`/`m_gizmoSpace` from the `DrawSimTimeToolbar` call site.

- [ ] **Step 5: Regenerate + build `ArcaneTests;Grimoire`, verify clean.** `& "Arcane\GenerateProjects.bat"`, build both. Expected: clean build, `[gizmo]` unchanged, `[grimoire]` still green.

- [ ] **Step 6: Commit** — `feat(grimoire): transform gizmo in the viewport (W/E/R, Global/Local, drag->undo)`.

---

## Task 5: Gate + desk-verify

- [ ] **Step 1: Headless gate** (runs here): from the exe dir `.\ArcaneTests.exe "~[gpu]"` — the new `[gizmo]` cases pass and the CPU floor (27809/336 + the new `[gizmo]` cases) does not drop. Record the new count.
- [ ] **Step 2: Desk interactive** (Grimoire, at the desk — GPU hazard headless): select an entity; **W/E/R** switch translate/rotate/scale; the gizmo is **screen-constant** across zoom; drag an axis / center / the ring — the transform updates live and the **physics body follows** (SPEC #1 reconcile); **Ctrl+Z** reverts the whole drag in one step and **Ctrl+Y** re-applies; **Ctrl-hold** snaps (grid / 15deg / 0.1); the **Global/Local** toggle flips the translate axes (scale stays local); **Play** disables the gizmo + clears history; hover highlights the handle; no NVRHI/validation noise in the console.
- [ ] **Step 3:** Append a completion note to `.superpowers/sdd/progress.md`.

---

## Self-Review

**Spec coverage:** architecture (Arcane core + Grimoire consumer) -> T1-T4; core API `HitTest`/`Draw`/`ApplyDrag` + types -> T1 (types+ApplyDrag), T2 (HitTest), T3 (Draw); handle layout + screen-constant sizing -> T2/T3 (pixel constants); interaction + drag->command bracketing -> T4 Step 2; mode keys + Global/Local toolbar -> T4 Steps 3-4; transform math (translate world/local, rotate+snap, scale ratio/uniform/clamp/snap) -> T1 + tests; edge cases (no-move drop, off-handle, degenerate scale, Play gate) -> T1 (clamp/eps) + T4 (gating) + inherent (Commit drop); testing -> T1/T2 headless + T3/T4/T5 build+desk; non-goals untouched; implementation-time confirmations -> T1 Step 0, T3 Step 0, T4 Step 0. Covered.

**Placeholder scan:** the flagged items (viewport world<->screen convention, exact `Batcher2D` draw API, `InputSnapshot`/camera/descriptor plumbing, W/E/R key conflict) are read-first confirmations against named files with the intended code shown, not deferred TODOs. The T4 Step 2 `/* ... */` tokens are explicitly enumerated as read-first adaptations with their sources named. No "TBD"/"add error handling"/uncoded logic steps.

**Type consistency:** `GizmoMode{Translate,Rotate,Scale}`, `GizmoSpace{World,Local}`, `GizmoAxis{None,X,Y,Center}`, `GizmoTransform{position,rotation,scale}`, `GizmoView{cameraOffset,zoom,pixelsPerMeter,viewportOriginPx,viewportSizePx}`, `GizmoSnap{enabled,translate,rotationDeg,scale}`, `HitTest(mode,space,t,view,mouseScreen)`, `Draw(batcher,mode,space,t,view,hovered,active)`, `ApplyDrag(mode,space,axis,start,view,mouseStartScreen,mouseCurScreen,snap)` — consistent across tasks and matching the spec.
