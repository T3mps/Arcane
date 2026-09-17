# F4 Plan 1 — +Y up, one ViewTransform, the editor camera, grids, mesh-create polish

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land F4's foundation — the world flips to +Y up, every camera consumer takes one `ViewTransform`, the editor viewport gains a 3D camera with a `2D | Persp` toggle, a 2D line grid and a depth-tested 3D grid, and mesh creation gains per-primitive entries and a scene-level "Add 3D Object" — so F3 can start and plan 2 (mesh picking + the 3D gizmo) has its APIs.

**Architecture:** `Arcane::ViewTransform` (Core) is the one camera type; the orthographic scene camera and the editor's two persisted camera transforms both produce it, and `ClientRuntime::SetView` replaces `SetCamera`. The batcher gains a WORLD space for sprites (vec3 vertices, view-projection in the shader) beside its existing pixel OVERLAY space; the 2D-era overlays (gizmo, pick emitter, physics debug draw, camera rect) project through a per-axis `Affine2D` derived from the orthographic view so their change is mechanical. A new `GridNode` after `MeshNode` draws an analytic ground grid depth-tested against the mesh pass's depth transient, which `AddMeshNode` already returns.

**Tech Stack:** C++23, glm, Astra (vendored), NRI render graph (D3D12 + Vulkan, dxc offline via `data/shaders/compile-shaders.bat`), ImGui (`ImGuiSettingsHandler` for persistence), Catch2 + rapidcheck (ArcaneTests), premake5 → `Arcane.slnx`.

**Spec:** `docs/specs/2026-09-17-f4-editor-3d-authoring-design.md` (read §2 the flip, §3 the view transform, §4 the camera, §5 grids, §8 mesh-create, §9 testing, §13 rulings R1–R9). Plan 2 (picking + gizmo) is written after this plan closes.

## Global Constraints

- **One right-handed world, +Y up, camera forward −Z, depth [0,1] forward-Z** (spec §2; `SceneCamera.hpp`'s pinned conventions). The 2D plane is XY viewed down −Z. Never reintroduce a Y flip in world space; the flip lives only in the orthographic NDC → pixel mapping.
- **Units are metres (MKS)**; the grid unit is the metre and is independent of `GizmoSnap::translate` (R7).
- **No new third-party dependencies.** No Source 2 / Unreal code is copied (shapes only).
- **Batcher2D: new virtuals go at the END of the class** (`Batcher2D.hpp` "NEW VIRTUALS GO AT THE END"), and the plugin ABI (`PluginABI.hpp` `kGamePluginABIVersion`) bumps with a changelog line whenever `ClientRuntime`, `Batcher2D`'s vtable, `RenderContext2D` or `EngineContext` change shape. ReferenceProject's `.arcproj` `engine.abi` restamps in the same commit.
- **Build:** `cd D:\dev\starworks\Arcane && msbuild Arcane.slnx /p:Configuration=Debug /m` (Release the same way before the close). Shaders compile in the prebuild via `data/shaders/compile-shaders.bat` (dxc, DXIL + SPIR-V).
- **Tests run FROM THE EXE DIRECTORY:** `cd bin\Debug-windows-x86_64-md\ArcaneTests && .\ArcaneTests.exe "<tag>"`. The suite runs in random order; on a failure capture the seed. `[gpu]` cases need the desk GPU; `~[gpu]` is the baseline-comparable invocation (`scripts/automation-baselines.json`, currently 57613 / 1794 Debug and Release), re-booked at this plan's close by `scripts/check-baselines.ps1`.
- **Goldens move ONCE** (the flip + the grid): re-bless by `--bless` into the STAGED slot from the host's exe dir, then COPY TO SOURCE IMMEDIATELY (`ReferenceProject/Verify/References/`), then run `scripts/golden-gate.ps1` in both configs (memory: the msbuild post-build restages `Verify/` and clobbers a staged-only bless). Delete the exe-dir `imgui.ini` before a golden run.
- **Commit after every task**, message style `feat(scope): ...` / `fix(scope): ...`, ending with the session's attribution trailer.
- **Scope guard:** picking meshes, the 3D gizmo, Top/Side views, ortho near/far auto-calc, sprite depth are NOT this plan (spec §7, §12). In Perspective mode the gizmo and click-pick are hidden/disabled until plan 2 (Task 7 states it in code and in the view-settings tooltip).

---

## File structure

| Path | Responsibility |
|---|---|
| `ArcaneCore/src/Arcane/Scene/ViewTransform.hpp` (new) | `ViewTransform`, `Ray`, `Affine2D`; pure math, header-only |
| `ArcaneCore/src/Arcane/Scene/SceneCamera.hpp` | `ActiveSceneCamera` → `SceneCameraView` gains `ViewTransform view`; `PerspectiveCameraView` becomes an alias of `ViewTransform` |
| `ArcaneCore/src/Arcane/Scene/Components.hpp`, `Project/ProjectManifest.hpp`, `Project/Project.cpp` | gravity defaults → (0, −9.81); tooltips |
| `ArcaneCore/src/Arcane/Serialization/SceneSerializer.hpp` | `kSceneJsonVersion = 6`; `Detail::MigrateV5ToYUp(json&)` applied on load for version ≤ 5 |
| `ArcaneCore/src/Arcane/Scene/SceneResources.hpp` | `RenderContext2D { batcher, ViewTransform view, alpha }` |
| `ArcaneClient/src/Arcane/Client/ClientRuntime.{hpp,cpp}`, `RuntimePresentation.hpp` | `SetView` / `View()` replace `SetCamera` / `CameraOffset` / `CameraZoom` |
| `ArcaneClient/src/Arcane/Render/Batcher2D.{hpp,cpp}` | `Batch2DVertex::pos` → vec3; `Batch2DDrawSpan::worldSpace`; `QuadWorld`, `CircleWorld` (appended virtuals) |
| `data/shaders/sprite.hlsl`, `circle.hlsl`, `sprite_material.hlsl` | 80-byte `BatchConstants { viewProj, invHalfViewport, worldSpace }`; VS branches on `worldSpace` |
| `ArcaneClient/src/Arcane/Render/Nri/nodes/Batch2DNode.cpp` | root constants 80 bytes, per-span `worldSpace`, POSITION = RGB32 |
| `ArcaneClient/src/Arcane/Render/RenderSystems.hpp` | sprites submit world-space corners from the full basis |
| `ArcaneClient/src/Arcane/Render/PhysicsDebugDraw.{hpp,cpp}`, `PickEmit.{hpp,cpp}`, `Edit/Gizmo.{hpp,cpp}` | take `Affine2D` (per-axis scale, angle sign) instead of `{offset, scale}` |
| `ArcaneClient/src/Arcane/Render/Nri/nodes/GridNode.{hpp,cpp}` (new), `data/shaders/grid.hlsl` (new) | the analytic depth-tested 3D grid |
| `ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.{hpp,cpp}` | `FrameDesc::grid`, `RgFrameShape::grid`, the `grid` node declared after `mesh`; `Grid()` accessor |
| `ArcaneEditor/src/Viewport/EditorCamera.{hpp,cpp}` | `ViewMode`, `Ortho2D`, `Orbit3D`, `Resolve`, navigation ops, framing |
| `ArcaneEditor/src/Viewport/ViewportGrid.{hpp,cpp}` (new) | the 2D grid: pure LOD plan + overlay draw |
| `ArcaneEditor/src/Viewport/ViewportSettings.{hpp,cpp}` (new) | persisted view settings + the `[EditorViewport][Camera]` handler + `--view-mode` seed |
| `ArcaneEditor/src/App/EditorAppFrame.cpp`, `EditorApp.{hpp,cpp}`, `Scene/EditModeSchedule.{hpp,cpp}` | input, pushes, mesh-pass camera, camera rect, gizmo/pick gating, framing |
| `ArcaneEditor/src/Panels/EditorPanels.{hpp,cpp}` | `2D \| Persp` control + view-settings dropdown on the viewport overlay; Outliner "Add 3D Object" |
| `ArcaneEditor/src/Panels/CreateAssetDialog.hpp`, `AssetPanelCommon.cpp`, `App/EditorAppProject.cpp`, `App/EditorAppFrame.cpp` | `Create > Mesh >` per-primitive; `MintOrReusePrimitiveMesh` |
| `ArcaneClient/src/Arcane/Host/HostConfig.{hpp,cpp}` | `--view-mode {2d\|perspective}` |
| `ArcaneTests/src/ViewTransformTest.cpp`, `EditorCameraTest.cpp`, `ViewportGridTest.cpp`, `SceneMigrationTest.cpp` (new/rewritten), `RenderGraphTest.cpp`, `GridNodeTest.cpp` (new), `Batcher2DTest.cpp`, `RenderSubmissionTest.cpp` | the pins |
| `ReferenceProject/Content/scenes/{main,physics}.arcscene`, `ReferenceProject/Verify/References/*.png`, `scripts/automation-baselines.json` | re-saved at v6; goldens re-blessed; baselines re-booked |

---

### Task 1: `ViewTransform` in Core

**Files:**
- Create: `ArcaneCore/src/Arcane/Scene/ViewTransform.hpp`
- Test: `ArcaneTests/src/ViewTransformTest.cpp` (new; add to `premake5.lua`'s ArcaneTests `files` list if the glob does not pick up `ArcaneTests/src/**` — it does: `files { "%{prj.location}/src/**.cpp" ... }` at line 913 covers it)

**Interfaces:**
- Produces:
  ```cpp
  namespace Arcane {
  struct Ray { glm::vec3 origin; glm::vec3 direction; };
  struct Affine2D {            // the legacy 2D overlay mapping, PER-AXIS scale
      glm::vec2 offset{0,0}; glm::vec2 scale{1,1};
      glm::vec2 Point(glm::vec2 world) const noexcept;      // world.xy -> pixels
      glm::vec2 Unpoint(glm::vec2 pixel) const noexcept;    // pixels -> world.xy
      float     Length(float metres) const noexcept;        // |scale.x| * metres
      float     AngleSign() const noexcept;                 // -1 when the map mirrors (scale.x*scale.y < 0)
  };
  struct ViewTransform {
      glm::mat4 view{1}; glm::mat4 projection{1}; glm::uvec2 viewport{0,0};
      glm::mat4 ViewProjection() const noexcept;
      bool IsOrthographic() const noexcept;
      glm::vec3 WorldToScreen(glm::vec3 world) const noexcept;   // .xy pixels (y down), .z ndc depth; NaN when w ~ 0
      Ray ScreenToRay(glm::vec2 pixel) const noexcept;
      std::optional<Affine2D> AsAffine2D() const noexcept;        // orthographic + axis-aligned only
      static ViewTransform Orthographic(glm::vec2 center, float halfHeight, glm::uvec2 viewport,
                                        float nearZ = -1000.0f, float farZ = 1000.0f) noexcept;
      static ViewTransform Perspective(glm::vec3 eye, glm::vec3 target, glm::vec3 up, float fovYDegrees,
                                       glm::uvec2 viewport, float nearZ, float farZ) noexcept;
  };
  }
  ```

- [ ] **Step 1: Write the failing tests**

`ArcaneTests/src/ViewTransformTest.cpp`:
```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <Arcane/Scene/ViewTransform.hpp>
#include <cmath>
using Catch::Approx; using Arcane::ViewTransform;

TEST_CASE("Orthographic: +Y up -- a point above the centre lands ABOVE the viewport centre", "[viewtransform]")
{
    const auto v = ViewTransform::Orthographic({0,0}, 5.0f, {800,600});
    const glm::vec3 p = v.WorldToScreen({0, 1, 0});
    CHECK(p.x == Approx(400.0f));
    CHECK(p.y < 300.0f);                      // screen y is DOWN, world +Y is UP
    CHECK(p.y == Approx(300.0f - 60.0f));     // 600 px / (2*5 m) = 60 px per metre
    CHECK(p.z == Approx(0.5f));               // z = 0 sits mid-range of [-1000, 1000]
}

TEST_CASE("Orthographic matches the old affine mapping in X for the same centre and half-height", "[viewtransform]")
{
    // The retired mapping: screen = world * zoom + offset with zoom = H/(2*halfH), offset = viewport/2 - center*zoom.
    const glm::vec2 center{2.5f, -1.0f}; const float halfH = 4.0f; const glm::uvec2 vp{1024, 512};
    const float zoom = 512.0f / (2.0f * halfH);
    const auto v = ViewTransform::Orthographic(center, halfH, vp);
    const glm::vec3 w{7.0f, 3.0f, 0.0f};
    const glm::vec3 s = v.WorldToScreen(w);
    CHECK(s.x == Approx(w.x * zoom + (512.0f - center.x * zoom)));           // X byte-stable
    CHECK(s.y == Approx(256.0f - (w.y - center.y) * zoom));                  // Y flipped about the centre
}

TEST_CASE("Perspective: +Y up too, and the eye looks down -Z by default", "[viewtransform]")
{
    const auto v = ViewTransform::Perspective({0,0,10}, {0,0,0}, {0,1,0}, 60.0f, {800,600}, 0.1f, 1000.0f);
    CHECK_FALSE(v.IsOrthographic());
    const glm::vec3 up = v.WorldToScreen({0, 1, 0});
    const glm::vec3 c  = v.WorldToScreen({0, 0, 0});
    CHECK(c.x == Approx(400.0f)); CHECK(c.y == Approx(300.0f));
    CHECK(up.y < c.y);
    CHECK(v.WorldToScreen({0,0,9.9f}).z < v.WorldToScreen({0,0,0}).z);   // nearer = smaller depth (forward-Z)
}

TEST_CASE("ScreenToRay inverts WorldToScreen in both projections", "[viewtransform]")
{
    const glm::vec3 target{1.5f, -0.75f, -3.0f};
    for (int mode = 0; mode < 2; ++mode)
    {
        const ViewTransform v = mode == 0
            ? ViewTransform::Orthographic({0.5f, 0.25f}, 3.0f, {640, 480})
            : ViewTransform::Perspective({2,3,8}, {0,0,0}, {0,1,0}, 50.0f, {640,480}, 0.1f, 100.0f);
        const glm::vec3 s = v.WorldToScreen(target);
        const Arcane::Ray r = v.ScreenToRay({s.x, s.y});
        // distance from target to the ray line
        const glm::vec3 d = target - r.origin;
        const float along = glm::dot(d, r.direction);
        const float miss = glm::length(d - along * r.direction);
        CHECK(miss < 1e-3f);
        CHECK(along > 0.0f);
        if (mode == 0) CHECK(r.direction == glm::vec3(0,0,-1));   // ortho rays are parallel to the view axis
    }
}

TEST_CASE("AsAffine2D exists only for the orthographic view and carries the Y mirror", "[viewtransform]")
{
    const auto o = ViewTransform::Orthographic({1,2}, 5.0f, {800,600});
    const auto a = o.AsAffine2D();
    REQUIRE(a.has_value());
    CHECK(a->scale.x == Approx(60.0f)); CHECK(a->scale.y == Approx(-60.0f));
    CHECK(a->AngleSign() == -1.0f);
    const glm::vec2 w{3.0f, 4.0f};
    const glm::vec3 s = o.WorldToScreen(glm::vec3(w, 0));
    CHECK(a->Point(w).x == Approx(s.x)); CHECK(a->Point(w).y == Approx(s.y));
    CHECK(a->Unpoint(a->Point(w)).x == Approx(w.x)); CHECK(a->Unpoint(a->Point(w)).y == Approx(w.y));
    CHECK(a->Length(2.0f) == Approx(120.0f));
    const auto p = ViewTransform::Perspective({0,0,10},{0,0,0},{0,1,0},60.0f,{800,600},0.1f,100.0f);
    CHECK_FALSE(p.AsAffine2D().has_value());
}
```

- [ ] **Step 2: Build and run to verify they fail**

Run: `msbuild Arcane.slnx /p:Configuration=Debug /m` → expect a compile error: `ViewTransform.hpp` not found.

- [ ] **Step 3: Write the header**

`ArcaneCore/src/Arcane/Scene/ViewTransform.hpp`:
```cpp
#pragma once

// ViewTransform: THE ONE camera type every consumer reads (F4, spec s3).
// view + projection + viewport. The orthographic scene camera and the editor's
// own camera both produce it; sprites, the mesh pass, picking, the gizmo and
// the physics overlay all consume it. The 2D affine mapping the engine used to
// carry (screen = world * zoom + offset) is its orthographic case, exposed as
// Affine2D for the overlays that still draw in pixels (AsAffine2D below).
//
// CONVENTIONS (pinned in SceneCamera.hpp and PerspectiveCameraTest): right-
// handed, +Y up, camera forward -Z, clip depth [0,1] forward-Z (the *_ZO glm
// entry points). Pixels are y-DOWN; the flip lives in WorldToScreen's NDC ->
// pixel step and nowhere in world space (spec s2: no Y flip in the world).

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <limits>
#include <optional>

namespace Arcane
{
    struct Ray
    {
        glm::vec3 origin{0.0f};
        glm::vec3 direction{0.0f, 0.0f, -1.0f};   // unit
    };

    // The legacy per-pixel overlay mapping, now PER-AXIS so it can carry the
    // Y mirror: pixel = world.xy * scale + offset, with scale.y NEGATIVE for a
    // +Y-up world on a y-down canvas. Overlay code must project POINTS through
    // Point() (never "project the centre, then rotate in screen space with the
    // world angle"): a mirrored map reverses the sense of every angle, which
    // AngleSign() reports for the one place that needs it (a canvas-space
    // rotation in the id pass).
    struct Affine2D
    {
        glm::vec2 offset{0.0f, 0.0f};
        glm::vec2 scale{1.0f, 1.0f};

        [[nodiscard]] glm::vec2 Point(glm::vec2 world) const noexcept { return world * scale + offset; }
        [[nodiscard]] glm::vec2 Unpoint(glm::vec2 pixel) const noexcept { return (pixel - offset) / scale; }
        [[nodiscard]] float     Length(float metres) const noexcept { return std::abs(scale.x) * metres; }
        [[nodiscard]] float     AngleSign() const noexcept { return (scale.x * scale.y) < 0.0f ? -1.0f : 1.0f; }
    };

    struct ViewTransform
    {
        glm::mat4  view{1.0f};
        glm::mat4  projection{1.0f};
        glm::uvec2 viewport{0u, 0u};

        [[nodiscard]] glm::mat4 ViewProjection() const noexcept { return projection * view; }

        // An orthographic projection has no perspective divide: the w row is (0,0,0,1).
        [[nodiscard]] bool IsOrthographic() const noexcept
        {
            return projection[3][3] == 1.0f && projection[2][3] == 0.0f;
        }

        // .xy = pixels (top-left origin, y down), .z = NDC depth in [0,1].
        // A point at (or behind) the eye has w ~ 0 and returns NaN in every
        // component; callers that draw must std::isfinite-check.
        [[nodiscard]] glm::vec3 WorldToScreen(glm::vec3 world) const noexcept
        {
            const glm::vec4 clip = projection * (view * glm::vec4(world, 1.0f));
            if (!(std::abs(clip.w) > 1e-12f))
            {
                const float nan = std::numeric_limits<float>::quiet_NaN();
                return glm::vec3(nan, nan, nan);
            }
            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            return glm::vec3((ndc.x * 0.5f + 0.5f) * float(viewport.x),
                             (0.5f - ndc.y * 0.5f) * float(viewport.y),
                             ndc.z);
        }

        // Perspective: origin = the eye, direction through the pixel.
        // Orthographic: origin = the pixel's point on the NEAR plane, direction =
        // the view axis (parallel rays). UE unprojects its ortho origin at NDC
        // z = 0.5 under reversed-Z; with forward-Z [0,1] the near plane is the
        // safe origin (spec s3).
        [[nodiscard]] Ray ScreenToRay(glm::vec2 pixel) const noexcept
        {
            const float nx = viewport.x ? (pixel.x / float(viewport.x)) * 2.0f - 1.0f : 0.0f;
            const float ny = viewport.y ? 1.0f - (pixel.y / float(viewport.y)) * 2.0f : 0.0f;
            const glm::mat4 inv = glm::inverse(projection * view);
            glm::vec4 nearP = inv * glm::vec4(nx, ny, 0.0f, 1.0f);
            glm::vec4 farP  = inv * glm::vec4(nx, ny, 1.0f, 1.0f);
            nearP /= nearP.w;
            farP  /= farP.w;
            Ray r;
            if (IsOrthographic())
            {
                r.origin    = glm::vec3(nearP);
                r.direction = glm::normalize(glm::vec3(farP) - glm::vec3(nearP));
            }
            else
            {
                r.origin    = glm::vec3(glm::inverse(view)[3]);
                r.direction = glm::normalize(glm::vec3(farP) - r.origin);
            }
            return r;
        }

        // The overlay mapping, when this view IS the orthographic XY case
        // (no rotation in the view's upper 2x2). nullopt otherwise -- a
        // perspective or tilted view has no per-axis affine.
        [[nodiscard]] std::optional<Affine2D> AsAffine2D() const noexcept
        {
            if (!IsOrthographic()) return std::nullopt;
            const glm::mat4 vp = ViewProjection();
            if (std::abs(vp[0][1]) > 1e-6f || std::abs(vp[1][0]) > 1e-6f) return std::nullopt;
            Affine2D a;
            a.scale.x  =  vp[0][0] * 0.5f * float(viewport.x);
            a.scale.y  = -vp[1][1] * 0.5f * float(viewport.y);
            a.offset.x = (vp[3][0] * 0.5f + 0.5f) * float(viewport.x);
            a.offset.y = (0.5f - vp[3][1] * 0.5f) * float(viewport.y);
            return a;
        }

        [[nodiscard]] static ViewTransform Orthographic(glm::vec2 center, float halfHeight, glm::uvec2 viewport,
                                                        float nearZ = -1000.0f, float farZ = 1000.0f) noexcept
        {
            ViewTransform v;
            v.viewport = viewport;
            const float aspect = viewport.y ? float(viewport.x) / float(viewport.y) : 1.0f;
            const float halfW  = halfHeight * aspect;
            v.view       = glm::translate(glm::mat4(1.0f), glm::vec3(-center, 0.0f));
            v.projection = glm::orthoRH_ZO(-halfW, halfW, -halfHeight, halfHeight, nearZ, farZ);
            return v;
        }

        [[nodiscard]] static ViewTransform Perspective(glm::vec3 eye, glm::vec3 target, glm::vec3 up, float fovYDegrees,
                                                       glm::uvec2 viewport, float nearZ, float farZ) noexcept
        {
            ViewTransform v;
            v.viewport = viewport;
            const float aspect = viewport.y ? float(viewport.x) / float(viewport.y) : 1.0f;
            v.view       = glm::lookAtRH(eye, target, up);
            v.projection = glm::perspectiveRH_ZO(glm::radians(fovYDegrees), aspect, nearZ, farZ);
            return v;
        }
    };
}
```

- [ ] **Step 4: Build and run**

Run: `msbuild Arcane.slnx /p:Configuration=Debug /m` then `cd bin\Debug-windows-x86_64-md\ArcaneTests && .\ArcaneTests.exe "[viewtransform]"`
Expected: 5 cases pass. If the `AsAffine2D` scale test fails, check the sign derivation: for `orthoRH_ZO`, `projection[1][1] = 1/halfHeight`, and `view` translates by `-center`, so `vp[3][1] = -center.y/halfHeight`.

- [ ] **Step 5: Commit**

```bash
git add ArcaneCore/src/Arcane/Scene/ViewTransform.hpp ArcaneTests/src/ViewTransformTest.cpp
git commit -m "feat(core): ViewTransform -- the one camera type (+Y up, RH, [0,1] forward-Z), WorldToScreen/ScreenToRay in both projections, Affine2D for the pixel overlays (F4 plan 1 T1)"
```

---

### Task 2: The +Y flip — gravity defaults, scene JSON v6 with the migration, sprite pivot semantics

**Files:**
- Modify: `ArcaneCore/src/Arcane/Scene/Components.hpp:185` (PhysicsSettings gravity), `:285` (Transform tooltip), `:182` (comment)
- Modify: `ArcaneCore/src/Arcane/Project/ProjectManifest.hpp:58`, `ArcaneCore/src/Arcane/Project/Project.cpp:406`
- Modify: `ArcaneCore/src/Arcane/Serialization/SceneSerializer.hpp:83` (version), `:312-331` (LoadJson gate + migration)
- Modify: `ArcaneCore/src/Arcane/Sprite/SpriteAsset.hpp:38` (pivot comment: y = 0 is the BOTTOM)
- Modify: `ReferenceProject/Content/scenes/main.arcscene`, `physics.arcscene` (re-saved at v6 — Step 8)
- Test: `ArcaneTests/src/SceneMigrationTest.cpp` (new); existing `SceneJsonTest.cpp` / `SceneAssetTest.cpp` version pins (`"a v4 scene still loads after the v5 bump"`) get a v6 sibling

**Interfaces:**
- Produces: `Arcane::Scene::Detail::MigrateV5ToYUp(nlohmann::json& doc)` (pure, applied by `LoadJson` when `version <= 5`); `kSceneJsonVersion = 6`, `kSceneJsonVersionMin` stays 3.
- The migration rule (spec §2): for every entity's `"Arcane::Transform"`: `position[1] = -position[1]`; `rotation` (stored `[x, y, z, w]`, `ReflectionJson.hpp:279`) becomes `[-x, y, -z, w]` (a reflection across the XZ plane conjugates the axis: rotations about Y keep their sense, about X and Z reverse). For every `"Arcane::Collider2D"` fixture: `localPos[1] = -localPos[1]`, `localAngle = -localAngle`. For `"Arcane::RigidBody2D"`: `velocity[1] = -velocity[1]`. For `"Arcane::PhysicsSettings"`: `gravity[1] = -gravity[1]`. Nothing else in a scene carries a Y (Camera has no pose fields; SpriteRenderer has none).

- [ ] **Step 1: Write the failing migration test**

`ArcaneTests/src/SceneMigrationTest.cpp`:
```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <Arcane/Serialization/SceneSerializer.hpp>
#include <nlohmann/json.hpp>
using Catch::Approx;

TEST_CASE("MigrateV5ToYUp negates Y, conjugates the rotation across XZ, and flips physics Y", "[scene][migration]")
{
    nlohmann::json doc = nlohmann::json::parse(R"({
      "version": 5, "assets": [], "entities": [
        { "components": {
            "Arcane::Transform": { "position": [1.0, 2.0, 3.0], "rotation": [0.1, 0.2, 0.3, 0.9], "scale": [1,1,1] },
            "Arcane::Collider2D": { "fixtures": [ { "localPos": [0.5, -0.25], "localAngle": 0.4 } ] },
            "Arcane::RigidBody2D": { "velocity": [3.0, 4.0] },
            "Arcane::PhysicsSettings": { "gravity": [0.0, 9.81] }
        } } ] })");
    Arcane::Scene::Detail::MigrateV5ToYUp(doc);
    const auto& c = doc["entities"][0]["components"];
    CHECK(c["Arcane::Transform"]["position"][1].get<float>() == Approx(-2.0f));
    CHECK(c["Arcane::Transform"]["rotation"][0].get<float>() == Approx(-0.1f));
    CHECK(c["Arcane::Transform"]["rotation"][1].get<float>() == Approx( 0.2f));
    CHECK(c["Arcane::Transform"]["rotation"][2].get<float>() == Approx(-0.3f));
    CHECK(c["Arcane::Transform"]["rotation"][3].get<float>() == Approx( 0.9f));
    CHECK(c["Arcane::Collider2D"]["fixtures"][0]["localPos"][1].get<float>() == Approx(0.25f));
    CHECK(c["Arcane::Collider2D"]["fixtures"][0]["localAngle"].get<float>() == Approx(-0.4f));
    CHECK(c["Arcane::RigidBody2D"]["velocity"][1].get<float>() == Approx(-4.0f));
    CHECK(c["Arcane::PhysicsSettings"]["gravity"][1].get<float>() == Approx(-9.81f));
    CHECK(doc["version"].get<int>() == 6);
}

TEST_CASE("LoadJson migrates a v5 scene on load and a v6 scene untouched; SaveJson writes v6", "[scene][migration]")
{
    // Build through the real path: a registry with a Transform at y=+2 saved by
    // THIS build is v6 and must round-trip unchanged; the same JSON hand-stamped
    // v5 must come back at y=-2.
    Astra::Registry reg;
    Arcane::Scene::CreateEmpty(reg);   // SceneRoot + Main Camera
    const Astra::Entity e = reg.CreateEntity();
    reg.AddComponent<Arcane::Transform>(e, Arcane::Transform{ {0.0f, 2.0f, 0.0f}, glm::quat(1,0,0,0), {1,1,1} });
    reg.SetParent(e, reg.GetResource<Arcane::SceneRoot>()->entity);
    nlohmann::json v6 = Arcane::Scene::SaveJson(reg);
    REQUIRE(v6["version"].get<int>() == 6);

    Astra::Registry back6;
    REQUIRE(Arcane::Scene::LoadJson(back6, v6));
    float y6 = 0.0f;
    back6.CreateView<const Arcane::Transform>().ForEach([&](Astra::Entity, const Arcane::Transform& t){ if (t.position.y != 0.0f) y6 = t.position.y; });
    CHECK(y6 == Approx(2.0f));

    nlohmann::json v5 = v6; v5["version"] = 5;
    Astra::Registry back5;
    REQUIRE(Arcane::Scene::LoadJson(back5, v5));
    float y5 = 0.0f;
    back5.CreateView<const Arcane::Transform>().ForEach([&](Astra::Entity, const Arcane::Transform& t){ if (t.position.y != 0.0f) y5 = t.position.y; });
    CHECK(y5 == Approx(-2.0f));
}
```
(Match the `CreateEmpty` / `SceneRoot` usage in `SceneJsonTest.cpp`; copy its fixture helpers if it has them.)

- [ ] **Step 2: Build and run to verify failure**

Run: build, then `.\ArcaneTests.exe "[migration]"` → expect a compile error (`MigrateV5ToYUp` undefined).

- [ ] **Step 3: Implement the version bump + migration**

In `SceneSerializer.hpp`, replace the version constant and add the migration in `Detail`:
```cpp
    // v6 (2026-09-17, F4 plan 1, spec 2026-09-17-f4-editor-3d-authoring s2): the
    // world is +Y UP. A v5 (or older) file was authored +Y down, so the loader
    // MIGRATES it on load -- MigrateV5ToYUp below -- and the next save writes
    // it as v6. No file is refused for being old, and none is ever silently
    // mirrored: the migration is the one place the sign changes.
    inline constexpr int kSceneJsonVersion = 6;
```
(keep `kSceneJsonVersionMin = 3`), and in `namespace Detail` (before `LoadJson`):
```cpp
        // The +Y flip for a pre-v6 scene document, IN PLACE. Reflecting the
        // world across the XZ plane: y -> -y for positions/offsets/velocities/
        // gravity, and a rotation quaternion (stored [x,y,z,w]) conjugates to
        // [-x, y, -z, w] -- a rotation about an axis IN the mirror plane keeps
        // its sense, one about a perpendicular axis (X or Z) reverses.
        inline void MigrateV5ToYUp(nlohmann::json& doc)
        {
            auto negAt = [](nlohmann::json& arr, std::size_t i)
            {
                if (arr.is_array() && arr.size() > i && arr[i].is_number())
                    arr[i] = -arr[i].get<double>();
            };
            auto negScalar = [](nlohmann::json& v)
            {
                if (v.is_number()) v = -v.get<double>();
            };
            if (!doc.contains("entities") || !doc["entities"].is_array()) return;
            for (auto& entry : doc["entities"])
            {
                auto cit = entry.find("components");
                if (cit == entry.end() || !cit->is_object()) continue;
                nlohmann::json& c = *cit;
                if (auto t = c.find("Arcane::Transform"); t != c.end() && t->is_object())
                {
                    if (t->contains("position")) negAt((*t)["position"], 1);
                    if (t->contains("rotation")) { negAt((*t)["rotation"], 0); negAt((*t)["rotation"], 2); }
                }
                if (auto col = c.find("Arcane::Collider2D"); col != c.end() && col->is_object()
                    && col->contains("fixtures") && (*col)["fixtures"].is_array())
                {
                    for (auto& fx : (*col)["fixtures"])
                    {
                        if (!fx.is_object()) continue;
                        if (fx.contains("localPos"))   negAt(fx["localPos"], 1);
                        if (fx.contains("localAngle")) negScalar(fx["localAngle"]);
                    }
                }
                if (auto rb = c.find("Arcane::RigidBody2D"); rb != c.end() && rb->is_object() && rb->contains("velocity"))
                    negAt((*rb)["velocity"], 1);
                if (auto ps = c.find("Arcane::PhysicsSettings"); ps != c.end() && ps->is_object() && ps->contains("gravity"))
                    negAt((*ps)["gravity"], 1);
            }
            doc["version"] = kSceneJsonVersion;
        }
```
In `LoadJson`, after the version gate (line 331) the document is `const&`; migrate on a copy:
```cpp
            // v6 (F4): a pre-v6 file is +Y down; migrate a COPY and load that.
            nlohmann::json migrated;
            const nlohmann::json* src = &doc;
            if (version < 6)
            {
                migrated = doc;
                Detail::MigrateV5ToYUp(migrated);
                src = &migrated;
                ARC_INFO("scene load: migrated a v{} (+Y down) scene to v6 (+Y up); it will be written as v6 on the next save", version);
            }
            const auto& entities = (*src)["entities"];
```
and replace the remaining `doc[...]` reads inside `LoadJson` with `(*src)[...]`. `ReadSceneFile` (`SceneAsset.hpp`) validates the version range with the same constants; it needs no change beyond `kSceneJsonVersion` if it compares against the constants (verify: grep `kSceneJsonVersion` in `SceneAsset.hpp`).

- [ ] **Step 4: Flip the gravity defaults and the prose**

- `Components.hpp:185`: `glm::vec2 gravity{0.0f, -9.81f};   // m/s^2; +Y UP (F4, spec s2)` and rewrite the comment at `:182` to "+Y is UP (one right-handed world; the 2D plane is XY viewed down -Z)".
- `Components.hpp:285` Transform tooltip → `"World position in meters (MKS units). +Y is UP: the 2D plane is XY viewed down -Z, the same convention as the 3D camera."`
- `ProjectManifest.hpp:58`: `glm::vec2 gravity{0.0f, -9.81f};   // m/s^2; +Y is up (F4)`.
- `Project.cpp:406`: `{ "gravity", { 0.0, -9.81 } }` and the trailing comment.
- `SpriteAsset.hpp:38`: comment `// normalized; (0,0) = BOTTOM-left of the image, (1,1) = top-right (+Y up world, F4)`.
- Update every test that pins `9.81f` positive: grep `9.81` in `ArcaneTests/src` (RuntimeTest "ResolvedGravity: the engine default...", ProjectManifestTest "a manifest physics block sets gravity") and flip the expected sign. The physics-wiring desk tests that drop a body (`EditorPlayModeTest` "Play lets a body fall...", `AuthoredTransformSyncTest`) assert a body moved to `-1.0`/`+0.236`-class values in +Y-down terms — flip their expected signs too, keeping magnitudes. Run `.\ArcaneTests.exe "[physics],[runtime],[transform-sync],[playmode]"` and fix each failing sign.

- [ ] **Step 5: Build and run the affected suites**

Run: `.\ArcaneTests.exe "[migration],[scene],[physics],[runtime],[transform-sync],[playmode],[project]"`
Expected: all pass; the two new migration cases pass.

- [ ] **Step 6: Add the v6 version pin beside the existing v5 one**

In `SceneJsonTest.cpp`, next to "a v4 scene still loads after the v5 bump", add:
```cpp
TEST_CASE("a v5 scene still loads after the v6 bump, migrated to +Y up", "[scene]")
{
    Astra::Registry reg; Arcane::Scene::CreateEmpty(reg);
    nlohmann::json doc = Arcane::Scene::SaveJson(reg);
    doc["version"] = 5;
    Astra::Registry back;
    CHECK(Arcane::Scene::LoadJson(back, doc));
}
```

- [ ] **Step 7: Re-save the ReferenceProject scenes at v6**

Open each scene through the loader and save it back with the migration applied — a one-shot: write a tiny throwaway test-free path using the editor is slower than a script, so do it with the existing headless editor? No: the simplest reliable way is a small `ArcaneTests` case guarded by an env var that loads and re-saves the two files. Instead, use the runtime API directly from a scratch program? Keep it in the repo's tooling: add to `ArcaneTests/src/SceneMigrationTest.cpp`:
```cpp
// One-shot tool, not a test of the engine: re-saves the ReferenceProject scenes
// at the current schema. Runs only when ARCANE_RESAVE_SCENES names the directory.
TEST_CASE("resave ReferenceProject scenes at the current schema (tool)", "[migration][tool]")
{
    const char* dir = std::getenv("ARCANE_RESAVE_SCENES");
    if (!dir) { SUCCEED("ARCANE_RESAVE_SCENES unset -- tool skipped"); return; }
    for (const auto& f : std::filesystem::directory_iterator(dir))
    {
        if (f.path().extension() != ".arcscene") continue;
        const auto read = Arcane::Scene::ReadSceneFile(f.path());
        REQUIRE(read.has_value());
        Astra::Registry reg;
        REQUIRE(Arcane::Scene::ApplySceneDocument(reg, *read));
        std::string err;
        REQUIRE(Arcane::Scene::SaveSceneFile(f.path(), reg, read->id, &err));
    }
}
```
(Adjust `ReadSceneFile` / `ApplySceneDocument` / `SaveSceneFile` argument shapes to `SceneAsset.hpp`'s actual signatures — read that header first.) Run: `set ARCANE_RESAVE_SCENES=D:\dev\starworks\Arcane\ReferenceProject\Content\scenes && .\ArcaneTests.exe "[tool]"`, then `git diff ReferenceProject/Content/scenes` and confirm every `position[1]` sign flipped and `"version": 6`. Unset the env var.

- [ ] **Step 8: Build Release too, run both configs' `~[gpu]`, commit**

```bash
git add ArcaneCore/src/Arcane/Scene/Components.hpp ArcaneCore/src/Arcane/Project/ProjectManifest.hpp ArcaneCore/src/Arcane/Project/Project.cpp ArcaneCore/src/Arcane/Serialization/SceneSerializer.hpp ArcaneCore/src/Arcane/Sprite/SpriteAsset.hpp ArcaneTests/src/SceneMigrationTest.cpp ArcaneTests/src/SceneJsonTest.cpp ArcaneTests/src/*.cpp ReferenceProject/Content/scenes/*.arcscene
git commit -m "feat(scene): the +Y flip -- gravity defaults (0,-9.81), scene JSON v6 with the v5 migration on load (y, quat conjugate across XZ, fixtures, velocity, gravity), sprite pivot y=0 is the bottom, ReferenceProject scenes re-saved (F4 plan 1 T2, spec s2)"
```
The viewport goldens now FAIL (everything mirrored). That is expected until Task 12; do NOT re-bless yet.

---

### Task 3: `SetView` replaces `SetCamera`; `RenderContext2D` carries the view; the scene camera produces a `ViewTransform`

**Files:**
- Modify: `ArcaneCore/src/Arcane/Scene/SceneCamera.hpp` (`SceneCameraView` gains `ViewTransform view`; `PerspectiveCameraView` → `using PerspectiveCameraView = ViewTransform;` with `ActivePerspectiveSceneCamera(reg, viewport, outCount)` taking `glm::uvec2`)
- Modify: `ArcaneCore/src/Arcane/Scene/SceneResources.hpp:98-108` (`RenderContext2D`)
- Modify: `ArcaneClient/src/Arcane/Client/RuntimePresentation.hpp:26-27`, `ClientRuntime.hpp:93-104`, `ClientRuntime.cpp:87-103`
- Modify: `ArcaneClient/src/Arcane/Render/PhysicsDebugDraw.hpp:57-58` + `.cpp` (37 sites), `PickEmit.hpp:36-40` + `.cpp`, `Edit/Gizmo.hpp:35-39` + `.cpp:16-22`
- Modify: `ArcaneRuntime/src/RuntimeApp.cpp:765-769`, `RuntimeFrame.cpp:460,507,575-579`; `ArcaneEditor/src/App/EditorAppFrame.cpp:935,994,1389-1395,1644,1681-1695,1715,1822-1824,1877`
- Modify: `ArcaneCore/src/Arcane/Plugin/PluginABI.hpp:869` (bump to 32 with a changelog line), `ReferenceProject/ReferenceProject.arcproj` (`engine.abi`)
- Test: `ArcaneTests/src/RenderInterpolationTest.cpp`, `PickEmitTest.cpp`, `GizmoTest.cpp`, `PhysicsDebugDrawTest.cpp` (whatever exists — grep `PickView{` / `GizmoView{` / `cameraOffset` in `ArcaneTests/src`) — adapt call sites to the new types

**Interfaces:**
- Produces:
  ```cpp
  // SceneResources.hpp
  struct RenderContext2D { class Batcher2D* batcher = nullptr; ViewTransform view{}; float alpha = 0.0f; };
  // ClientRuntime
  void SetView(const ViewTransform& view) noexcept;   const ViewTransform& View() const noexcept;
  // SceneCamera.hpp
  struct SceneCameraView { glm::vec2 worldCenter; float halfHeight; ViewTransform view; };
  std::optional<SceneCameraView> ActiveSceneCamera(Astra::Registry&, glm::uvec2 viewport, int* outCount = nullptr);
  using PerspectiveCameraView = ViewTransform;
  std::optional<ViewTransform> ActivePerspectiveSceneCamera(Astra::Registry&, glm::uvec2 viewport, int* outCount = nullptr);
  // overlays
  struct PickView  { Affine2D affine; };      // PickEmit.hpp   (plan 2 replaces with ViewTransform)
  struct GizmoView { Affine2D affine; };      // Gizmo.hpp      (plan 2 replaces)
  PhysicsDebugDrawOptions { Affine2D view; ... }   // replaces cameraOffset/zoom
  ```
- Consumes: Task 1's `ViewTransform`, `Affine2D`.

- [ ] **Step 1: Write the failing tests**

Append to `ArcaneTests/src/ViewTransformTest.cpp` (or the existing scene-camera test file; grep `ActiveSceneCamera` in `ArcaneTests/src` and add beside it):
```cpp
TEST_CASE("ActiveSceneCamera hands back a ViewTransform whose Affine2D mirrors Y", "[camera][viewtransform]")
{
    Astra::Registry reg;
    const Astra::Entity cam = reg.CreateEntity();
    reg.AddComponent<Arcane::Transform>(cam, Arcane::Transform{ {1.0f, 2.0f, 0.0f}, glm::quat(1,0,0,0), {1,1,1} });
    reg.AddComponent<Arcane::Camera>(cam, Arcane::Camera{});   // orthographicSize 5
    const auto v = Arcane::ActiveSceneCamera(reg, glm::uvec2{800, 600});
    REQUIRE(v.has_value());
    CHECK(v->halfHeight == Approx(5.0f));
    CHECK(v->worldCenter.x == Approx(1.0f)); CHECK(v->worldCenter.y == Approx(2.0f));
    const auto a = v->view.AsAffine2D();
    REQUIRE(a.has_value());
    CHECK(a->scale.x == Approx(60.0f)); CHECK(a->scale.y == Approx(-60.0f));
    CHECK(a->Point({1.0f, 2.0f}).x == Approx(400.0f)); CHECK(a->Point({1.0f, 2.0f}).y == Approx(300.0f));
}
```
And a pick-emitter pin (in the existing `PickEmitTest.cpp`, beside its sprite case): a sprite at world (0, +1) with the view `ViewTransform::Orthographic({0,0}, 5, {800,600}).AsAffine2D()` must produce `d.center.y < 300` and, for a sprite rotated by +0.3 rad, `d.angle == Approx(-0.3f)` (the canvas angle carries the mirror sign).

- [ ] **Step 2: Build to verify failure** (compile errors on the new signatures).

- [ ] **Step 3: Rewrite the scene camera producers**

`SceneCamera.hpp`: include `ViewTransform.hpp`; `SceneCameraView` becomes `{ glm::vec2 worldCenter; float halfHeight; ViewTransform view; }`; in `ActiveSceneCamera` replace the `zoom`/`offset` derivation with `v.view = ViewTransform::Orthographic(center, halfH, viewport);` and change the signature to `(Astra::Registry& reg, glm::uvec2 viewport, int* outCount = nullptr)` (guard `viewport.y == 0` → nullopt as before). Replace `struct PerspectiveCameraView` with `using PerspectiveCameraView = ViewTransform;` and make `ActivePerspectiveSceneCamera` take `glm::uvec2 viewport`, ending with:
```cpp
        ViewTransform v;
        v.viewport   = viewport;
        v.view       = glm::lookAtRH(eye, eye + forward, up);
        v.projection = PerspectiveProjection(fovYDegrees, aspectRatio, nearZ, farZ);
        return v;
```
(compute `aspectRatio` from the viewport, guarding `viewport.y == 0`). Update `PerspectiveCameraTest.cpp` call sites.

- [ ] **Step 4: `RenderContext2D` + `ClientRuntime`**

`SceneResources.hpp`: `struct RenderContext2D { class Batcher2D* batcher = nullptr; ViewTransform view{}; float alpha = 0.0f; };` (include `ViewTransform.hpp`).
`RuntimePresentation.hpp`: replace `cameraOffset`/`cameraZoom` with `ViewTransform view{};` (default = identity matrices, viewport 0 — the old "identity" default; `Runtime` hosts that never push a view still render, at 1 px per metre scaled by NDC, which is the pre-existing "no camera" posture; RuntimeApp already warns).
`ClientRuntime.hpp`: replace the three camera methods with
```cpp
        // --- camera bridge: ONE ViewTransform (F4 plan 1). The plugin, the scene camera
        // or the editor pushes it; the render bridge, picking and the overlays read it.
        void                 SetView(const ViewTransform& view) noexcept;
        const ViewTransform& View() const noexcept;
```
`ClientRuntime.cpp`: `SetView` stores `m_pres.view`; `SetRenderContext` writes `RenderContext2D{batcher, m_pres.view, alpha}`.

- [ ] **Step 5: The overlays take `Affine2D`**

`PickEmit.hpp`: `struct PickView { Affine2D affine; };` and in `PickEmit.cpp` replace every `x * view.worldToScreenScale + view.offset` with `view.affine.Point(x)`, every `len * view.worldToScreenScale` with `view.affine.Length(len)`, and set `d.angle = angle * view.affine.AngleSign();` at both angle sites (`:80`, `:136`). Update the header comment: "world rotation, in CANVAS sense (AngleSign applied)".
`Gizmo.hpp`: `struct GizmoView { Affine2D affine; };` and in `Gizmo.cpp` the two helpers at `:16-22` become `view.affine.Point(world)` / `view.affine.Unpoint(screen)` (drop the `kEps` divide guard: `Affine2D::Unpoint` divides by the per-axis scale, which the producer guarantees non-zero); at `:137` the rotate drag becomes `r.rotation = start.rotation + view.affine.AngleSign() * (a1 - a0);`. `AxisDirScreen` already projects two points, so axis arrows point +Y UP on screen automatically.
`PhysicsDebugDraw.hpp`: replace `cameraOffset`/`zoom` with `Affine2D view;` and in `.cpp` convert all 37 sites the same way: points via `opts.view.Point(p)`, lengths via `opts.view.Length(l)`, and any place that rotated a shape in SCREEN space by a world angle must instead compute the shape's corner/endpoints in WORLD space and project each (`RotateVec` then `Point`). Read each site; the capsule and box outlines are the ones that rotate.
Update `EditorAppFrame.cpp:1644`: `opts.view = *ctx->view.AsAffine2D();` guarded — when `AsAffine2D()` is nullopt (Perspective mode, Task 7) skip the physics overlay this frame.

- [ ] **Step 6: The hosts push a view**

- `RuntimeApp.cpp:765-769`: `ActiveSceneCamera(reg, glm::uvec2{w, h}, &camCount)` → `m_runtime->SetView(view->view);`.
- `RuntimeFrame.cpp:460, 507`: `const Arcane::PickView view{ *io.runtime->View().AsAffine2D() };` guarded (skip the pick emit when nullopt).
- `RuntimeFrame.cpp:575`: `ActivePerspectiveSceneCamera(reg, glm::uvec2{frameWidth, frameHeight})`; `meshScene.view = cam->view; meshScene.projection = cam->projection;` (unchanged shape).
- `EditorAppFrame.cpp:935` and `:1389`: `m_runtime->SetView(m_camera.Resolve(glm::uvec2{ViewportWidth(), ViewportHeight()}));` — Task 6 defines `Resolve`; until then keep the old `EditorCamera` and push `ViewTransform::Orthographic(centerFromOffset, halfHFromZoom, viewport)` with `halfH = viewportH / (2*zoom)` and `center = (viewport/2 - offset)/zoom` with the Y sign negated (a temporary shim, deleted in Task 6).
- `EditorAppFrame.cpp:1395`: `m_runtime->SetView(sceneCam->view);`.
- `EditorAppFrame.cpp:994, 1715`: `const Arcane::GizmoView view{ *m_runtime->View().AsAffine2D() };` guarded (skip gizmo hit-test/draw when nullopt).
- `EditorAppFrame.cpp:1681-1695` (camera rect): project the four WORLD corners `(cx ± hw, cy ± hh, 0)` through `m_runtime->View().WorldToScreen` and draw the four lines between the projected points (works in both modes; skip if any component is non-finite).
- `EditorAppFrame.cpp:1877`: `const Arcane::PickView view{ *m_runtime->View().AsAffine2D() };` guarded.
- `EditorAppFrame.cpp:1820-1824`: unchanged this task (Task 7 switches the mesh pass to the editor view in Edit mode).

- [ ] **Step 7: ABI bump**

`PluginABI.hpp:869`: `kGamePluginABIVersion = 32;` with a changelog line above it: `// v32 (2026-09-17, F4 plan 1): ClientRuntime::SetCamera(vec2,float)/CameraOffset/CameraZoom REMOVED in favour of SetView(const ViewTransform&)/View(); RenderContext2D carries a ViewTransform; PickView/GizmoView carry Affine2D. A module built against v31 calls a removed export -- refuse.` Restamp `ReferenceProject/ReferenceProject.arcproj` `engine.abi` to 32 and rebuild ReferenceProject (`arcbuild build --project ReferenceProject --config Debug` from the SDK's bin) so the witness suites load a current module.

- [ ] **Step 8: Build, run, commit**

Run: full Debug build; `.\ArcaneTests.exe "~[gpu]"` from the exe dir — fix every test that used `PickView{offset, scale}` / `GizmoView{...}` / `cameraOffset`/`zoom` on `PhysicsDebugDrawOptions` (construct them from `ViewTransform::Orthographic(...).AsAffine2D()`; note their expected Y values flip sign and their expected angles flip). Then run the unfiltered suite once for the `[witness][gpu]` scenarios (the restamped module).
```bash
git add -A ArcaneCore ArcaneClient ArcaneRuntime ArcaneEditor ArcaneTests ReferenceProject/ReferenceProject.arcproj
git commit -m "feat(client): SetView replaces SetCamera -- RenderContext2D carries a ViewTransform, the scene cameras produce one, PickView/GizmoView/PhysicsDebugDrawOptions take Affine2D (per-axis scale, angle sign); ABI 32 (F4 plan 1 T3, spec s3)"
```

---

### Task 4: The batcher's WORLD space — vec3 vertices, `QuadWorld`/`CircleWorld`, the view-projection in the sprite shaders

**Files:**
- Modify: `ArcaneClient/src/Arcane/Render/Batcher2D.hpp:115-121` (vertex), `:127-140` (span), append two virtuals after `QuadTextured` (`:362-369`)
- Modify: `ArcaneClient/src/Arcane/Render/Batcher2D.cpp:30-38` (DrawRecord), `:195-234` (Line/Circle/Triangle push vec3 with z=0), `:307-340` (DrainInternal copies `worldSpace` into the run), `:359-394` (PushQuadVertices takes a `worldSpace` flag; PushQuad unchanged = screen)
- Modify: `data/shaders/sprite.hlsl`, `data/shaders/circle.hlsl`, `data/shaders/sprite_material.hlsl` (the `BatchConstants` block and `vs_main`); `data/shaders/msdf.hlsl` (VSInput pos float3, screen path only)
- Modify: `ArcaneClient/src/Arcane/Render/Nri/nodes/Batch2DNode.cpp:58-65` (root constants), `:132-135` (POSITION format), `:291-294`, `:391-394` (root constant size), `:1397-1402` + `:1481-1492` (per-span push)
- Test: `ArcaneTests/src/Batcher2DTest.cpp` (existing — grep the file that drains a `Batcher2D` and checks vertices/spans), `RenderGraphTest.cpp`'s device-less Batch2D declaration pins (root-constant size), a `[gpu][pixel]` case in `Batch2DNodeTest.cpp` if one exists (grep `RenderFrameOffscreen` + `batch`)

**Interfaces:**
- Produces:
  ```cpp
  struct Batch2DVertex { glm::vec3 pos; glm::vec2 uv; glm::vec4 color; };   // 36 bytes
  struct Batch2DDrawSpan { ...; bool worldSpace = false; };                  // per-span projection selector
  // Batcher2D, APPENDED after QuadTextured:
  virtual void QuadWorld(uint16_t materialId, const Guid& textureId,
                         const std::array<glm::vec3, 4>& corners /*TL,TR,BR,BL in world*/,
                         glm::vec2 uvMin, glm::vec2 uvMax, glm::vec4 color) = 0;
  virtual void CircleWorld(glm::vec3 center, glm::vec3 right, glm::vec3 up, float radius, glm::vec4 color) = 0;
  ```
  Shader `BatchConstants { float4x4 viewProj; float2 invHalfViewport; uint worldSpace; float pad; }` = 80 bytes, `BatchRootConstants` mirrors it.
- Consumes: `Batch2DDrained::viewport`; `RenderContext2D::view` is applied by the RECORDER (Batch2DNode) — so `Batch2DDrained` gains `glm::mat4 viewProjection` set by the host before Drain: add `virtual void SetViewProjection(const glm::mat4&)` APPENDED after `CircleWorld`, stored sticky like `SetGlobals`, and surfaced on `Batch2DDrained::viewProjection`.

- [ ] **Step 1: Write the failing batcher test**

In `ArcaneTests/src/Batcher2DTest.cpp` (or beside the existing drain test):
```cpp
TEST_CASE("QuadWorld records a world-space span; Rect records a screen-space one; both sort by layer", "[batcher]")
{
    auto b = Arcane::Batcher2D::Create();
    b->Begin(800, 600);
    b->SetViewProjection(glm::mat4(2.0f));
    b->SetLayer(1, 0);
    b->Rect({10, 10}, {20, 20}, {1,1,1,1});
    b->SetLayer(0, 0);
    const std::array<glm::vec3, 4> corners{ glm::vec3{-1, 1, 0}, {1, 1, 0}, {1, -1, 0}, {-1, -1, 0} };
    b->QuadWorld(Arcane::Batcher2D::kMaterialSprite, Arcane::Guid::Nil(), corners, {0,0}, {1,1}, {1,1,1,1});
    const Arcane::Batch2DDrained d = b->Drain();
    REQUIRE(d.spans.size() == 2);
    CHECK(d.spans[0].worldSpace);          // layer 0 first
    CHECK_FALSE(d.spans[1].worldSpace);
    CHECK(d.vertices[d.spans[0].firstIndex == 0 ? 0 : 4].pos == glm::vec3(-1, 1, 0));   // the world quad's TL, verbatim
    CHECK(d.vertices[4].pos.z == 0.0f);    // the screen rect's vertices carry z = 0
    CHECK(d.viewProjection == glm::mat4(2.0f));
    CHECK(sizeof(Arcane::Batch2DVertex) == 36);
}
```
(Index the vertex through the span's `firstIndex` → `d.indices[firstIndex]` if the vertex order after sorting is not the push order; the existing drain test shows the idiom.)

- [ ] **Step 2: Build to verify failure.**

- [ ] **Step 3: Batcher changes**

`Batcher2D.hpp`: `struct Batch2DVertex { glm::vec3 pos; glm::vec2 uv; glm::vec4 color; }; static_assert(sizeof(Batch2DVertex) == 36, ...)`; `Batch2DDrawSpan` gains `bool worldSpace = false;`; `Batch2DDrained` gains `glm::mat4 viewProjection{1.0f};`; append after `QuadTextured`:
```cpp
        // ===== WORLD SPACE (F4 plan 1, spec s3) =====
        // A quad whose four corners are WORLD positions (metres, +Y up), in
        // TL,TR,BR,BL order as the caller sees the sprite's own plane. The
        // recorder multiplies these by the frame's view-projection in the
        // vertex shader, so a tilted or distant quad interpolates its UVs with
        // correct perspective -- which projecting the corners here on the CPU
        // would NOT do. Sorting, materials and textures are exactly QuadTextured's.
        // APPENDED (ABI v32): see Drain()'s comment for why every new virtual
        // goes at the end of this class.
        virtual void QuadWorld(uint16_t materialId, const Guid& textureId,
                               const std::array<glm::vec3, 4>& corners,
                               glm::vec2 uvMin, glm::vec2 uvMax, glm::vec4 color) = 0;

        // The SDF disc in world space: an axis-aligned quad in the (right, up)
        // plane about `center`, radius in metres. circle.hlsl's unit-disc SDF
        // rides the uv exactly as Circle()'s does.
        virtual void CircleWorld(glm::vec3 center, glm::vec3 right, glm::vec3 up,
                                 float radius, glm::vec4 color) = 0;

        // The frame's view-projection for WORLD-space spans. Sticky per Begin()
        // like SetGlobals; leaves through Batch2DDrained::viewProjection. Screen-
        // space spans ignore it.
        virtual void SetViewProjection(const glm::mat4& viewProjection) = 0;
```
`Batcher2D.cpp`: `DrawRecord` gains `bool worldSpace = false;`; `PushQuadVertices` gains a trailing `bool worldSpace = false` parameter and copies it into the record; every existing `Vertex{ vec2, uv, color }` initialiser becomes `{ glm::vec3(p, 0.0f), uv, color }` (Line, Triangle, PushQuad); `DrainInternal` copies `record.worldSpace` into `run.worldSpace` and starts a new run when it changes (add it to the run-break condition beside material/texture); `Begin` resets `m_viewProjection` to identity; `Drain` sets `out.viewProjection = m_viewProjection`. Implement:
```cpp
            void QuadWorld(uint16_t material, const Guid& textureId,
                           const std::array<glm::vec3, 4>& c,
                           glm::vec2 uvMin, glm::vec2 uvMax, glm::vec4 color) override
            {
                PushQuadVertices(material, textureId,
                    { c[0], uvMin, color },
                    { c[1], { uvMax.x, uvMin.y }, color },
                    { c[2], uvMax, color },
                    { c[3], { uvMin.x, uvMax.y }, color },
                    /*worldSpace=*/true);
            }
            void CircleWorld(glm::vec3 center, glm::vec3 right, glm::vec3 up, float radius, glm::vec4 color) override
            {
                if (radius <= 0.0f) return;
                const glm::vec3 r = right * radius, u = up * radius;
                PushQuadVertices(kMaterialCircle, Guid::Nil(),
                    { center - r + u, {-1.0f, -1.0f}, color },
                    { center + r + u, { 1.0f, -1.0f}, color },
                    { center + r - u, { 1.0f,  1.0f}, color },
                    { center - r - u, {-1.0f,  1.0f}, color },
                    /*worldSpace=*/true);
            }
            void SetViewProjection(const glm::mat4& vp) override { m_viewProjection = vp; }
```
(uv convention for the world circle: TL = (-1,-1) matching `PushQuad`'s uvMin at TL; check `circle.hlsl` reads `length(uv) <= 1`, which is sign-agnostic.)

- [ ] **Step 4: Shaders**

`sprite.hlsl` (and identically the `BatchConstants` block + `vs_main` in `circle.hlsl`, `msdf.hlsl`, `sprite_material.hlsl`):
```hlsl
struct BatchConstants
{
    float4x4 viewProj;         // world -> clip, WORLD-space spans only (column-major, glm layout)
    float2   invHalfViewport;  // 2.0 / (canvasW, canvasH), SCREEN-space spans
    uint     worldSpace;       // 1 = pos is world metres; 0 = canvas pixels (y down)
    float    pad;
};
...
struct VSInput { float3 pos : POSITION; float2 uv : TEXCOORD0; float4 color : COLOR0; };
...
    if (g_worldSpace != 0)
        output.pos = mul(g_viewProj, float4(input.pos, 1.0));
    else
        output.pos = float4(input.pos.x * g_invHalfViewport.x - 1.0,
                            1.0 - input.pos.y * g_invHalfViewport.y, 0.0, 1.0);
```
with the `#if SPIRV` / `cbuffer` macro pairs extended for `g_viewProj` and `g_worldSpace`. The SPIR-V push-constant block is 80 bytes (≤ 128, Vulkan's minimum).

- [ ] **Step 5: Batch2DNode**

`BatchRootConstants { float viewProj[16]; float invHalfViewportX, invHalfViewportY; uint32_t worldSpace; float pad; }` with `static_assert(sizeof == 80)`; both `rootConstant.size` sites → `sizeof(BatchRootConstants)`; POSITION attribute → `nri::Format::RGB32_SFLOAT`; `vertexBuffer.stride = sizeof(Batch2DVertex)` is already by sizeof. In the span loop, the push constants now vary per span: fill `push.viewProj` from `batch.viewProjection` (memcpy the glm::mat4), and inside the loop set `push.worldSpace = span.worldSpace ? 1u : 0u;` and re-issue `CmdSetRootConstants` whenever `worldSpace` changed since the last set OR the layout changed (track `lastWorldSpace` beside `lastLayout`). Registered-material pipelines share the same b0 block — `sprite_material.hlsl` gets the same edit.

- [ ] **Step 6: Build, run, commit**

Run: build (the prebuild recompiles shaders); `.\ArcaneTests.exe "[batcher],[nri],[batch2d]"` and any `[gpu][pixel]` batch case; then the unfiltered suite once on the desk GPU.
```bash
git add ArcaneClient/src/Arcane/Render/Batcher2D.hpp ArcaneClient/src/Arcane/Render/Batcher2D.cpp ArcaneClient/src/Arcane/Render/Nri/nodes/Batch2DNode.cpp data/shaders/*.hlsl ArcaneTests/src/Batcher2DTest.cpp
git commit -m "feat(render): Batcher2D WORLD space -- vec3 vertices, QuadWorld/CircleWorld/SetViewProjection appended, per-span worldSpace selects the view-projection path in the sprite shaders (80-byte root constants) (F4 plan 1 T4)"
```

---

### Task 5: Sprites submit as world quads from the full basis

**Files:**
- Modify: `ArcaneClient/src/Arcane/Render/RenderSystems.hpp:40-201`
- Modify: `ArcaneEditor/src/App/EditorAppFrame.cpp` `SubmitSceneToBatcher` (`b.SetViewProjection(m_runtime->View().ViewProjection())` before `SubmitRender`), `ArcaneRuntime/src/RuntimeFrame.cpp` (same, before the runtime's submit)
- Modify: `ArcaneEditor/src/Viewport/EditorCamera.cpp:19-85` (framing bounds: use the same corner derivation; Task 6 rewrites this file anyway — keep the change minimal here)
- Test: `ArcaneTests/src/RenderInterpolationTest.cpp` (existing cases "RenderSubmissionSystem interpolates a sprite by PhysicsInterpBuffer + alpha", "...snaps to the current pose on any buffer miss", "...blends FROM the captured pose...") — they record through a geometry-recording `Batcher2D` double; the double gains `QuadWorld`/`CircleWorld`/`SetViewProjection` overrides that record corners

**Interfaces:**
- Consumes: `RenderContext2D::view`, `Batcher2D::QuadWorld/CircleWorld`, `SpriteEntry`.
- Produces: the sprite corner rule (also used by framing and, in plan 2, by the pick emitter):
  ```cpp
  // Arcane/Render/SpriteGeometry.hpp (new, header-only, Core-free of NRI)
  struct SpriteQuad { std::array<glm::vec3, 4> corners; /* TL,TR,BR,BL in the sprite's plane, +Y up = image top */ };
  SpriteQuad SpriteWorldQuad(const glm::mat4& world, glm::vec2 baseSizeMetres, glm::vec2 pivot);
  ```
  where local corners are `(-pivot.x*w, (1-pivot.y)*h)` TL, `((1-pivot.x)*w, (1-pivot.y)*h)` TR, `((1-pivot.x)*w, -pivot.y*h)` BR, `(-pivot.x*w, -pivot.y*h)` BL, each transformed by `world * vec4(local, 0, 1)`. Pivot (0,0) = bottom-left, (0.5,0.5) = centre (spec §2, Task 2's `SpriteAsset.hpp` comment).

- [ ] **Step 1: Write the failing test**

In `RenderInterpolationTest.cpp` (or a new `SpriteGeometryTest.cpp`):
```cpp
TEST_CASE("SpriteWorldQuad: the image top is +Y, the pivot anchors the quad, the full basis applies", "[render][sprite]")
{
    const glm::mat4 world = glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 3.0f, 0.5f));
    const auto q = Arcane::SpriteWorldQuad(world, {2.0f, 1.0f}, {0.5f, 0.5f});
    CHECK(q.corners[0] == glm::vec3(1.0f, 3.5f, 0.5f));   // TL: left, UP
    CHECK(q.corners[2] == glm::vec3(3.0f, 2.5f, 0.5f));   // BR
    const auto tilted = Arcane::SpriteWorldQuad(glm::rotate(glm::mat4(1.0f), glm::half_pi<float>(), glm::vec3(1,0,0)), {1,1}, {0.5f,0.5f});
    CHECK(std::abs(tilted.corners[0].y) < 1e-5f);          // a 90-degree X tilt lays the quad into the XZ plane
    CHECK(tilted.corners[0].z == Approx(0.5f));
}
TEST_CASE("RenderSubmissionSystem submits sprites as WORLD quads with the interpolated XY pose", "[render][sprite]")
{
    // reuse the existing interpolation fixture; assert the recorder saw QuadWorld with corners whose
    // centre == lerp(prev, current, alpha) and whose z == the entity's z.
}
```

- [ ] **Step 2: Build to verify failure.**

- [ ] **Step 3: Implement**

Create `ArcaneClient/src/Arcane/Render/SpriteGeometry.hpp` with `SpriteWorldQuad` as specified. Rewrite the body of `RenderSubmissionSystem::operator()`:
- keep the interp blend, but apply it to a working copy of the WORLD matrix: `glm::mat4 m = world.matrix;` then, on an interp hit, rebuild the XY translation and Z-angle: `m[3].x = lerpX; m[3].y = lerpY;` and rotate the upper-left 2×2 to the blended angle while preserving the column lengths (scale): `const glm::vec2 sx = length(vec2(m[0])), sy = ...; m[0].x = c*sx; m[0].y = s*sx; m[1].x = -s*sy; m[1].y = c*sy;` (the 2D physics pose lives in XY; Z rows untouched).
- Rect: `ctx->batcher->QuadWorld(materialId or kMaterialSprite, texId, SpriteWorldQuad(m, baseSize, pivot).corners, uvMin, uvMax, sprite.tint)`; untextured → `QuadWorld(kMaterialSprite, Guid::Nil(), corners, {0,0},{1,1}, tint)` (the white texel).
- Circle: `CircleWorld(vec3(m[3]), normalize(vec3(m[0])), normalize(vec3(m[1])), dstSize.x * 0.5f (metres: baseSize.x * worldScale.x * 0.5), tint)`.
- Capsule: a `QuadWorld` band (corners from `SpriteWorldQuad` with size `(size.x - size.y, size.y)`) plus two `CircleWorld`s at `centre ± right * halfLen`.
- Delete the `ctx->zoom` / `ctx->cameraOffset` uses; the system no longer projects anything. Layer/order unchanged.
- Hosts: `b.SetViewProjection(m_runtime->View().ViewProjection());` immediately after `b.Begin(...)` in `EditorAppFrame.cpp::RenderSceneToViewport` and the runtime's equivalent (grep `Begin(` in `RuntimeFrame.cpp`).
- UV orientation: `SpriteEntry::uvMin/uvMax` come from the pixel sub-rect with (0,0) at the image TOP-left; the TL corner keeps `uvMin` — so the image top lands on the quad's +Y edge. Verify at the desk (a text sprite reads upright).

- [ ] **Step 4: Update the recording double and the tests**

The `Batcher2D` test double(s) in `ArcaneTests` (grep `: public Arcane::Batcher2D`) implement the three new pure virtuals (record corners; `SetViewProjection` stores). Existing interpolation cases assert on `Rect`/`Quad` pixel positions — rewrite them to assert on `QuadWorld` corners' centre in metres (no zoom factor anymore).

- [ ] **Step 5: Build, run, commit**

Run: `.\ArcaneTests.exe "[render],[sprite]"`, then the unfiltered suite (the `[witness][gpu]` runtime-scene lane will show mirrored-but-now-correct content; goldens still fail until Task 12).
```bash
git add ArcaneClient/src/Arcane/Render/SpriteGeometry.hpp ArcaneClient/src/Arcane/Render/RenderSystems.hpp ArcaneEditor/src/App/EditorAppFrame.cpp ArcaneRuntime/src/RuntimeFrame.cpp ArcaneTests/src/*.cpp
git commit -m "feat(render): sprites submit as WORLD quads from the full basis (SpriteWorldQuad; pivot (0,0) = bottom-left; interp pose re-baked into the matrix); hosts push the view-projection into the batcher (F4 plan 1 T5)"
```

---

### Task 6: `EditorCamera` — two persisted transforms, `Resolve`, navigation, framing

**Files:**
- Rewrite: `ArcaneEditor/src/Viewport/EditorCamera.hpp`, `EditorCamera.cpp`
- Modify: `ArcaneEditor/src/Scene/EditModeSchedule.cpp:25-54` (`camera.Frame(bounds, viewportSize)`; the empty-scene case → `camera.CentreOrigin()`), `EditModeSchedule.hpp` (unchanged signature)
- Rewrite: `ArcaneTests/src/EditorCameraTest.cpp` (the `[editor]` cases); `EditModeScheduleTest.cpp:112-160` adapts to the new state fields
- Delete the Task 3 shim in `EditorAppFrame.cpp` (`:935`, `:1389` now `m_camera.Resolve(viewport)`)

**Interfaces:**
- Produces:
  ```cpp
  namespace Arcane::Editor {
  enum class ViewMode : std::uint8_t { TwoD = 0, Perspective = 1 };   // persisted as int; append only
  struct Ortho2D  { glm::vec2 center{0,0}; float halfHeight = 5.0f; };
  struct Orbit3D  { glm::vec3 pivot{0,0,0}; float yawDeg = -30.0f; float pitchDeg = 30.0f; float distance = 10.0f; float fovYDeg = 60.0f; };
  struct FramingBounds { glm::vec3 min, max; std::size_t count = 0; bool Valid() const; };   // now 3D
  struct EditorCamera {
      static constexpr float kMinHalfHeight = 0.01f, kMaxHalfHeight = 1.0e6f, kWheelStep = 1.12f, kFrameFill = 0.9f;
      static constexpr float kMinDistance = 0.05f, kMaxDistance = 1.0e5f, kNearZ = 0.05f, kFarZ = 5000.0f;
      static constexpr float kBaseFlySpeed = 5.0f;   // m/s at speedScalar 1, distance 10 m
      ViewMode mode = ViewMode::TwoD; Ortho2D ortho; Orbit3D orbit;
      float speedScalar = 1.0f;                       // persisted; wheel while flying adjusts x1.1
      ViewTransform Resolve(glm::uvec2 viewport) const noexcept;
      // 2D
      void Pan2D(glm::vec2 screenDelta, glm::uvec2 viewport) noexcept;
      void ZoomAt2D(glm::vec2 screenPos, float wheelTicks, glm::uvec2 viewport) noexcept;
      // 3D
      void Look(glm::vec2 mouseDeltaPx) noexcept;                    // right-drag: yaw/pitch about the EYE (pivot moves)
      void Orbit(glm::vec2 mouseDeltaPx) noexcept;                   // alt+left-drag: yaw/pitch about the PIVOT
      void Fly(glm::vec3 localAxis /*x right, y up, z forward*/, float dtSeconds, bool boost) noexcept;
      void Pan3D(glm::vec2 screenDelta, glm::uvec2 viewport) noexcept;
      void Dolly(float wheelTicks) noexcept;
      void AdjustSpeed(float wheelTicks) noexcept;
      glm::vec3 Eye() const noexcept; glm::vec3 Forward() const noexcept; glm::vec3 Right() const noexcept; glm::vec3 Up() const noexcept;
      // both
      void Frame(const FramingBounds& b, glm::uvec2 viewport) noexcept;   // mode-aware
      void CentreOrigin() noexcept;
      glm::vec3 FocusPoint() const noexcept;                              // 2D: (center, 0); 3D: pivot
  };
  FramingBounds SelectionFramingBounds(Astra::Registry&, std::span<const Astra::Entity>);   // 3D AABB (sprites via SpriteWorldQuad; meshes via MeshTable bounds when a MeshRenderer resolves; bare nodes as points)
  FramingBounds SceneFramingBounds(Astra::Registry&);
  }
  ```
- Consumes: `ViewTransform`, `SpriteWorldQuad` (Task 5), `MeshTable`/`MeshEntry::bounds` (`SceneResources.hpp`; grep the bounds field name).

- [ ] **Step 1: Write the failing tests** (replace `EditorCameraTest.cpp` wholesale)

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <Viewport/EditorCamera.hpp>
using Catch::Approx; using namespace Arcane::Editor;
static const glm::uvec2 kVp{800, 600};

TEST_CASE("EditorCamera defaults: 2D mode, 5 m half-height at the origin, and Resolve is orthographic", "[editor][camera]")
{
    const EditorCamera cam;
    CHECK(cam.mode == ViewMode::TwoD);
    const auto v = cam.Resolve(kVp);
    CHECK(v.IsOrthographic());
    CHECK(v.WorldToScreen({0,0,0}).x == Approx(400.0f));
    CHECK(v.WorldToScreen({0,1,0}).y == Approx(300.0f - 60.0f));
}
TEST_CASE("Pan2D: the world follows the cursor 1:1; dragging DOWN moves the world down", "[editor][camera]")
{
    EditorCamera cam;
    const glm::vec3 before = cam.Resolve(kVp).WorldToScreen({1,1,0});
    cam.Pan2D({30.0f, -12.0f}, kVp);
    const glm::vec3 after = cam.Resolve(kVp).WorldToScreen({1,1,0});
    CHECK(after.x - before.x == Approx(30.0f)); CHECK(after.y - before.y == Approx(-12.0f));
}
TEST_CASE("ZoomAt2D keeps the world point under the cursor fixed and clamps", "[editor][camera]")
{
    EditorCamera cam;
    const glm::vec2 cursor{123.0f, 456.0f};
    const Arcane::Ray r = cam.Resolve(kVp).ScreenToRay(cursor);
    cam.ZoomAt2D(cursor, 3.0f, kVp);
    const glm::vec3 s = cam.Resolve(kVp).WorldToScreen(r.origin);
    CHECK(s.x == Approx(cursor.x).margin(1e-2f)); CHECK(s.y == Approx(cursor.y).margin(1e-2f));
    CHECK(cam.ortho.halfHeight == Approx(5.0f / std::pow(EditorCamera::kWheelStep, 3.0f)));
    cam.ZoomAt2D(cursor, -1000.0f, kVp); CHECK(cam.ortho.halfHeight == Approx(EditorCamera::kMaxHalfHeight));
    cam.ZoomAt2D(cursor,  1000.0f, kVp); CHECK(cam.ortho.halfHeight == Approx(EditorCamera::kMinHalfHeight));
}
TEST_CASE("Perspective Resolve: the eye orbits the pivot at yaw/pitch/distance and looks at it", "[editor][camera]")
{
    EditorCamera cam; cam.mode = ViewMode::Perspective;
    cam.orbit = { {1,2,3}, 0.0f, 0.0f, 10.0f, 60.0f };
    CHECK(cam.Eye() == glm::vec3(1, 2, 13));                       // yaw 0 / pitch 0 sits on +Z looking down -Z
    const auto v = cam.Resolve(kVp);
    CHECK_FALSE(v.IsOrthographic());
    const glm::vec3 c = v.WorldToScreen(cam.orbit.pivot);
    CHECK(c.x == Approx(400.0f)); CHECK(c.y == Approx(300.0f));
    CHECK(v.WorldToScreen({1, 3, 3}).y < 300.0f);                   // +Y up on screen
    cam.orbit.pitchDeg = 90.0f - 1e-3f;                              // clamp holds
    cam.Orbit({0.0f, -1000.0f}); CHECK(cam.orbit.pitchDeg <= 89.999f); CHECK(cam.orbit.pitchDeg >= -89.999f);
}
TEST_CASE("Orbit turns about the pivot; Look turns about the eye and moves the pivot", "[editor][camera]")
{
    EditorCamera cam; cam.mode = ViewMode::Perspective;
    const glm::vec3 pivot = cam.orbit.pivot; const glm::vec3 eye = cam.Eye();
    cam.Orbit({40.0f, 0.0f});
    CHECK(cam.orbit.pivot == pivot); CHECK(glm::length(cam.Eye() - pivot) == Approx(10.0f));
    EditorCamera cam2; cam2.mode = ViewMode::Perspective;
    cam2.Look({40.0f, 0.0f});
    CHECK(glm::length(cam2.Eye() - eye) < 1e-4f); CHECK(cam2.orbit.pivot != pivot);
}
TEST_CASE("Fly moves eye and pivot together along the camera axes, scaled by distance and the scalar", "[editor][camera]")
{
    EditorCamera cam; cam.mode = ViewMode::Perspective;   // distance 10 -> scale 1
    const glm::vec3 eye0 = cam.Eye(), piv0 = cam.orbit.pivot;
    cam.Fly({0,0,1}, 1.0f, false);                         // forward for one second
    CHECK(glm::length(cam.Eye() - eye0) == Approx(EditorCamera::kBaseFlySpeed));
    CHECK(glm::length(cam.orbit.pivot - piv0) == Approx(EditorCamera::kBaseFlySpeed));
    CHECK(glm::dot(cam.Eye() - eye0, cam.Forward()) > 0.0f);
    cam.speedScalar = 2.0f; cam.orbit.distance = 20.0f;
    const glm::vec3 eye1 = cam.Eye();
    cam.Fly({1,0,0}, 0.5f, true);                          // right, half a second, boosted x2
    CHECK(glm::length(cam.Eye() - eye1) == Approx(EditorCamera::kBaseFlySpeed * 2.0f * 2.0f * 2.0f * 0.5f));
}
TEST_CASE("Dolly scales the distance about the pivot; AdjustSpeed steps x1.1 within limits", "[editor][camera]")
{
    EditorCamera cam; cam.mode = ViewMode::Perspective;
    cam.Dolly(1.0f); CHECK(cam.orbit.distance == Approx(10.0f / EditorCamera::kWheelStep));
    cam.Dolly(-1000.0f); CHECK(cam.orbit.distance == Approx(EditorCamera::kMaxDistance));
    cam.AdjustSpeed(1.0f); CHECK(cam.speedScalar == Approx(1.1f));
    cam.AdjustSpeed(-100.0f); CHECK(cam.speedScalar >= 0.01f);
}
TEST_CASE("Frame: 2D fits the tighter axis with the margin; perspective solves distance = radius / (tan(fov/2) * min(aspect,1))", "[editor][camera]")
{
    FramingBounds b; b.min = {-2, -1, 0}; b.max = {2, 1, 0}; b.count = 1;
    EditorCamera cam;
    cam.Frame(b, kVp);
    CHECK(cam.ortho.center == glm::vec2(0, 0));
    // width 4 m across 800 px * 0.9 = 180 px/m -> halfHeight = 300/180
    CHECK(cam.ortho.halfHeight == Approx(300.0f / 180.0f));
    cam.mode = ViewMode::Perspective;
    cam.Frame(b, kVp);
    CHECK(cam.orbit.pivot == glm::vec3(0, 0, 0));
    const float radius = glm::length(glm::vec3(2, 1, 0));
    CHECK(cam.orbit.distance == Approx(radius / (std::tan(glm::radians(30.0f)) * 1.0f) / EditorCamera::kFrameFill));
}
TEST_CASE("Frame ignores an invalid bounds and a zero viewport; CentreOrigin resets the 2D centre only", "[editor][camera]")
{
    EditorCamera cam; const EditorCamera before = cam;
    cam.Frame(FramingBounds{}, kVp); CHECK(cam.ortho.halfHeight == before.ortho.halfHeight);
    FramingBounds b; b.min = b.max = {3,3,3}; b.count = 1;
    cam.Frame(b, {0, 0}); CHECK(cam.ortho.center == before.ortho.center);
    cam.ortho.center = {9, 9}; cam.CentreOrigin(); CHECK(cam.ortho.center == glm::vec2(0, 0));
}
```
Keep the existing `SelectionFramingBounds` / `SceneFramingBounds` cases at the end of the old file, updated for 3D bounds (a sprite at (2,3,0) with base 1×1 contributes min (1.5,2.5,0) max (2.5,3.5,0)).

- [ ] **Step 2: Build to verify failure.**

- [ ] **Step 3: Implement the header** (replace the file-top comment with the spec's §4 summary) and the `.cpp`:

```cpp
    namespace
    {
        constexpr float kPi = 3.14159265358979323846f;
        glm::vec3 DirFrom(float yawDeg, float pitchDeg) noexcept   // pivot -> eye direction
        {
            const float y = glm::radians(yawDeg), p = glm::radians(pitchDeg);
            return { std::cos(p) * std::sin(y), std::sin(p), std::cos(p) * std::cos(y) };
        }
        float PixelsPerMetreAtPivot(const Orbit3D& o, glm::uvec2 vp) noexcept
        {
            const float halfH = o.distance * std::tan(glm::radians(o.fovYDeg) * 0.5f);
            return halfH > 0.0f ? float(vp.y) * 0.5f / halfH : 0.0f;
        }
        float DistanceScale(float distance) noexcept   // UE's bUseDistanceScaledCameraSpeed, in metres
        {
            return std::clamp(distance / 10.0f, 0.1f, 1000.0f);
        }
    }

    ViewTransform EditorCamera::Resolve(glm::uvec2 viewport) const noexcept
    {
        if (mode == ViewMode::TwoD)
            return ViewTransform::Orthographic(ortho.center, ortho.halfHeight, viewport);
        return ViewTransform::Perspective(Eye(), orbit.pivot, glm::vec3(0, 1, 0), orbit.fovYDeg, viewport, kNearZ, kFarZ);
    }
    glm::vec3 EditorCamera::Eye() const noexcept { return orbit.pivot + DirFrom(orbit.yawDeg, orbit.pitchDeg) * orbit.distance; }
    glm::vec3 EditorCamera::Forward() const noexcept { return -DirFrom(orbit.yawDeg, orbit.pitchDeg); }
    glm::vec3 EditorCamera::Right() const noexcept { return glm::normalize(glm::cross(Forward(), glm::vec3(0, 1, 0))); }
    glm::vec3 EditorCamera::Up() const noexcept { return glm::cross(Right(), Forward()); }

    void EditorCamera::Pan2D(glm::vec2 d, glm::uvec2 vp) noexcept
    {
        if (!(vp.y > 0u)) return;
        const float ppm = float(vp.y) * 0.5f / ortho.halfHeight;   // px per metre
        ortho.center += glm::vec2(-d.x, d.y) / ppm;                  // screen y down -> world y up
    }
    void EditorCamera::ZoomAt2D(glm::vec2 screenPos, float ticks, glm::uvec2 vp) noexcept
    {
        if (!(vp.x > 0u) || !(vp.y > 0u)) return;
        const float next = std::clamp(ortho.halfHeight / std::pow(kWheelStep, ticks), kMinHalfHeight, kMaxHalfHeight);
        if (!(next > 0.0f) || next == ortho.halfHeight) return;
        const glm::vec2 anchored = glm::vec2(Resolve(vp).ScreenToRay(screenPos).origin);   // world under the cursor (ortho: z ignored)
        ortho.halfHeight = next;
        const float ppm = float(vp.y) * 0.5f / next;
        ortho.center.x = anchored.x - (screenPos.x - float(vp.x) * 0.5f) / ppm;
        ortho.center.y = anchored.y + (screenPos.y - float(vp.y) * 0.5f) / ppm;
    }
    void EditorCamera::Look(glm::vec2 d) noexcept
    {
        const glm::vec3 eye = Eye();
        orbit.yawDeg   -= d.x * 0.2f;
        orbit.pitchDeg  = std::clamp(orbit.pitchDeg + d.y * 0.2f, -90.0f + 1e-3f, 90.0f - 1e-3f);
        orbit.pivot = eye - DirFrom(orbit.yawDeg, orbit.pitchDeg) * orbit.distance;   // the eye stays put
    }
    void EditorCamera::Orbit(glm::vec2 d) noexcept
    {
        orbit.yawDeg   -= d.x * 0.4f;
        orbit.pitchDeg  = std::clamp(orbit.pitchDeg + d.y * 0.4f, -90.0f + 1e-3f, 90.0f - 1e-3f);
    }
    void EditorCamera::Fly(glm::vec3 local, float dt, bool boost) noexcept
    {
        if (!(dt > 0.0f)) return;
        const float speed = kBaseFlySpeed * speedScalar * DistanceScale(orbit.distance) * (boost ? 2.0f : 1.0f);
        const glm::vec3 delta = (Right() * local.x + glm::vec3(0, 1, 0) * local.y + Forward() * local.z) * (speed * dt);
        orbit.pivot += delta;   // eye = pivot + dir*distance, so the eye moves by the same delta
    }
    void EditorCamera::Pan3D(glm::vec2 d, glm::uvec2 vp) noexcept
    {
        const float ppm = PixelsPerMetreAtPivot(orbit, vp);
        if (!(ppm > 0.0f)) return;
        orbit.pivot += (Right() * -d.x + Up() * d.y) / ppm;
    }
    void EditorCamera::Dolly(float ticks) noexcept
    {
        orbit.distance = std::clamp(orbit.distance / std::pow(kWheelStep, ticks), kMinDistance, kMaxDistance);
    }
    void EditorCamera::AdjustSpeed(float ticks) noexcept
    {
        speedScalar = std::clamp(speedScalar * std::pow(1.1f, ticks), 0.01f, 100.0f);
    }
    void EditorCamera::Frame(const FramingBounds& b, glm::uvec2 vp) noexcept
    {
        if (!b.Valid() || !(vp.x > 0u) || !(vp.y > 0u)) return;
        const glm::vec3 lo = glm::min(b.min, b.max), hi = glm::max(b.min, b.max);
        const glm::vec3 centre = (lo + hi) * 0.5f, extent = hi - lo;
        if (mode == ViewMode::TwoD)
        {
            ortho.center = glm::vec2(centre);
            float halfH = 0.0f;
            if (extent.y > 0.0f) halfH = extent.y * 0.5f / kFrameFill;
            if (extent.x > 0.0f) { const float aspect = float(vp.x) / float(vp.y); halfH = std::max(halfH, extent.x * 0.5f / kFrameFill / aspect); }
            if (halfH > 0.0f) ortho.halfHeight = std::clamp(halfH, kMinHalfHeight, kMaxHalfHeight);
            return;
        }
        orbit.pivot = centre;
        const float radius = std::max(glm::length(extent) * 0.5f, 0.05f);
        const float aspect = float(vp.x) / float(vp.y);
        const float tanHalf = std::tan(glm::radians(orbit.fovYDeg) * 0.5f) * std::min(aspect, 1.0f);
        orbit.distance = std::clamp(radius / tanHalf / kFrameFill, kMinDistance, kMaxDistance);
    }
    void EditorCamera::CentreOrigin() noexcept { ortho.center = glm::vec2(0.0f); }
    glm::vec3 EditorCamera::FocusPoint() const noexcept { return mode == ViewMode::TwoD ? glm::vec3(ortho.center, 0.0f) : orbit.pivot; }
```
`SelectionFramingBounds` / `SceneFramingBounds`: grow a 3D AABB over each sprite's `SpriteWorldQuad` corners (Task 5's helper; the same rule the renderer draws by) and over each `MeshRenderer` whose mesh resolves in the `MeshTable` (transform the mesh's local AABB corners by the world matrix; grep `MeshEntry` for the bounds field), and bare `WorldTransform` nodes as points. `SceneFramingBounds` includes meshes and visible sprites, excludes `Hidden`.

- [ ] **Step 4: `EditModeSchedule::ServicePendingFrame`**: `camera.Frame(bounds, glm::uvec2(viewportSize));` and the empty-scene `SceneOpen` case → `camera.CentreOrigin();`. Update `EditModeScheduleTest.cpp` expectations (`cam.ortho.center`, `cam.ortho.halfHeight`).

- [ ] **Step 5: Replace the Task 3 shim** in `EditorAppFrame.cpp` with `m_runtime->SetView(m_camera.Resolve(ViewportSize()));` (add a `glm::uvec2 ViewportSize() const` helper on `EditorApp` returning `{ViewportWidth(), ViewportHeight()}`); the existing `m_camera.Pan(...)` / `ZoomAt(...)` calls become `Pan2D(..., ViewportSize())` / `ZoomAt2D(..., ..., ViewportSize())` (Task 7 rewrites the input block fully; keep 2D working here).

- [ ] **Step 6: Build, run, commit**

Run: `.\ArcaneTests.exe "[editor],[camera]"`; then the whole `~[gpu]`.
```bash
git add ArcaneEditor/src/Viewport/EditorCamera.hpp ArcaneEditor/src/Viewport/EditorCamera.cpp ArcaneEditor/src/Scene/EditModeSchedule.cpp ArcaneEditor/src/App/EditorAppFrame.cpp ArcaneEditor/src/App/EditorApp.hpp ArcaneTests/src/EditorCameraTest.cpp ArcaneTests/src/EditModeScheduleTest.cpp
git commit -m "feat(editor): EditorCamera -- two persisted transforms (Ortho2D / Orbit3D) by ViewMode, Resolve to a ViewTransform, Unreal navigation ops (look/orbit/fly/pan/dolly, distance-scaled speed + scalar), mode-aware framing over 3D bounds (F4 plan 1 T6, spec s4)"
```

---

### Task 7: The editor host — navigation input, the mesh pass through the editor camera, persistence, `--view-mode`, Perspective gating

**Files:**
- Modify: `ArcaneEditor/src/App/EditorAppFrame.cpp:853-936` (`UpdateEditorCamera`), `:1806-1837` (mesh pass camera), `:1703` + `:940-1000` (gizmo gated on `AsAffine2D()`), `:1863-1879` (pick gated), `:1387-1396` (the push)
- Create: `ArcaneEditor/src/Viewport/ViewportSettings.{hpp,cpp}` — `struct ViewportSettings { bool showGrid = true; GridPlane gridPlane = GridPlane::XZ; float gizmoSize = 1.0f; }`, the `[EditorViewport][Camera]` handler (read/write the camera + settings), and `ApplyViewModeSeed(const std::string& flag)`
- Modify: `ArcaneEditor/src/App/EditorApp.{hpp,cpp}` (`m_viewSettings`; `RegisterViewportSettings()` beside `RegisterPlayModeSettings()`; `EditorApp.hpp:1090-1095` the pan gesture grows into a `CameraGesture { enum { None, Pan2D, Look, Orbit, Pan3D } kind; glm::vec2 lastMouse; }`)
- Modify: `ArcaneClient/src/Arcane/Host/HostConfig.{hpp,cpp}` (`std::string viewMode; // "" | "2d" | "perspective"` + `cli.Option("view-mode", "", "editor viewport mode to start in: 2d | perspective")`; refuse other spellings)
- Modify: `ArcaneEditor/src/App/EditorApp.cpp` `Init` / `StageFinalize` (seed the camera from `m_config.viewMode` AFTER the ini is read, so a flag beats a persisted mode)
- Test: `ArcaneTests/src/ViewportSettingsTest.cpp` (new: the ini line round-trip, pure), `HostConfigTest.cpp` (+1 case: `--view-mode perspective` parses; `--view-mode sideways` refuses)

**Interfaces:**
- Consumes: Task 6's `EditorCamera` API; `InputSnapshot` (`mouseButtons` bits LMB=0x1 RMB=0x2 MMB=0x4; `wheelY`; scancodes `kScW=26 kScA=4 kScS=22 kScD=7 kScQ=20 kScE=8`, `kScLShift=225`, `kScLAlt=226`, `kScRAlt=230`); `ShortcutsLive`, `m_edges`.
- Produces: `ViewportSettings::WriteIni(ImGuiTextBuffer&, const EditorCamera&, const ViewportSettings&)` and `ReadIniLine(const char*, EditorCamera&, ViewportSettings&) -> bool` (pure; the handler callbacks wrap them).

- [ ] **Step 1: Write the failing tests**

`ArcaneTests/src/ViewportSettingsTest.cpp`:
```cpp
TEST_CASE("Viewport settings ini lines round-trip the camera, mode, grid and speed", "[editor][settings]")
{
    Arcane::Editor::EditorCamera cam; cam.mode = Arcane::Editor::ViewMode::Perspective;
    cam.ortho = { {1.5f, -2.0f}, 3.25f }; cam.orbit = { {1,2,3}, 12.0f, -8.0f, 42.0f, 55.0f }; cam.speedScalar = 2.5f;
    Arcane::Editor::ViewportSettings s; s.showGrid = false; s.gridPlane = Arcane::Editor::GridPlane::XY; s.gizmoSize = 1.5f;
    ImGuiTextBuffer buf;
    Arcane::Editor::ViewportSettings::WriteIni(buf, cam, s);
    Arcane::Editor::EditorCamera back; Arcane::Editor::ViewportSettings sb;
    for (const std::string& line : SplitLines(buf.c_str()))   // a tiny local helper; skips the [section] header
        Arcane::Editor::ViewportSettings::ReadIniLine(line.c_str(), back, sb);
    CHECK(back.mode == cam.mode); CHECK(back.ortho.halfHeight == Approx(3.25f)); CHECK(back.orbit.distance == Approx(42.0f));
    CHECK(back.orbit.yawDeg == Approx(12.0f)); CHECK(back.speedScalar == Approx(2.5f));
    CHECK_FALSE(sb.showGrid); CHECK(sb.gridPlane == Arcane::Editor::GridPlane::XY); CHECK(sb.gizmoSize == Approx(1.5f));
}
TEST_CASE("A malformed or out-of-range ini line leaves the defaults", "[editor][settings]")
{
    Arcane::Editor::EditorCamera cam; Arcane::Editor::ViewportSettings s;
    CHECK_FALSE(Arcane::Editor::ViewportSettings::ReadIniLine("Mode=7", cam, s));
    CHECK(cam.mode == Arcane::Editor::ViewMode::TwoD);
    CHECK_FALSE(Arcane::Editor::ViewportSettings::ReadIniLine("Orbit=nan", cam, s));
    CHECK(cam.orbit.distance == Approx(10.0f));
}
```
`HostConfigTest.cpp`: `--view-mode perspective` → `config->viewMode == "perspective"`; `--view-mode sideways` → `exitCode != 0`.

- [ ] **Step 2: Build to verify failure.**

- [ ] **Step 3: `ViewportSettings`** — lines: `Mode=%d`, `Ortho=%f %f %f`, `Orbit=%f %f %f %f %f %f %f` (pivot xyz, yaw, pitch, distance, fovY), `Speed=%f`, `Grid=%d %d` (show, plane), `GizmoSize=%f`. Every read validates finiteness and ranges (mode 0..1, halfHeight within the clamps, distance within the clamps, plane 0..1) and returns false without touching state otherwise. The handler mirrors `EditorApp.cpp:130-196` exactly with `kViewportIniType = "EditorViewport"`, `kViewportIniName = "Camera"`.

- [ ] **Step 4: Input** — rewrite `UpdateEditorCamera(snap, inViewport, lx, ly, dt)`:
```cpp
        const bool lmb = snap.mouseButtons & 0x1u, rmb = snap.mouseButtons & 0x2u, mmb = snap.mouseButtons & 0x4u;
        const bool alt = snap.ScancodeDown(kScLAlt) || snap.ScancodeDown(kScRAlt);
        const bool shift = snap.ScancodeDown(kScLShift) || snap.ScancodeDown(kScRShift);
        const glm::vec2 mouse(snap.mouseX, snap.mouseY);
        const glm::vec2 delta = mouse - m_camGesture.lastMouse;
        if (!InPlayMode())
        {
            // A gesture may only START over the viewport; once started it tracks anywhere (the pan rule).
            if (m_camGesture.kind == CameraGesture::None && inViewport)
            {
                if (m_camera.mode == ViewMode::TwoD) { if (m_edges.rmb.pressed) m_camGesture.kind = CameraGesture::Pan2D; }
                else if (alt && m_edges.lmb.pressed) m_camGesture.kind = CameraGesture::Orbit;
                else if (m_edges.rmb.pressed)        m_camGesture.kind = CameraGesture::Look;
                else if (m_edges.mmb.pressed)        m_camGesture.kind = CameraGesture::Pan3D;
            }
            const bool held = (m_camGesture.kind == CameraGesture::Pan2D && rmb) || (m_camGesture.kind == CameraGesture::Look && rmb)
                           || (m_camGesture.kind == CameraGesture::Orbit && lmb) || (m_camGesture.kind == CameraGesture::Pan3D && mmb);
            if (!held) m_camGesture.kind = CameraGesture::None;
            else if (!m_camGesture.fresh)   // not the press frame: lastMouse is a real previous cursor
            {
                switch (m_camGesture.kind)
                {
                case CameraGesture::Pan2D: m_camera.Pan2D(delta, ViewportSize()); break;
                case CameraGesture::Look:  m_camera.Look(delta);  break;
                case CameraGesture::Orbit: m_camera.Orbit(delta); break;
                case CameraGesture::Pan3D: m_camera.Pan3D(delta, ViewportSize()); break;
                default: break;
                }
            }
            if (m_camGesture.kind == CameraGesture::Look)   // WASD/QE fly only while looking (UE: WASD_RMBOnly)
            {
                glm::vec3 axis(0.0f);
                if (snap.ScancodeDown(kScW)) axis.z += 1; if (snap.ScancodeDown(kScS)) axis.z -= 1;
                if (snap.ScancodeDown(kScD)) axis.x += 1; if (snap.ScancodeDown(kScA)) axis.x -= 1;
                if (snap.ScancodeDown(kScE)) axis.y += 1; if (snap.ScancodeDown(kScQ)) axis.y -= 1;
                if (axis != glm::vec3(0.0f)) m_camera.Fly(glm::normalize(axis), dt, shift);
                if (snap.wheelY != 0.0f) m_camera.AdjustSpeed(snap.wheelY);
            }
            else if (inViewport && snap.wheelY != 0.0f)
            {
                if (m_camera.mode == ViewMode::TwoD) m_camera.ZoomAt2D(glm::vec2(lx, ly), snap.wheelY, ViewportSize());
                else                                  m_camera.Dolly(snap.wheelY);
            }
        }
        else m_camGesture.kind = CameraGesture::None;
        m_camGesture.fresh = (m_camGesture.kind != CameraGesture::None) && m_camGesture.fresh == false ? false : (m_camGesture.kind != CameraGesture::None && (m_edges.rmb.pressed || m_edges.lmb.pressed || m_edges.mmb.pressed));
        m_camGesture.lastMouse = mouse;
```
(Track `fresh` as "this is the press frame": set it true on the frame the gesture starts, false on every later frame; `m_edges` gains `lmb`/`mmb` edge trackers beside `rmb`.) Keep the F/Home framing block and push `m_runtime->SetView(m_camera.Resolve(ViewportSize()))` at the tail. The gizmo W/E/R keys already exist; `Alt+G` / `Alt+J` set `m_camera.mode` (scancodes `kScG = 10`, `kScJ = 13`) gated on `ShortcutsLive`. Mask RMB/MMB/wheel and (in Perspective) Alt+LMB out of the plugin snapshot the way RMB/wheel already are (`:704`).

- [ ] **Step 5: The mesh pass camera** (`:1806-1837`): in Edit mode `m_meshScene.view = m_runtime->View().view; m_meshScene.projection = m_runtime->View().projection; vp.mesh = &m_meshScene;` whenever instances exist (the editor camera is always valid); in Play mode keep the guarded `ActivePerspectiveSceneCamera` path. Also arm the mesh pass when `m_meshInstances.empty()` is false only (unchanged).

- [ ] **Step 6: Perspective gating** — `GizmoLive()` additionally requires `m_runtime->View().AsAffine2D().has_value()`; the pick chain's `wantOutline`/`wantPick` are forced false when `!AsAffine2D()` (plan 2 lifts both). The tool overlay's Move/Rotate/Scale buttons show a tooltip suffix " (2D mode only until plan 2)" when disabled.

- [ ] **Step 7: `--view-mode`** — `HostConfig` option + validation; in `EditorApp::StageFinalize` after the ini load: `if (m_config.viewMode == "perspective") m_camera.mode = ViewMode::Perspective; else if (m_config.viewMode == "2d") m_camera.mode = ViewMode::TwoD;`. The headless verify layout (`ReferenceProject/Saved/verify-layout.ini`) carries NO `[EditorViewport]` block, so goldens run at the defaults (2D) unless the flag says otherwise.

- [ ] **Step 8: Build, run, desk-check, commit**

Run: `.\ArcaneTests.exe "[editor],[settings],[hostconfig]"`; open ReferenceProject in the editor: right-drag pans in 2D; Alt+G → perspective; right-drag looks, WASD flies, Alt+left orbits, middle-drag pans, wheel dollies; F frames the cube; Alt+J returns to 2D with the 2D framing intact; restart the editor: the mode persisted.
```bash
git add ArcaneEditor/src ArcaneClient/src/Arcane/Host ArcaneTests/src/ViewportSettingsTest.cpp ArcaneTests/src/HostConfigTest.cpp
git commit -m "feat(editor): 3D navigation (look/fly/orbit/pan/dolly), the mesh pass renders through the editor camera in Edit, [EditorViewport][Camera] persistence, --view-mode seed; gizmo + click-pick gated to 2D until plan 2 (F4 plan 1 T7)"
```

---

### Task 8: The `2D | Persp` control and the view-settings dropdown

**Files:**
- Modify: `ArcaneEditor/src/Panels/EditorPanels.hpp:254-256` (signature), `EditorPanels.cpp:1057-1133`
- Modify: `ArcaneEditor/src/App/EditorAppFrame.cpp` (the `DrawViewportPanel` call site, grep `DrawViewportPanel(`)
- Test: none headless (ImGui draw); desk-verified

**Interfaces:**
- Produces: `DrawViewportPanel(uint64_t textureId, uint32_t texW, uint32_t texH, ViewportToolState& tools, bool showToolOverlay)` where
  ```cpp
  struct ViewportToolState {
      bool& gizmoEnabled; Arcane::GizmoMode& mode; Arcane::GizmoSpace& space;
      Arcane::Editor::ViewMode& viewMode; Arcane::Editor::ViewportSettings& settings;
      float& fovYDeg; float& speedScalar; bool gizmoToolsEnabled;   // false in Perspective until plan 2
  };
  ```

- [ ] **Step 1: Implement** — at the LEFT of the overlay group, before the Select button: a two-segment toggle `iconToggle(ICON_LC_SQUARE, "##view_2d", viewMode == TwoD, "2D view (Alt+J)")` and `iconToggle(ICON_LC_BOX, "##view_persp", viewMode == Perspective, "Perspective view (Alt+G)")`, then a `ICON_LC_SETTINGS_2` button opening `ImGui::BeginPopup("##viewsettings")` with: `Checkbox("Show grid", &settings.showGrid)`, `Combo("Grid plane", ...{"XZ (ground)", "XY (2D plane)"})`, `SliderFloat("Field of view", &fovYDeg, 20, 120, "%.0f deg")`, `SliderFloat("Camera speed", &speedScalar, 0.01f, 100.0f, "%.2fx", ImGuiSliderFlags_Logarithmic)`, `SliderFloat("Gizmo size", &settings.gizmoSize, 0.5f, 3.0f)`. Recompute `totalW` to include the three new buttons. Wrap the gizmo buttons in `ImGui::BeginDisabled(!tools.gizmoToolsEnabled)`.

- [ ] **Step 2: Build; desk-check** the control switches modes, the popup persists across restarts (via Task 7's handler), and `overlayHovered` still swallows clicks on the new buttons (no pick fires).

- [ ] **Step 3: Commit**
```bash
git add ArcaneEditor/src/Panels/EditorPanels.hpp ArcaneEditor/src/Panels/EditorPanels.cpp ArcaneEditor/src/App/EditorAppFrame.cpp
git commit -m "feat(editor): the viewport's 2D | Persp control and the view-settings dropdown (grid, plane, fov, speed, gizmo size) (F4 plan 1 T8, spec s6)"
```

---

### Task 9: The 2D grid

**Files:**
- Create: `ArcaneEditor/src/Viewport/ViewportGrid.{hpp,cpp}`
- Modify: `ArcaneEditor/src/App/EditorAppFrame.cpp` `SubmitSceneToBatcher` (draw the grid FIRST, at layer (0,0), before `SubmitRender`, when `mode == TwoD && settings.showGrid`)
- Modify: `premake5.lua` ArcaneTests `files` list (+ `ViewportGrid.cpp`)
- Test: `ArcaneTests/src/ViewportGridTest.cpp` (new)

**Interfaces:**
- Produces:
  ```cpp
  namespace Arcane::Editor {
  struct GridLevel { float spacingMetres; float alpha; };           // alpha in (0,1]
  struct Grid2DPlan { std::array<GridLevel, 3> levels; int count = 0; };
  Grid2DPlan PlanGrid2D(float pixelsPerMetre) noexcept;              // pure
  void DrawGrid2D(Arcane::Batcher2D& b, const Arcane::ViewTransform& view, const Grid2DPlan& plan);   // overlay lines, layer (0,0)
  inline constexpr float kGridFadeInPx = 8.0f, kGridFadeFullPx = 24.0f;
  }
  ```
  Rule: candidate spacings are the decades `10^k` metres; a level is included when its screen spacing `s = spacing * ppm >= kGridFadeInPx`, with `alpha = clamp((s - 8) / (24 - 8), 0, 1) * 0.35` for minors; the level ten times coarser is the major line (alpha 0.55), and only the three finest qualifying levels are kept (finer ones are invisible anyway). Axes draw last: X red `(0.85,0.25,0.25,0.9)`, Y green `(0.3,0.8,0.3,0.9)`.

- [ ] **Step 1: Write the failing tests**
```cpp
TEST_CASE("PlanGrid2D picks decade levels by screen spacing and crossfades between 8 and 24 px", "[editor][grid]")
{
    using namespace Arcane::Editor;
    const Grid2DPlan a = PlanGrid2D(100.0f);        // 1 m = 100 px: 0.1 m (10 px, faint), 1 m, 10 m
    REQUIRE(a.count >= 2);
    CHECK(a.levels[0].spacingMetres == Approx(0.1f)); CHECK(a.levels[0].alpha > 0.0f); CHECK(a.levels[0].alpha < 0.35f);
    CHECK(a.levels[1].spacingMetres == Approx(1.0f)); CHECK(a.levels[1].alpha == Approx(0.35f));
    const Grid2DPlan b = PlanGrid2D(5.0f);          // 1 m = 5 px: 1 m is below fade-in; 10 m (50 px) is the finest
    CHECK(b.levels[0].spacingMetres == Approx(10.0f));
    const Grid2DPlan c = PlanGrid2D(0.0f);          // degenerate: nothing
    CHECK(c.count == 0);
}
TEST_CASE("DrawGrid2D emits only lines inside the visible world rect, plus the two axes on top", "[editor][grid]")
{
    // a recording Batcher2D double (the one Task 5 extended) counts Line() calls; with a 4 m x 3 m view at 100 ppm
    // and a 1 m level, expect 5 vertical + 4 horizontal + 2 axes (deduped against the level lines at 0).
}
```

- [ ] **Step 2: Build to verify failure.**

- [ ] **Step 3: Implement** — `PlanGrid2D` as the rule above; `DrawGrid2D`: from `view.ScreenToRay({0,0}).origin` and `ScreenToRay({W,H}).origin` derive the visible world rect (orthographic only; return immediately if `!view.IsOrthographic()`), for each level iterate `x = floor(minX/spacing)*spacing .. maxX` and draw `b.Line(Point(x, minY), Point(x, maxY), 1.0f, colour)` via `view.WorldToScreen` (skip `x == 0` and `y == 0`, drawn as axes after), colour `(0.5,0.5,0.5,alpha)`; `b.SetLayer(0, 0)` first and restore `SetLayer(0,0)` after (the batcher resets per Begin anyway).

- [ ] **Step 4: Wire + build + desk-check + commit** (the grid sits under sprites; zooming crossfades levels; axes are coloured).
```bash
git add ArcaneEditor/src/Viewport/ViewportGrid.hpp ArcaneEditor/src/Viewport/ViewportGrid.cpp ArcaneEditor/src/App/EditorAppFrame.cpp premake5.lua ArcaneTests/src/ViewportGridTest.cpp
git commit -m "feat(editor): the 2D grid -- decade levels on the metre with an 8-24 px crossfade, axes on top, drawn at the bottom batcher layer (F4 plan 1 T9, spec s5.1)"
```

---

### Task 10: `GridNode` — the analytic depth-tested 3D grid

**Files:**
- Create: `ArcaneClient/src/Arcane/Render/Nri/nodes/GridNode.{hpp,cpp}`, `data/shaders/grid.hlsl`
- Modify: `data/shaders/compile-shaders.bat` (+ `call :compile grid vs_main vs_6_5 grid_vs` / `ps_main ... grid_ps` after the mesh lines)
- Modify: `ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.hpp` (`FrameDesc::grid` (`const GridSceneDesc*`), `RgFrameShape::grid`, `GridNode* Grid()`, `m_grid` owned beside `m_mesh`), `NriGraphContext.cpp` (create/release beside the mesh node; declare after the mesh block: `if (shape.grid) AddGridNode(graph, context, handles.canvas, kGraphCanvasFormat, wantsMesh ? handles.depth : RgTexture{}, *shape.grid, w, h);`)
- Modify: `ArcaneEditor/src/App/EditorAppFrame.cpp` `ArmGraphViewportFrame` (`vp.grid = &m_gridScene` when Edit + Perspective + showGrid)
- Test: `ArcaneTests/src/RenderGraphTest.cpp` (+1 device-less shape case: a frame with `grid` declares a node named "grid" after "mesh" that reads the mesh depth), `ArcaneTests/src/GridNodeTest.cpp` (new `[gpu][pixel]` case)

**Interfaces:**
- Produces:
  ```cpp
  struct GridSceneDesc {
      ViewTransform view;                       // the editor camera
      enum class Plane : std::uint8_t { XZ = 0, XY = 1 } plane = Plane::XZ;
      float minorSpacing = 1.0f, majorEvery = 10.0f;      // metres
      float fadeDistance = 200.0f;                       // metres: alpha reaches 0 here
      glm::vec4 minorColor{0.5f,0.5f,0.5f,0.35f}, majorColor{0.6f,0.6f,0.6f,0.6f};
  };
  ARCANE_API void AddGridNode(RenderGraph&, NriGraphContext*, RgTexture canvas, nri::Format canvasFormat,
                              RgTexture depth /*invalid = no depth test*/, const GridSceneDesc&, std::uint32_t w, std::uint32_t h);
  class ARCANE_API GridNode { static std::unique_ptr<GridNode> Create(NriGraphContext&); void Release(Graveyard&, std::uint64_t);
                              void Prepare(nri::Format canvasFormat, bool hasDepth); void Record(RenderGraphNodeContext&, const GridSceneDesc&, std::uint32_t frameSlot); };
  ```
  Shader: `grid.hlsl` — VS generates a 4-vertex (2-triangle, `SV_VertexID`) quad of half-extent `kExtent = 2000 m` in the plane, centred under the camera at `floor(eye.xz / 10) * 10` (UE's `FmodFloor` wrap), pushes `viewProj` + `eye` + params via root constants (≤128 bytes: `float4x4 viewProj; float4 eyeAndPlane; float4 params; float4 minorColor; float4 majorColor;` = 128); PS computes the analytic grid:
  ```hlsl
  float2 coord = worldPos in-plane / minorSpacing;
  float2 d = fwidth(coord);
  float2 g = abs(frac(coord - 0.5) - 0.5) / d;   // distance to the nearest line in pixels
  float line = 1.0 - min(min(g.x, g.y), 1.0);
  // major: same with coord / majorEvery
  float dist = length(worldPos - eye);
  float fade = saturate(1.0 - dist / fadeDistance);
  float grazing = saturate(abs(dot(normalize(eye - worldPos), planeNormal)) * 4.0);
  color = lerp(minorColor, majorColor, majorLine) with alpha *= max(line, majorLine) * fade * grazing;
  ```
  Pipeline: blend straight alpha, cull NONE, depth `LESS_OR_EQUAL` + `write = false` when a depth attachment is bound, no depth otherwise; shader-pair id `0x6000` (MeshNode is `0x5000`, TonemapNode `0x3000`, outline `0x4000..`, pick `0x4100`, batch `0x2000..`).

- [ ] **Step 1: Write the failing device-less shape test** (in `RenderGraphTest.cpp`, beside the `handles.depth` cases at `:6250-6420`):
```cpp
TEST_CASE("a frame with a grid scene declares 'grid' after 'mesh', reading the mesh depth as its depth attachment", "[nri][graph]")
{
    // Drive DeclareGraphFrame device-less (context = null) with shape.mesh (one instance) and shape.grid set;
    // assert the node order contains "batch2d", "mesh", "grid" in that order, that "grid" wrote the canvas
    // (ColorWrite) and declared the SAME depth handle as "mesh" (graph.WasWritten(handles.depth) by both), and
    // that with shape.mesh null the grid node is still declared and holds no depth attachment.
}
```
and the `[gpu][pixel]` case (`GridNodeTest.cpp`, modelled on `MeshNodeTest.cpp:240-244`): render a 256×256 frame with `grid` (XZ plane) through a perspective view at `eye (0, 5, 10)` looking at the origin, no mesh; read back; assert the centre column has at least one pixel whose luminance exceeds the clear colour (a line is visible) and that the top rows (sky) equal the clear colour (the plane does not cover them). Then with a cube mesh at the origin: the pixel at the cube's centre equals the cube's colour, not a grid line (occlusion).

- [ ] **Step 2: Build to verify failure.**

- [ ] **Step 3: Implement `GridNode`** by copying `MeshNode`'s skeleton for the parts it shares (Create → `Init` loads `grid_vs`/`grid_ps` from the vehicle's bin cache the way `MeshNode::Init` does; a pipeline layout with ONE root-constant block of 128 bytes and no descriptor sets, so the register-space rule is moot; `PipelineFor(canvasFormat, hasDepth)` keyed on both; `Release` buries the layout). No vertex buffer: `CmdDraw(6)` with `SV_VertexID`. `Prepare` resolves the pipeline at declaration time (never in `Record`). `AddGridNode`: `builder.Write(canvas, RgUsage::ColorWrite); if (depth valid) { builder.Write(depth, RgUsage::DepthWrite); graph.SetDepthAttachment(depth); } graph.SetColorAttachments(canvas)` — `DepthWrite` is the graph's barrier state for a bound depth attachment; the PSO's `depth.write = false` is what keeps the grid from writing. Record fills the root constants from the desc (`viewProj = projection * view`, `eye = inverse(view)[3]`), sets the viewport/scissor like MeshNode, binds the pipeline, draws 6.

- [ ] **Step 4: Wire the editor** — `m_gridScene.view = m_runtime->View(); m_gridScene.plane = settings.gridPlane == XY ? Plane::XY : Plane::XZ; vp.grid = (!InPlayMode() && m_camera.mode == Perspective && m_viewSettings.showGrid) ? &m_gridScene : nullptr;`.

- [ ] **Step 5: Build, run, desk-check, commit** — the cube sits on the grid and occludes it; the grid fades with distance; switching the plane to XY puts the grid through the sprites' plane.
```bash
git add ArcaneClient/src/Arcane/Render/Nri/nodes/GridNode.hpp ArcaneClient/src/Arcane/Render/Nri/nodes/GridNode.cpp data/shaders/grid.hlsl data/shaders/compile-shaders.bat ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.hpp ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.cpp ArcaneEditor/src/App/EditorAppFrame.cpp ArcaneTests/src/RenderGraphTest.cpp ArcaneTests/src/GridNodeTest.cpp
git commit -m "feat(render): GridNode -- the analytic ground grid after the mesh pass, depth-tested against its depth transient without writing it, distance + grazing fade, XZ/XY plane (F4 plan 1 T10, spec s5.2)"
```

---

### Task 11: Mesh-creation polish — per-primitive `Create > Mesh >` and the scene's `Add > 3D Object`

**Files:**
- Modify: `ArcaneEditor/src/Panels/CreateAssetDialog.hpp:69-75` (`CreateAssetRequest` gains `int prefillMeshSource = -1;`), `CreateAssetDialog.cpp:542` (the Mesh arm shows the source as a read-only line when prefilled), `AssetPanelCommon.cpp:50-69` (submenu), `App/EditorAppProject.cpp:1348-1371` (`MintMeshAsset(target, MeshSource)`), `App/EditorAppFrame.cpp:2746-2748` (pass the source), `App/EditorApp.hpp` (declarations)
- Create: `EditorApp::MintOrReusePrimitiveMesh(MeshSource) -> Guid` in `EditorAppProject.cpp` (looks for `Content/Meshes/<Name>.arcmesh` by path through the project's registry; mints if absent, the `MintOrReuseSpriteForTexture` shape)
- Modify: `ArcaneEditor/src/Panels/EditorPanels.cpp:1825-1848` (Outliner row context menu: `Add 3D Object >` submenu beside "New Child Entity") and the Scene menu (grep `"New Child Entity"`'s top-level sibling in `EditorPanels.cpp:1976`)
- Test: `ArcaneTests/src/CreateAssetDialogTest.cpp` (+1: a request with `prefillMeshSource = Cube` keeps the bypass invariant), `EntityOpsTest.cpp` (+1: `AddPrimitiveEntity` creates a child with Transform at the focus point and a MeshRenderer bound to the given guid, undoable)

**Interfaces:**
- Produces: `Arcane::Edit::AddPrimitiveEntity(Astra::Registry&, Astra::Entity parent, glm::vec3 position, const Guid& mesh, std::string_view name) -> Astra::Entity` in `ArcaneClient/src/Arcane/Edit/EntityOps.{hpp,cpp}` (uses `CreateEntityInScene`, sets `Transform::position`, adds `MeshRenderer{ mesh }`, names via `Identity`); the Outliner routes it through `ApplyStructural(undo, binding, "Add 3D Object", ...)` exactly like "New Child Entity".
- `MeshSource` names for files: `Cube.arcmesh`, `Plane.arcmesh`, `Sphere.arcmesh`, `Cylinder.arcmesh`, `Capsule.arcmesh` under `Content/Meshes/` (create the folder if missing).

- [ ] **Step 1: Write the failing tests** (as described; the `EntityOpsTest` case asserts the component set and that a second call with the same guid creates a second entity, not a reuse).
- [ ] **Step 2: Build to verify failure.**
- [ ] **Step 3: Implement** — the `Create` submenu: `if (ImGui::BeginMenu(ICON_LC_BOX " Mesh")) { for each MeshSource in {Cube, Plane, UvSphere, Cylinder, Capsule}: if (MenuItem(label)) { actions.requestCreateKind = Mesh; actions.requestMeshSource = (int)source; } EndMenu(); }` and the app copies `requestMeshSource` into `CreateAssetRequest::prefillMeshSource`; `MintMeshAsset(target, source)` sets `data.source = source` before saving. The Outliner submenu: for each primitive, `MintOrReusePrimitiveMesh(source)` then `AddPrimitiveEntity(registry, row.entity, m_camera.FocusPoint(), guid, label)` inside `ApplyStructural`, then select + request a frame of the selection.
- [ ] **Step 4: Build, run `[create],[entityops]`, desk-check** (Create > Mesh > Sphere mints `Content/meshes/<Name>.arcmesh` with source UvSphere and opens the document; Outliner > Add 3D Object > Cube spawns a cube at the focus point, selected; Ctrl+Z removes it).
- [ ] **Step 5: Commit**
```bash
git add ArcaneEditor/src ArcaneClient/src/Arcane/Edit ArcaneTests/src/CreateAssetDialogTest.cpp ArcaneTests/src/EntityOpsTest.cpp
git commit -m "feat(editor): Create > Mesh > per-primitive entries through the one CreateAssetRequest; Outliner Add 3D Object spawns Transform + MeshRenderer bound to Content/Meshes/<Primitive>.arcmesh, minted on first use (F4 plan 1 T11, spec s8)"
```

---

### Task 12: Close — goldens re-blessed, the perspective witness, baselines, docs, the owed comments

**Files:**
- Modify: `ReferenceProject/Verify/References/editor-ui.png`, `runtime-scene-{dx12,vulkan}.png` (re-blessed), a new `editor-ui-perspective.png` slot if `--compare` supports naming one (check `VerifyReport.hpp`'s slot list; if slots are fixed, add `editor-ui-perspective` there with the same shape as `editor-ui`)
- Modify: `scripts/automation-baselines.json` (re-booked), `docs/specs/2026-09-17-f4-editor-3d-authoring-design.md` (Status line: plan 1 closed at `<sha>`), `docs/specs/2026-09-11-physics-2d-wiring-design.md` (a one-line note at its "+Y is down" sentence pointing at the F4 spec §2), `CLAUDE.md` (the 3D-target bullets: "+Y up everywhere")
- Modify: `ArcaneEditor/src/Viewport/EditorCamera.cpp` (the old "framing stays PLANAR (F4 owns the 3D one)" comment is gone with the rewrite — verify), `ArcaneClient/src/Arcane/Edit/Gizmo.hpp:95-101` ("THE GIZMO STAYS 2D (making it 3D is F4)" → "... is F4 PLAN 2"), `ArcaneCore/src/Arcane/Scene/SceneCamera.hpp:173-199` (the Task 3/Task 7 deferral notes: append "F4 plan 1 lifted the 2D path onto ViewTransform; the ortho camera still frames XY by definition")
- Test: `ArcaneTests/src/EditorWitnessTest.cpp` (+1 `[witness][gpu]`: the editor with `--view-mode perspective --headless --frames 60 --settle 30 --report` on ReferenceProject exits 0 and the report's `viewMode == "perspective"`; `VerifyReport` gains that field)

- [ ] **Step 1: Read the diff artifacts** from `scripts/golden-gate.ps1` in Debug: confirm the editor-ui delta is the flip + the new overlay buttons + the 2D grid, and the runtime-scene delta is the flip only.
- [ ] **Step 2: Re-bless** per the procedure in Global Constraints (staged slot, copy to source immediately, gate both configs, end on Debug). Delete the exe-dir `imgui.ini` first.
- [ ] **Step 3: The perspective witness** — add the report field and the case; bless its golden the same way.
- [ ] **Step 4: Baselines** — `scripts/check-baselines.ps1 -Configuration Debug -Invocation "~[gpu]"` and Release; re-book the four rows with a `measured` note attributing the rise per task (T1 5 cases, T2 3, T3 2, T4 1, T5 2, T6 9, T7 3, T9 2, T10 1 device-less, T11 2; derive the assertion counts from `-r json`, never recall them).
- [ ] **Step 5: Docs + comments** as listed; run the unfiltered suite once on the desk GPU (all `[witness][gpu]` green).
- [ ] **Step 6: Desk pass** (spec §9 minus picking/gizmo): orbit the cube; toggle 2D ↔ Persp and back with both framings kept; F frames a mesh in both modes; the cube occludes the grid; sprites in perspective stay in the world; a text sprite reads upright in 2D; the physics scene falls DOWN (−Y) in Play.
- [ ] **Step 7: Commit**
```bash
git add ReferenceProject/Verify scripts/automation-baselines.json docs CLAUDE.md ArcaneClient/src/Arcane/Edit/Gizmo.hpp ArcaneCore/src/Arcane/Scene/SceneCamera.hpp ArcaneClient/src/Arcane/Host/VerifyReport.* ArcaneTests/src/EditorWitnessTest.cpp
git commit -m "chore(f4): plan 1 close -- goldens re-blessed once for the +Y flip and the grid, the perspective editor witness, baselines re-booked, spec/CLAUDE.md/physics-spec notes, the 'F4 owns this' comments retargeted at plan 2"
```

---

## Self-review (run against the spec, fixed inline)

- **Spec coverage:** §2 flip → T2 (+ the tests' sign flips); §3 ViewTransform → T1, T3, T4, T5; §4 camera → T6, T7; §5.1 → T9; §5.2 → T10; §6 → T8; §8 → T11; §9 tests/goldens/witness/desk → per task + T12; §10/§12 nothing to build; §13 R1–R9 all honoured (R8/R9 are plan 2). The `MeshNode` "exposes its depth as a graph resource" item is satisfied by `AddMeshNode`'s existing return value, consumed by T10 — no MeshNode change needed beyond none.
- **Placeholders:** none; every step names code or the exact analog to copy (`MeshNode` for the node skeleton, `EditorApp.cpp:130-196` for the ini handler, `MintOrReuseSpriteForTexture` for the primitive mint).
- **Type consistency:** `ViewTransform::Orthographic(center, halfHeight, viewport)` (T1) is what `EditorCamera::Resolve` (T6) and `ActiveSceneCamera` (T3) call; `Affine2D` (T1) is what `PickView`/`GizmoView`/`PhysicsDebugDrawOptions` (T3) carry; `Batcher2D::QuadWorld/CircleWorld/SetViewProjection` (T4) are what `RenderSubmissionSystem` (T5) and the hosts call; `SpriteWorldQuad` (T5) is what framing (T6) uses; `GridSceneDesc` (T10) is what the editor arms; `ViewportSettings` (T7) is what T8's dropdown edits and T10 reads.
- **Order of risk:** T2 lands the flip first and alone (spec §14), gate green on `~[gpu]` before T3; goldens are knowingly red from T2 to T12.
