# F3 Plan 1 — world bounds, the frustum, the CPU visible set, and the GPU scene (indirect draws, no cull yet)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every world drawable carries an engine-written `WorldBounds`; a `Frustum` comes off `ViewTransform`; a per-view CPU `VisibleSet` culls sprites, pickables and — through the new persistent GPU instance scene — meshes, which the rewritten `MeshNode` draws with one `CmdDrawIndexedIndirect` per (mesh, section) batch, the visible-index list and the indirect arguments written by the CPU this plan (the compute cull replaces that writer in plan 2).

**Architecture:** Core gains `Aabb`, `Frustum`, `WorldBounds` + `BoundsSystem` (fixedUpdate, after `TransformPropagationSystem`; the editor's Edit-mode schedule too). ArcaneClient gains `VisibleSet`/`SceneVisibility` (a registry resource the host fills between the fixed and render schedulers), `GpuSceneMirror` (a registry resource: entity → row span, per-row `lastModel` history, the re-dirty list, a generation stamp), the device-free `GpuSceneSync` + `BuildGpuSceneFrame` (dirty rows → staged `GpuInstance`s; batches ordered opaque-first nearest-first; CPU-written `visibleIndices` + `DrawIndexedArgs`), and the device-side `GpuScene` (one persistent 240-byte-row instance buffer, per-frame-slot `visibleIndices` and `args` buffers, a scratch row region for registry-less callers). `GpuSceneSyncNode` copies the staged bytes through the upload ring; `MeshNode` binds the instance buffer + the slot's index buffer as structured SRVs, pushes an 8-byte root block `{firstOutput, flags}` and draws indirect per batch, or direct by row for ad-hoc instances (previews, thumbnails, tests). The 128-byte `MeshConstants` root block and `CollectMeshInstances` are retired.

**Tech Stack:** C++23, glm (`gtc/matrix_access`), Astra (vendored; `Changed<T>`, `AstraChangeTracked`, `FlatMap`), NRI render graph (D3D12 + Vulkan; dxc offline via `data/shaders/compile-shaders.bat`), Catch2 + rapidcheck (ArcaneTests), premake5 → `Arcane.slnx`, PowerShell (`scripts/golden-gate.ps1`, `scripts/check-baselines.ps1`).

**Spec:** `docs/specs/2026-09-18-f3-visibility-and-gpu-scene-design.md` — §2 (bounds), §3 (frustum), §4 (visible set), §5 (the GPU scene; §5.3 as amended by the UE vet: CPU `lastModel` history + re-dirty, no compute in Sync; §5.6 the draw), §8 (graph), §9 (instrument), §12 plan 1, §14 rulings R1–R8, §15 costs. Plan 2 owns `MeshCullNode`, the three blend modes, the transparent/masked order, the witness `visibility{}` block's GPU figure, the stats overlay and the declared slots' prose.

## Global Constraints

- **One right-handed world, +Y up, camera forward −Z, clip depth [0,1] forward-Z** (F4 spec §2; `ViewTransform.hpp`). Units are metres (MKS).
- **The culling frustum is always extracted from the UNJITTERED `ViewTransform`** (spec §3) and **widened by `kVisibilitySlack = 0.25f` metres** on the CPU test (spec §4; plan 2's GPU test uses the same widened planes). `Frustum::Contains` is conservative by contract: a box that intersects the frustum is never rejected.
- **No `SceneVisibility` resource ⇒ nothing is culled.** Every consumer (the sprite sweep, `PickEmit`, the batch builder) treats an absent resource as "draw everything" so every existing test and the device-less hosts keep their behaviour.
- **`WorldBounds` is engine-written, never authored, never serialized** (`Serializable(false)` + `Hidden` on its one field — `WorldTransform`'s exact reflection shape, `Components.hpp:311-316`). `GpuSceneMirror` and `SceneVisibility` are transient registry resources: never serialized, rebuilt on any registry swap.
- **`GpuInstance` is 240 bytes** and `static_assert`ed against that number in C++; `data/shaders/gpu_scene.hlsli` carries the same field order. `DrawIndexedArgs` is `static_assert`ed to `sizeof(nri::DrawIndexedDesc) == 20` and field-for-field equal where NRI is visible (`GpuScene.cpp`).
- **Plugin ABI bumps to 36** (`PluginABI.hpp` `kGamePluginABIVersion`, a v36 changelog line in the v-list) and `ReferenceProject/ReferenceProject.arcproj` restamps `"abi": 36` in the same commit (Task 2). Aphelyon's module (`D:\dev\starworks\Aphelyon`) is NOT touched; it rebuilds against the new header before its editor loads it (say so in the close report).
- **`RegisterSceneComponents` APPENDS** (`SceneModule.hpp:27-31`): `WorldBounds` goes after `PhysicsSettings`.
- **Build:** `cd D:\dev\starworks\Arcane && msbuild Arcane.slnx /p:Configuration=Debug /m` (Release the same way before the close). Shaders compile in the prebuild via `data/shaders/compile-shaders.bat`. **`ARCANE_SDK` may be stale in the process** — build `Arcane.slnx`, never a bare `.vcxproj`. A header-only change can survive an incremental build with stale test objects — if a test runs old code, delete the test `.obj`s. After a Release gate, `/t:Rebuild` before any Debug run (the single-slot ReferenceProject trap).
- **Tests run FROM THE EXE DIRECTORY:** `cd bin\Debug-windows-x86_64-md\ArcaneTests && .\ArcaneTests.exe "<tag>"`. Random order; on a failure capture the seed. `[gpu]` cases need the desk GPU (run them). `~[gpu]` is the baseline-comparable invocation (`scripts/automation-baselines.json`: 58530 assertions / 1844 cases Debug at F4's close), re-booked at this plan's close by `scripts/check-baselines.ps1 -ReportPath <a Catch2 JSON from ArcaneTests.exe "~[gpu]" -r json::out=<file>>` (it does NOT run tests and hangs on a prompt without `-ReportPath`).
- **Goldens:** `scripts/golden-gate.ps1 -Configuration Debug` at the close BEFORE blessing anything. Expected: all six lanes UNCHANGED — nothing in ReferenceProject's scenes is off-screen, and the mesh shader's arithmetic is unchanged (the normal matrix moves from the root block to the row; `NormalMatrixFor` is still the producer). If a lane diffs, name the pixel cause; a re-bless goes into the STAGED slot then is COPIED TO SOURCE IMMEDIATELY (`ReferenceProject/Verify/References/`), gate both configs, end on Debug. Delete the exe-dir `imgui.ini` before a golden run. `golden-gate.ps1 -SelfTest` refuses a dirty tree.
- **Four hosts stage `ReferenceGame.dll`** (Editor, Runtime, Server, the slot): a stale copy fails the ServerWitness cases as "module-load-failed" — rebuild `Arcane.slnx` whole.
- **Process:** implementers COMMIT before writing their report; never terminate an editor you did not launch; FOREGROUND suite runs only; never `git add -A` (untracked build dirs + `out.txt` are the user's). Commit after every task, message style `feat(scope): ...` / `test(scope): ...`, ending with the session's attribution trailer.
- **Scope guard (spec §13):** no compute cull, no blend modes, no transparent/masked path, no Hi-Z, no spatial index, no sprites in the GPU scene, no stats overlay, no cvars (plan 2 or later). The `Nri/nodes/` reorg trigger is re-counted at the close (this plan brings the tree to eight pass types), not pulled.

**Two deliberate cross-task seams:** Task 6 (the device-side `GpuScene` + `GpuSceneSyncNode`) and Task 7 (the `MeshNode` rewrite + shader) change `MeshSceneDesc` and `AddMeshNode`'s shape; Task 6's commit builds (the new node is declared but `MeshNode` still draws the old way from `MeshSceneDesc::instances`), Task 7's commit switches the draw. Task 8 (the hosts) is where a registry-backed scene first reaches the screen; between Task 7 and Task 8 the hosts draw nothing for `MeshRenderer` entities (they still call the retired sweep's replacement with an empty frame) — execute 7 and 8 in the same session, no golden gate between them.

---

## File structure

| Path | Responsibility |
|---|---|
| `ArcaneCore/src/Arcane/Math/Aabb.hpp` (new) | `Aabb` — aggregate `{min,max}`, `Empty/IsEmpty/FromPoints/Center/Extent/Union/Transformed/Widened` |
| `ArcaneCore/src/Arcane/Mesh/MeshBuilder.hpp` | `using MeshBounds = Aabb;` (the struct at :110 deleted) |
| `ArcaneCore/src/Arcane/Scene/Frustum.hpp` (new) | `Plane`, `Frustum` — `FromViewProjection` (Gribb–Hartmann rows), `From(ViewTransform)`, `Widened`, `Contains(Aabb)` (p-vertex / push-out) |
| `ArcaneCore/src/Arcane/Scene/SpriteGeometry.hpp` (moved from `ArcaneClient/src/Arcane/Render/`) | `SpriteQuad`, `SpriteWorldQuad` — glm-only, now reachable from Core; the old path is a one-line shim |
| `ArcaneCore/src/Arcane/Scene/Components.hpp` | `WorldBounds { Aabb box; }` + reflection (hidden, unserialized); `WorldTransform`, `MeshRenderer` gain `AstraChangeTracked = true` |
| `ArcaneCore/src/Arcane/Scene/BoundsSystem.hpp` (new) | `BoundsSystem` (`After<TransformPropagationSystem>`): mesh box = `MeshEntry::bounds` through the world matrix; sprite box = `SpriteWorldQuad` corners widened by `kSpriteDepthEpsilon`; unresolved mesh ⇒ no component; removal reconciliation |
| `ArcaneCore/src/Arcane/Scene/SceneModule.hpp` | `RegisterComponent<WorldBounds>()` appended |
| `ArcaneCore/src/Arcane/Base/Runtime.cpp` | `InstallEngineSystems` adds `BoundsSystem` to `fixedUpdate` |
| `ArcaneEditor/src/Scene/EditModeSchedule.cpp` | adds `BoundsSystem` after `TransformPropagationSystem` |
| `ArcaneCore/src/Arcane/Plugin/PluginABI.hpp`, `ReferenceProject/ReferenceProject.arcproj` | ABI 36 |
| `ArcaneClient/src/Arcane/Render/VisibilitySystem.hpp` (new) | `kVisibilitySlack`, `VisibleEntry`, `VisibleSet`, `SceneVisibility`, `BuildVisibleSet(reg, view, out)` |
| `ArcaneClient/src/Arcane/Render/RenderSystems.hpp` | the sprite sweep skips non-members |
| `ArcaneClient/src/Arcane/Render/PickEmit.cpp` | sprite and mesh walks emit members only |
| `ArcaneEditor/src/Viewport/EditorCamera.cpp` | `SelectionFramingBounds` / `SceneFramingBounds` read `WorldBounds` |
| `ArcaneClient/src/Arcane/Render/GpuSceneTypes.hpp` (new) | `GpuInstance` (240 B), `GpuBatchKey`, `GpuSceneMirror` (+ `RowSpanAllocator`), `DrawIndexedArgs`, `GpuBatchDraw`, `GpuSceneStage`, `GpuSceneFrame` — glm/Guid/Astra only, no NRI |
| `ArcaneClient/src/Arcane/Render/GpuSceneSync.hpp` (new) | header-only `GpuSceneSync(reg, mirror, deviceGeneration, stage)` and `BuildGpuSceneFrame(reg, mirror, visible, frame)` |
| `ArcaneClient/src/Arcane/Render/MeshSubmissionSystem.hpp` | DELETED (`CollectMeshInstances`); its material-chain tests move to `GpuSceneSyncTest.cpp` |
| `ArcaneClient/src/Arcane/Render/Nri/RenderGraph.{hpp,cpp}` | `RgUsage::IndirectArgs` → `{ARGUMENT_BUFFER, UNDEFINED, INDIRECT}` |
| `ArcaneClient/src/Arcane/Render/Nri/GpuScene.{hpp,cpp}` (new) | device side: the instance buffer (DEVICE, doubling growth, buried on grow), per-slot `visibleIndices`/`args` buffers, scratch rows, structured views, `Apply(frame, adHoc, slot, nodeContext)` |
| `ArcaneClient/src/Arcane/Render/Nri/nodes/GpuSceneSyncNode.{hpp,cpp}` (new) | `AddGpuSceneSyncNode(graph, context, frame, adHoc)` — the ring copies |
| `ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.{hpp,cpp}` | `MeshInstance` keeps only the ad-hoc role; `MeshSceneDesc` gains `const GpuSceneFrame* scene`; root block 8 B; space1 = `{b1, t0 instances, t1 visibleIndices}`; `Prepare` resolves batch meshes + stages ad-hoc rows; `Record` = indirect batches then direct ad-hoc rows |
| `data/shaders/gpu_scene.hlsli` (new), `data/shaders/mesh.hlsl`, `data/shaders/compile-shaders.bat`, `ArcaneClient/src/Arcane/Render/ShaderConventions.hpp` | the row struct; VS reads the row by `SV_InstanceID`; `-fvk-t-shift 0 1` |
| `ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.{hpp,cpp}` | owns `GpuScene`; `DeclareGraphFrame` declares sync → mesh |
| `ArcaneEditor/src/App/EditorAppFrame.cpp`, `EditorApp.hpp`, `ArcaneRuntime/src/RuntimeFrame.{cpp,hpp}`, `RuntimeApp.hpp` | build `SceneVisibility`, run `GpuSceneSync` + `BuildGpuSceneFrame`, hand `vp.mesh->scene` |
| `ArcaneClient/src/Arcane/Host/VerifyReport.{hpp,cpp}` | `SetVisibility(total, coarseVisible, batches, draws)` → `j["visibility"]` |
| `ArcaneTests/src/AabbTest.cpp`, `FrustumTest.cpp`, `BoundsSystemTest.cpp`, `VisibilityTest.cpp`, `GpuSceneSyncTest.cpp` (new); `RenderGraphTest.cpp`, `NriGraphPixelTest.cpp`, `EditorCameraTest.cpp`, `PickBufferTest.cpp` (modified); `MeshSubmissionTest.cpp` (the `CollectMeshInstances` cases move) | the pins |
| `docs/specs/2026-09-18-f3-visibility-and-gpu-scene-design.md`, `CLAUDE.md` | Status line; the GPU scene stated |

---

### Task 1: `Aabb` and `Frustum` (Core, pure)

**Files:**
- Create: `ArcaneCore/src/Arcane/Math/Aabb.hpp`
- Modify: `ArcaneCore/src/Arcane/Mesh/MeshBuilder.hpp:100-116` (`MeshBounds` becomes an alias)
- Create: `ArcaneCore/src/Arcane/Scene/Frustum.hpp`
- Test: `ArcaneTests/src/AabbTest.cpp`, `ArcaneTests/src/FrustumTest.cpp`

**Interfaces:**
- Produces: `Arcane::Aabb { glm::vec3 min, max; static Aabb Empty(); bool IsEmpty() const; static Aabb FromPoints(std::span<const glm::vec3>); glm::vec3 Center() const; glm::vec3 Extent() const; Aabb Union(const Aabb&) const; Aabb Transformed(const glm::mat4&) const; Aabb Widened(float) const; }` (an aggregate — `MeshBounds{min, max}` brace-inits keep compiling).
- Produces: `Arcane::Plane { glm::vec3 n; float d; float Distance(glm::vec3) const; }`, `Arcane::Frustum { std::array<Plane,6> planes; static Frustum FromViewProjection(const glm::mat4&); static Frustum From(const ViewTransform&); Frustum Widened(float) const; bool Contains(const Aabb&) const; }`.

- [ ] **Step 1: Write the failing tests**

`ArcaneTests/src/AabbTest.cpp`:

```cpp
#include <Arcane/Math/Aabb.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <catch2/catch_test_macros.hpp>
#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include <array>

TEST_CASE("Aabb: Empty is the fold sentinel and unions away", "[aabb]")
{
    const Arcane::Aabb e = Arcane::Aabb::Empty();
    CHECK(e.IsEmpty());
    const Arcane::Aabb b{ glm::vec3(-1.0f), glm::vec3(2.0f) };
    CHECK_FALSE(b.IsEmpty());
    const Arcane::Aabb u = e.Union(b);
    CHECK(u.min == b.min);
    CHECK(u.max == b.max);
}

TEST_CASE("Aabb: FromPoints, Center, Extent, Widened", "[aabb]")
{
    const std::array<glm::vec3, 3> pts{ glm::vec3(1, 2, 3), glm::vec3(-1, 0, 5), glm::vec3(0, 4, -2) };
    const Arcane::Aabb b = Arcane::Aabb::FromPoints(pts);
    CHECK(b.min == glm::vec3(-1, 0, -2));
    CHECK(b.max == glm::vec3(1, 4, 5));
    CHECK(b.Center() == glm::vec3(0, 2, 1.5f));
    CHECK(b.Extent() == glm::vec3(1, 2, 3.5f));
    const Arcane::Aabb w = b.Widened(0.5f);
    CHECK(w.min == glm::vec3(-1.5f, -0.5f, -2.5f));
    CHECK(w.max == glm::vec3(1.5f, 4.5f, 5.5f));
}

TEST_CASE("Aabb: Transformed is the box of the eight transformed corners", "[aabb]")
{
    const Arcane::Aabb unit{ glm::vec3(-0.5f), glm::vec3(0.5f) };
    // 45 degrees about Z: the unit cube's XY footprint grows to sqrt(2)/2 a side.
    const glm::mat4 m = glm::rotate(glm::translate(glm::mat4(1.0f), glm::vec3(10, 0, 0)),
                                    glm::radians(45.0f), glm::vec3(0, 0, 1));
    const Arcane::Aabb t = unit.Transformed(m);
    const float h = std::sqrt(2.0f) * 0.5f;
    CHECK(t.min.x == Catch::Approx(10.0f - h));
    CHECK(t.max.x == Catch::Approx(10.0f + h));
    CHECK(t.min.y == Catch::Approx(-h));
    CHECK(t.max.y == Catch::Approx(h));
    CHECK(t.min.z == Catch::Approx(-0.5f));
    CHECK(t.max.z == Catch::Approx(0.5f));
}

TEST_CASE("Aabb: Transformed contains every transformed corner (property)", "[aabb]")
{
    rc::prop("conservative under a random affine", [] {
        const glm::vec3 lo = glm::vec3(*rc::gen::inRange(-50, 50), *rc::gen::inRange(-50, 50), *rc::gen::inRange(-50, 50));
        const glm::vec3 ext = glm::vec3(*rc::gen::inRange(0, 20), *rc::gen::inRange(0, 20), *rc::gen::inRange(0, 20));
        const Arcane::Aabb box{ lo, lo + ext };
        const glm::quat q = glm::normalize(glm::quat(
            float(*rc::gen::inRange(-100, 100)), float(*rc::gen::inRange(-100, 100)),
            float(*rc::gen::inRange(-100, 100)), float(*rc::gen::inRange(-100, 100)) + 0.01f));
        glm::mat4 m = glm::mat4_cast(q);
        m[0] *= float(*rc::gen::inRange(1, 5)); m[1] *= float(*rc::gen::inRange(1, 5)); m[2] *= float(*rc::gen::inRange(1, 5));
        m[3] = glm::vec4(float(*rc::gen::inRange(-100, 100)), float(*rc::gen::inRange(-100, 100)), float(*rc::gen::inRange(-100, 100)), 1.0f);
        const Arcane::Aabb t = box.Transformed(m);
        for (int i = 0; i < 8; ++i)
        {
            const glm::vec3 c((i & 1) ? box.max.x : box.min.x, (i & 2) ? box.max.y : box.min.y, (i & 4) ? box.max.z : box.min.z);
            const glm::vec3 w = glm::vec3(m * glm::vec4(c, 1.0f));
            const float eps = 1e-3f;
            RC_ASSERT(w.x >= t.min.x - eps && w.x <= t.max.x + eps);
            RC_ASSERT(w.y >= t.min.y - eps && w.y <= t.max.y + eps);
            RC_ASSERT(w.z >= t.min.z - eps && w.z <= t.max.z + eps);
        }
    });
}
```

`ArcaneTests/src/FrustumTest.cpp`:

```cpp
#include <Arcane/Scene/Frustum.hpp>
#include <Arcane/Scene/ViewTransform.hpp>

#include <catch2/catch_test_macros.hpp>
#include <rapidcheck.h>
#include <rapidcheck/catch.h>

namespace
{
    Arcane::ViewTransform PerspView()
    {
        return Arcane::ViewTransform::Perspective(glm::vec3(0, 0, 10), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0),
                                                  60.0f, glm::uvec2{ 800, 600 }, 0.1f, 100.0f);
    }
    Arcane::ViewTransform OrthoView()
    {
        return Arcane::ViewTransform::Orthographic(glm::vec2(0.0f), 5.0f, glm::uvec2{ 800, 600 });
    }
}

TEST_CASE("Frustum: every plane is unit-length and the eye's look point is inside (perspective)", "[frustum]")
{
    const Arcane::Frustum f = Arcane::Frustum::From(PerspView());
    for (const Arcane::Plane& p : f.planes)
        CHECK(glm::length(p.n) == Catch::Approx(1.0f).margin(1e-4f));
    const Arcane::Aabb origin{ glm::vec3(-0.1f), glm::vec3(0.1f) };
    CHECK(f.Contains(origin));
}

TEST_CASE("Frustum: boxes behind the eye, beyond far, and far off to the side are rejected", "[frustum]")
{
    const Arcane::Frustum f = Arcane::Frustum::From(PerspView());
    CHECK_FALSE(f.Contains(Arcane::Aabb{ glm::vec3(-1, -1, 20), glm::vec3(1, 1, 22) }));     // behind the eye (eye at z=10, looking -Z)
    CHECK_FALSE(f.Contains(Arcane::Aabb{ glm::vec3(-1, -1, -200), glm::vec3(1, 1, -198) }));  // beyond far
    CHECK_FALSE(f.Contains(Arcane::Aabb{ glm::vec3(500, -1, -1), glm::vec3(502, 1, 1) }));    // far right
    CHECK(f.Contains(Arcane::Aabb{ glm::vec3(-1, -1, -50), glm::vec3(1, 1, -48) }));          // ahead, inside far
}

TEST_CASE("Frustum: the orthographic case needs no branch", "[frustum]")
{
    const Arcane::Frustum f = Arcane::Frustum::From(OrthoView());   // half-height 5 m, aspect 4:3 -> half-width 6.667
    CHECK(f.Contains(Arcane::Aabb{ glm::vec3(-1, -1, -1), glm::vec3(1, 1, 1) }));
    CHECK(f.Contains(Arcane::Aabb{ glm::vec3(6.0f, 0, 0), glm::vec3(8.0f, 1, 1) }));       // straddles the right edge
    CHECK_FALSE(f.Contains(Arcane::Aabb{ glm::vec3(7.0f, 0, 0), glm::vec3(8.0f, 1, 1) })); // wholly outside right
    CHECK_FALSE(f.Contains(Arcane::Aabb{ glm::vec3(0, 6.0f, 0), glm::vec3(1, 7.0f, 1) }));  // wholly above
}

TEST_CASE("Frustum: Widened pushes every plane outward by the slack", "[frustum]")
{
    const Arcane::Frustum f = Arcane::Frustum::From(OrthoView());
    const Arcane::Aabb justOutside{ glm::vec3(6.7f, 0, 0), glm::vec3(6.8f, 1, 1) };
    CHECK_FALSE(f.Contains(justOutside));
    CHECK(f.Widened(0.25f).Contains(justOutside));
}

TEST_CASE("Frustum: Contains is conservative -- a box holding an inside point is never rejected (property)", "[frustum]")
{
    const Arcane::Frustum f = Arcane::Frustum::From(PerspView());
    rc::prop("no false negatives", [&] {
        // A point inside the frustum, then a random box around it.
        const float z = -float(*rc::gen::inRange(1, 80));               // ahead of the eye (z=10), inside far
        const float halfW = std::tan(glm::radians(30.0f)) * (10.0f - z) * (800.0f / 600.0f);
        const float halfH = std::tan(glm::radians(30.0f)) * (10.0f - z);
        const glm::vec3 p(halfW * float(*rc::gen::inRange(-90, 90)) / 100.0f,
                          halfH * float(*rc::gen::inRange(-90, 90)) / 100.0f, z);
        const glm::vec3 ext(float(*rc::gen::inRange(0, 30)), float(*rc::gen::inRange(0, 30)), float(*rc::gen::inRange(0, 30)));
        const glm::vec3 off(float(*rc::gen::inRange(0, 100)) / 100.0f, float(*rc::gen::inRange(0, 100)) / 100.0f, float(*rc::gen::inRange(0, 100)) / 100.0f);
        const Arcane::Aabb box{ p - ext * off, p + ext * (glm::vec3(1.0f) - off) };
        RC_ASSERT(f.Contains(box));
    });
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `msbuild Arcane.slnx /p:Configuration=Debug /m` — expected: compile errors, `Arcane/Math/Aabb.hpp` and `Arcane/Scene/Frustum.hpp` not found.

- [ ] **Step 3: Write `Aabb.hpp`**

```cpp
#pragma once

// Aabb -- the ONE axis-aligned box (F3, spec s2.1). Header-only, glm-only: no
// engine dependency, so Core's mesh code, the scene's WorldBounds, the
// editor's framing and the render layer's culling all read one type.
//
// An AGGREGATE on purpose: `MeshBounds` (MeshBuilder.hpp) is an alias of this
// struct and its brace-inits `{min, max}` keep compiling. The default-
// constructed ZERO box is the "framing an empty mesh" answer ComputeMeshBounds
// documents; Empty() is the fold sentinel (+inf/-inf) a Union chain starts
// from and is never stored on a component.
#include <glm/glm.hpp>

#include <limits>
#include <span>

namespace Arcane
{
    struct Aabb
    {
        glm::vec3 min{0.0f, 0.0f, 0.0f};
        glm::vec3 max{0.0f, 0.0f, 0.0f};

        [[nodiscard]] static Aabb Empty() noexcept
        {
            constexpr float inf = std::numeric_limits<float>::infinity();
            return Aabb{ glm::vec3(inf), glm::vec3(-inf) };
        }

        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return min.x > max.x || min.y > max.y || min.z > max.z;
        }

        [[nodiscard]] static Aabb FromPoints(std::span<const glm::vec3> points) noexcept
        {
            Aabb b = Empty();
            for (const glm::vec3& p : points)
            {
                b.min = glm::min(b.min, p);
                b.max = glm::max(b.max, p);
            }
            return b;
        }

        [[nodiscard]] glm::vec3 Center() const noexcept { return (min + max) * 0.5f; }
        [[nodiscard]] glm::vec3 Extent() const noexcept { return (max - min) * 0.5f; }   // half-size

        [[nodiscard]] Aabb Union(const Aabb& o) const noexcept
        {
            if (IsEmpty()) return o;
            if (o.IsEmpty()) return *this;
            return Aabb{ glm::min(min, o.min), glm::max(max, o.max) };
        }

        // The box of the eight transformed corners: conservative for any
        // affine (rotation grows it; the true rotated box is smaller).
        [[nodiscard]] Aabb Transformed(const glm::mat4& m) const noexcept
        {
            Aabb b = Empty();
            for (int i = 0; i < 8; ++i)
            {
                const glm::vec3 c((i & 1) ? max.x : min.x,
                                  (i & 2) ? max.y : min.y,
                                  (i & 4) ? max.z : min.z);
                const glm::vec3 w = glm::vec3(m * glm::vec4(c, 1.0f));
                b.min = glm::min(b.min, w);
                b.max = glm::max(b.max, w);
            }
            return b;
        }

        [[nodiscard]] Aabb Widened(float epsilon) const noexcept
        {
            return Aabb{ min - glm::vec3(epsilon), max + glm::vec3(epsilon) };
        }

        [[nodiscard]] bool operator==(const Aabb&) const noexcept = default;
    };
}
```

- [ ] **Step 4: `MeshBounds` becomes the alias**

In `ArcaneCore/src/Arcane/Mesh/MeshBuilder.hpp`, add `#include <Arcane/Math/Aabb.hpp>` beside the glm include and replace the struct at :110-114 with:

```cpp
    // The mesh's local AABB (F3, spec s2.1): ONE box type for the whole tree.
    // Keeps the name every mesh caller uses; see Aabb.hpp for the zero-box /
    // Empty() distinction the comment above describes.
    using MeshBounds = Aabb;
```

Keep the comment block above it (lines 100-109) — it still describes `ComputeMeshBounds`'s zero-box answer.

- [ ] **Step 5: Write `Frustum.hpp`**

```cpp
#pragma once

// Frustum -- six planes off a ViewTransform (F3, spec s3). Gribb & Hartmann
// (2001): with glm's column-major `clip = M * p`, the planes are sums and
// differences of M's ROWS; in [0,1] forward-Z the near plane is row 2 alone.
// The orthographic case needs no branch -- the same rows of orthoRH_ZO yield
// six planes. Every plane is normalised so `Distance` is metres.
//
// Contains() is CONSERVATIVE BY CONTRACT (spec s3): the centre/extent
// "push-out" test (UE's FConvexVolume::IntersectBox, ConvexVolume.cpp:283-289;
// algebraically the p-vertex test) has false positives at corners and never a
// false negative -- a box that touches the frustum is never rejected.
// FrustumTest.cpp pins the property with rapidcheck.
//
// THE JITTER RULE: extract from the UNJITTERED view. F5's TAA jitter must not
// flip a culling decision frame to frame.
#include <Arcane/Math/Aabb.hpp>
#include <Arcane/Scene/ViewTransform.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_access.hpp>

#include <array>
#include <cmath>

namespace Arcane
{
    struct Plane
    {
        glm::vec3 n{0.0f, 0.0f, 1.0f};
        float     d = 0.0f;   // n . p + d >= 0 is INSIDE

        [[nodiscard]] float Distance(const glm::vec3& p) const noexcept { return glm::dot(n, p) + d; }
    };

    struct Frustum
    {
        // L, R, B, T, N, F
        std::array<Plane, 6> planes{};

        [[nodiscard]] static Frustum FromViewProjection(const glm::mat4& vp) noexcept
        {
            const glm::vec4 r0 = glm::row(vp, 0);
            const glm::vec4 r1 = glm::row(vp, 1);
            const glm::vec4 r2 = glm::row(vp, 2);
            const glm::vec4 r3 = glm::row(vp, 3);
            const glm::vec4 rows[6] = { r3 + r0, r3 - r0, r3 + r1, r3 - r1, r2, r3 - r2 };
            Frustum f;
            for (int i = 0; i < 6; ++i)
            {
                const glm::vec3 n(rows[i]);
                const float len = glm::length(n);
                const float inv = len > 0.0f ? 1.0f / len : 0.0f;
                f.planes[static_cast<std::size_t>(i)] = Plane{ n * inv, rows[i].w * inv };
            }
            return f;
        }

        [[nodiscard]] static Frustum From(const ViewTransform& view) noexcept
        {
            return FromViewProjection(view.ViewProjection());
        }

        // Every plane pushed OUTWARD by `slack` metres: a bigger frustum.
        [[nodiscard]] Frustum Widened(float slack) const noexcept
        {
            Frustum f = *this;
            for (Plane& p : f.planes)
                p.d += slack;
            return f;
        }

        [[nodiscard]] bool Contains(const Aabb& box) const noexcept
        {
            const glm::vec3 c = box.Center();
            const glm::vec3 e = box.Extent();
            for (const Plane& p : planes)
            {
                const float r = e.x * std::fabs(p.n.x) + e.y * std::fabs(p.n.y) + e.z * std::fabs(p.n.z);
                if (p.Distance(c) + r < 0.0f)
                    return false;
            }
            return true;
        }
    };
}
```

- [ ] **Step 6: Build and run**

Run: `msbuild Arcane.slnx /p:Configuration=Debug /m`, then from `bin\Debug-windows-x86_64-md\ArcaneTests`: `.\ArcaneTests.exe "[aabb],[frustum]"`
Expected: all cases PASS. Then `.\ArcaneTests.exe "[mesh]"` — the `MeshBounds` alias must leave every existing mesh case green.

- [ ] **Step 7: Commit**

```bash
git add ArcaneCore/src/Arcane/Math/Aabb.hpp ArcaneCore/src/Arcane/Scene/Frustum.hpp ArcaneCore/src/Arcane/Mesh/MeshBuilder.hpp ArcaneTests/src/AabbTest.cpp ArcaneTests/src/FrustumTest.cpp
git commit -m "feat(core): Aabb (the one box; MeshBounds is its alias) and Frustum off ViewTransform -- Gribb-Hartmann rows, conservative push-out Contains, Widened slack; rapidcheck pins conservativeness (F3 plan 1 T1)"
```

---
### Task 2: `WorldBounds` + `BoundsSystem` (Core), `SpriteGeometry` moves to Core, ABI 36

**Files:**
- Move: `ArcaneClient/src/Arcane/Render/SpriteGeometry.hpp` → `ArcaneCore/src/Arcane/Scene/SpriteGeometry.hpp` (old path becomes a shim)
- Modify: `ArcaneCore/src/Arcane/Scene/Components.hpp` (`WorldTransform` :84, `MeshRenderer` :157, the reflection block :300-360)
- Create: `ArcaneCore/src/Arcane/Scene/BoundsSystem.hpp`
- Modify: `ArcaneCore/src/Arcane/Scene/SceneModule.hpp:21-31`, `ArcaneCore/src/Arcane/Base/Runtime.cpp:398-420`, `ArcaneEditor/src/Scene/EditModeSchedule.cpp:11-13`
- Modify: `ArcaneCore/src/Arcane/Plugin/PluginABI.hpp:900-904`, `ReferenceProject/ReferenceProject.arcproj:6`
- Test: `ArcaneTests/src/BoundsSystemTest.cpp`

**Interfaces:**
- Consumes: `Aabb` (Task 1); `SpriteWorldQuad(world, baseSizeMetres, pivot) -> SpriteQuad{corners[4]}`; `MeshEntry::bounds`, `MeshTable::Resolve`, `SpriteTable::Resolve` (`SceneResources.hpp`); `TransformPropagationSystem`.
- Produces: `struct WorldBounds { Aabb box; }` (component); `struct BoundsSystem : Astra::SystemTraits<Reads<WorldTransform, MeshRenderer, SpriteRenderer, Hidden>, Writes<WorldBounds>, After<TransformPropagationSystem>>` with `void operator()(Astra::Registry&)`; `inline constexpr float kSpriteDepthEpsilon = 0.001f;`.

- [ ] **Step 1: Move `SpriteGeometry.hpp`**

```bash
git mv ArcaneClient/src/Arcane/Render/SpriteGeometry.hpp ArcaneCore/src/Arcane/Scene/SpriteGeometry.hpp
```

Then create the shim at the old path `ArcaneClient/src/Arcane/Render/SpriteGeometry.hpp`:

```cpp
#pragma once
// Moved to Core (F3 plan 1 T2): BoundsSystem needs the sprite corner rule and
// Core cannot include ArcaneClient. Every Client/Editor/Tests include keeps
// working through this line.
#include <Arcane/Scene/SpriteGeometry.hpp>
```

- [ ] **Step 2: Write the failing test**

`ArcaneTests/src/BoundsSystemTest.cpp`:

```cpp
#include <Arcane/Scene/BoundsSystem.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/TransformSystems.hpp>
#include <Arcane/Mesh/MeshBuilder.hpp>

#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Registry/Registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <unordered_map>

namespace
{
    struct World
    {
        std::shared_ptr<Astra::ComponentRegistry> components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{ components };
        std::unordered_map<Arcane::Guid, Arcane::MeshEntry>   meshes;
        std::unordered_map<Arcane::Guid, Arcane::SpriteEntry> sprites;
        Astra::Entity root{};

        World()
        {
            Arcane::RegisterSceneComponents(reg);
            root = reg.CreateEntity();
            reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});
            reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{ root });
            reg.SetResource<Arcane::MeshTable>(Arcane::MeshTable{ &meshes });
            reg.SetResource<Arcane::SpriteTable>(Arcane::SpriteTable{ &sprites });
        }

        Astra::Entity Spawn(glm::vec3 pos)
        {
            Astra::Entity e = reg.CreateEntity();
            Arcane::Transform t; t.position = pos;
            reg.AddComponent<Arcane::Transform>(e, t);
            reg.SetParent(e, root);
            return e;
        }

        void Tick()
        {
            Arcane::TransformPropagationSystem{}(reg);
            Arcane::BoundsSystem{}(reg);
        }
    };

    Arcane::Guid AddCube(World& w)
    {
        const Arcane::Guid id = Arcane::Guid::Generate();
        Arcane::MeshEntry entry;
        entry.data   = Arcane::BuildCube(2.0f);              // local box [-1, 1]^3
        entry.bounds = Arcane::ComputeMeshBounds(entry.data);
        w.meshes.emplace(id, entry);
        return id;
    }
}

TEST_CASE("BoundsSystem: a resolved mesh gets its local box through the world matrix", "[bounds]")
{
    World w;
    const Arcane::Guid cube = AddCube(w);
    Astra::Entity e = w.Spawn(glm::vec3(10, 0, 0));
    w.reg.AddComponent<Arcane::MeshRenderer>(e, Arcane::MeshRenderer{ cube, {} });
    w.Tick();
    const Arcane::WorldBounds* b = std::as_const(w.reg).GetComponent<Arcane::WorldBounds>(e);
    REQUIRE(b);
    CHECK(b->box.min == glm::vec3(9, -1, -1));
    CHECK(b->box.max == glm::vec3(11, 1, 1));
}

TEST_CASE("BoundsSystem: an unresolved mesh gets NO WorldBounds", "[bounds]")
{
    World w;
    Astra::Entity e = w.Spawn(glm::vec3(0));
    w.reg.AddComponent<Arcane::MeshRenderer>(e, Arcane::MeshRenderer{ Arcane::Guid::Generate(), {} });
    w.Tick();
    CHECK_FALSE(w.reg.HasComponent<Arcane::WorldBounds>(e));
}

TEST_CASE("BoundsSystem: a sprite gets the SpriteWorldQuad box widened by the depth epsilon", "[bounds]")
{
    World w;
    Astra::Entity e = w.Spawn(glm::vec3(2, 3, 0));
    Arcane::SpriteRenderer s; s.shape = Arcane::SpriteShape::Rect;   // unresolved: 1x1 m, centre pivot
    w.reg.AddComponent<Arcane::SpriteRenderer>(e, s);
    w.Tick();
    const Arcane::WorldBounds* b = std::as_const(w.reg).GetComponent<Arcane::WorldBounds>(e);
    REQUIRE(b);
    CHECK(b->box.min.x == Catch::Approx(1.5f - Arcane::kSpriteDepthEpsilon));
    CHECK(b->box.max.x == Catch::Approx(2.5f + Arcane::kSpriteDepthEpsilon));
    CHECK(b->box.min.y == Catch::Approx(2.5f - Arcane::kSpriteDepthEpsilon));
    CHECK(b->box.max.y == Catch::Approx(3.5f + Arcane::kSpriteDepthEpsilon));
    CHECK(b->box.min.z == Catch::Approx(-Arcane::kSpriteDepthEpsilon));
    CHECK(b->box.max.z == Catch::Approx(+Arcane::kSpriteDepthEpsilon));
}

TEST_CASE("BoundsSystem: a bare transform node has no bounds; removing the renderer removes the box", "[bounds]")
{
    World w;
    const Arcane::Guid cube = AddCube(w);
    Astra::Entity bare = w.Spawn(glm::vec3(0));
    Astra::Entity mesh = w.Spawn(glm::vec3(0));
    w.reg.AddComponent<Arcane::MeshRenderer>(mesh, Arcane::MeshRenderer{ cube, {} });
    w.Tick();
    CHECK_FALSE(w.reg.HasComponent<Arcane::WorldBounds>(bare));
    REQUIRE(w.reg.HasComponent<Arcane::WorldBounds>(mesh));
    w.reg.RemoveComponent<Arcane::MeshRenderer>(mesh);
    w.Tick();
    CHECK_FALSE(w.reg.HasComponent<Arcane::WorldBounds>(mesh));
}

TEST_CASE("BoundsSystem: a moved entity's box follows; an untouched one is not rewritten", "[bounds]")
{
    World w;
    const Arcane::Guid cube = AddCube(w);
    Astra::Entity moving = w.Spawn(glm::vec3(0));
    Astra::Entity still  = w.Spawn(glm::vec3(5, 5, 5));
    w.reg.AddComponent<Arcane::MeshRenderer>(moving, Arcane::MeshRenderer{ cube, {} });
    w.reg.AddComponent<Arcane::MeshRenderer>(still,  Arcane::MeshRenderer{ cube, {} });
    w.Tick();
    const Astra::Tick after1 = w.reg.CurrentTick();
    w.reg.GetComponent<Arcane::Transform>(moving)->position = glm::vec3(3, 0, 0);
    w.Tick();
    CHECK(std::as_const(w.reg).GetComponent<Arcane::WorldBounds>(moving)->box.min == glm::vec3(2, -1, -1));
    CHECK(w.reg.IsChanged<Arcane::WorldBounds>(moving, after1));
    CHECK_FALSE(w.reg.IsChanged<Arcane::WorldBounds>(still, after1));
}

TEST_CASE("BoundsSystem: a Hidden entity keeps its box (Hidden is a draw decision, not a bounds one)", "[bounds]")
{
    World w;
    const Arcane::Guid cube = AddCube(w);
    Astra::Entity e = w.Spawn(glm::vec3(0));
    w.reg.AddComponent<Arcane::MeshRenderer>(e, Arcane::MeshRenderer{ cube, {} });
    w.reg.AddComponent<Arcane::Hidden>(e, Arcane::Hidden{});
    w.Tick();
    CHECK(w.reg.HasComponent<Arcane::WorldBounds>(e));
}
```

- [ ] **Step 3: Run to verify it fails**

Run: `msbuild Arcane.slnx /p:Configuration=Debug /m` — expected: `Arcane/Scene/BoundsSystem.hpp` not found.

- [ ] **Step 4: `WorldBounds` + change tracking in `Components.hpp`**

After `struct WorldTransform` (:84-87) add:

```cpp
    // The drawable's WORLD-space box (F3, spec s2.2): engine-written by
    // BoundsSystem after transform propagation, never authored, never
    // serialized. Mesh = the artifact AABB through the world matrix; sprite =
    // the SpriteWorldQuad corners, Z widened by kSpriteDepthEpsilon. An entity
    // with nothing drawable (or an unresolved mesh) carries none. Read by the
    // CPU visible set, the GPU scene rows, the editor's framing.
    struct WorldBounds
    {
        static constexpr bool AstraChangeTracked = true;   // exact per entity: the GPU scene re-uploads exactly the moved rows
        Aabb box;
    };
```

and `#include <Arcane/Math/Aabb.hpp>` with the file's includes. Give `WorldTransform` (:84) and `MeshRenderer` (:157) the line `static constexpr bool AstraChangeTracked = true;` as their first member, each with a one-line comment: `// F3: Changed<> is exact per entity -- BoundsSystem and GpuSceneSync re-walk only the rows that moved`. Update the `Transform` comment at :50-56 ("Nothing else is tracked") to name the three. (There are no non-const `CreateView<WorldTransform ...>` in the tree — verified 2026-09-18 — so no `Mut<T>` yield changes; re-grep before flipping `MeshRenderer`: `grep -rn "CreateView<[^>]*MeshRenderer" --include=*.hpp --include=*.cpp` must show only `const MeshRenderer`.)

In the reflection block, after `WorldTransform`'s (:311-316):

```cpp
    ASTRA_REFLECT_TYPE(WorldBounds)
        ASTRA_REFLECT_FIELD(WorldBounds, box)
            ASTRA_REFLECT_ATTR(Serializable, false)
            ASTRA_REFLECT_ATTR(Hidden)
    ASTRA_END_REFLECT_TYPE()
```

Check whether `Aabb` needs its own `ASTRA_REFLECT_TYPE` for the field to reflect (the `Guid` block at :322-325 is the precedent for a nested struct): if the build errors on an unreflected field type, add `ASTRA_REFLECT_TYPE(Aabb) ASTRA_REFLECT_FIELD(Aabb, min) ASTRA_REFLECT_FIELD(Aabb, max) ASTRA_END_REFLECT_TYPE()` above it.

- [ ] **Step 5: Write `BoundsSystem.hpp`**

```cpp
#pragma once

// BoundsSystem -- writes WorldBounds for every drawable (F3, spec s2.3).
// fixedUpdate, After<TransformPropagationSystem> (it reads the composed
// WorldTransform); the editor's EditModeSchedule runs the same pair.
//
// DIRTY-DRIVEN, the TransformSystems idiom: the rows visited are the union of
// Changed<WorldTransform> / Changed<MeshRenderer> / Changed<SpriteRenderer>
// since the last run (WorldTransform and MeshRenderer are change-tracked --
// exact per entity; SpriteRenderer is chunk-coarse, which only recomputes a
// few boxes), every drawable with no WorldBounds yet, and the removal
// reconciliation. A static scene does no work.
//
// Hidden is NOT consulted: it is a draw decision (the sweeps skip it), not a
// bounds one -- the editor frames what exists, and a hidden entity unhidden
// next frame must already have its box.
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/SpriteGeometry.hpp>
#include <Arcane/Scene/TransformSystems.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/System/System.hpp>

#include <glm/glm.hpp>

#include <vector>

namespace Arcane
{
    // A sprite is a zero-thickness quad; a zero-extent axis is a degenerate
    // frustum input, so the box gets a millimetre of Z.
    inline constexpr float kSpriteDepthEpsilon = 0.001f;

    // The last-run tick, a registry resource like TransformOrder (both hosts'
    // schedulers own a system instance; the registry is the shared lifetime).
    struct BoundsSystemState
    {
        Astra::Tick lastRun = 0;
        std::vector<Astra::Entity> scratch;
    };

    struct BoundsSystem
        : Astra::SystemTraits<Astra::Reads<WorldTransform, MeshRenderer, SpriteRenderer, Hidden>,
                              Astra::Writes<WorldBounds>,
                              Astra::After<TransformPropagationSystem>>
    {
        // The one rule for a drawable's local box; nullopt = not drawable.
        [[nodiscard]] static std::optional<Aabb> WorldBoxFor(const Astra::Registry& reg, Astra::Entity e,
                                                             const WorldTransform& world,
                                                             const MeshTable* meshes, const SpriteTable* sprites)
        {
            if (const MeshRenderer* mr = reg.GetComponent<MeshRenderer>(e))
            {
                const MeshEntry* entry = meshes ? meshes->Resolve(mr->mesh) : nullptr;
                if (entry)
                    return entry->bounds.Transformed(world.matrix);   // mesh beats sprite (the pick rule)
            }
            if (const SpriteRenderer* sr = reg.GetComponent<SpriteRenderer>(e))
            {
                const SpriteEntry* entry =
                    (sr->shape == SpriteShape::Rect && sprites) ? sprites->Resolve(sr->sprite) : nullptr;
                const SpriteQuad q = SpriteWorldQuad(world.matrix,
                                                     entry ? entry->sizeMeters : glm::vec2(1.0f),
                                                     entry ? entry->pivot      : glm::vec2(0.5f));
                return Aabb::FromPoints(q.corners).Widened(kSpriteDepthEpsilon);
            }
            return std::nullopt;
        }

        void operator()(Astra::Registry& reg)
        {
            BoundsSystemState* state = reg.GetResource<BoundsSystemState>();
            if (!state)
                state = reg.EmplaceResource<BoundsSystemState>();
            if (!state) return;
            const MeshTable*   meshes  = reg.GetResource<MeshTable>();
            const SpriteTable* sprites = reg.GetResource<SpriteTable>();

            std::vector<Astra::Entity>& todo = state->scratch;
            todo.clear();

            // 1. Changed since last run (the three producers), 2. drawables with
            //    no box yet (first sight, or a registry just loaded).
            const Astra::Tick since = state->lastRun;
            reg.CreateView<const WorldTransform, Astra::Changed<WorldTransform>>().Since(since)
                .ForEach([&](Astra::Entity e, const WorldTransform&) { todo.push_back(e); });
            reg.CreateView<const WorldTransform, const MeshRenderer, Astra::Changed<MeshRenderer>>().Since(since)
                .ForEach([&](Astra::Entity e, const WorldTransform&, const MeshRenderer&) { todo.push_back(e); });
            reg.CreateView<const WorldTransform, const SpriteRenderer, Astra::Changed<SpriteRenderer>>().Since(since)
                .ForEach([&](Astra::Entity e, const WorldTransform&, const SpriteRenderer&) { todo.push_back(e); });
            reg.CreateView<const WorldTransform, const MeshRenderer, Astra::Not<WorldBounds>>()
                .ForEach([&](Astra::Entity e, const WorldTransform&, const MeshRenderer&) { todo.push_back(e); });
            reg.CreateView<const WorldTransform, const SpriteRenderer, Astra::Not<WorldBounds>>()
                .ForEach([&](Astra::Entity e, const WorldTransform&, const SpriteRenderer&) { todo.push_back(e); });

            // 3. Removal reconciliation: a box whose entity no longer draws.
            reg.CreateView<const WorldBounds, Astra::Not<MeshRenderer>, Astra::Not<SpriteRenderer>>()
                .ForEach([&](Astra::Entity e, const WorldBounds&) { todo.push_back(e); });

            for (Astra::Entity e : todo)
            {
                const WorldTransform* world = std::as_const(reg).GetComponent<WorldTransform>(e);
                const std::optional<Aabb> box =
                    world ? WorldBoxFor(std::as_const(reg), e, *world, meshes, sprites) : std::nullopt;
                if (!box)
                {
                    if (reg.HasComponent<WorldBounds>(e))
                        reg.RemoveComponent<WorldBounds>(e);
                    continue;
                }
                if (WorldBounds* wb = reg.GetComponent<WorldBounds>(e))   // non-const: stamps the change
                    wb->box = *box;
                else
                    reg.AddComponent<WorldBounds>(e, WorldBounds{ *box });
            }

            state->lastRun = reg.CurrentTick();
            reg.AdvanceTick();
        }

        static constexpr bool RequiresExclusive = true;   // it advances the tick (TransformPropagationSystem's contract)
    };
}
```

The `todo` list may hold an entity twice (moved AND first-seen); the loop is idempotent, so no dedup. If `Astra::Changed<T>` requires `T` also listed as `const T` in the same view (Query.hpp:380 says so), the three change views above already comply. `reg.GetComponent<T>(e)` on a `const Astra::Registry&` is the non-stamping overload.

- [ ] **Step 6: Register and schedule**

`SceneModule.hpp` — append after `PhysicsSettings`:

```cpp
        creg.RegisterComponent<WorldBounds>();       // F3 plan 1 -- APPENDED (see the note above)
```

`Runtime.cpp` `InstallEngineSystems` — after the `TransformPropagationSystem` guard:

```cpp
        if (!fixed.HasSystem<BoundsSystem>())
            std::ignore = fixed.AddSystem<BoundsSystem>();
```

with `#include <Arcane/Scene/BoundsSystem.hpp>` beside the TransformSystems include, and extend the comment above ("the engine's HEADLESS pair" → "trio"; the After<> trait places it).

`ArcaneEditor/src/Scene/EditModeSchedule.cpp:13` — after the existing AddSystem:

```cpp
        std::ignore = m_schedule.AddSystem<Arcane::BoundsSystem>();   // F3: boxes follow the Edit-mode propagation
```

and the include. Update the file's "The one Edit-mode system" comment to "two".

- [ ] **Step 7: ABI 36**

`PluginABI.hpp`: add a v36 line in the v-list style at :900-903 and set `kGamePluginABIVersion = 36`:

```cpp
    // v36 (2026-09-18, F3 plan 1): Scene/Components.hpp gains WorldBounds (a
    //     new engine component; RegisterSceneComponents appends it), WorldTransform
    //     and MeshRenderer become AstraChangeTracked (their reflected layout is
    //     unchanged, their column ticks are not); Scene/SpriteGeometry.hpp is
    //     new in Core. A module built against v35 registers one component fewer
    //     -- refuse. ReferenceProject.arcproj restamped.
    inline constexpr uint32_t kGamePluginABIVersion = 36;
```

`ReferenceProject/ReferenceProject.arcproj:6` → `"abi": 36`.

- [ ] **Step 8: Build and run**

Run: `msbuild Arcane.slnx /p:Configuration=Debug /m`, then `.\ArcaneTests.exe "[bounds]"` — PASS. Then `.\ArcaneTests.exe "~[gpu]"` — the whole suite green (`Changed<>` exactness on `WorldTransform` must not disturb `TransformOrderTest`; a `RegisterSceneComponents` count pin, if one exists, moves by one — update it and say so in the commit).

- [ ] **Step 9: Commit**

```bash
git add ArcaneCore/src/Arcane/Scene/SpriteGeometry.hpp ArcaneClient/src/Arcane/Render/SpriteGeometry.hpp ArcaneCore/src/Arcane/Scene/Components.hpp ArcaneCore/src/Arcane/Scene/BoundsSystem.hpp ArcaneCore/src/Arcane/Scene/SceneModule.hpp ArcaneCore/src/Arcane/Base/Runtime.cpp ArcaneEditor/src/Scene/EditModeSchedule.cpp ArcaneCore/src/Arcane/Plugin/PluginABI.hpp ReferenceProject/ReferenceProject.arcproj ArcaneTests/src/BoundsSystemTest.cpp
git commit -m "feat(scene): WorldBounds on every drawable -- BoundsSystem after transform propagation (fixedUpdate + Edit mode), mesh = artifact AABB through the world matrix, sprite = SpriteWorldQuad widened 1 mm in Z (SpriteGeometry moves to Core), WorldTransform/MeshRenderer change-tracked; ABI 36 (F3 plan 1 T2)"
```

---
### Task 3: `VisibleSet` — the CPU coarse stage, and its sprite + pick consumers

**Files:**
- Create: `ArcaneClient/src/Arcane/Render/VisibilitySystem.hpp`
- Modify: `ArcaneClient/src/Arcane/Render/RenderSystems.hpp:47-60` (the sprite sweep), `ArcaneClient/src/Arcane/Render/PickEmit.cpp:30-60,158-175`
- Test: `ArcaneTests/src/VisibilityTest.cpp`; `ArcaneTests/src/PickBufferTest.cpp` (one new case)

**Interfaces:**
- Consumes: `WorldBounds`, `Frustum`, `ViewTransform`, `Hidden`.
- Produces:
  ```cpp
  inline constexpr float kVisibilitySlack = 0.25f;
  struct VisibleEntry { Astra::Entity entity; Aabb box; float nearDepth; };
  struct VisibleSet {
      ViewTransform view; Frustum frustum;               // frustum already Widened(kVisibilitySlack)
      std::vector<VisibleEntry> entries;
      std::vector<std::uint64_t> members;                 // bitset by entity index
      [[nodiscard]] bool Contains(Astra::Entity) const noexcept;
      void Clear();
  };
  struct SceneVisibility { std::vector<VisibleSet> views; };   // registry resource; views[0] = the main view
  void BuildVisibleSet(Astra::Registry&, const ViewTransform&, VisibleSet& out);
  [[nodiscard]] const VisibleSet* MainVisibleSet(const Astra::Registry&) noexcept;   // nullptr = no resource = cull nothing
  ```

- [ ] **Step 1: Write the failing tests**

`ArcaneTests/src/VisibilityTest.cpp`:

```cpp
#include <Arcane/Render/VisibilitySystem.hpp>
#include <Arcane/Scene/BoundsSystem.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/TransformSystems.hpp>
#include <Arcane/Mesh/MeshBuilder.hpp>

#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Registry/Registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <unordered_map>

namespace
{
    struct World
    {
        std::shared_ptr<Astra::ComponentRegistry> components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{ components };
        std::unordered_map<Arcane::Guid, Arcane::MeshEntry> meshes;
        Astra::Entity root{};
        Arcane::Guid cube;

        World()
        {
            Arcane::RegisterSceneComponents(reg);
            root = reg.CreateEntity();
            reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});
            reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{ root });
            reg.SetResource<Arcane::MeshTable>(Arcane::MeshTable{ &meshes });
            cube = Arcane::Guid::Generate();
            Arcane::MeshEntry entry;
            entry.data   = Arcane::BuildCube(2.0f);
            entry.bounds = Arcane::ComputeMeshBounds(entry.data);
            meshes.emplace(cube, entry);
        }
        Astra::Entity Cube(glm::vec3 pos)
        {
            Astra::Entity e = reg.CreateEntity();
            Arcane::Transform t; t.position = pos;
            reg.AddComponent<Arcane::Transform>(e, t);
            reg.SetParent(e, root);
            reg.AddComponent<Arcane::MeshRenderer>(e, Arcane::MeshRenderer{ cube, {} });
            return e;
        }
        void Tick() { Arcane::TransformPropagationSystem{}(reg); Arcane::BoundsSystem{}(reg); }
    };

    Arcane::ViewTransform Ortho10()   // half-height 10 m, 4:3 -> half-width 13.33
    {
        return Arcane::ViewTransform::Orthographic(glm::vec2(0.0f), 10.0f, glm::uvec2{ 800, 600 });
    }
}

TEST_CASE("BuildVisibleSet: inside in, outside out, straddling in, nearDepth is the box's nearest view depth", "[visibility]")
{
    World w;
    Astra::Entity in       = w.Cube(glm::vec3(0, 0, 0));
    Astra::Entity out      = w.Cube(glm::vec3(40, 0, 0));
    Astra::Entity straddle = w.Cube(glm::vec3(13.5f, 0, 0));   // [12.5, 14.5] crosses the 13.33 edge
    Astra::Entity deep     = w.Cube(glm::vec3(0, 0, -5));
    w.Tick();
    Arcane::VisibleSet vis;
    Arcane::BuildVisibleSet(w.reg, Ortho10(), vis);
    CHECK(vis.Contains(in));
    CHECK_FALSE(vis.Contains(out));
    CHECK(vis.Contains(straddle));
    CHECK(vis.Contains(deep));
    REQUIRE(vis.entries.size() == 3);
    // The ortho view sits at z=0 looking -Z; the [-1,1] cube at the origin has its nearest face at z=+1 -> depth -1 -> clamped 0; the deep one at z=-4 -> depth 4.
    float depthIn = -1.0f, depthDeep = -1.0f;
    for (const Arcane::VisibleEntry& e : vis.entries)
    {
        if (e.entity == in)   depthIn   = e.nearDepth;
        if (e.entity == deep) depthDeep = e.nearDepth;
    }
    CHECK(depthIn == Catch::Approx(0.0f));
    CHECK(depthDeep == Catch::Approx(4.0f));
}

TEST_CASE("BuildVisibleSet: the slack admits a box just outside the frustum", "[visibility]")
{
    World w;
    Astra::Entity e = w.Cube(glm::vec3(14.5f, 0, 0));   // [13.5, 15.5]: 0.17 m outside the 13.33 edge, inside the 0.25 slack
    w.Tick();
    Arcane::VisibleSet vis;
    Arcane::BuildVisibleSet(w.reg, Ortho10(), vis);
    CHECK(vis.Contains(e));
}

TEST_CASE("BuildVisibleSet: Hidden entities and entities without WorldBounds are not members; Clear on entry", "[visibility]")
{
    World w;
    Astra::Entity hidden = w.Cube(glm::vec3(0));
    w.reg.AddComponent<Arcane::Hidden>(hidden, Arcane::Hidden{});
    Astra::Entity bare = w.reg.CreateEntity();
    w.reg.AddComponent<Arcane::Transform>(bare, Arcane::Transform{});
    w.reg.SetParent(bare, w.root);
    w.Tick();
    Arcane::VisibleSet vis;
    vis.entries.push_back(Arcane::VisibleEntry{ bare, {}, 0.0f });   // stale content must not survive
    Arcane::BuildVisibleSet(w.reg, Ortho10(), vis);
    CHECK_FALSE(vis.Contains(hidden));
    CHECK_FALSE(vis.Contains(bare));
    CHECK(vis.entries.empty());
}

TEST_CASE("MainVisibleSet: absent resource is nullptr; views[0] once set", "[visibility]")
{
    World w;
    CHECK(Arcane::MainVisibleSet(w.reg) == nullptr);
    Arcane::SceneVisibility* sv = w.reg.EmplaceResource<Arcane::SceneVisibility>();
    REQUIRE(sv);
    CHECK(Arcane::MainVisibleSet(w.reg) == nullptr);   // no views yet
    sv->views.emplace_back();
    CHECK(Arcane::MainVisibleSet(w.reg) == &sv->views[0]);
}
```

Add to `ArcaneTests/src/PickBufferTest.cpp` (beside the existing `CollectPickables` cases, using their fixture idiom for a sprite entity):

```cpp
TEST_CASE("CollectPickables: with a SceneVisibility resource only members are emitted; without one everything is", "[pick]")
{
    // Fixture: two sprite entities with WorldTransform + SpriteRenderer, one at the origin, one at x = 1000.
    // Build the registry the way the file's other CollectPickables cases do, then:
    std::vector<Arcane::PickDrawable> all;
    Arcane::CollectPickables(reg, all);
    REQUIRE(all.size() == 2);

    Arcane::TransformPropagationSystem{}(reg);
    Arcane::BoundsSystem{}(reg);
    Arcane::SceneVisibility* sv = reg.EmplaceResource<Arcane::SceneVisibility>();
    sv->views.emplace_back();
    Arcane::BuildVisibleSet(reg, Arcane::ViewTransform::Orthographic(glm::vec2(0.0f), 5.0f, glm::uvec2{ 800, 600 }), sv->views[0]);

    std::vector<Arcane::PickDrawable> some;
    Arcane::CollectPickables(reg, some);
    REQUIRE(some.size() == 1);
    CHECK(some[0].entity == nearEntity);
}
```

(The fixture requires a `SceneRoot` and parenting for `TransformPropagationSystem` — copy the `World` shape from `VisibilityTest.cpp` if the file's fixture lacks one.)

- [ ] **Step 2: Run to verify they fail**

Run: `msbuild Arcane.slnx /p:Configuration=Debug /m` — expected: `Arcane/Render/VisibilitySystem.hpp` not found.

- [ ] **Step 3: Write `VisibilitySystem.hpp`**

```cpp
#pragma once

// The CPU coarse visibility stage (F3, spec s4): one VisibleSet per view,
// linear over every WorldBounds. A registry RESOURCE (SceneVisibility)
// because RenderSubmissionSystem is an Astra::System and must read it; the
// host builds views[0] between the fixed and render schedulers
// (Sim/SystemSchedulers.hpp) from the viewport's ViewTransform.
//
// NO RESOURCE => NOTHING IS CULLED. Every consumer (the sprite sweep,
// CollectPickables, the GPU batch builder) reads MainVisibleSet() and treats
// nullptr as "draw everything" -- device-less hosts and every pre-F3 test keep
// their behaviour.
//
// THE SLACK: WorldBounds is the fixed-step pose; the sprite sweep renders a
// pose interpolated toward it (PhysicsInterpBuffer), so a fast sprite at the
// screen edge can sit a fraction of one step outside its box. The frustum is
// widened by kVisibilitySlack metres before every test -- conservative, one
// constant. Plan 2's GPU cull uses the SAME widened planes (VisibleSet::frustum).
//
// HEADER-ONLY, device-free (the CollectMeshInstances idiom): ArcaneTests
// drives it under ~[gpu]. No spatial structure -- the trigger is a measured
// BuildVisibleSet above 0.5 ms (spec s4).
#include <Arcane/Math/Aabb.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/Frustum.hpp>
#include <Arcane/Scene/ViewTransform.hpp>

#include <Astra/Registry/Registry.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Arcane
{
    inline constexpr float kVisibilitySlack = 0.25f;

    struct VisibleEntry
    {
        Astra::Entity entity{};
        Aabb          box;
        float         nearDepth = 0.0f;   // view-space distance to the box's nearest point along -Z, clamped >= 0
    };

    struct VisibleSet
    {
        ViewTransform              view;
        Frustum                    frustum;   // already Widened(kVisibilitySlack)
        std::vector<VisibleEntry>  entries;
        std::vector<std::uint64_t> members;   // bitset by entity index

        [[nodiscard]] bool Contains(Astra::Entity e) const noexcept
        {
            const std::uint32_t idx = static_cast<std::uint32_t>(e.GetID());
            const std::size_t word = idx / 64u;
            return word < members.size() && ((members[word] >> (idx % 64u)) & 1ull);
        }

        void Clear()
        {
            entries.clear();
            std::fill(members.begin(), members.end(), 0ull);
        }

        void Insert(Astra::Entity e, const Aabb& box, float nearDepth)
        {
            const std::uint32_t idx = static_cast<std::uint32_t>(e.GetID());
            const std::size_t word = idx / 64u;
            if (word >= members.size())
                members.resize(word + 1, 0ull);
            members[word] |= (1ull << (idx % 64u));
            entries.push_back(VisibleEntry{ e, box, nearDepth });
        }
    };

    struct SceneVisibility
    {
        std::vector<VisibleSet> views;   // index 0 = the main view; shadow views / previews append (later arcs)
    };

    [[nodiscard]] inline const VisibleSet* MainVisibleSet(const Astra::Registry& reg) noexcept
    {
        const SceneVisibility* sv = reg.GetResource<SceneVisibility>();
        return (sv && !sv->views.empty()) ? &sv->views[0] : nullptr;
    }

    // View-space depth of the box's nearest point: the eight corners through
    // `view`, the largest z (nearest, since the camera looks down -Z),
    // negated and clamped at 0 (a box that straddles the eye is "at" it).
    [[nodiscard]] inline float NearViewDepth(const Aabb& box, const glm::mat4& view) noexcept
    {
        float nearest = -std::numeric_limits<float>::infinity();
        for (int i = 0; i < 8; ++i)
        {
            const glm::vec3 c((i & 1) ? box.max.x : box.min.x,
                              (i & 2) ? box.max.y : box.min.y,
                              (i & 4) ? box.max.z : box.min.z);
            nearest = std::max(nearest, (view * glm::vec4(c, 1.0f)).z);
        }
        return std::max(0.0f, -nearest);
    }

    inline void BuildVisibleSet(Astra::Registry& reg, const ViewTransform& view, VisibleSet& out)
    {
        out.Clear();
        out.view    = view;
        out.frustum = Frustum::From(view).Widened(kVisibilitySlack);
        reg.CreateView<const WorldBounds, Astra::Not<Hidden>>().ForEach(
            [&](Astra::Entity e, const WorldBounds& wb)
            {
                if (out.frustum.Contains(wb.box))
                    out.Insert(e, wb.box, NearViewDepth(wb.box, view.view));
            });
    }
}
```

`Astra::Entity::GetID()` is the slot index without the version (`ThirdParty/Astra/include/Astra/Entity/Entity.hpp:98`).

- [ ] **Step 4: The sprite sweep and the pick emitter consume it**

`RenderSystems.hpp` — add `#include <Arcane/Render/VisibilitySystem.hpp>`; extend the traits to `Astra::Reads<WorldTransform, SpriteRenderer, Hidden, WorldBounds>` (the resource read needs no trait); at the top of `operator()` after the `ctx` guard:

```cpp
            const VisibleSet* vis = MainVisibleSet(reg);   // nullptr: cull nothing (spec s4)
```

and as the first line inside the `ForEach` lambda:

```cpp
                if (vis && !vis->Contains(e))
                    return;   // off-screen this frame (the CPU coarse stage)
```

`PickEmit.cpp` — same include; in `CollectPickables`, before the sprite block `const VisibleSet* vis = MainVisibleSet(registry);` and the same two-line skip as the first statement of BOTH the sprite lambda (:35) and the mesh lambda (:164). The collider walk (:85) is untouched — physics silhouettes carry no `WorldBounds`.

- [ ] **Step 5: Build and run**

Run: `msbuild Arcane.slnx /p:Configuration=Debug /m`, then `.\ArcaneTests.exe "[visibility],[pick],[render]"` — PASS; then `"~[gpu]"` whole — green (no existing test sets the resource, so nothing changes for them).

- [ ] **Step 6: Commit**

```bash
git add ArcaneClient/src/Arcane/Render/VisibilitySystem.hpp ArcaneClient/src/Arcane/Render/RenderSystems.hpp ArcaneClient/src/Arcane/Render/PickEmit.cpp ArcaneTests/src/VisibilityTest.cpp ArcaneTests/src/PickBufferTest.cpp
git commit -m "feat(render): VisibleSet -- the CPU coarse visibility stage per view (linear over WorldBounds, frustum widened by kVisibilitySlack, membership bitset, nearDepth); the sprite sweep and CollectPickables skip non-members, absent resource culls nothing (F3 plan 1 T3)"
```

---

### Task 4: The editor's framing reads `WorldBounds`

**Files:**
- Modify: `ArcaneEditor/src/Viewport/EditorCamera.cpp:160-290` (`SelectionFramingBounds`, `SceneFramingBounds`, the anonymous helpers)
- Test: `ArcaneTests/src/EditorCameraTest.cpp` (the framing cases)

**Interfaces:**
- Consumes: `WorldBounds`, `Aabb::Union`.
- Produces: unchanged signatures `FramingBounds SelectionFramingBounds(Astra::Registry&, std::span<const Astra::Entity>)`, `FramingBounds SceneFramingBounds(Astra::Registry&)`.

- [ ] **Step 1: Adjust the framing tests to run the bounds pass**

In `EditorCameraTest.cpp`, every case that calls `SelectionFramingBounds` / `SceneFramingBounds` builds entities with a `WorldTransform` directly (no propagation). Give each such fixture a bounds pass before the framing call:

```cpp
    Arcane::BoundsSystem{}(reg);   // F3: framing reads WorldBounds, which this pass writes from WorldTransform + the renderer
```

(`BoundsSystem` reads `WorldTransform` as given; it does not need `TransformPropagationSystem` to have run.) Add `#include <Arcane/Scene/BoundsSystem.hpp>`. Then add one new case:

```cpp
TEST_CASE("framing reads WorldBounds: a box the renderer would draw is the box that frames", "[editor][camera][framing]")
{
    // A cube [-1,1]^3 at (10,0,0) -> WorldBounds [9,11]x[-1,1]x[-1,1]; a bare node at (-3,0,0) contributes its position.
    // Build: registry + MeshTable with a BuildCube(2) entry, entity A (WorldTransform translate 10, MeshRenderer), entity B (WorldTransform translate -3).
    Arcane::BoundsSystem{}(reg);
    const Astra::Entity sel[] = { a, b };
    const Arcane::FramingBounds fb = Arcane::SelectionFramingBounds(reg, sel);
    REQUIRE(fb.count == 2);
    CHECK(fb.min == glm::vec3(-3, -1, -1));
    CHECK(fb.max == glm::vec3(11, 1, 1));
    const Arcane::FramingBounds scene = Arcane::SceneFramingBounds(reg);
    REQUIRE(scene.count == 1);   // the bare node is not drawn, so Frame All ignores it
    CHECK(scene.min == glm::vec3(9, -1, -1));
}
```

- [ ] **Step 2: Run to verify the new case fails**

Run the build then `.\ArcaneTests.exe "[framing]"` — the existing cases pass (they now run the pass first), the new case fails only if the implementation still recomputes; proceed regardless to Step 3 (the rewrite is the deliverable).

- [ ] **Step 3: Rewrite the two functions**

Replace the anonymous-namespace helpers `AddSprite`, `AddMesh`, `ResolveSprite`, `ResolveMesh` and both functions (:180-290) with:

```cpp
    namespace
    {
        void Grow(FramingBounds& b, const Aabb& box) noexcept
        {
            if (b.count == 0) { b.min = box.min; b.max = box.max; }
            else              { b.min = glm::min(b.min, box.min); b.max = glm::max(b.max, box.max); }
            ++b.count;
        }
    }

    // F3 (spec s2.3): framing is a CONSUMER of WorldBounds -- the same box the
    // renderer culls with and the pick pass emits, so the three can never
    // disagree. A bare node (no WorldBounds) frames as its position.
    FramingBounds SelectionFramingBounds(Astra::Registry& reg, std::span<const Astra::Entity> entities)
    {
        FramingBounds b;
        for (Astra::Entity e : entities)
        {
            const WorldTransform* world = std::as_const(reg).GetComponent<WorldTransform>(e);
            if (!world)
                continue;
            if (const WorldBounds* wb = std::as_const(reg).GetComponent<WorldBounds>(e))
                Grow(b, wb->box);
            else
            {
                const glm::vec3 p(world->matrix[3]);
                Grow(b, Aabb{ p, p });
            }
        }
        return b;
    }

    FramingBounds SceneFramingBounds(Astra::Registry& reg)
    {
        FramingBounds b;
        reg.CreateView<const WorldBounds, Astra::Not<Hidden>>().ForEach(
            [&](Astra::Entity, const WorldBounds& wb) { Grow(b, wb.box); });
        return b;
    }
```

Drop the now-unused includes (`SpriteGeometry.hpp`, `SceneResources.hpp` if nothing else in the file needs them) and delete the `EntityBox` helper struct.

- [ ] **Step 4: Build and run**

`.\ArcaneTests.exe "[editor],[camera],[framing]"` — PASS.

- [ ] **Step 5: Commit**

```bash
git add ArcaneEditor/src/Viewport/EditorCamera.cpp ArcaneTests/src/EditorCameraTest.cpp
git commit -m "refactor(editor): Frame Selected / Frame All read WorldBounds instead of recomputing the mesh corners and sprite quad -- framing, culling and picking now share one box (F3 plan 1 T4)"
```

---
### Task 5: The GPU scene's CPU half — `GpuInstance`, the mirror, `GpuSceneSync`, `BuildGpuSceneFrame`

**Files:**
- Create: `ArcaneCore/src/Arcane/Math/NormalMatrix.hpp` (`NormalMatrixFor` moves here from `MeshNode.hpp:174-183`, verbatim; `MeshNode.hpp` includes it and keeps the name)
- Create: `ArcaneClient/src/Arcane/Render/GpuSceneTypes.hpp`, `ArcaneClient/src/Arcane/Render/GpuSceneSync.hpp`
- Delete: `ArcaneClient/src/Arcane/Render/MeshSubmissionSystem.hpp` (`CollectMeshInstances`) — in Task 8, once the hosts no longer include it; THIS task moves its tests
- Test: `ArcaneTests/src/GpuSceneSyncTest.cpp` (new; absorbs the fourteen `CollectMeshInstances` / "mesh submission" cases of `MeshSubmissionTest.cpp:742-1160`, which are deleted from there)

**Interfaces:**
- Consumes: `WorldBounds`, `VisibleSet` (Task 3), `MeshTable`/`MeshEntry::data.sections`/`MeshMaterialTable`/`ResolvedMeshMaterial` (`SceneResources.hpp`), `Astra::Changed<T>`, `BindlessTable::kInvalidSlot` == `0xFFFFFFFF` (restated here as `kGpuInvalidMaterialSlot`, static_asserted equal in `GpuScene.cpp` where NRI is visible).
- Produces (all in `GpuSceneTypes.hpp`, no NRI):
  ```cpp
  inline constexpr std::uint32_t kGpuInvalidMaterialSlot = 0xFFFFFFFFu;
  struct GpuInstance { glm::mat4 model, prevModel; glm::vec4 normal0, normal1, normal2; glm::vec4 boundsMin, boundsMax, baseColor; std::uint32_t materialSlot, batch, flags, pad; };   // 240 B
  struct GpuBatchKey { Guid mesh; std::uint32_t section; std::uint32_t blend; };  + GpuBatchKeyHash
  struct RowSpanAllocator { std::uint32_t Allocate(std::uint32_t count); void Free(std::uint32_t first, std::uint32_t count); std::uint32_t HighWater() const; };
  struct GpuSceneRow { Astra::Entity entity; Guid mesh; std::uint32_t section, batch; glm::mat4 lastModel; glm::vec4 lastBaseColor; std::uint32_t lastSlot; bool live; std::uint64_t touched; };
  struct GpuSceneMirror { FlatMap<Entity, Rows{first,count}> slots; std::vector<GpuSceneRow> rows; RowSpanAllocator allocator; unordered_map<GpuBatchKey,uint32> batchIds; std::vector<GpuBatchKey> batchKeys; std::vector<uint32> batchRowCount; std::vector<uint32> dirtyLastFrame; std::uint64_t generation; Astra::Tick lastSyncTick; std::uint64_t syncCounter; };
  struct GpuSceneStage { std::vector<std::uint32_t> rows; std::vector<GpuInstance> values; std::uint32_t rowCapacity; bool fullRebuild; std::uint64_t generation; };
  struct DrawIndexedArgs { std::uint32_t indexNum, instanceNum, baseIndex; std::int32_t baseVertex; std::uint32_t baseInstance; };   // 20 B == nri::DrawIndexedDesc
  struct GpuBatchDraw { Guid mesh; std::uint32_t section, indexOffset, indexCount, firstOutput, capacity, argIndex, blend; float nearDepth; };
  struct GpuSceneFrame { GpuSceneStage stage; std::vector<GpuBatchDraw> batches; std::vector<DrawIndexedArgs> args; std::vector<std::uint32_t> visibleIndices; std::uint32_t rowCount; Frustum frustum; struct Stats { std::uint32_t total, coarseVisible, batches, draws; } stats; bool HasDraws() const; };
  ```
  and in `GpuSceneSync.hpp`: `void GpuSceneSync(Astra::Registry&, GpuSceneMirror&, std::uint64_t deviceSyncedGeneration, GpuSceneStage& out)` and `void BuildGpuSceneFrame(const GpuSceneMirror&, const VisibleSet* vis, const MeshTable*, const ViewTransform&, GpuSceneFrame& out)`.

- [ ] **Step 1: Move `NormalMatrixFor`**

Create `ArcaneCore/src/Arcane/Math/NormalMatrix.hpp` with `#pragma once`, `#include <glm/glm.hpp>`, `#include <cmath>`, and the function + its comment block cut verbatim from `MeshNode.hpp:106-183` (the SINGULAR GUARD prose included). In `MeshNode.hpp` replace them with `#include <Arcane/Math/NormalMatrix.hpp>` and a one-line pointer comment. Build: `MeshNodeTest.cpp`'s `NormalMatrixFor` cases stay green unchanged.

- [ ] **Step 2: Write the failing tests**

`ArcaneTests/src/GpuSceneSyncTest.cpp` — the fixture, then the cases:

```cpp
#include <Arcane/Render/GpuSceneSync.hpp>
#include <Arcane/Render/GpuSceneTypes.hpp>
#include <Arcane/Render/VisibilitySystem.hpp>
#include <Arcane/Scene/BoundsSystem.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/TransformSystems.hpp>
#include <Arcane/Mesh/MeshBuilder.hpp>

#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Registry/Registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <unordered_map>

namespace
{
    struct World
    {
        std::shared_ptr<Astra::ComponentRegistry> components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{ components };
        std::unordered_map<Arcane::Guid, Arcane::MeshEntry>            meshes;
        std::unordered_map<Arcane::Guid, Arcane::ResolvedMeshMaterial> materials;
        Astra::Entity root{};
        Arcane::GpuSceneMirror mirror;
        Arcane::GpuSceneStage  stage;
        std::uint64_t          deviceGen = 0;   // what the device side last synced; 0 = never

        World()
        {
            Arcane::RegisterSceneComponents(reg);
            root = reg.CreateEntity();
            reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});
            reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{ root });
            reg.SetResource<Arcane::MeshTable>(Arcane::MeshTable{ &meshes });
            reg.SetResource<Arcane::MeshMaterialTable>(Arcane::MeshMaterialTable{ &materials });
        }
        Arcane::Guid Mesh(std::uint32_t sections = 1, Arcane::Guid slotMaterial = {})
        {
            const Arcane::Guid id = Arcane::Guid::Generate();
            Arcane::MeshEntry entry;
            entry.data   = Arcane::BuildCube(2.0f);
            entry.bounds = Arcane::ComputeMeshBounds(entry.data);
            const std::uint32_t per = static_cast<std::uint32_t>(entry.data.indices.size()) / sections;
            entry.data.sections.clear();
            for (std::uint32_t s = 0; s < sections; ++s)
            {
                entry.data.sections.push_back(Arcane::MeshSection{ std::string(), s * per, per, s });
                entry.slots.push_back(Arcane::MeshSlot{ std::string(), slotMaterial });
            }
            meshes.emplace(id, entry);
            return id;
        }
        Astra::Entity Spawn(glm::vec3 pos, Arcane::Guid mesh, Arcane::Guid overrideMat = {})
        {
            Astra::Entity e = reg.CreateEntity();
            Arcane::Transform t; t.position = pos;
            reg.AddComponent<Arcane::Transform>(e, t);
            reg.SetParent(e, root);
            reg.AddComponent<Arcane::MeshRenderer>(e, Arcane::MeshRenderer{ mesh, overrideMat });
            return e;
        }
        // One host frame: propagate, bounds, sync (the device side "applies" by stamping the generation).
        void Frame()
        {
            Arcane::TransformPropagationSystem{}(reg);
            Arcane::BoundsSystem{}(reg);
            Arcane::GpuSceneSync(reg, mirror, deviceGen, stage);
            deviceGen = mirror.generation;
        }
        const Arcane::GpuInstance* Staged(std::uint32_t row) const
        {
            for (std::size_t i = 0; i < stage.rows.size(); ++i)
                if (stage.rows[i] == row) return &stage.values[i];
            return nullptr;
        }
        std::uint32_t RowOf(Astra::Entity e, std::uint32_t section = 0) const
        {
            const Arcane::GpuSceneMirror::Rows* r = mirror.slots.TryGet(e);
            REQUIRE(r);
            return r->first + section;
        }
    };
}

TEST_CASE("GpuSceneSync: a new entity allocates one row per section, staged with prev == model", "[gpuscene]")
{
    World w;
    const Arcane::Guid mesh = w.Mesh(3);
    Astra::Entity e = w.Spawn(glm::vec3(1, 2, 3), mesh);
    w.Frame();
    REQUIRE(w.stage.rows.size() == 3);
    CHECK(w.stage.fullRebuild);   // first sync against a device that never synced this mirror
    for (std::uint32_t s = 0; s < 3; ++s)
    {
        const Arcane::GpuInstance* v = w.Staged(w.RowOf(e, s));
        REQUIRE(v);
        CHECK(v->model[3] == glm::vec4(1, 2, 3, 1));
        CHECK(v->prevModel == v->model);
        CHECK(v->boundsMin == glm::vec4(0, 1, 2, 0));
        CHECK(v->boundsMax == glm::vec4(2, 3, 4, 0));
        CHECK(v->materialSlot == Arcane::kGpuInvalidMaterialSlot);
        CHECK(v->baseColor == glm::vec4(1.0f));
    }
    // Three distinct batch keys (same mesh, sections 0/1/2), each with one row.
    CHECK(w.mirror.batchKeys.size() == 3);
    CHECK(w.mirror.batchRowCount == std::vector<std::uint32_t>{ 1, 1, 1 });
}

TEST_CASE("GpuSceneSync: a static scene stages nothing on the second frame", "[gpuscene]")
{
    World w;
    w.Spawn(glm::vec3(0), w.Mesh());
    w.Frame();
    w.Frame();
    CHECK(w.stage.rows.empty());
    CHECK_FALSE(w.stage.fullRebuild);
}

TEST_CASE("GpuSceneSync: a move stages prev = the old pose, and the frame after re-stages prev == model (the re-dirty)", "[gpuscene]")
{
    World w;
    Astra::Entity e = w.Spawn(glm::vec3(0), w.Mesh());
    w.Frame();
    w.reg.GetComponent<Arcane::Transform>(e)->position = glm::vec3(5, 0, 0);
    w.Frame();                                             // frame N: moved
    const std::uint32_t row = w.RowOf(e);
    REQUIRE(w.stage.rows.size() == 1);
    CHECK(w.Staged(row)->model[3] == glm::vec4(5, 0, 0, 1));
    CHECK(w.Staged(row)->prevModel[3] == glm::vec4(0, 0, 0, 1));
    w.Frame();                                             // frame N+1: nothing changed, the re-dirty settles it
    REQUIRE(w.stage.rows.size() == 1);
    CHECK(w.Staged(row)->prevModel == w.Staged(row)->model);
    w.Frame();                                             // frame N+2: at rest
    CHECK(w.stage.rows.empty());
}

TEST_CASE("GpuSceneSync: moved on N and N+1 carries N's pose as prev on N+1", "[gpuscene]")
{
    World w;
    Astra::Entity e = w.Spawn(glm::vec3(0), w.Mesh());
    w.Frame();
    w.reg.GetComponent<Arcane::Transform>(e)->position = glm::vec3(1, 0, 0);
    w.Frame();
    w.reg.GetComponent<Arcane::Transform>(e)->position = glm::vec3(2, 0, 0);
    w.Frame();
    const std::uint32_t row = w.RowOf(e);
    REQUIRE(w.stage.rows.size() == 1);
    CHECK(w.Staged(row)->model[3] == glm::vec4(2, 0, 0, 1));
    CHECK(w.Staged(row)->prevModel[3] == glm::vec4(1, 0, 0, 1));
}

TEST_CASE("GpuSceneSync: a destroyed entity frees its rows and a later spawn reuses the span", "[gpuscene]")
{
    World w;
    const Arcane::Guid mesh = w.Mesh(2);
    Astra::Entity a = w.Spawn(glm::vec3(0), mesh);
    w.Frame();
    const std::uint32_t firstA = w.RowOf(a);
    w.reg.DestroyEntity(a);
    w.Frame();
    CHECK(w.mirror.slots.TryGet(a) == nullptr);
    CHECK(w.mirror.batchRowCount == std::vector<std::uint32_t>{ 0, 0 });
    Astra::Entity b = w.Spawn(glm::vec3(0), mesh);
    w.Frame();
    CHECK(w.RowOf(b) == firstA);
    CHECK(w.mirror.allocator.HighWater() == 2);
}

TEST_CASE("GpuSceneSync: Hidden frees the rows; unhiding re-allocates them as new (prev == model)", "[gpuscene]")
{
    World w;
    Astra::Entity e = w.Spawn(glm::vec3(0), w.Mesh());
    w.Frame();
    w.reg.AddComponent<Arcane::Hidden>(e, Arcane::Hidden{});
    w.Frame();
    CHECK(w.mirror.slots.TryGet(e) == nullptr);
    w.reg.RemoveComponent<Arcane::Hidden>(e);
    w.reg.GetComponent<Arcane::Transform>(e)->position = glm::vec3(7, 0, 0);
    w.Frame();
    const Arcane::GpuInstance* v = w.Staged(w.RowOf(e));
    REQUIRE(v);
    CHECK(v->prevModel == v->model);
}

TEST_CASE("GpuSceneSync: reassigning the mesh to one with a different section count reallocates", "[gpuscene]")
{
    World w;
    const Arcane::Guid one = w.Mesh(1);
    const Arcane::Guid three = w.Mesh(3);
    Astra::Entity e = w.Spawn(glm::vec3(0), one);
    w.Frame();
    w.reg.GetComponent<Arcane::MeshRenderer>(e)->mesh = three;
    w.Frame();
    CHECK(w.mirror.slots.TryGet(e)->count == 3);
    CHECK(w.stage.rows.size() == 3);
}

TEST_CASE("GpuSceneSync: a generation mismatch (registry swap) re-stages every live row with prev == model", "[gpuscene]")
{
    World w;
    Astra::Entity e = w.Spawn(glm::vec3(0), w.Mesh());
    w.Frame();
    w.reg.GetComponent<Arcane::Transform>(e)->position = glm::vec3(1, 0, 0);
    w.Frame();
    w.deviceGen = 0;   // the device forgot this mirror (a new context, or a mirror from a swapped registry)
    w.Frame();
    REQUIRE(w.stage.rows.size() == 1);
    CHECK(w.stage.fullRebuild);
    CHECK(w.Staged(w.RowOf(e))->prevModel == w.Staged(w.RowOf(e))->model);
}

TEST_CASE("GpuSceneSync: the material chain -- override wins, else the slot's material, else white; an unresolvable override falls to the slot", "[gpuscene][material]")
{
    World w;
    const Arcane::Guid slotMat = Arcane::Guid::Generate();
    const Arcane::Guid overMat = Arcane::Guid::Generate();
    w.materials.emplace(slotMat, Arcane::ResolvedMeshMaterial{ glm::vec4(1, 0, 0, 1) });
    w.materials.emplace(overMat, Arcane::ResolvedMeshMaterial{ glm::vec4(0, 1, 0, 1) });
    const Arcane::Guid withSlot = w.Mesh(1, slotMat);
    const Arcane::Guid noSlot   = w.Mesh(1);
    Astra::Entity a = w.Spawn(glm::vec3(0), withSlot, overMat);
    Astra::Entity b = w.Spawn(glm::vec3(0), withSlot);
    Astra::Entity c = w.Spawn(glm::vec3(0), noSlot);
    Astra::Entity d = w.Spawn(glm::vec3(0), withSlot, Arcane::Guid::Generate());   // valid Guid, absent from the table
    w.Frame();
    CHECK(w.Staged(w.RowOf(a))->baseColor == glm::vec4(0, 1, 0, 1));
    CHECK(w.Staged(w.RowOf(b))->baseColor == glm::vec4(1, 0, 0, 1));
    CHECK(w.Staged(w.RowOf(c))->baseColor == glm::vec4(1, 1, 1, 1));
    CHECK(w.Staged(w.RowOf(d))->baseColor == glm::vec4(1, 0, 0, 1));
}

TEST_CASE("GpuSceneSync: a material whose resolved colour changed re-stages the row without any component write", "[gpuscene][material]")
{
    World w;
    const Arcane::Guid slotMat = Arcane::Guid::Generate();
    w.materials.emplace(slotMat, Arcane::ResolvedMeshMaterial{ glm::vec4(1, 0, 0, 1) });
    Astra::Entity e = w.Spawn(glm::vec3(0), w.Mesh(1, slotMat));
    w.Frame();
    w.materials[slotMat].baseColor = glm::vec4(0, 0, 1, 1);   // the resolver re-read the .arcmat
    w.Frame();
    REQUIRE(w.stage.rows.size() == 1);
    CHECK(w.Staged(w.RowOf(e))->baseColor == glm::vec4(0, 0, 1, 1));
    CHECK(w.Staged(w.RowOf(e))->prevModel == w.Staged(w.RowOf(e))->model);   // a material change is not a move
}

TEST_CASE("GpuSceneSync: materialSlot copies the resolved bindless slot; override repaints every section", "[gpuscene][material]")
{
    World w;
    const Arcane::Guid s0 = Arcane::Guid::Generate(), s1 = Arcane::Guid::Generate(), over = Arcane::Guid::Generate();
    Arcane::ResolvedMeshMaterial m0{ glm::vec4(1, 0, 0, 1) }; m0.materialSlot = 7;
    Arcane::ResolvedMeshMaterial m1{ glm::vec4(0, 1, 0, 1) }; m1.materialSlot = 9;
    w.materials.emplace(s0, m0); w.materials.emplace(s1, m1);
    w.materials.emplace(over, Arcane::ResolvedMeshMaterial{ glm::vec4(0, 0, 1, 1) });
    const Arcane::Guid mesh = w.Mesh(2);
    w.meshes[mesh].slots[0].material = s0;
    w.meshes[mesh].slots[1].material = s1;
    Astra::Entity plain = w.Spawn(glm::vec3(0), mesh);
    Astra::Entity painted = w.Spawn(glm::vec3(0), mesh, over);
    w.Frame();
    CHECK(w.Staged(w.RowOf(plain, 0))->materialSlot == 7);
    CHECK(w.Staged(w.RowOf(plain, 1))->materialSlot == 9);
    CHECK(w.Staged(w.RowOf(painted, 0))->baseColor == glm::vec4(0, 0, 1, 1));
    CHECK(w.Staged(w.RowOf(painted, 1))->baseColor == glm::vec4(0, 0, 1, 1));
    CHECK(w.Staged(w.RowOf(painted, 0))->materialSlot == Arcane::kGpuInvalidMaterialSlot);
}

TEST_CASE("GpuSceneSync: nil mesh, a Guid not in the table, and an entity missing WorldTransform get no rows", "[gpuscene]")
{
    World w;
    Astra::Entity nil = w.Spawn(glm::vec3(0), Arcane::Guid{});
    Astra::Entity absent = w.Spawn(glm::vec3(0), Arcane::Guid::Generate());
    Astra::Entity noWorld = w.reg.CreateEntity();
    w.reg.AddComponent<Arcane::MeshRenderer>(noWorld, Arcane::MeshRenderer{ w.Mesh(), {} });
    w.Frame();
    CHECK(w.mirror.slots.TryGet(nil) == nullptr);
    CHECK(w.mirror.slots.TryGet(absent) == nullptr);
    CHECK(w.mirror.slots.TryGet(noWorld) == nullptr);
    CHECK(w.stage.rows.empty());
}

TEST_CASE("BuildGpuSceneFrame: capacities prefix-sum by batch id; only coarse-visible rows are written; empty batches are not emitted; nearest first", "[gpuscene][frame]")
{
    World w;
    const Arcane::Guid cube = w.Mesh(1);
    const Arcane::Guid other = w.Mesh(1);
    Astra::Entity near = w.Spawn(glm::vec3(0, 0, -2), cube);     // nearest
    Astra::Entity far  = w.Spawn(glm::vec3(0, 0, -9), cube);
    Astra::Entity mid  = w.Spawn(glm::vec3(0, 0, -5), other);
    Astra::Entity off  = w.Spawn(glm::vec3(80, 0, -5), other);   // outside
    Astra::Entity gone = w.Spawn(glm::vec3(80, 0, -5), w.Mesh(1));   // its whole batch is off-screen
    w.Frame();
    const Arcane::ViewTransform view = Arcane::ViewTransform::Orthographic(glm::vec2(0.0f), 10.0f, glm::uvec2{ 800, 600 });
    Arcane::VisibleSet vis;
    Arcane::BuildVisibleSet(w.reg, view, vis);
    Arcane::GpuSceneFrame frame;
    Arcane::BuildGpuSceneFrame(w.mirror, &vis, w.reg.GetResource<Arcane::MeshTable>(), view, frame);

    REQUIRE(frame.batches.size() == 2);                    // cube batch, other batch; gone's batch not emitted
    CHECK(frame.batches[0].mesh == cube);                  // nearDepth 1 (near's front face at z=-1)
    CHECK(frame.batches[1].mesh == other);                 // nearDepth 4
    CHECK(frame.batches[0].argIndex == 0);
    CHECK(frame.batches[1].argIndex == 1);
    CHECK(frame.args[0].instanceNum == 2);
    CHECK(frame.args[1].instanceNum == 1);
    CHECK(frame.args[0].indexNum == frame.batches[0].indexCount);
    CHECK(frame.batches[0].capacity == 2);
    CHECK(frame.batches[1].capacity == 2);                 // `other` has two resident rows (mid + off), one visible
    CHECK(frame.batches[1].firstOutput == 2);
    CHECK(frame.rowCount == 5);
    // The visible index region of the cube batch holds exactly near's and far's rows (any order).
    std::vector<std::uint32_t> cubeRows{ frame.visibleIndices[0], frame.visibleIndices[1] };
    std::sort(cubeRows.begin(), cubeRows.end());
    std::vector<std::uint32_t> expected{ w.RowOf(near), w.RowOf(far) };
    std::sort(expected.begin(), expected.end());
    CHECK(cubeRows == expected);
    CHECK(frame.visibleIndices[2] == w.RowOf(mid));
    CHECK(frame.stats.total == 5);
    CHECK(frame.stats.coarseVisible == 3);
    CHECK(frame.stats.batches == 2);
    CHECK(frame.stats.draws == 2);
    CHECK(frame.HasDraws());
}

TEST_CASE("BuildGpuSceneFrame: no VisibleSet means every row is written", "[gpuscene][frame]")
{
    World w;
    w.Spawn(glm::vec3(0), w.Mesh(1));
    w.Spawn(glm::vec3(500, 0, 0), w.Mesh(1));
    w.Frame();
    Arcane::GpuSceneFrame frame;
    Arcane::BuildGpuSceneFrame(w.mirror, nullptr, w.reg.GetResource<Arcane::MeshTable>(), Arcane::ViewTransform{}, frame);
    CHECK(frame.stats.coarseVisible == 2);
    CHECK(frame.batches.size() == 2);
}

TEST_CASE("RowSpanAllocator: first-fit reuse, split, and high water", "[gpuscene]")
{
    Arcane::RowSpanAllocator a;
    CHECK(a.Allocate(3) == 0);
    CHECK(a.Allocate(2) == 3);
    CHECK(a.HighWater() == 5);
    a.Free(0, 3);
    CHECK(a.Allocate(1) == 0);   // splits the freed span
    CHECK(a.Allocate(2) == 1);
    CHECK(a.Allocate(1) == 5);   // nothing free fits -> bump
    CHECK(a.HighWater() == 6);
}
```

Then DELETE `MeshSubmissionTest.cpp:742-1160` (the `CollectMeshInstances` / "mesh submission" cases and the `MakeMeshEntry`/`SpawnMeshEntity` helpers at :700-725) — their behaviour is pinned above.

- [ ] **Step 3: Run to verify they fail**

Build — expected: the two headers not found.

- [ ] **Step 4: Write `GpuSceneTypes.hpp`**

```cpp
#pragma once

// The GPU scene's CPU-side vocabulary (F3, spec s5). NO NRI here: the mirror
// is a registry resource, Sync and the batch builder are header-only and
// device-free (GpuSceneSync.hpp), and ArcaneTests drives them under ~[gpu].
// Render/Nri/GpuScene.{hpp,cpp} is the device half.
#include <Arcane/Guid.hpp>
#include <Arcane/Math/Aabb.hpp>
#include <Arcane/Scene/Frustum.hpp>

#include <Astra/Container/FlatMap.hpp>
#include <Astra/Core/Tick.hpp>
#include <Astra/Entity/Entity.hpp>

#include <glm/glm.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Arcane
{
    // Mirrors BindlessTable::kInvalidSlot (0xFFFFFFFF) without pulling <NRI.h>;
    // GpuScene.cpp static_asserts the two agree.
    inline constexpr std::uint32_t kGpuInvalidMaterialSlot = 0xFFFFFFFFu;

    // Row flags (bit0 reserved for `teleported`, bits 1-2 the blend mode -- plan 2).
    inline constexpr std::uint32_t kGpuInstanceFlagTeleported = 1u << 0;

    // ONE ROW PER (entity, mesh section). 240 bytes, std430; data/shaders/
    // gpu_scene.hlsli carries the same field order -- change both or neither.
    // The normal matrix is NormalMatrixFor(model) computed on the CPU at
    // staging (R8: no per-vertex 3x3 inverse); prevModel is the pose the row
    // was drawn with LAST frame (spec s5.3's G2 contract).
    struct GpuInstance
    {
        glm::mat4     model{1.0f};
        glm::mat4     prevModel{1.0f};
        glm::vec4     normal0{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec4     normal1{0.0f, 1.0f, 0.0f, 0.0f};
        glm::vec4     normal2{0.0f, 0.0f, 1.0f, 0.0f};
        glm::vec4     boundsMin{0.0f};                  // world AABB; w unused
        glm::vec4     boundsMax{0.0f};                  // w = alphaCutoff for masked rows (plan 2)
        glm::vec4     baseColor{1.0f};
        std::uint32_t materialSlot = kGpuInvalidMaterialSlot;
        std::uint32_t batch        = 0;                 // the batch KEY id (stable per (mesh, section, blend))
        std::uint32_t flags        = 0;
        std::uint32_t pad          = 0;
    };
    static_assert(sizeof(GpuInstance) == 240, "GpuInstance must stay 240 bytes -- gpu_scene.hlsli mirrors it");

    struct GpuBatchKey
    {
        Guid          mesh{};
        std::uint32_t section = 0;
        std::uint32_t blend   = 0;   // 0 opaque (plan 1); 1 masked, 2 transparent (plan 2)
        [[nodiscard]] bool operator==(const GpuBatchKey&) const noexcept = default;
    };
    struct GpuBatchKeyHash
    {
        [[nodiscard]] std::size_t operator()(const GpuBatchKey& k) const noexcept
        {
            std::size_t h = std::hash<std::uint64_t>{}(k.mesh.hi);
            h ^= std::hash<std::uint64_t>{}(k.mesh.lo) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
            h ^= std::hash<std::uint64_t>{}((std::uint64_t(k.section) << 32) | k.blend) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
            return h;
        }
    };

    // Contiguous row spans, first-fit, UE's FSpanAllocator shape in miniature:
    // a multi-section entity owns `count` consecutive rows.
    struct RowSpanAllocator
    {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> freeSpans;   // {first, count}
        std::uint32_t highWater = 0;

        [[nodiscard]] std::uint32_t Allocate(std::uint32_t count)
        {
            for (std::size_t i = 0; i < freeSpans.size(); ++i)
            {
                auto& [first, n] = freeSpans[i];
                if (n >= count)
                {
                    const std::uint32_t out = first;
                    first += count;
                    n     -= count;
                    if (n == 0)
                        freeSpans.erase(freeSpans.begin() + static_cast<std::ptrdiff_t>(i));
                    return out;
                }
            }
            const std::uint32_t out = highWater;
            highWater += count;
            return out;
        }
        void Free(std::uint32_t first, std::uint32_t count) { freeSpans.emplace_back(first, count); }
        [[nodiscard]] std::uint32_t HighWater() const noexcept { return highWater; }
    };

    struct GpuSceneRow
    {
        Astra::Entity entity{};
        Guid          mesh{};
        std::uint32_t section = 0;
        std::uint32_t batch   = 0;
        glm::mat4     lastModel{1.0f};          // the matrix last uploaded -- the prior-pose history (spec s5.3)
        glm::vec4     lastBaseColor{1.0f};      // what the row was last staged with: a resolved material that
        std::uint32_t lastSlot = kGpuInvalidMaterialSlot;   //   changed under it re-stages the row (no generation plumbing)
        bool          live    = false;
        std::uint64_t touched = 0;              // the sync counter that last saw the entity
    };

    struct GpuSceneMirror
    {
        struct Rows { std::uint32_t first = 0; std::uint32_t count = 0; };

        Astra::FlatMap<Astra::Entity, Rows>                              slots;
        std::vector<GpuSceneRow>                                         rows;          // by row; size == allocator.HighWater()
        RowSpanAllocator                                                 allocator;
        std::unordered_map<GpuBatchKey, std::uint32_t, GpuBatchKeyHash>  batchIds;
        std::vector<GpuBatchKey>                                         batchKeys;     // by id
        std::vector<std::uint32_t>                                       batchRowCount; // by id: live rows with that key
        std::vector<std::uint32_t>                                       dirtyLastFrame;
        std::uint64_t                                                    generation;    // fresh per mirror; the device stamps what it synced
        Astra::Tick                                                      lastSyncTick = 0;
        std::uint64_t                                                    syncCounter  = 0;

        GpuSceneMirror() : generation(NextGeneration()) {}
        static std::uint64_t NextGeneration() noexcept
        {
            static std::atomic<std::uint64_t> counter{ 1 };
            return counter.fetch_add(1);
        }
    };

    // What one Sync hands the device side: the rows to (re)write this frame.
    struct GpuSceneStage
    {
        std::vector<std::uint32_t> rows;
        std::vector<GpuInstance>   values;       // parallel to `rows`
        std::uint32_t              rowCapacity = 0;   // allocator high water: the buffer must hold this many rows
        bool                       fullRebuild = false;
        std::uint64_t              generation  = 0;   // the mirror's; the device stamps it as synced after a successful Apply
        void Clear() { rows.clear(); values.clear(); rowCapacity = 0; fullRebuild = false; generation = 0; }
    };

    // == nri::DrawIndexedDesc, field for field (GpuScene.cpp static_asserts it).
    struct DrawIndexedArgs
    {
        std::uint32_t indexNum     = 0;
        std::uint32_t instanceNum  = 0;
        std::uint32_t baseIndex    = 0;
        std::int32_t  baseVertex   = 0;
        std::uint32_t baseInstance = 0;
    };
    static_assert(sizeof(DrawIndexedArgs) == 20);

    struct GpuBatchDraw
    {
        Guid          mesh{};
        std::uint32_t section     = 0;
        std::uint32_t indexOffset = 0;
        std::uint32_t indexCount  = 0;
        std::uint32_t firstOutput = 0;   // where this batch's visible rows start in visibleIndices
        std::uint32_t capacity    = 0;   // resident rows with this key
        std::uint32_t argIndex    = 0;   // position in `args`
        std::uint32_t blend       = 0;
        float         nearDepth   = 0.0f;
    };

    struct GpuSceneFrame
    {
        GpuSceneStage              stage;
        std::vector<GpuBatchDraw>  batches;          // EMITTED, in draw order
        std::vector<DrawIndexedArgs> args;           // by argIndex
        std::vector<std::uint32_t> visibleIndices;   // rowCapacity entries, partitioned by batch capacity
        std::uint32_t              rowCount = 0;     // == stage.rowCapacity
        Frustum                    frustum;          // the widened planes the CPU test used (plan 2's cull CB)
        struct Stats { std::uint32_t total = 0, coarseVisible = 0, batches = 0, draws = 0; } stats;

        [[nodiscard]] bool HasDraws() const noexcept { return !batches.empty(); }
    };
}
```

- [ ] **Step 5: Write `GpuSceneSync.hpp`**

```cpp
#pragma once

// GpuSceneSync -- the mirror's per-frame reconciliation (F3, spec s5.3 as
// amended by the UE vet: CPU history + re-dirty, no compute), and
// BuildGpuSceneFrame -- the batch table, the indirect args and (plan 1) the
// CPU-written visible-index list (plan 2's MeshCullNode takes over that loop).
//
// Host order per frame: schedulers (TransformPropagation -> Bounds) ->
// BuildVisibleSet -> GpuSceneSync -> BuildGpuSceneFrame -> RenderFrame.
// Sync ADVANCES THE TICK after recording lastSyncTick (TransformPropagation
// System's contract): a write made later in the same tick must compare
// strictly newer next frame. It runs on the host thread with no system in
// flight.
//
// THE G2 CONTRACT (spec s5.3): a row's prevModel is the model it was drawn
// with on the previous frame; a new row, a full rebuild, or a teleported row
// has prev == model. Implemented as UE's FSceneVelocityData: `lastModel` per
// row is the matrix last uploaded; a row uploaded with prev != model is
// re-dirtied next frame so it lands at rest.
#include <Arcane/Math/NormalMatrix.hpp>
#include <Arcane/Render/GpuSceneTypes.hpp>
#include <Arcane/Render/VisibilitySystem.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/ViewTransform.hpp>

#include <Astra/Registry/Registry.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Arcane
{
    namespace detail
    {
        inline std::uint32_t BatchIdFor(GpuSceneMirror& m, const GpuBatchKey& key)
        {
            if (auto it = m.batchIds.find(key); it != m.batchIds.end())
                return it->second;
            const std::uint32_t id = static_cast<std::uint32_t>(m.batchKeys.size());
            m.batchIds.emplace(key, id);
            m.batchKeys.push_back(key);
            m.batchRowCount.push_back(0);
            return id;
        }

        inline void FreeEntityRows(GpuSceneMirror& m, Astra::Entity e, const GpuSceneMirror::Rows& r)
        {
            for (std::uint32_t i = 0; i < r.count; ++i)
            {
                GpuSceneRow& row = m.rows[r.first + i];
                if (row.live)
                    --m.batchRowCount[row.batch];
                row = GpuSceneRow{};
            }
            m.allocator.Free(r.first, r.count);
            m.slots.Erase(e);
        }

        // The material chain (the retired CollectMeshInstances' rule, verbatim):
        // override, if it resolves, wins; else the section slot's material; else white.
        struct RowMaterial { glm::vec4 baseColor{1.0f}; std::uint32_t slot = kGpuInvalidMaterialSlot; };
        inline RowMaterial ResolveRowMaterial(const MeshMaterialTable* mats, const MeshEntry& entry,
                                              const MeshSection& section, const Guid& override)
        {
            const ResolvedMeshMaterial* mat = mats ? mats->Resolve(override) : nullptr;
            if (!mat)
            {
                Guid slotMat{};
                if (section.slotIndex < entry.slots.size())
                    slotMat = entry.slots[section.slotIndex].material;
                mat = mats ? mats->Resolve(slotMat) : nullptr;
            }
            return mat ? RowMaterial{ mat->baseColor, mat->materialSlot } : RowMaterial{};
        }
    }

    inline void GpuSceneSync(Astra::Registry& reg, GpuSceneMirror& m,
                             std::uint64_t deviceSyncedGeneration, GpuSceneStage& out)
    {
        out.Clear();
        const MeshTable*         meshes = reg.GetResource<MeshTable>();
        const MeshMaterialTable* mats   = reg.GetResource<MeshMaterialTable>();
        const bool full = (deviceSyncedGeneration != m.generation);
        ++m.syncCounter;

        std::vector<std::uint8_t> dirty(m.rows.size(), 0);
        auto markDirty = [&](std::uint32_t row)
        {
            if (row >= dirty.size()) dirty.resize(row + 1, 0);
            dirty[row] = 1;
        };

        // 1. Reconcile: every drawable mesh entity owns a span; a changed mesh or
        //    section count reallocates; everything else is left alone.
        reg.CreateView<const WorldTransform, const WorldBounds, const MeshRenderer, Astra::Not<Hidden>>().ForEach(
            [&](Astra::Entity e, const WorldTransform&, const WorldBounds&, const MeshRenderer& mr)
            {
                const MeshEntry* entry = meshes ? meshes->Resolve(mr.mesh) : nullptr;
                const std::uint32_t sections = entry ? static_cast<std::uint32_t>(entry->data.sections.size()) : 0;
                GpuSceneMirror::Rows* r = m.slots.TryGet(e);
                if (sections == 0)
                {
                    if (r) detail::FreeEntityRows(m, e, *r);
                    return;
                }
                if (r && (r->count != sections || m.rows[r->first].mesh != mr.mesh))
                {
                    detail::FreeEntityRows(m, e, *r);
                    r = nullptr;
                }
                if (!r)
                {
                    const std::uint32_t first = m.allocator.Allocate(sections);
                    if (m.rows.size() < m.allocator.HighWater())
                        m.rows.resize(m.allocator.HighWater());
                    for (std::uint32_t s = 0; s < sections; ++s)
                    {
                        GpuSceneRow& row = m.rows[first + s];
                        row.entity  = e;
                        row.mesh    = mr.mesh;
                        row.section = s;
                        row.batch   = detail::BatchIdFor(m, GpuBatchKey{ mr.mesh, s, 0 });
                        row.live    = true;
                        row.lastModel = glm::mat4(0.0f);   // "never uploaded": step 3 sets prev = model
                        ++m.batchRowCount[row.batch];
                        markDirty(first + s);
                    }
                    r = &m.slots[e];
                    *r = GpuSceneMirror::Rows{ first, sections };
                }
                for (std::uint32_t s = 0; s < r->count; ++s)
                    m.rows[r->first + s].touched = m.syncCounter;
            });

        // Removal: spans whose entity the walk did not see (destroyed, Hidden,
        // lost its renderer / transform / bounds).
        {
            std::vector<Astra::Entity> victims;
            for (const auto& kv : m.slots)
                if (m.rows[kv.second.first].touched != m.syncCounter)
                    victims.push_back(kv.first);
            for (Astra::Entity e : victims)
                if (const GpuSceneMirror::Rows* r = m.slots.TryGet(e))
                    detail::FreeEntityRows(m, e, *r);
        }

        // 2. Dirty: moved (exact -- WorldTransform is change-tracked), a component
        //    write on MeshRenderer, the re-dirty list, or every live row on a rebuild.
        if (full)
        {
            for (std::uint32_t row = 0; row < m.rows.size(); ++row)
                if (m.rows[row].live) markDirty(row);
        }
        else
        {
            auto markEntity = [&](Astra::Entity e)
            {
                if (const GpuSceneMirror::Rows* r = m.slots.TryGet(e))
                    for (std::uint32_t s = 0; s < r->count; ++s) markDirty(r->first + s);
            };
            reg.CreateView<const WorldTransform, Astra::Changed<WorldTransform>>().Since(m.lastSyncTick)
                .ForEach([&](Astra::Entity e, const WorldTransform&) { markEntity(e); });
            reg.CreateView<const MeshRenderer, Astra::Changed<MeshRenderer>>().Since(m.lastSyncTick)
                .ForEach([&](Astra::Entity e, const MeshRenderer&) { markEntity(e); });
            for (std::uint32_t row : m.dirtyLastFrame)
                if (row < m.rows.size() && m.rows[row].live) markDirty(row);
        }
        m.dirtyLastFrame.clear();

        // 3. Stage. A material re-resolve per live row is the same hash lookup
        //    the retired per-frame sweep did; a changed colour/slot dirties the
        //    row without a component write (the resolver re-read the .arcmat).
        for (std::uint32_t row = 0; row < m.rows.size(); ++row)
        {
            GpuSceneRow& r = m.rows[row];
            if (!r.live) continue;
            const MeshEntry* entry = meshes ? meshes->Resolve(r.mesh) : nullptr;
            if (!entry || r.section >= entry->data.sections.size()) continue;   // reconciled away next frame
            const MeshRenderer* mr = std::as_const(reg).GetComponent<MeshRenderer>(r.entity);
            const WorldTransform* wt = std::as_const(reg).GetComponent<WorldTransform>(r.entity);
            const WorldBounds* wb = std::as_const(reg).GetComponent<WorldBounds>(r.entity);
            if (!mr || !wt || !wb) continue;
            const detail::RowMaterial mat =
                detail::ResolveRowMaterial(mats, *entry, entry->data.sections[r.section], mr->materialOverride);
            const bool isDirty = (row < dirty.size() && dirty[row])
                              || mat.baseColor != r.lastBaseColor || mat.slot != r.lastSlot;
            if (!isDirty) continue;

            const bool neverUploaded = (r.lastModel == glm::mat4(0.0f));
            GpuInstance v;
            v.model     = wt->matrix;
            v.prevModel = (full || neverUploaded) ? v.model : r.lastModel;
            const glm::mat3 n = NormalMatrixFor(v.model);
            v.normal0 = glm::vec4(n[0], 0.0f);
            v.normal1 = glm::vec4(n[1], 0.0f);
            v.normal2 = glm::vec4(n[2], 0.0f);
            v.boundsMin    = glm::vec4(wb->box.min, 0.0f);
            v.boundsMax    = glm::vec4(wb->box.max, 0.0f);
            v.baseColor    = mat.baseColor;
            v.materialSlot = mat.slot;
            v.batch        = r.batch;
            v.flags        = 0;
            if (v.prevModel != v.model)
                m.dirtyLastFrame.push_back(row);   // settle it next frame (the re-dirty)
            r.lastModel     = v.model;
            r.lastBaseColor = v.baseColor;
            r.lastSlot      = v.materialSlot;
            out.rows.push_back(row);
            out.values.push_back(v);
        }

        out.rowCapacity = m.allocator.HighWater();
        out.fullRebuild = full;
        out.generation  = m.generation;
        m.lastSyncTick  = reg.CurrentTick();
        reg.AdvanceTick();
    }

    inline void BuildGpuSceneFrame(const GpuSceneMirror& m, const VisibleSet* vis, const MeshTable* meshes,
                                   const ViewTransform& view, GpuSceneFrame& out)
    {
        out.batches.clear();
        out.args.clear();
        out.rowCount = m.allocator.HighWater();
        out.visibleIndices.assign(out.rowCount, 0xFFFFFFFFu);
        out.frustum = vis ? vis->frustum : Frustum::From(view).Widened(kVisibilitySlack);
        out.stats   = {};

        const std::size_t nb = m.batchKeys.size();
        std::vector<std::uint32_t> firstOutput(nb, 0), cursor(nb, 0);
        std::vector<float> nearDepth(nb, std::numeric_limits<float>::infinity());
        std::uint32_t prefix = 0;
        for (std::size_t b = 0; b < nb; ++b) { firstOutput[b] = prefix; prefix += m.batchRowCount[b]; }

        // The coarse pass: every live row whose entity is a member (or every row, no set).
        for (std::uint32_t row = 0; row < m.rows.size(); ++row)
        {
            const GpuSceneRow& r = m.rows[row];
            if (!r.live) continue;
            ++out.stats.total;
            if (vis && !vis->Contains(r.entity)) continue;
            ++out.stats.coarseVisible;
            out.visibleIndices[firstOutput[r.batch] + cursor[r.batch]++] = row;
        }
        // nearDepth per batch = the minimum over its visible entities' VisibleEntry::nearDepth.
        if (vis)
        {
            for (const VisibleEntry& e : vis->entries)
                if (const GpuSceneMirror::Rows* r = m.slots.TryGet(e.entity))
                    for (std::uint32_t s = 0; s < r->count; ++s)
                    {
                        const std::uint32_t b = m.rows[r->first + s].batch;
                        nearDepth[b] = std::min(nearDepth[b], e.nearDepth);
                    }
        }
        else
            std::fill(nearDepth.begin(), nearDepth.end(), 0.0f);

        // Emit: batches with >= 1 visible row, opaque before masked (blend asc), nearest first.
        std::vector<std::uint32_t> emitted;
        for (std::uint32_t b = 0; b < nb; ++b)
            if (cursor[b] > 0 && m.batchKeys[b].blend != 2u) emitted.push_back(b);
        std::stable_sort(emitted.begin(), emitted.end(), [&](std::uint32_t a, std::uint32_t b)
        {
            if (m.batchKeys[a].blend != m.batchKeys[b].blend) return m.batchKeys[a].blend < m.batchKeys[b].blend;
            return nearDepth[a] < nearDepth[b];
        });
        for (std::uint32_t b : emitted)
        {
            const GpuBatchKey& key = m.batchKeys[b];
            const MeshEntry* entry = meshes ? meshes->Resolve(key.mesh) : nullptr;
            if (!entry || key.section >= entry->data.sections.size()) continue;
            const MeshSection& section = entry->data.sections[key.section];
            const std::uint32_t indexCount = section.indexCount ? section.indexCount
                                                                : static_cast<std::uint32_t>(entry->data.indices.size());
            GpuBatchDraw d;
            d.mesh = key.mesh; d.section = key.section;
            d.indexOffset = section.indexOffset; d.indexCount = indexCount;
            d.firstOutput = firstOutput[b]; d.capacity = m.batchRowCount[b];
            d.argIndex = static_cast<std::uint32_t>(out.args.size());
            d.blend = key.blend; d.nearDepth = nearDepth[b];
            out.batches.push_back(d);
            out.args.push_back(DrawIndexedArgs{ indexCount, cursor[b], section.indexOffset, 0, 0 });
        }
        out.stats.batches = static_cast<std::uint32_t>(out.batches.size());
        out.stats.draws   = out.stats.batches;   // + transparent rows in plan 2
    }
}
```

- [ ] **Step 6: Build and run**

`.\ArcaneTests.exe "[gpuscene]"` — PASS; `.\ArcaneTests.exe "[mesh]"` — the trimmed `MeshSubmissionTest.cpp` still green; `[mesh][node]` (`NormalMatrixFor`) green.

- [ ] **Step 7: Commit**

```bash
git add ArcaneCore/src/Arcane/Math/NormalMatrix.hpp ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.hpp ArcaneClient/src/Arcane/Render/GpuSceneTypes.hpp ArcaneClient/src/Arcane/Render/GpuSceneSync.hpp ArcaneTests/src/GpuSceneSyncTest.cpp ArcaneTests/src/MeshSubmissionTest.cpp
git commit -m "feat(render): the GPU scene's CPU half -- GpuInstance (240 B rows), the registry-side mirror (row spans, lastModel history, re-dirty list, generation stamp), GpuSceneSync (reconcile + exact dirty + the material chain) and BuildGpuSceneFrame (batches nearest-first, CPU-written visible indices + indirect args); CollectMeshInstances' pins move here (F3 plan 1 T5)"
```

---
### Task 6: The device half — `RgUsage::IndirectArgs`, `GpuScene`, `GpuSceneSyncNode`

**Files:**
- Modify: `ArcaneClient/src/Arcane/Render/Nri/RenderGraph.hpp:130-134` (the `RgUsage` enum), `RenderGraph.cpp:25-40` (the table comment) and `:84-115` (`StateFor`), `RenderGraphExec.cpp:213-222` (texture-usage mapping: `IndirectArgs` falls in the `break` group with the copy usages) and wherever buffer `BufferUsageBits` are derived from `RgUsage` for transients (grep `BufferUsageBits` in `RenderGraphExec.cpp`; `IndirectArgs` → `ARGUMENT_BUFFER`)
- Create: `ArcaneClient/src/Arcane/Render/Nri/GpuScene.{hpp,cpp}`, `ArcaneClient/src/Arcane/Render/Nri/nodes/GpuSceneSyncNode.{hpp,cpp}`
- Modify: `ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.hpp:940-960` (accessor), `NriGraphContext.cpp:413-440` (creation beside `m_meshBuffers`), `:794` (release)
- Test: `ArcaneTests/src/RenderGraphTest.cpp` (`StateFor` pin + a declaration-shape case), `ArcaneTests/src/NriGraphPixelTest.cpp` (a `[gpu]` buffer round-trip case)

**Interfaces:**
- Consumes: `GpuSceneFrame`, `GpuSceneStage`, `GpuInstance`, `DrawIndexedArgs` (Task 5); `NriUploadRing::Allocate`, `Graveyard::Bury`, `core.CreateCommittedBuffer`, `core.CmdCopyBuffer`, `RenderGraphBuilder::ImportBuffer/Read/Write`, `RenderGraphNodeContext{cmd, core, ring, Resolve}`.
- Produces:
  ```cpp
  enum class RgUsage : std::uint8_t { ColorWrite, DepthWrite, ShaderRead, ShaderWriteCs, CopySrc, CopyDst, Present, ReadbackHost, IndirectArgs };
  class ARCANE_API GpuScene {
      static std::unique_ptr<GpuScene> Create(NriDevice&);
      static constexpr std::uint32_t kInitialRows = 256, kScratchRows = 64;   // scratch: per frame slot, for ad-hoc instances
      // Called from GpuSceneSyncNode::Record. Grows the instance buffer (copying live rows, burying the old one),
      // copies staged rows + scratch rows through the ring, copies this slot's args + visibleIndices. Returns false (logged) on refusal.
      bool Apply(const GpuSceneFrame* frame, std::span<const GpuInstance> adHoc, std::uint32_t frameSlot,
                 RenderGraphNodeContext& ctx, Graveyard& graves, std::uint64_t fence);
      [[nodiscard]] nri::Buffer* Instances() const; [[nodiscard]] nri::Descriptor* InstancesView() const;   // STRUCTURED_BUFFER, stride 240
      [[nodiscard]] nri::Buffer* Args(std::uint32_t slot) const;
      [[nodiscard]] nri::Buffer* VisibleIndices(std::uint32_t slot) const; [[nodiscard]] nri::Descriptor* VisibleIndicesView(std::uint32_t slot) const;   // STRUCTURED_BUFFER, stride 4
      [[nodiscard]] std::uint32_t ScratchFirstRow(std::uint32_t slot) const;   // rowCapacity + slot * kScratchRows
      [[nodiscard]] std::uint64_t InstanceBufferGeneration() const;             // bumps on every grow: MeshNode rewrites its per-slot view when it changes
      [[nodiscard]] std::uint64_t SyncedGeneration() const; void SetSyncedGeneration(std::uint64_t);
      void Release(Graveyard&, std::uint64_t fence);
  };
  struct GpuSceneNodeInputs { RgBuffer instances, args, visibleIndices; };   // the imported handles the mesh node reads
  GpuSceneNodeInputs AddGpuSceneSyncNode(RenderGraph&, NriGraphContext*, const GpuSceneFrame* frame, std::span<const GpuInstance> adHoc);
  ```

- [ ] **Step 1: Write the failing tests**

In `RenderGraphTest.cpp`, beside the existing `StateFor` pins (search `RgUsage::ShaderWriteCs` for the idiom):

```cpp
TEST_CASE("RgUsage::IndirectArgs derives the argument-buffer state at the INDIRECT stage", "[rendergraph]")
{
    const nri::AccessLayoutStage s = Arcane::StateFor(Arcane::RgUsage::IndirectArgs, /*isTexture*/ false);
    CHECK(s.access == nri::AccessBits::ARGUMENT_BUFFER);
    CHECK(s.layout == nri::Layout::UNDEFINED);
    CHECK(s.stages == nri::StageBits::INDIRECT);
}

TEST_CASE("declaration shape: sync -> mesh reads three imported buffers with a copy-to-indirect edge", "[rendergraph]")
{
    Arcane::RenderGraph graph;
    Arcane::RgBuffer instances{}, args{}, indices{};
    graph.AddNode("gpuscene-sync", Arcane::RenderGraph::NodeKind::Copy, [&](Arcane::RenderGraphBuilder& b)
    {
        instances = b.ImportBuffer("gpuscene.instances", nullptr, 240 * 256);
        args      = b.ImportBuffer("gpuscene.args", nullptr, 20 * 64);
        indices   = b.ImportBuffer("gpuscene.visible", nullptr, 4 * 256);
        b.Write(instances, Arcane::RgUsage::CopyDst);
        b.Write(args, Arcane::RgUsage::CopyDst);
        b.Write(indices, Arcane::RgUsage::CopyDst);
    }, [](Arcane::RenderGraphNodeContext&) {});
    graph.AddNode("mesh", Arcane::RenderGraph::NodeKind::Raster, [&](Arcane::RenderGraphBuilder& b)
    {
        b.Read(instances, Arcane::RgUsage::ShaderRead);
        b.Read(args, Arcane::RgUsage::IndirectArgs);
        b.Read(indices, Arcane::RgUsage::ShaderRead);
    }, [](Arcane::RenderGraphNodeContext&) {});
    std::string err;
    const auto compiled = graph.Compile(&err);
    REQUIRE(compiled);
    // One barrier per buffer between the two nodes, the args one landing in ARGUMENT_BUFFER/INDIRECT.
    // Use the file's existing compiled-barrier inspection idiom (search "barriers" in RenderGraphTest.cpp) to assert
    // that the barrier list for node "mesh" contains a buffer barrier whose `after` is {ARGUMENT_BUFFER, INDIRECT}.
}
```

(If `StateFor` is file-local in `RenderGraph.cpp`, the first case pins it through the compiled barriers instead, the way the file already pins `ShaderWriteCs`. `NodeKind::Copy` is the transfer kind, `RenderGraph.hpp:479`.)

In `NriGraphPixelTest.cpp`, one `[gpu]` case per backend using the file's device fixture (`ARC_REQUIRE_BACKEND`, the context creation the mesh cases use):

```cpp
    void CheckGpuSceneRoundTrip(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        // A frame with two staged rows + one ad-hoc row and one batch; drive one RenderFrame whose only
        // scene content is the sync node (FrameDesc::mesh->scene set, no instances -> the mesh node declares
        // nothing to draw), then read the instance buffer back through the context's capture/readback path
        // (the `pickreadback` idiom: a ReadbackHost transient copied from the imported instances buffer) and
        // check bytes [row*240 .. +240) equal the staged GpuInstance for both rows and the scratch row.
        // Assert Arcane::RenderErrorCount() is unchanged.
    }
    TEST_CASE("gpuscene: staged rows and a scratch row land byte-exact in the instance buffer (d3d12)", "[gpu][gpuscene]") { CheckGpuSceneRoundTrip(Arcane::GraphicsBackend::D3D12); }
    TEST_CASE("gpuscene: staged rows and a scratch row land byte-exact in the instance buffer (vulkan)", "[gpu][gpuscene]") { CheckGpuSceneRoundTrip(Arcane::GraphicsBackend::Vulkan); }
```

Write the body against the fixture the file actually has (`CaptureMesh`'s device setup + a readback of an imported buffer via `RgUsage::ReadbackHost`, which `PickOutlineNodes.cpp:1320-1345` demonstrates); the assertions are the ones in the comment.

- [ ] **Step 2: Run to verify they fail**

Build — `RgUsage::IndirectArgs` undefined; the pixel case fails on the missing header.

- [ ] **Step 3: `RgUsage::IndirectArgs`**

`RenderGraph.hpp:130-134`: append `IndirectArgs` to the enum with the comment `// an indirect-draw argument buffer (buffers only)`. `RenderGraph.cpp` `StateFor`:

```cpp
        case Arcane::RgUsage::IndirectArgs:
            state = { nri::AccessBits::ARGUMENT_BUFFER, nri::Layout::UNDEFINED, nri::StageBits::INDIRECT };
            break;
```

and the table comment gains the row `//  IndirectArgs   ARGUMENT_BUFFER   (bit 5)         --  (buffers only)          INDIRECT           (bit 23)`. In `RenderGraphExec.cpp:213-222` the texture-usage switch gets `case RgUsage::IndirectArgs:` in the `break` group; where transient BUFFER usage bits are derived (grep `nri::BufferUsageBits::` in `RenderGraphExec.cpp` / `RenderGraph.cpp`), `IndirectArgs` maps to `nri::BufferUsageBits::ARGUMENT_BUFFER` (the persistent buffers this task creates carry the bit at creation regardless).

- [ ] **Step 4: Write `GpuScene.hpp`**

```cpp
#pragma once

// GpuScene -- the device half of the GPU scene (F3, spec s5): the ONE
// persistent instance buffer (GpuInstance rows, DEVICE memory, doubling
// growth), per-frame-slot indirect-args and visible-index buffers, and a
// SCRATCH row region per slot for registry-less callers (MeshDocument's
// preview, the thumbnail harvester, the [gpu] tests -- MeshSceneDesc::
// instances). Owned by NriGraphContext beside NriMeshBufferCache.
//
// Everything here is written by GpuSceneSyncNode::Record through the upload
// ring + CmdCopyBuffer -- NO compute (the UE vet: prev is CPU history).
// Growth happens INSIDE Record: a new buffer is created, the live rows are
// copied old->new on the command list, the old buffer is buried against the
// frame fence, and InstanceBufferGeneration() bumps so MeshNode rewrites its
// per-slot structured view before drawing (its previous frame on that slot
// has completed; the set is not in flight).
//
// Include order: NRI headers first, ALWAYS (NriCommon.hpp).
#include <NRI.h>

#include <Arcane/Base/Api.hpp>
#include <Arcane/Render/FramePacing.hpp>      // kSwapchainFramesInFlight
#include <Arcane/Render/GpuSceneTypes.hpp>
#include <Arcane/Render/Nri/RenderGraph.hpp>

#include <cstdint>
#include <memory>
#include <span>

namespace Arcane
{
    class Graveyard;
    class NriDevice;

    class ARCANE_API GpuScene
    {
    public:
        static constexpr std::uint32_t kInitialRows = 256;
        static constexpr std::uint32_t kScratchRows = 64;    // per frame slot; ad-hoc instances beyond this are dropped with one WARN

        static std::unique_ptr<GpuScene> Create(NriDevice& device);
        ~GpuScene();
        GpuScene(const GpuScene&)            = delete;
        GpuScene& operator=(const GpuScene&) = delete;

        bool Apply(const GpuSceneFrame* frame, std::span<const GpuInstance> adHoc, std::uint32_t frameSlot,
                   RenderGraphNodeContext& ctx, Graveyard& graves, std::uint64_t fence);

        [[nodiscard]] nri::Buffer*     Instances() const noexcept { return m_instances; }
        [[nodiscard]] nri::Descriptor* InstancesView() const noexcept { return m_instancesView; }
        [[nodiscard]] nri::Buffer*     Args(std::uint32_t slot) const noexcept { return m_args[slot]; }
        [[nodiscard]] nri::Buffer*     VisibleIndices(std::uint32_t slot) const noexcept { return m_visible[slot]; }
        [[nodiscard]] nri::Descriptor* VisibleIndicesView(std::uint32_t slot) const noexcept { return m_visibleView[slot]; }
        [[nodiscard]] std::uint32_t    RowCapacity() const noexcept { return m_rowCapacity; }
        [[nodiscard]] std::uint32_t    ScratchFirstRow(std::uint32_t slot) const noexcept { return m_rowCapacity + slot * kScratchRows; }
        [[nodiscard]] std::uint64_t    InstanceBufferGeneration() const noexcept { return m_instanceGeneration; }
        [[nodiscard]] std::uint64_t    SyncedGeneration() const noexcept { return m_syncedGeneration; }
        void SetSyncedGeneration(std::uint64_t g) noexcept { m_syncedGeneration = g; }
        [[nodiscard]] std::uint64_t    InstanceBytes() const noexcept;   // the whole buffer incl. scratch (for the readback test)

        void Release(Graveyard& graves, std::uint64_t fence);

    private:
        GpuScene() = default;
        bool CreateInstances(std::uint32_t rowCapacity);            // buffer + view for rowCapacity + kScratchRows * frames
        bool CreateSlotBuffers(std::uint32_t slot, std::uint32_t rows, std::uint32_t argCount);
        bool EnsureSlotCapacity(std::uint32_t slot, std::uint32_t rows, std::uint32_t argCount, Graveyard&, std::uint64_t fence);
        bool CopyRows(RenderGraphNodeContext& ctx, std::span<const std::uint32_t> rows,
                      std::span<const GpuInstance> values, std::uint32_t firstRowOverride, bool contiguous);

        NriDevice*       m_device = nullptr;
        nri::Buffer*     m_instances = nullptr;
        nri::Descriptor* m_instancesView = nullptr;
        std::uint32_t    m_rowCapacity = 0;
        std::uint64_t    m_instanceGeneration = 1;
        std::uint64_t    m_syncedGeneration = 0;
        nri::Buffer*     m_args[kSwapchainFramesInFlight] = {};
        std::uint32_t    m_argCapacity[kSwapchainFramesInFlight] = {};
        nri::Buffer*     m_visible[kSwapchainFramesInFlight] = {};
        nri::Descriptor* m_visibleView[kSwapchainFramesInFlight] = {};
        std::uint32_t    m_visibleCapacity[kSwapchainFramesInFlight] = {};
        bool             m_warnedScratchOverflow = false;
    };
}
```

- [ ] **Step 5: Write `GpuScene.cpp`**

```cpp
#include <Arcane/Render/Nri/GpuScene.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/Nri/BindlessTable.hpp>
#include <Arcane/Render/Nri/Graveyard.hpp>
#include <Arcane/Render/Nri/NriCommon.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/Nri/NriUploadRing.hpp>

#include <algorithm>
#include <cstring>

namespace Arcane
{
    static_assert(kGpuInvalidMaterialSlot == BindlessTable::kInvalidSlot,
                  "GpuSceneTypes.hpp restates BindlessTable::kInvalidSlot -- keep them equal");
    static_assert(sizeof(DrawIndexedArgs) == sizeof(nri::DrawIndexedDesc), "DrawIndexedArgs mirrors nri::DrawIndexedDesc");
    static_assert(offsetof(DrawIndexedArgs, indexNum)     == offsetof(nri::DrawIndexedDesc, indexNum));
    static_assert(offsetof(DrawIndexedArgs, instanceNum)  == offsetof(nri::DrawIndexedDesc, instanceNum));
    static_assert(offsetof(DrawIndexedArgs, baseIndex)    == offsetof(nri::DrawIndexedDesc, baseIndex));
    static_assert(offsetof(DrawIndexedArgs, baseVertex)   == offsetof(nri::DrawIndexedDesc, baseVertex));
    static_assert(offsetof(DrawIndexedArgs, baseInstance) == offsetof(nri::DrawIndexedDesc, baseInstance));

    namespace
    {
        constexpr std::uint64_t kRowBytes = sizeof(GpuInstance);

        bool CreateDeviceBuffer(NriDevice& device, std::uint64_t bytes, std::uint32_t stride,
                                nri::BufferUsageBits usage, const char* name, nri::Buffer*& out)
        {
            const nri::CoreInterface& core = device.Core();
            nri::BufferDesc desc = {};
            desc.size            = bytes;
            desc.structureStride = stride;
            desc.usage           = usage;
            if (!ARC_NRI_CHECK(core.CreateCommittedBuffer(device.Device(), nri::MemoryLocation::DEVICE, 0.0f, desc, out)) || !out)
            {
                out = nullptr;
                ARC_ERROR("[nri-graph] GpuScene: could not create {} ({} bytes)", name, bytes);
                return false;
            }
            core.SetDebugName(out, name);
            return true;
        }

        bool CreateStructuredView(NriDevice& device, nri::Buffer* buffer, std::uint32_t stride, nri::Descriptor*& out)
        {
            nri::BufferViewDesc view = {};
            view.buffer          = buffer;
            view.type            = nri::BufferView::STRUCTURED_BUFFER;
            view.offset          = 0;
            view.size            = nri::WHOLE_SIZE;
            view.structureStride = stride;
            return ARC_NRI_CHECK(device.Core().CreateBufferView(view, out)) && out;
        }
    }

    std::unique_ptr<GpuScene> GpuScene::Create(NriDevice& device)
    {
        std::unique_ptr<GpuScene> s(new GpuScene());
        s->m_device = &device;
        if (!s->CreateInstances(kInitialRows))
            return nullptr;
        for (std::uint32_t slot = 0; slot < kSwapchainFramesInFlight; ++slot)
            if (!s->CreateSlotBuffers(slot, kInitialRows, 64))
                return nullptr;
        return s;
    }

    GpuScene::~GpuScene() = default;   // Release() buries; a context tears down through it

    bool GpuScene::CreateInstances(std::uint32_t rowCapacity)
    {
        const std::uint64_t rows = std::uint64_t(rowCapacity) + std::uint64_t(kScratchRows) * kSwapchainFramesInFlight;
        nri::Buffer* buffer = nullptr;
        if (!CreateDeviceBuffer(*m_device, rows * kRowBytes, static_cast<std::uint32_t>(kRowBytes),
                                nri::BufferUsageBits::SHADER_RESOURCE, "gpuscene instances", buffer))
            return false;
        nri::Descriptor* view = nullptr;
        if (!CreateStructuredView(*m_device, buffer, static_cast<std::uint32_t>(kRowBytes), view))
        {
            m_device->Core().DestroyBuffer(buffer);
            return false;
        }
        m_instances     = buffer;
        m_instancesView = view;
        m_rowCapacity   = rowCapacity;
        return true;
    }

    std::uint64_t GpuScene::InstanceBytes() const noexcept
    {
        return (std::uint64_t(m_rowCapacity) + std::uint64_t(kScratchRows) * kSwapchainFramesInFlight) * kRowBytes;
    }

    bool GpuScene::CreateSlotBuffers(std::uint32_t slot, std::uint32_t rows, std::uint32_t argCount)
    {
        if (!CreateDeviceBuffer(*m_device, std::uint64_t(argCount) * sizeof(DrawIndexedArgs), 0,
                                nri::BufferUsageBits::ARGUMENT_BUFFER, "gpuscene args", m_args[slot]))
            return false;
        m_argCapacity[slot] = argCount;
        if (!CreateDeviceBuffer(*m_device, std::uint64_t(rows) * sizeof(std::uint32_t), sizeof(std::uint32_t),
                                nri::BufferUsageBits::SHADER_RESOURCE, "gpuscene visible", m_visible[slot]))
            return false;
        if (!CreateStructuredView(*m_device, m_visible[slot], sizeof(std::uint32_t), m_visibleView[slot]))
            return false;
        m_visibleCapacity[slot] = rows;
        return true;
    }

    bool GpuScene::EnsureSlotCapacity(std::uint32_t slot, std::uint32_t rows, std::uint32_t argCount,
                                      Graveyard& graves, std::uint64_t fence)
    {
        if (rows <= m_visibleCapacity[slot] && argCount <= m_argCapacity[slot])
            return true;
        const nri::CoreInterface& core = m_device->Core();
        nri::Buffer* oldArgs = m_args[slot]; nri::Buffer* oldVis = m_visible[slot]; nri::Descriptor* oldView = m_visibleView[slot];
        graves.Bury(fence, [&core, oldArgs, oldVis, oldView]
        {
            if (oldView) core.DestroyDescriptor(oldView);
            if (oldVis)  core.DestroyBuffer(oldVis);
            if (oldArgs) core.DestroyBuffer(oldArgs);
        });
        m_args[slot] = nullptr; m_visible[slot] = nullptr; m_visibleView[slot] = nullptr;
        std::uint32_t newRows = std::max(m_visibleCapacity[slot], 1u), newArgs = std::max(m_argCapacity[slot], 1u);
        while (newRows < rows) newRows *= 2;
        while (newArgs < argCount) newArgs *= 2;
        return CreateSlotBuffers(slot, newRows, newArgs);
    }

    // Copies `values` into rows: either each value to its own `rows[i]`, or --
    // `contiguous` -- the whole span to consecutive rows from firstRowOverride.
    bool GpuScene::CopyRows(RenderGraphNodeContext& ctx, std::span<const std::uint32_t> rows,
                            std::span<const GpuInstance> values, std::uint32_t firstRowOverride, bool contiguous)
    {
        if (values.empty())
            return true;
        const std::uint64_t bytes = values.size() * kRowBytes;
        const NriUploadRing::Alloc a = ctx.ring.Allocate(bytes, 16);
        if (!a.buffer || !a.cpu)
        {
            ARC_ERROR("[nri-graph] GpuScene: the upload ring refused {} bytes of instance rows", bytes);
            return false;
        }
        std::memcpy(a.cpu, values.data(), bytes);
        if (contiguous)
        {
            ctx.core.CmdCopyBuffer(ctx.cmd, *m_instances, std::uint64_t(firstRowOverride) * kRowBytes, *a.buffer, a.offset, bytes);
            return true;
        }
        for (std::size_t i = 0; i < values.size(); ++i)
            ctx.core.CmdCopyBuffer(ctx.cmd, *m_instances, std::uint64_t(rows[i]) * kRowBytes,
                                   *a.buffer, a.offset + i * kRowBytes, kRowBytes);
        return true;
    }

    bool GpuScene::Apply(const GpuSceneFrame* frame, std::span<const GpuInstance> adHoc, std::uint32_t frameSlot,
                         RenderGraphNodeContext& ctx, Graveyard& graves, std::uint64_t fence)
    {
        const nri::CoreInterface& core = ctx.core;

        // 1. Grow the instance buffer if the mirror outgrew it: copy the live
        //    range old->new on this command list, bury the old, bump the generation.
        const std::uint32_t needRows = frame ? frame->stage.rowCapacity : 0;
        if (needRows > m_rowCapacity)
        {
            std::uint32_t newCap = m_rowCapacity;
            while (newCap < needRows) newCap *= 2;
            nri::Buffer* oldBuf = m_instances; nri::Descriptor* oldView = m_instancesView;
            const std::uint32_t oldCap = m_rowCapacity;
            if (!CreateInstances(newCap))
            {
                m_instances = oldBuf; m_instancesView = oldView; m_rowCapacity = oldCap;   // keep drawing the old one
                return false;
            }
            if (!(frame && frame->stage.fullRebuild))
                core.CmdCopyBuffer(ctx.cmd, *m_instances, 0, *oldBuf, 0, std::uint64_t(oldCap) * kRowBytes);
            graves.Bury(fence, [&core, oldBuf, oldView] { core.DestroyDescriptor(oldView); core.DestroyBuffer(oldBuf); });
            ++m_instanceGeneration;
        }

        // 2. The staged rows (dirty this frame), then the scratch rows for this slot.
        if (frame && !CopyRows(ctx, frame->stage.rows, frame->stage.values, 0, /*contiguous*/ false))
            return false;
        if (!adHoc.empty())
        {
            std::span<const GpuInstance> rows = adHoc;
            if (rows.size() > kScratchRows)
            {
                if (!m_warnedScratchOverflow)
                {
                    m_warnedScratchOverflow = true;
                    ARC_WARN("[nri-graph] GpuScene: {} ad-hoc instances exceed the {} scratch rows per frame slot -- the rest are dropped",
                             rows.size(), kScratchRows);
                }
                rows = rows.subspan(0, kScratchRows);
            }
            if (!CopyRows(ctx, {}, rows, ScratchFirstRow(frameSlot), /*contiguous*/ true))
                return false;
        }

        // 3. This slot's indirect args and visible indices, whole arrays.
        if (frame && frame->HasDraws())
        {
            if (!EnsureSlotCapacity(frameSlot, frame->rowCount, static_cast<std::uint32_t>(frame->args.size()), graves, fence))
                return false;
            const std::uint64_t argBytes = frame->args.size() * sizeof(DrawIndexedArgs);
            const NriUploadRing::Alloc a = ctx.ring.Allocate(argBytes, 16);
            const std::uint64_t visBytes = frame->visibleIndices.size() * sizeof(std::uint32_t);
            const NriUploadRing::Alloc v = ctx.ring.Allocate(visBytes, 16);
            if (!a.cpu || !v.cpu)
            {
                ARC_ERROR("[nri-graph] GpuScene: the upload ring refused the args/visible arrays ({} + {} bytes)", argBytes, visBytes);
                return false;
            }
            std::memcpy(a.cpu, frame->args.data(), argBytes);
            std::memcpy(v.cpu, frame->visibleIndices.data(), visBytes);
            core.CmdCopyBuffer(ctx.cmd, *m_args[frameSlot], 0, *a.buffer, a.offset, argBytes);
            core.CmdCopyBuffer(ctx.cmd, *m_visible[frameSlot], 0, *v.buffer, v.offset, visBytes);
        }
        // 4. Acknowledge the mirror this frame wrote (spec s5.2): the next Sync
        //    against a DIFFERENT mirror generation (a swapped registry) rebuilds.
        if (frame)
            m_syncedGeneration = frame->stage.generation;
        return true;
    }

    std::uint64_t GpuSceneSyncedGeneration(const GpuScene* device) noexcept
    {
        return device ? device->SyncedGeneration() : 0;   // Host/GpuSceneHost.hpp's NRI-free seam
    }

    void GpuScene::Release(Graveyard& graves, std::uint64_t fence)
    {
        const nri::CoreInterface& core = m_device->Core();
        nri::Buffer* inst = m_instances; nri::Descriptor* instView = m_instancesView;
        nri::Buffer* args[kSwapchainFramesInFlight]; nri::Buffer* vis[kSwapchainFramesInFlight]; nri::Descriptor* visView[kSwapchainFramesInFlight];
        for (std::uint32_t s = 0; s < kSwapchainFramesInFlight; ++s) { args[s] = m_args[s]; vis[s] = m_visible[s]; visView[s] = m_visibleView[s]; }
        graves.Bury(fence, [&core, inst, instView, args, vis, visView]
        {
            if (instView) core.DestroyDescriptor(instView);
            if (inst)     core.DestroyBuffer(inst);
            for (std::uint32_t s = 0; s < kSwapchainFramesInFlight; ++s)
            {
                if (visView[s]) core.DestroyDescriptor(visView[s]);
                if (vis[s])     core.DestroyBuffer(vis[s]);
                if (args[s])    core.DestroyBuffer(args[s]);
            }
        });
        m_instances = nullptr; m_instancesView = nullptr;
        for (std::uint32_t s = 0; s < kSwapchainFramesInFlight; ++s) { m_args[s] = nullptr; m_visible[s] = nullptr; m_visibleView[s] = nullptr; }
    }
}
```

`core.DestroyBuffer` / `core.DestroyDescriptor` are the NRI names (`NriMeshBufferCache.cpp:60-77` buries the same way, capturing `const nri::CoreInterface*`). Growth WHILE a slot's previous frame may still read the old buffer is safe: the old buffer lives until `fence` retires (the mesh-buffer cache's rule).

- [ ] **Step 6: Write `GpuSceneSyncNode.{hpp,cpp}`**

`GpuSceneSyncNode.hpp`:

```cpp
#pragma once

// GpuSceneSyncNode -- the GPU scene's per-frame writer as a graph node (F3,
// spec s8): imports the three persistent buffers, declares them CopyDst,
// and in Record hands GpuScene::Apply the frame's staged rows, this slot's
// args + visible indices, and the ad-hoc rows. Transfer only; no shader.
// Declared BEFORE the mesh node, which Reads the same handles (ShaderRead /
// IndirectArgs) -- the graph derives the copy->read barriers.
#include <NRI.h>

#include <Arcane/Base/Api.hpp>
#include <Arcane/Render/GpuSceneTypes.hpp>
#include <Arcane/Render/Nri/RenderGraph.hpp>

#include <span>

namespace Arcane
{
    class NriGraphContext;

    struct GpuSceneNodeInputs
    {
        RgBuffer instances{};
        RgBuffer args{};
        RgBuffer visibleIndices{};
    };

    // `frame` may be null (no registry-backed scene this frame); `adHoc` may be
    // empty. Both are borrowed for the RenderFrame call, like FrameDesc::pickables.
    ARCANE_API GpuSceneNodeInputs AddGpuSceneSyncNode(RenderGraph& graph, NriGraphContext* context,
                                                       const GpuSceneFrame* frame,
                                                       std::span<const GpuInstance> adHoc);
}
```

`GpuSceneSyncNode.cpp`:

```cpp
#include <Arcane/Render/Nri/nodes/GpuSceneSyncNode.hpp>

#include <Arcane/Render/Nri/GpuScene.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>

namespace Arcane
{
    GpuSceneNodeInputs AddGpuSceneSyncNode(RenderGraph& graph, NriGraphContext* context,
                                           const GpuSceneFrame* frame, std::span<const GpuInstance> adHoc)
    {
        GpuSceneNodeInputs in;
        GpuScene* scene = context ? context->Scene() : nullptr;
        const std::uint32_t slot = context ? context->FrameSlot() : 0;
        graph.AddNode("gpuscene-sync", RenderGraph::NodeKind::Copy,
            [&, scene, slot](RenderGraphBuilder& builder)
            {
                // Device-less declaration-shape drives (RenderGraphTest) pass null buffers; the graph tolerates them.
                in.instances      = builder.ImportBuffer("gpuscene.instances", scene ? scene->Instances() : nullptr,
                                                         scene ? scene->InstanceBytes() : 0);
                in.args           = builder.ImportBuffer("gpuscene.args", scene ? scene->Args(slot) : nullptr, 0);
                in.visibleIndices = builder.ImportBuffer("gpuscene.visible", scene ? scene->VisibleIndices(slot) : nullptr, 0);
                builder.Write(in.instances, RgUsage::CopyDst);
                builder.Write(in.args, RgUsage::CopyDst);
                builder.Write(in.visibleIndices, RgUsage::CopyDst);
            },
            [context, frame, adHoc](RenderGraphNodeContext& nodeContext)
            {
                if (!context) return;
                GpuScene* s = context->Scene();
                if (!s) return;
                s->Apply(frame, adHoc, context->FrameSlot(), nodeContext, context->Graves(), context->CurrentFence());
            });
        return in;
    }
}
```

`context->CurrentFence()` — NEW accessor on `NriGraphContext`: the timeline value THIS frame's submit will signal, i.e. the value a resource still referenced by the command list being recorded must be buried against. The context evicts the mesh cache after a successful Execute against `m_graph->DebugSubmitCount()` (`NriGraphContext.cpp:1782`), the just-submitted frame's value, so mid-record the pending frame's value is `DebugSubmitCount() + 1` — verify against where the executor signals the fence (`RenderGraphExec.cpp:1412-1420`) and implement `CurrentFence()` as that; a wrong-by-one here is a use-after-free the [gpu] round-trip case on a grown buffer would catch (grow once by staging `kInitialRows + 1` rows). If a slot buffer was REPLACED by `EnsureSlotCapacity` inside `Apply`, the imported handle declared at setup names the OLD buffer — so `Apply` grows slot buffers BEFORE the copies and the mesh node resolves `scene->Args(slot)` directly at record time rather than through the handle (Task 7 does exactly that); the handle exists for the barrier, and the executor's imported-buffer start state {NONE, ALL} makes the first-use barrier conservative either way. The instance buffer's growth has the same shape (MeshNode binds `scene->InstancesView()` at record time).

- [ ] **Step 7: The context owns a `GpuScene`**

`NriGraphContext.hpp`: `[[nodiscard]] GpuScene* Scene() noexcept { return m_scene.get(); }` beside `Mesh()`, a `std::unique_ptr<GpuScene> m_scene;` member, the forward declaration. `NriGraphContext.cpp:413`: after `m_meshBuffers = NriMeshBufferCache::Create(*m_device);` add `m_scene = GpuScene::Create(*m_device);` with the same refusal shape (`if (!m_scene) { ARC_ERROR(...); return false; }` — match the surrounding pattern). `:794`: `if (m_scene) m_scene->Release(graves, fence);` beside the mesh node's release.

- [ ] **Step 8: Build and run**

Build; `.\ArcaneTests.exe "[rendergraph]"` and `.\ArcaneTests.exe "[gpu][gpuscene]"` — PASS on both backends; `~[gpu]` whole green; the six golden lanes unchanged (nothing draws through the new node yet).

- [ ] **Step 9: Commit**

```bash
git add ArcaneClient/src/Arcane/Render/Nri/RenderGraph.hpp ArcaneClient/src/Arcane/Render/Nri/RenderGraph.cpp ArcaneClient/src/Arcane/Render/Nri/RenderGraphExec.cpp ArcaneClient/src/Arcane/Render/Nri/GpuScene.hpp ArcaneClient/src/Arcane/Render/Nri/GpuScene.cpp ArcaneClient/src/Arcane/Render/Nri/nodes/GpuSceneSyncNode.hpp ArcaneClient/src/Arcane/Render/Nri/nodes/GpuSceneSyncNode.cpp ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.hpp ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.cpp ArcaneTests/src/RenderGraphTest.cpp ArcaneTests/src/NriGraphPixelTest.cpp
git commit -m "feat(nri): the GPU scene's device half -- one persistent 240 B-row instance buffer (doubling growth, buried on grow), per-slot indirect-args + visible-index buffers, scratch rows for ad-hoc instances, GpuSceneSyncNode copying through the upload ring; RgUsage::IndirectArgs (ARGUMENT_BUFFER/INDIRECT); [gpu] byte-exact round trip on both backends (F3 plan 1 T6)"
```

---
### Task 7: The `MeshNode` rewrite — structured instance reads, indirect batches, direct ad-hoc rows

**Files:**
- Create: `data/shaders/gpu_scene.hlsli`; Modify: `data/shaders/mesh.hlsl`, `data/shaders/compile-shaders.bat:34` (`SPIRV_FLAGS`), `ArcaneClient/src/Arcane/Render/ShaderConventions.hpp:44-56` (`kSpirvArgs`)
- Modify: `ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.hpp` (`MeshInstance` comment, `MeshSceneDesc`, the class), `MeshNode.cpp` (`MeshRootConstants` :96-103, `PoolSizes` :251, `CreateBindings` :274, `CreateSets` :508, `Prepare` :767, `Record` :807, `AddMeshNode` :1005)
- Modify: `ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.cpp:1250` (`wantsMesh`)
- Test: `ArcaneTests/src/MeshNodeTest.cpp`, `ArcaneTests/src/NriGraphPixelTest.cpp`, `ArcaneTests/src/RenderGraphTest.cpp`

**Interfaces:**
- Consumes: `GpuScene` (`InstancesView`, `VisibleIndicesView(slot)`, `Args(slot)`, `ScratchFirstRow(slot)`, `InstanceBufferGeneration`, `kScratchRows`), `AddGpuSceneSyncNode`, `GpuSceneFrame`, `GpuInstance`, `NormalMatrixFor`.
- Produces:
  ```cpp
  struct MeshSceneDesc {
      std::span<const MeshInstance> instances;   // AD-HOC rows: registry-less callers; drawn direct, unculled, in order (<= GpuScene::kScratchRows)
      const GpuSceneFrame* scene = nullptr;      // the registry-backed scene (hosts, Task 8)
      glm::mat4 view{1.0f}, projection{1.0f}; glm::vec3 lightDirection{0,0,1}, lightColor{1}, ambient{0.05f};
      [[nodiscard]] bool Empty() const noexcept { return instances.empty() && !(scene && scene->HasDraws()); }
  };
  struct MeshRootConstants { std::uint32_t firstOutput; std::uint32_t flags; };   // 8 B; flags bit0 = direct (firstOutput IS the row)
  // MeshNode: Prepare(...) additionally builds AdHocRows(); Record(ctx, scene, frameSlot, GpuScene*) draws scene->batches indirect then the ad-hoc rows direct.
  [[nodiscard]] std::span<const GpuInstance> MeshNode::AdHocRows() const noexcept;
  ```
  `AddMeshNode(graph, context, canvas, canvasFormat, scene, width, height)` keeps its signature and now declares `gpuscene-sync` BEFORE `mesh` internally.

- [ ] **Step 1: Write the failing tests**

`MeshNodeTest.cpp` — add:

```cpp
TEST_CASE("MeshSceneDesc::Empty: no ad-hoc rows and no scene draws", "[mesh][node]")
{
    Arcane::MeshSceneDesc d;
    CHECK(d.Empty());
    Arcane::GpuSceneFrame f;
    d.scene = &f;
    CHECK(d.Empty());                       // a frame with no emitted batches is empty
    f.batches.push_back(Arcane::GpuBatchDraw{});
    CHECK_FALSE(d.Empty());
    d.scene = nullptr;
    const Arcane::MeshInstance one{};
    d.instances = std::span<const Arcane::MeshInstance>(&one, 1);
    CHECK_FALSE(d.Empty());
}

TEST_CASE("MeshRootConstants is the 8-byte {firstOutput, flags} block", "[mesh][node]")
{
    CHECK(sizeof(Arcane::MeshRootConstants) == 8);
}
```

(`MeshRootConstants` moves from `MeshNode.cpp`'s anonymous namespace into the header for this pin.)

`RenderGraphTest.cpp` — beside the existing `FrameDesc` declaration-shape cases (search `"mesh"` in that file for how a `MeshSceneDesc` with instances is driven device-less):

```cpp
TEST_CASE("declaration shape: a registry-backed scene declares gpuscene-sync then mesh; an empty one declares neither", "[rendergraph][mesh]")
{
    // Drive DeclareGraphFrame the way the file's other mesh shape cases do, with:
    //   (a) FrameDesc::mesh -> a MeshSceneDesc whose `scene` has one emitted batch -> node names contain "gpuscene-sync" immediately before "mesh";
    //   (b) FrameDesc::mesh -> a MeshSceneDesc with one ad-hoc instance and no scene -> the same two nodes;
    //   (c) FrameDesc::mesh -> a MeshSceneDesc with neither -> no node named "gpuscene-sync" and none named "mesh".
}
```

`NriGraphPixelTest.cpp` — the existing four `mesh:` cases keep passing through the ad-hoc path (that is the pin that the scratch path renders identically). Add the registry-backed case:

```cpp
    void CheckGpuSceneDrawsAndCulls(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        const std::uint64_t before = Arcane::RenderErrorCount();
        // A registry with the scene root, a MeshTable holding BuildCube(2.0f) under cubeId, one red cube entity at the
        // origin and one at (1000, 0, 0); TransformPropagationSystem + BoundsSystem; a VisibleSet from the SAME camera
        // FillCamera() puts on the MeshSceneDesc (build a ViewTransform from those matrices); GpuSceneMirror + GpuSceneSync
        // + BuildGpuSceneFrame -> `frame`. Supply the cube to the context exactly as CaptureMesh's SupplyOne does.
        Arcane::MeshSceneDesc scene;
        scene.scene = &frame;
        FillCamera(scene);
        std::uint32_t w = 0, h = 0;
        const std::vector<unsigned char> lit = CaptureMesh(backend, scene, w, h, SupplyOne(cubeId, cube));
        const Rgba centre = At(lit, w, w / 2u, h / 2u);
        const Rgba corner = At(lit, w, 10u, 10u);
        CHECK(centre.r > centre.g + 60);
        CHECK(centre.r > corner.r + 120);
        CHECK(frame.stats.total == 2);
        CHECK(frame.stats.coarseVisible == 1);          // the (1000,0,0) cube is outside the frustum
        CHECK(frame.args[0].instanceNum == 1);

        // Now a VisibleSet that admits ONLY the far cube: the indirect draw has instanceNum 0 and the centre is background.
        Arcane::VisibleSet onlyFar;                      // build: Clear, Insert(farEntity, its box, 0)
        Arcane::GpuSceneFrame culled;
        Arcane::BuildGpuSceneFrame(mirror, &onlyFar, &meshTable, view, culled);
        CHECK(culled.args.empty() || culled.args[0].instanceNum == 0);   // the batch is emitted only if the far row is visible
        scene.scene = &culled;
        const std::vector<unsigned char> dark = CaptureMesh(backend, scene, w, h, SupplyOne(cubeId, cube));
        const Rgba centreDark = At(dark, w, w / 2u, h / 2u);
        CHECK(centreDark.r < 96);
        CHECK(Arcane::RenderErrorCount() == before);
    }
    TEST_CASE("gpuscene: a registry-backed cube draws through the indirect path and a culled one does not (d3d12)", "[gpu][gpuscene][mesh]") { CheckGpuSceneDrawsAndCulls(Arcane::GraphicsBackend::D3D12); }
    TEST_CASE("gpuscene: a registry-backed cube draws through the indirect path and a culled one does not (vulkan)", "[gpu][gpuscene][mesh]") { CheckGpuSceneDrawsAndCulls(Arcane::GraphicsBackend::Vulkan); }
```

`CaptureMesh` renders one frame through the context; `GpuSceneSync` was driven with `deviceSyncedGeneration = 0` so the frame is a full rebuild, and the test never calls `SetSyncedGeneration` (a second capture with the same mirror re-stages everything — fine for a test).

- [ ] **Step 2: Run to verify they fail**

Build — `MeshSceneDesc::scene`, `MeshRootConstants` in the header, and the new node order are all missing.

- [ ] **Step 3: The shaders**

`data/shaders/gpu_scene.hlsli`:

```hlsl
// The GPU scene row (F3, spec s5.1). MIRRORS Arcane::GpuInstance
// (ArcaneClient/src/Arcane/Render/GpuSceneTypes.hpp) field for field: 240
// bytes -- change both or neither. Shared by mesh.hlsl and (plan 2)
// mesh_cull.hlsl.
#ifndef ARCANE_GPU_SCENE_HLSLI
#define ARCANE_GPU_SCENE_HLSLI

struct GpuInstance
{
    float4x4 model;
    float4x4 prevModel;
    float4   normal0;        // xyz: NormalMatrixFor(model) columns (CPU-computed, R8)
    float4   normal1;
    float4   normal2;
    float4   boundsMin;      // world AABB; w unused
    float4   boundsMax;      // w = alphaCutoff for masked rows (plan 2)
    float4   baseColor;
    uint     materialSlot;   // kMeshInvalidMaterialSlot = the flat path
    uint     batch;
    uint     flags;
    uint     pad;
};

#define kGpuInstanceFlagTeleported 1u

#endif
```

`data/shaders/mesh.hlsl` — the whole file becomes:

```hlsl
// mesh.hlsl -- the opaque 3D pass, reading its per-instance data from the GPU
// scene (F3 plan 1). ONE 8-byte root block {firstOutput, flags}: an indirect
// batch draw reads row = g_VisibleIndices[firstOutput + SV_InstanceID]; a
// DIRECT draw (flags & 1) reads row = firstOutput itself (ad-hoc scratch rows,
// plan 2's transparent rows). Lighting is unchanged from the F2a pass: one
// directional light, Lambert + a constant ambient, one albedo texture through
// the bindless table or the flat baseColor path.
#include "gpu_scene.hlsli"

struct MeshRoot
{
    uint firstOutput;
    uint flags;
};
#define kMeshRootDirect 1u

#if SPIRV
[[vk::push_constant]] ConstantBuffer<MeshRoot> g_PC;
#define g_firstOutput g_PC.firstOutput
#define g_flags       g_PC.flags
#else
cbuffer MeshRootCB : register(b0)
{
    MeshRoot g_PCData;
}
#define g_firstOutput g_PCData.firstOutput
#define g_flags       g_PCData.flags
#endif

cbuffer MeshFrameCB : register(b1, space1)
{
    float4x4 g_viewProjection;
    float4   g_lightDirection;
    float4   g_lightColor;       // rgb: linear radiance
    float4   g_ambient;          // rgb: the constant ambient term
};

StructuredBuffer<GpuInstance> g_Instances      : register(t0, space1);
StructuredBuffer<uint>        g_VisibleIndices : register(t1, space1);

struct VSInput
{
    float3 pos    : POSITION;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD0;
};

struct VSOutput
{
    float4 pos    : SV_Position;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD0;
    nointerpolation float4 baseColor : COLOR0;
    nointerpolation uint   slot      : TEXCOORD1;
};

VSOutput vs_main(VSInput input, uint instanceId : SV_InstanceID)
{
    const uint row = (g_flags & kMeshRootDirect) ? g_firstOutput
                                                 : g_VisibleIndices[g_firstOutput + instanceId];
    const GpuInstance inst = g_Instances[row];

    VSOutput output;
    const float4 world = mul(inst.model, float4(input.pos, 1.0));
    output.pos    = mul(g_viewProjection, world);
    output.normal = input.normal.x * inst.normal0.xyz
                  + input.normal.y * inst.normal1.xyz
                  + input.normal.z * inst.normal2.xyz;
    output.uv        = input.uv;
    output.baseColor = inst.baseColor;
    output.slot      = inst.materialSlot;
    return output;
}

#define kMeshBindlessCapacity 256
Texture2D<float4> g_BindlessTextures[kMeshBindlessCapacity] : register(t0, space2);
SamplerState g_Sampler : register(s0);
#define kMeshInvalidMaterialSlot 0xFFFFFFFFu

float4 ps_main(VSOutput input) : SV_Target0
{
    const float3 n     = normalize(input.normal);
    const float  ndotl = saturate(dot(n, g_lightDirection.xyz));
    const float4 albedo = (input.slot == kMeshInvalidMaterialSlot)
        ? input.baseColor
        : g_BindlessTextures[NonUniformResourceIndex(input.slot)].Sample(g_Sampler, input.uv) * input.baseColor;
    const float3 lit = albedo.rgb * (g_ambient.rgb + g_lightColor.rgb * ndotl);
    return float4(lit, albedo.a);
}
```

`compile-shaders.bat:34` — append ` -fvk-t-shift 0 1` to `SPIRV_FLAGS` (t0/t1 in space1 land at bindings 0/1 of set 1; b1 there is at 257). `ShaderConventions.hpp:54-55` — add the matching entry `"-fvk-t-shift", "0", "1",` with the comment `// mesh.hlsl (F3): the instance and visible-index SRVs at t0/t1 in space1`. dxc resolves `#include "gpu_scene.hlsli"` relative to the source file; if the prebuild's `:compile` label passes a working directory that breaks that, add `-I %~dp0` to its dxc line.

- [ ] **Step 4: `MeshNode.hpp`**

- `#include <Arcane/Render/GpuSceneTypes.hpp>` and forward-declare `class GpuScene;`.
- `MeshInstance`'s header comment: it is now the AD-HOC instance (registry-less callers; drawn direct, unculled, through `GpuScene::kScratchRows` scratch rows per frame slot); fields unchanged.
- `MeshSceneDesc`: add `const GpuSceneFrame* scene = nullptr;` (comment: the registry-backed scene; BORROWED for the RenderFrame call like `instances`) and `Empty()` as in Interfaces; the `instances` comment now says "EMPTY IS LEGAL; see Empty()".
- Move `MeshRootConstants` into the header (public, above the class):

```cpp
    // THE 8-BYTE ROOT BLOCK (F3): `firstOutput` is the batch's start in the
    // visible-index buffer for an indirect draw, or THE ROW ITSELF when
    // `flags & kMeshRootDirect`. mesh.hlsl's MeshRoot.
    struct MeshRootConstants
    {
        std::uint32_t firstOutput = 0;
        std::uint32_t flags       = 0;
    };
    inline constexpr std::uint32_t kMeshRootDirect = 1u;
    static_assert(sizeof(MeshRootConstants) == 8);
```

- The class: `Prepare` keeps its signature; add `[[nodiscard]] std::span<const GpuInstance> AdHocRows() const noexcept { return m_adHocRows; }`; private members `std::vector<GpuInstance> m_adHocRows; struct AdHocDraw { Guid mesh; std::uint32_t indexOffset, indexCount; }; std::vector<AdHocDraw> m_adHocDraws; std::uint64_t m_setInstanceGen[kSwapchainFramesInFlight] = {}; const nri::Descriptor* m_setVisibleView[kSwapchainFramesInFlight] = {};`. Update the header's WHAT THIS NODE OWNS prose: the frame set is `{ b1, t0 instances, t1 visibleIndices }` and is REWRITTEN per slot when the GPU scene's buffers change (safe: that slot's previous frame retired at BeginFrame).

- [ ] **Step 5: `MeshNode.cpp`**

`CreateBindings`: `rootConstant.size = sizeof(MeshRootConstants);` (8). The frame set gets three ranges:

```cpp
        nri::DescriptorRangeDesc frameRanges[3] = {};
        frameRanges[0].baseRegisterIndex = 1;   // b1
        frameRanges[0].descriptorNum     = 1;
        frameRanges[0].descriptorType    = nri::DescriptorType::CONSTANT_BUFFER;
        frameRanges[0].shaderStages      = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;
        frameRanges[1].baseRegisterIndex = 0;   // t0 -- the instance rows
        frameRanges[1].descriptorNum     = 1;
        frameRanges[1].descriptorType    = nri::DescriptorType::STRUCTURED_BUFFER;
        frameRanges[1].shaderStages      = nri::StageBits::VERTEX_SHADER;
        frameRanges[2].baseRegisterIndex = 1;   // t1 -- this slot's visible indices
        frameRanges[2].descriptorNum     = 1;
        frameRanges[2].descriptorType    = nri::DescriptorType::STRUCTURED_BUFFER;
        frameRanges[2].shaderStages      = nri::StageBits::VERTEX_SHADER;
        for (auto& r : frameRanges) r.flags = nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;
        nri::DescriptorSetDesc frameSetDesc = {};
        frameSetDesc.registerSpace = 1;
        frameSetDesc.ranges        = frameRanges;
        frameSetDesc.rangeNum      = 3;
        frameSetDesc.flags         = nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET;
```

`PoolSizes`: `poolDesc.structuredBufferMaxNum = 2 * kFrameSets;`. `CreateSets` writes only range 0 (b1) as today; ranges 1-2 are written lazily in `Record` (below) because the visible-index view is per slot and the instance view changes on growth.

`Prepare`:

```cpp
    void MeshNode::Prepare(nri::Format canvasFormat, const MeshSceneDesc& scene,
                           NriMeshBufferCache* meshBuffers, std::uint64_t frameCounter)
    {
        m_pipeline = PipelineFor(canvasFormat);
        m_residents.clear();
        m_adHocRows.clear();
        m_adHocDraws.clear();
        if (!meshBuffers)
            return;
        const auto resolveOnce = [&](const Guid& id)
        {
            if (id.IsNil()) return;
            for (const auto& e : m_residents) if (e.first == id) return;
            m_residents.push_back({ id, meshBuffers->Resolve(id, frameCounter) });
        };
        if (scene.scene)
            for (const GpuBatchDraw& b : scene.scene->batches)
                resolveOnce(b.mesh);
        for (const MeshInstance& instance : scene.instances)
        {
            if (instance.mesh.IsNil()) continue;
            if (m_adHocRows.size() >= GpuScene::kScratchRows) break;   // GpuScene::Apply warns once for the overflow
            resolveOnce(instance.mesh);
            GpuInstance row;
            row.model = instance.model;
            const glm::mat3 n = NormalMatrixFor(instance.model);
            row.normal0 = glm::vec4(n[0], 0.0f); row.normal1 = glm::vec4(n[1], 0.0f); row.normal2 = glm::vec4(n[2], 0.0f);
            row.baseColor    = instance.baseColor;
            row.materialSlot = instance.materialSlot;
            m_adHocRows.push_back(row);
            m_adHocDraws.push_back(AdHocDraw{ instance.mesh, instance.indexOffset, instance.indexCount });
        }
    }
```

`Record` — after the existing early-outs (`scene.instances.empty()` becomes `scene.Empty()`), the finite-camera check, the layout fetch, the frame-constants write and the descriptor-pool/layout binds, replace the per-instance loop with:

```cpp
        if (!gpuScene)   // the new trailing Record parameter (AddMeshNode passes context->Scene())
        {
            GraphError("MeshNode: no GpuScene on the context -- nothing recorded");
            return;
        }
        // The frame set's t0/t1 views: rewritten for THIS slot when the GPU
        // scene's buffers changed. Safe: this slot's previous frame retired at
        // BeginFrame, so nothing in flight reads the set.
        if (m_setInstanceGen[frameSlot] != gpuScene->InstanceBufferGeneration()
            || m_setVisibleView[frameSlot] != gpuScene->VisibleIndicesView(frameSlot))
        {
            const nri::Descriptor* views[2] = { gpuScene->InstancesView(), gpuScene->VisibleIndicesView(frameSlot) };
            nri::UpdateDescriptorRangeDesc updates[2] = {};
            for (int i = 0; i < 2; ++i)
            {
                updates[i].descriptorSet = set;
                updates[i].rangeIndex    = static_cast<std::uint32_t>(1 + i);
                updates[i].descriptors   = &views[i];
                updates[i].descriptorNum = 1;
            }
            core.UpdateDescriptorRanges(updates, 2);
            m_setInstanceGen[frameSlot]  = gpuScene->InstanceBufferGeneration();
            m_setVisibleView[frameSlot]  = gpuScene->VisibleIndicesView(frameSlot);
        }
        // ...the existing CmdSetDescriptorSet x2 + CmdSetPipeline calls stay here...

        Guid lastMesh{};
        bool lastBound = false;
        const auto bindMesh = [&](const Guid& id) -> const NriMeshBufferCache::Resident*
        {
            const NriMeshBufferCache::Resident* resident = residentFor(id);
            if (!resident || !resident->ready || !resident->vertexBuffer || !resident->indexBuffer)
                return nullptr;
            if (!lastBound || id != lastMesh)
            {
                lastMesh = id; lastBound = true;
                nri::VertexBufferDesc vertexBuffer = {};
                vertexBuffer.buffer = resident->vertexBuffer;
                vertexBuffer.stride = sizeof(MeshVertex);
                core.CmdSetVertexBuffers(context.cmd, 0, &vertexBuffer, 1);
                core.CmdSetIndexBuffer(context.cmd, *resident->indexBuffer, 0, nri::IndexType::UINT32);
            }
            return resident;
        };
        const auto pushRoot = [&](std::uint32_t firstOutput, std::uint32_t flags)
        {
            const MeshRootConstants push{ firstOutput, flags };
            nri::SetRootConstantsDesc rootConstants = {};
            rootConstants.rootConstantIndex = 0;
            rootConstants.data              = &push;
            rootConstants.size              = sizeof(push);
            core.CmdSetRootConstants(context.cmd, rootConstants);
        };

        // 1. The registry-backed batches, indirect, in the frame's order.
        if (scene.scene && scene.scene->HasDraws())
        {
            nri::Buffer* args = gpuScene->Args(frameSlot);
            for (const GpuBatchDraw& batch : scene.scene->batches)
            {
                if (!bindMesh(batch.mesh))
                    continue;   // not resident: skip, never a stale bind
                pushRoot(batch.firstOutput, 0);
                core.CmdDrawIndexedIndirect(context.cmd, *args,
                                            std::uint64_t(batch.argIndex) * sizeof(nri::DrawIndexedDesc),
                                            1, sizeof(nri::DrawIndexedDesc), nullptr, 0);
            }
        }
        // 2. The ad-hoc rows, direct, in submission order, from this slot's scratch region.
        const std::uint32_t scratchFirst = gpuScene->ScratchFirstRow(frameSlot);
        for (std::size_t i = 0; i < m_adHocDraws.size(); ++i)
        {
            const AdHocDraw& d = m_adHocDraws[i];
            const NriMeshBufferCache::Resident* resident = bindMesh(d.mesh);
            if (!resident)
                continue;
            pushRoot(scratchFirst + static_cast<std::uint32_t>(i), kMeshRootDirect);
            nri::DrawIndexedDesc draw = {};
            draw.baseIndex   = d.indexOffset;
            draw.indexNum    = d.indexCount ? d.indexCount : resident->indexCount;
            draw.instanceNum = 1;
            core.CmdDrawIndexed(context.cmd, draw);
        }
```

Delete `PackedNormalMatrix` and the old `MeshRootConstants` from the anonymous namespace. `Record`'s signature gains a trailing `GpuScene* gpuScene` (the node keeps `m_device`/`m_pipelines` only, `MeshNode.cpp:167-171`, and should not start holding the context).

`AddMeshNode` — declare the sync node first and read its handles in the mesh node's setup:

```cpp
    RgTexture AddMeshNode(RenderGraph& graph, NriGraphContext* context,
                          RgTexture canvas, nri::Format canvasFormat,
                          const MeshSceneDesc& scene,
                          std::uint32_t width, std::uint32_t height)
    {
        std::span<const GpuInstance> adHoc;
        if (context)
        {
            if (MeshNode* node = context->Mesh())
            {
                node->Prepare(canvasFormat, scene, context->MeshBuffers(), context->PresentedFrames());
                adHoc = node->AdHocRows();
            }
        }
        const GpuSceneNodeInputs inputs = AddGpuSceneSyncNode(graph, context, scene.scene, adHoc);
        RgTexture depth{};
        graph.AddNode("mesh", RenderGraph::NodeKind::Raster,
            [&](RenderGraphBuilder& builder)
            {
                // ...the existing depth transient + canvas/depth writes + attachments...
                builder.Read(inputs.instances, RgUsage::ShaderRead);
                builder.Read(inputs.visibleIndices, RgUsage::ShaderRead);
                builder.Read(inputs.args, RgUsage::IndirectArgs);
            },
            [context, scene](RenderGraphNodeContext& nodeContext)
            {
                if (!context) return;
                if (MeshNode* node = context->Mesh())
                    node->Record(nodeContext, scene, context->FrameSlot(), context->Scene());
            });
        return depth;
    }
```

`NriGraphContext.cpp:1250`: `const bool wantsMesh = shape.mesh != nullptr && !shape.mesh->Empty();`. Add `#include <Arcane/Render/Nri/nodes/GpuSceneSyncNode.hpp>` and `<Arcane/Render/Nri/GpuScene.hpp>` to `MeshNode.cpp`.

- [ ] **Step 6: Build, then run the pins and the pixel cases on both backends**

`.\ArcaneTests.exe "[mesh]"`, `"[rendergraph]"`, then `"[gpu][mesh]"` and `"[gpu][gpuscene]"` — all PASS on D3D12 and Vulkan; the pre-existing `mesh:` pixel cases (ad-hoc path) unchanged. Then the whole `~[gpu]` suite.

- [ ] **Step 7: Commit**

```bash
git add data/shaders/gpu_scene.hlsli data/shaders/mesh.hlsl data/shaders/compile-shaders.bat ArcaneClient/src/Arcane/Render/ShaderConventions.hpp ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.hpp ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.cpp ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.cpp ArcaneTests/src/MeshNodeTest.cpp ArcaneTests/src/NriGraphPixelTest.cpp ArcaneTests/src/RenderGraphTest.cpp
git commit -m "feat(nri): MeshNode reads its instance from the GPU scene -- an 8-byte root block {firstOutput, flags}, structured t0/t1 SRVs rewritten per slot on growth, one CmdDrawIndexedIndirect per batch, ad-hoc instances drawn direct from scratch rows; mesh.hlsl by SV_InstanceID; the 128-byte MeshConstants block is gone; [gpu] draws-and-culls on both backends (F3 plan 1 T7)"
```

---
### Task 8: The hosts — visibility + sync + frame per frame; `CollectMeshInstances` retired; the witness counts

**Files:**
- Create: `ArcaneClient/src/Arcane/Host/GpuSceneHost.hpp`
- Modify: `ArcaneEditor/src/App/EditorAppFrame.cpp` (the `SubmitRender` bracket at ~1724-1730; the mesh-scene assembly at ~1887-1910), `ArcaneEditor/src/App/EditorApp.hpp:1253-1270` (members), `ArcaneEditor/src/App/EditorApp.cpp:2929` (the witness)
- Modify: `ArcaneRuntime/src/RuntimeFrame.cpp` (:436-444 the `SubmitRender` bracket; :545-592 the mesh scene), `RuntimeFrame.hpp:112`, `RuntimeApp.hpp:182-183`, `RuntimeApp.cpp:1252` (the witness)
- Modify: `ArcaneClient/src/Arcane/Host/VerifyReport.{hpp,cpp}` (`SetVisibility`, `j["visibility"]`)
- Delete: `ArcaneClient/src/Arcane/Render/MeshSubmissionSystem.hpp`
- Test: `ArcaneTests/src/GpuSceneSyncTest.cpp` (one host-helper case), `ArcaneTests/src/VerifyReportTest.cpp` (or wherever `AddCensus`'s JSON is pinned — search `"census"` in `ArcaneTests/src`)

**Interfaces:**
- Consumes: `BuildVisibleSet`, `GpuSceneSync`, `BuildGpuSceneFrame`, `GpuScene::SyncedGeneration`, `ClientRuntime::View()/Registry()`, `ActivePerspectiveSceneCamera`, `MeshSceneDesc::scene`.
- Produces:
  ```cpp
  // Host/GpuSceneHost.hpp -- the three calls every host makes between the schedulers and the render scheduler.
  // `meshView` is the view the mesh pass will draw with (Edit: the editor view; Play: the perspective scene camera, or nullopt = no mesh pass).
  // views[0] = mainView (sprites, picking); views[1] = meshView when it differs (the mesh pass culls against ITS camera).
  void PrepareSceneForRender(Astra::Registry& reg, const ViewTransform& mainView, const std::optional<ViewTransform>& meshView,
                             const GpuScene* device, GpuSceneFrame& out);
  void VerifyReport::SetVisibility(std::uint32_t total, std::uint32_t coarseVisible, std::uint32_t batches, std::uint32_t draws);   // -> j["visibility"] = { "total", "coarseVisible", "gpuVisible" (== coarseVisible until plan 2), "batches", "draws" }
  ```

- [ ] **Step 1: Write the failing tests**

`GpuSceneSyncTest.cpp` — add:

```cpp
TEST_CASE("PrepareSceneForRender: fills views[0] from the main view and views[1] from a differing mesh view; the frame culls against the mesh view", "[gpuscene][host]")
{
    World w;
    Astra::Entity e = w.Spawn(glm::vec3(0, 0, -5), w.Mesh());
    Arcane::TransformPropagationSystem{}(w.reg);
    Arcane::BoundsSystem{}(w.reg);
    const Arcane::ViewTransform main = Arcane::ViewTransform::Orthographic(glm::vec2(0.0f), 10.0f, glm::uvec2{ 800, 600 });
    const Arcane::ViewTransform mesh = Arcane::ViewTransform::Perspective(glm::vec3(0, 0, 10), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0),
                                                                          60.0f, glm::uvec2{ 800, 600 }, 0.1f, 100.0f);
    Arcane::GpuSceneFrame frame;
    Arcane::PrepareSceneForRender(w.reg, main, mesh, nullptr, frame);
    const Arcane::SceneVisibility* sv = w.reg.GetResource<Arcane::SceneVisibility>();
    REQUIRE(sv);
    REQUIRE(sv->views.size() == 2);
    CHECK(sv->views[0].Contains(e));
    CHECK(sv->views[1].Contains(e));
    CHECK(frame.stats.coarseVisible == 1);
    CHECK(frame.HasDraws());
    // The same main view twice: one view only, and the frame reads views[0].
    Arcane::PrepareSceneForRender(w.reg, main, main, nullptr, frame);
    CHECK(w.reg.GetResource<Arcane::SceneVisibility>()->views.size() == 1);
    // No mesh view: the frame has no draws but views[0] is still built (sprites and picking cull).
    Arcane::PrepareSceneForRender(w.reg, main, std::nullopt, nullptr, frame);
    CHECK_FALSE(frame.HasDraws());
    CHECK(w.reg.GetResource<Arcane::SceneVisibility>()->views.size() == 1);
}
```

The report pin — beside the `census` JSON case:

```cpp
TEST_CASE("VerifyReport: SetVisibility emits the visibility block", "[verify]")
{
    Arcane::VerifyReport report;
    report.SetRun("d3d12", 1, "frames");
    report.SetVisibility(12, 7, 3, 3);
    const nlohmann::json j = nlohmann::json::parse(report.ToJson());   // use the file's existing serialise accessor name
    REQUIRE(j.contains("visibility"));
    CHECK(j["visibility"]["total"] == 12);
    CHECK(j["visibility"]["coarseVisible"] == 7);
    CHECK(j["visibility"]["gpuVisible"] == 7);
    CHECK(j["visibility"]["batches"] == 3);
    CHECK(j["visibility"]["draws"] == 3);
}
```

- [ ] **Step 2: Run to verify they fail**

Build — `Arcane/Host/GpuSceneHost.hpp` and `SetVisibility` missing.

- [ ] **Step 3: Write `GpuSceneHost.hpp`**

```cpp
#pragma once

// PrepareSceneForRender -- the host's three calls per frame (F3, spec s4/s5):
// build the visible set(s), sync the GPU-scene mirror, build the frame. Called
// AFTER the fixed/update schedulers (WorldTransform + WorldBounds current) and
// BEFORE RunLoop::SubmitRender (the sprite sweep reads views[0]). Header-only
// so the two frame drivers (EditorAppFrame.cpp, RuntimeFrame.cpp) share one
// definition and ArcaneTests can drive it without a device.
//
// TWO VIEWS when they differ: in Play the sprites draw with View() (the scene
// camera's orthographic view) while the mesh pass draws with the perspective
// scene camera (RuntimeFrame.cpp's ActivePerspectiveSceneCamera). Each pass
// culls against its own camera: views[0] = main, views[1] = mesh. F5 unifies
// the two passes under one view; until then the second set is the honest one.
#include <Arcane/Render/GpuSceneSync.hpp>
#include <Arcane/Render/GpuSceneTypes.hpp>
#include <Arcane/Render/VisibilitySystem.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/ViewTransform.hpp>

#include <Astra/Registry/Registry.hpp>

#include <optional>

namespace Arcane
{
    class GpuScene;   // Render/Nri/GpuScene.hpp; only SyncedGeneration() is read here, through the inline below

    [[nodiscard]] ARCANE_API std::uint64_t GpuSceneSyncedGeneration(const GpuScene* device) noexcept;   // GpuScene.cpp; 0 for null

    inline bool SameView(const ViewTransform& a, const ViewTransform& b) noexcept
    {
        return a.view == b.view && a.projection == b.projection && a.viewport == b.viewport;
    }

    inline void PrepareSceneForRender(Astra::Registry& reg, const ViewTransform& mainView,
                                      const std::optional<ViewTransform>& meshView,
                                      const GpuScene* device, GpuSceneFrame& out)
    {
        SceneVisibility* sv = reg.GetResource<SceneVisibility>();
        if (!sv) sv = reg.EmplaceResource<SceneVisibility>();
        const bool twoViews = meshView && !SameView(*meshView, mainView);
        sv->views.resize(twoViews ? 2 : 1);
        BuildVisibleSet(reg, mainView, sv->views[0]);
        if (twoViews)
            BuildVisibleSet(reg, *meshView, sv->views[1]);

        GpuSceneMirror* mirror = reg.GetResource<GpuSceneMirror>();
        if (!mirror) mirror = reg.EmplaceResource<GpuSceneMirror>();
        GpuSceneSync(reg, *mirror, GpuSceneSyncedGeneration(device), out.stage);

        if (!meshView)
        {
            out.batches.clear(); out.args.clear(); out.visibleIndices.clear();
            out.rowCount = out.stage.rowCapacity; out.stats = {};
            return;   // no mesh pass this frame; the stage still lands (rows stay current for the frame that has one)
        }
        const VisibleSet* meshVis = twoViews ? &sv->views[1] : &sv->views[0];
        BuildGpuSceneFrame(*mirror, meshVis, reg.GetResource<MeshTable>(), *meshView, out);
    }
}
```

`GpuSceneSyncedGeneration` is defined in `GpuScene.cpp` (Task 6) so this header stays NRI-free (`#include <Arcane/Base/Api.hpp>` for the macro).

**The stage's generation:** `GpuSceneStage::generation` (Task 5, set by `GpuSceneSync` to `m.generation`) is what `GpuScene::Apply` stamps as synced on success (Task 6, step 4 of `Apply`) — the device acknowledges the mirror it just wrote; a swapped registry brings a mirror with a new generation, and the next Sync sees the mismatch and rebuilds (spec §5.2). `GpuSceneSyncedGeneration(const GpuScene*)` is the NRI-free free function `GpuScene.cpp` defines for this header.

- [ ] **Step 4: The editor**

`EditorApp.hpp:1253-1270`: replace `std::vector<Arcane::MeshInstance> m_meshInstances;` with `Arcane::GpuSceneFrame m_gpuSceneFrame;` and `std::optional<Arcane::ViewTransform> m_meshView;` (comment: the frame `MeshSceneDesc::scene` borrows for the RenderFrame call; rebuilt every frame by `PrepareSceneForRender`). Keep `m_meshScene`.

`EditorAppFrame.cpp` — immediately BEFORE `m_runtime->SetRenderContext(&b); m_runtime->Loop().SubmitRender();` (~1728):

```cpp
        // F3: the CPU visible set(s), the GPU-scene sync and the frame's batches,
        // between the schedulers (WorldBounds current) and the sprite sweep
        // (which reads views[0]). The mesh view is the editor view in Edit and
        // the perspective scene camera in Play (RuntimeFrame.cpp's rule).
        m_meshView.reset();
        if (!InPlayMode())
            m_meshView = m_runtime->View();
        else if (const auto cam = Arcane::ActivePerspectiveSceneCamera(
                     m_runtime->Registry(), glm::uvec2{ ViewportWidth(), ViewportHeight() }))
        {
            Arcane::ViewTransform v;
            v.view = cam->view; v.projection = cam->projection; v.viewport = glm::uvec2{ ViewportWidth(), ViewportHeight() };
            m_meshView = v;
        }
        Arcane::PrepareSceneForRender(m_runtime->Registry(), m_runtime->View(), m_meshView,
                                      gpuContext->Scene(), m_gpuSceneFrame);   // gpuContext: the NriGraphContext whose Batch() the line above uses
```

and the mesh-scene assembly at ~1887-1910 becomes:

```cpp
        if (m_meshView && m_gpuSceneFrame.HasDraws())
        {
            m_meshScene.scene      = &m_gpuSceneFrame;
            m_meshScene.view       = m_meshView->view;
            m_meshScene.projection = m_meshView->projection;
            vp.mesh = &m_meshScene;
        }
```

Delete the `#include <Arcane/Render/MeshSubmissionSystem.hpp>` (:37); add `<Arcane/Host/GpuSceneHost.hpp>`. `EditorApp.cpp:2929` (the witness, beside `AddCensus`): `report.SetVisibility(m_gpuSceneFrame.stats.total, m_gpuSceneFrame.stats.coarseVisible, m_gpuSceneFrame.stats.batches, m_gpuSceneFrame.stats.draws);`.

- [ ] **Step 5: The runtime**

`RuntimeApp.hpp:182-183`: `std::vector<Arcane::MeshInstance> m_meshInstances;` → `Arcane::GpuSceneFrame m_gpuSceneFrame;`; `RuntimeFrame.hpp:112`: `std::vector<Arcane::MeshInstance>& meshInstances;` → `Arcane::GpuSceneFrame& gpuSceneFrame;` (and the `FrameIo` construction site). `RuntimeFrame.cpp` — before `io.runtime->SetRenderContext(&io.gpu->Batch()); io.runtime->Loop().SubmitRender();` (:441-442):

```cpp
    std::optional<Arcane::ViewTransform> meshView;
    if (const auto cam = Arcane::ActivePerspectiveSceneCamera(io.runtime->Registry(), glm::uvec2{ frameWidth, frameHeight }))
    {
        Arcane::ViewTransform v;
        v.view = cam->view; v.projection = cam->projection; v.viewport = glm::uvec2{ frameWidth, frameHeight };
        meshView = v;
    }
    Arcane::PrepareSceneForRender(io.runtime->Registry(), io.runtime->View(), meshView, io.gpu->Scene(), io.gpuSceneFrame);
```

(`frameWidth`/`frameHeight` are the names the mesh block at :576 already uses; hoist them above if they are declared later.) The mesh block at :545-592 becomes:

```cpp
    Arcane::MeshSceneDesc meshScene;
    if (meshView && io.gpuSceneFrame.HasDraws())
    {
        meshScene.scene      = &io.gpuSceneFrame;
        meshScene.view       = meshView->view;
        meshScene.projection = meshView->projection;
        graphFrame.mesh = &meshScene;   // lights stay at MeshSceneDesc's documented defaults (no light component exists)
    }
```

Delete the `#include <Arcane/Render/MeshSubmissionSystem.hpp>` (:20); add the host header. `RuntimeApp.cpp:1252`: the same `SetVisibility` line from `m_gpuSceneFrame.stats`.

- [ ] **Step 6: `VerifyReport`**

`VerifyReport.hpp`: `void SetVisibility(std::uint32_t total, std::uint32_t coarseVisible, std::uint32_t batches, std::uint32_t draws);` + members `bool m_visibilitySet = false; std::uint32_t m_visTotal = 0, m_visCoarse = 0, m_visBatches = 0, m_visDraws = 0;`. `VerifyReport.cpp` after the `capture` block (:590-592):

```cpp
        if (m_visibilitySet)
            j["visibility"] = { { "total", m_visTotal }, { "coarseVisible", m_visCoarse },
                                { "gpuVisible", m_visCoarse },   // == coarse until plan 2's cull reads back
                                { "batches", m_visBatches }, { "draws", m_visDraws } };
```

- [ ] **Step 7: Delete `MeshSubmissionSystem.hpp`**

`git rm ArcaneClient/src/Arcane/Render/MeshSubmissionSystem.hpp`; grep the tree for `MeshSubmissionSystem` / `CollectMeshInstances` — the only survivors must be prose in `docs/` and the trimmed `MeshSubmissionTest.cpp` (which no longer includes it; rename that file `MeshCacheTest.cpp`? No — leave the name, it still pins `MeshCache`/`MeshMaterialCache`; just drop the include). `MaterialPreviewHarvester.cpp:881`'s comment references `CollectMeshInstances` — reword to `GpuSceneSync`.

- [ ] **Step 8: Build both hosts, run, look**

Build; `.\ArcaneTests.exe "[gpuscene],[verify],[witness-unit]"` — PASS; `~[gpu]` whole green. Then the desk: launch `ArcaneEditor --project ReferenceProject` (a NEW editor instance you launch yourself), open the 3D test scene, toggle `2D | Persp`, orbit so a mesh leaves and re-enters the view — it draws; Play → Edit — every mesh still draws (the generation reset); Ctrl+Z after a gizmo drag — draws. `ArcaneRuntime --project ReferenceProject --headless --frames 3 --verify <out.json>` — the JSON carries `visibility{}` with `total > 0` for the 3D scene.

- [ ] **Step 9: Commit**

```bash
git add ArcaneClient/src/Arcane/Host/GpuSceneHost.hpp ArcaneClient/src/Arcane/Host/VerifyReport.hpp ArcaneClient/src/Arcane/Host/VerifyReport.cpp ArcaneClient/src/Arcane/Render/Nri/GpuScene.cpp ArcaneClient/src/Arcane/Render/GpuSceneTypes.hpp ArcaneClient/src/Arcane/Render/GpuSceneSync.hpp ArcaneEditor/src/App/EditorAppFrame.cpp ArcaneEditor/src/App/EditorApp.hpp ArcaneEditor/src/App/EditorApp.cpp ArcaneEditor/src/Project/MaterialPreviewHarvester.cpp ArcaneRuntime/src/RuntimeFrame.cpp ArcaneRuntime/src/RuntimeFrame.hpp ArcaneRuntime/src/RuntimeApp.hpp ArcaneRuntime/src/RuntimeApp.cpp ArcaneTests/src/GpuSceneSyncTest.cpp ArcaneTests/src/VerifyReportTest.cpp ArcaneTests/src/MeshSubmissionTest.cpp
git rm ArcaneClient/src/Arcane/Render/MeshSubmissionSystem.hpp
git commit -m "feat(host): both frame drivers build the visible set(s), sync the GPU scene and hand the mesh pass its frame (PrepareSceneForRender; a second view when the mesh camera differs); CollectMeshInstances retired; the witness JSON carries visibility{} (F3 plan 1 T8)"
```

---

### Task 9: Close — Release, the gates, the baselines, the docs

**Files:**
- Modify: `docs/specs/2026-09-18-f3-visibility-and-gpu-scene-design.md` (Status line), `CLAUDE.md` (the render section: the GPU scene stated in two sentences), `scripts/automation-baselines.json` (via `check-baselines.ps1`), `docs/plans/2026-08-21-nri-phase4-3d-slice.md` (the MeshConstants prose, one line: superseded by the GPU scene)

- [ ] **Step 1: Release build + the full suite in both configs**

`msbuild Arcane.slnx /p:Configuration=Release /m`; from `bin\Release-windows-x86_64-md\ArcaneTests`: `.\ArcaneTests.exe` (unfiltered, `[gpu]` included) — green. Then `msbuild Arcane.slnx /p:Configuration=Debug /m /t:Rebuild` (the single-slot trap) and the same in Debug.

- [ ] **Step 2: Golden gate, both configs, end on Debug**

Delete the exe-dir `imgui.ini`. `scripts/golden-gate.ps1 -Configuration Release`, then `-Configuration Debug`. Expected: six lanes unchanged. A diff on `editor-ui-perspective` (either lane) means the shader or the row's normal matrix diverged from the F4 pixels — name the cause (`NormalMatrixFor` column order into `normal0..2`, the `nointerpolation` colour path, the depth clear) before touching a reference. If a re-bless is warranted, bless the STAGED slot, copy to `ReferenceProject/Verify/References/` immediately, re-gate both configs.

- [ ] **Step 3: Baselines**

`.\ArcaneTests.exe "~[gpu]" -r json::out=D:\dev\starworks\Arcane\out-baseline.json` in Debug, then `scripts/check-baselines.ps1 -ReportPath D:\dev\starworks\Arcane\out-baseline.json` — the count RISES (new `[aabb]`, `[frustum]`, `[bounds]`, `[visibility]`, `[gpuscene]` cases minus the fourteen moved ones); re-book the Debug entry, repeat for Release. Never commit the JSON report.

- [ ] **Step 4: Docs**

Spec Status line: `plan 1 closed at <sha> (2026-09-xx): bounds, visibility, the GPU scene with CPU-written indices + indirect draws; ABI 36`. `CLAUDE.md` render section: two sentences — every drawable carries `WorldBounds`; meshes draw from a persistent GPU instance scene through indirect batches, culled on the CPU per view until plan 2's compute cull. Re-count `Nri/nodes/` pass types (Batch2D, Mesh, Grid, Fullscreen, ImGui, PickOutline, GpuSceneSync = 7 files, 8 pass types with Pick + Outline) and state the count against the 10+ reorg trigger.

- [ ] **Step 5: Commit and report**

```bash
git add docs/specs/2026-09-18-f3-visibility-and-gpu-scene-design.md CLAUDE.md scripts/automation-baselines.json docs/plans/2026-08-21-nri-phase4-3d-slice.md
git commit -m "chore(f3): plan 1 closes -- baselines re-booked in both configs, six golden lanes unchanged, spec Status + CLAUDE.md state the GPU scene, the pass-type count against the reorg trigger"
```

The close report names: the baseline deltas, the golden verdict per lane, the Aphelyon rebuild owed (ABI 36), the pass-type count, and what plan 2 receives (`MeshCullNode` takes over `BuildGpuSceneFrame`'s visible-index loop; `GpuSceneFrame::frustum` is its constant buffer; the `blend` field on `GpuBatchKey` and `boundsMax.w` are already reserved).

---

## Self-review (run before handing this plan to an executor)

**Spec coverage.** §2.1 `Aabb` → T1; §2.2/2.3 `WorldBounds`/`BoundsSystem` incl. the sprite epsilon, the mesh-beats-sprite rule, removal, Edit-mode scheduling → T2; §2.3's framing lift → T4; §3 `Frustum` + the jitter rule + conservativeness pin → T1 (+ the rule restated in `VisibilitySystem.hpp`); §4 `VisibleSet`/`SceneVisibility`, the slack, absent-resource semantics, sprite + pick consumers, the multi-view seam → T3 + T8; §5.1 the row (240 B, normal columns, world bounds) → T5/T7; §5.2 the mirror + generation stamp → T5 + T8 (`stage.generation`); §5.3 Sync incl. reconcile, exact dirty, the CPU history + re-dirty, the material re-resolve → T5; §5.4 batches (capacity prefix, emission, ordering, args) → T5; §5.5 the cull → **plan 2** (T5's CPU loop is its stand-in, stated); §5.6 the draw (root block, `SV_InstanceID`, register spaces, no count buffer, `GpuSceneFrame` in `MeshSceneDesc`) → T7; §6/§7 → **plan 2**; §8 graph wiring (sync → mesh, `IndirectArgs` usage) → T6/T7; the declared slots' prose → plan 2; §9.1 oracle → plan 2 (T7's draws-and-culls case is the plan-1 stand-in); §9.2 witness counts → T8; §9.3 goldens → T9; §9.4 unit pins → T1–T5, T8; §9.5 desk → T8/T9; §12 plan 1's list → all above. Gap: none for plan 1.

**Placeholders.** No TBD/TODO. Two case bodies (T6's round-trip, T7's declaration-shape) describe assertions against a fixture the executor must read from the file rather than pasting a fixture the plan cannot see — the assertions themselves are stated.

**Type consistency.** `VisibleSet::Contains/Insert/Clear`, `MainVisibleSet`, `NearViewDepth` (T3) are what T5's `BuildGpuSceneFrame` and T8's helper call; `GpuSceneStage::{rows, values, rowCapacity, fullRebuild, generation}` (T5, +generation per T8) is what `GpuScene::Apply` reads (T6); `GpuSceneFrame::{stage, batches, args, visibleIndices, rowCount, frustum, stats, HasDraws}` is shared by T5/T6/T7/T8; `GpuBatchDraw::{mesh, indexOffset, indexCount, firstOutput, capacity, argIndex}` is what `MeshNode::Record` consumes; `MeshRootConstants{firstOutput, flags}` + `kMeshRootDirect` (T7) match `mesh.hlsl`'s `MeshRoot`; `GpuScene::{Instances, InstancesView, Args, VisibleIndices, VisibleIndicesView, ScratchFirstRow, InstanceBufferGeneration, SyncedGeneration, SetSyncedGeneration, kScratchRows}` (T6) are the names T7/T8 use; `Record(ctx, scene, frameSlot, GpuScene*)` is the signature `AddMeshNode` calls.
