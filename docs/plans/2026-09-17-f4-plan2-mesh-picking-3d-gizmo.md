# F4 Plan 2 — mesh picking through the id buffer, and the ONE 3D gizmo

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close F4's authoring loop — `MeshRenderer` entities become click-selectable and outlined in every view mode through the existing id buffer, and the transform gizmo becomes the single 3D, ray-based gizmo (translate / rotate / scale with axis, plane, centre and screen handles) that the 2D view merely masks down to its planar handles — so the desk pass spec §9 names (orbit, click-select a mesh, move it along X, Ctrl+Z reverts in one step) is performable.

**Architecture:** The pick emitter (`CollectPickables`) stops projecting: every `PickDrawable` is now WORLD-space geometry (sprite quads from `SpriteWorldQuad`, physics silhouettes as world-metre shapes at the body's pose, and a new `Mesh` kind carrying the entity's world matrix and mesh Guid), and the id pass projects through a `ViewTransform` handed to the frame (`FrameDesc::pickView`). `PickNode` gains a second pipeline (`entity_id_mesh.hlsl`) that rasterises resident mesh geometry with a pass-local D32 depth transient it mints, AFTER the 2D silhouettes (which draw depth-off), so meshes resolve among themselves by depth and always over sprites — the main pass's order reproduced (spec §7.1, R8). `Arcane::Gizmo` is rewritten around `GizmoTransform { vec3, quat, vec3 }` and `ViewTransform`: hit-testing stays in pixels on projected handle geometry, drags are ray-based (`ScreenToRay` → closest-point-on-line / ray–plane), drawing is overlay pixels on top, size is screen-constant by the projected `w` at the pivot times the persisted gizmo-size setting, and the 2D view mode is a HOST-SIDE `GizmoHandleMask` that hides the handles leaving the XY plane (spec §7.2, R9; the user's ruling 2026-09-17: one gizmo, no separate 2D path).

**Tech Stack:** C++23, glm (`gtc/quaternion`, `gtc/matrix_access`), Astra (vendored), NRI render graph (D3D12 + Vulkan; dxc offline via `data/shaders/compile-shaders.bat`), ImGui, Catch2 + rapidcheck (ArcaneTests), premake5 → `Arcane.slnx`, PowerShell (`scripts/golden-gate.ps1`).

**Spec:** `docs/specs/2026-09-17-f4-editor-3d-authoring-design.md` — §7 (this plan), §3 (the view transform), §9 (testing), §11 (plans), §12 (seams), §13 rulings R8/R9. Plan 1's rulings and their UE check: `docs/plans/2026-09-17-f4-plan1-rulings-ue-check.md` (Ruling P = add a Vulkan lane for `editor-ui-perspective`, folded in here as Task 4). Plan 1 itself: `docs/plans/2026-09-17-f4-plan1-view-transform-editor-camera.md` (the APIs this plan builds on: `ViewTransform`, `ClientRuntime::View()`, `EditorCamera`, `SpriteWorldQuad`, `GridNode`, `AddMeshNode`'s depth transient, `MeshTable`).

**The user's ruling for this plan (2026-09-17, binding):** ONE gizmo — the 3D one. `GizmoTransform` is `{ vec3 position; quat rotation; vec3 scale }`, the view is the `ViewTransform`, decompose/compose are full 3D with a DETERMINANT-AWARE read (negative determinant ⇒ negate the X scale, UE's convention; the editor re-homes the mirror onto the axis the entity authored it on). Drags use rays; hit-testing stays in pixels; size is screen-constant by projected `w` × the gizmo-size setting. `ViewMode::TwoD` is a host-side handle mask: hide the Z arrow, the XZ/YZ plane squares, the X and Y rings, the Z scale box and the screen ring; Z components pass through untouched. The old 2D gizmo code path is DELETED. Snap per axis in world units; one `CommandStack` step per drag; the off-viewport drag continuation stays. Mesh picking lands FIRST.

## Global Constraints

- **One right-handed world, +Y up, camera forward −Z, clip depth [0,1] forward-Z** (spec §2; `ViewTransform.hpp`'s header). Pixels are y-down; `ViewTransform::WorldToScreen` is the ONLY place the flip lives. No `Affine2D` in anything this plan touches — the gizmo and the pick emitter drop it entirely (the physics debug overlay and the camera rect keep theirs; they are not this plan).
- **Units are metres (MKS).** `GizmoSnap { 0.5 m, 15°, 0.1 }` unchanged (R7: the grid unit and the snap are independent).
- **No new third-party dependencies. No Unreal / Source 2 code copied** (shapes only; the UE references consulted are `Editor/UnrealEd/Private/UnrealWidget.cpp` for the projected-`w` size rule and `EditorViewportClient.cpp`'s `FViewportCursorLocation` for the ray split).
- **`Gizmo.hpp` is `ARCANE_API` on `Arcane.dll`** and its exported signatures change ⇒ **plugin ABI bumps to 33** (`PluginABI.hpp` `kGamePluginABIVersion`, with a changelog line in the v-list) and `ReferenceProject/ReferenceProject.arcproj` restamps `"abi": 33` in the same commit (Task 2). Aphelyon's module (`D:\dev\starworks\Aphelyon`) is NOT touched by this plan; it needs a rebuild against the new header before its editor loads it (the ABI gate refuses a v32 module) — say so in the close report.
- **Batcher2D's vtable is unchanged** (the gizmo draws with the existing overlay `Line/Rect/Triangle`).
- **Build:** `cd D:\dev\starworks\Arcane && msbuild Arcane.slnx /p:Configuration=Debug /m` (Release the same way before the close). Shaders compile in the prebuild via `data/shaders/compile-shaders.bat`. **`ARCANE_SDK` may be stale in the process** — build `Arcane.slnx`, never a bare `.vcxproj`.
- **Tests run FROM THE EXE DIRECTORY:** `cd bin\Debug-windows-x86_64-md\ArcaneTests && .\ArcaneTests.exe "<tag>"`. Random order; on a failure capture the seed. `[gpu]` cases need the desk GPU (run them — the GPU ban is retired); `~[gpu]` is the baseline-comparable invocation (`scripts/automation-baselines.json`, 58331 / 1841 Debug at plan 1's close), re-booked at this plan's close by `scripts/check-baselines.ps1`. Header-only inline edits can survive an incremental build with stale test objects — if a test runs old code, delete the test `.obj`s.
- **Goldens:** run `scripts/golden-gate.ps1 -Configuration Debug` at the close BEFORE blessing anything. Expected: the four existing lanes are UNCHANGED (nothing in this plan draws into an unselected, gizmo-off headless frame). If a lane diffs, name the pixel cause before re-blessing; a re-bless goes into the STAGED slot then is COPIED TO SOURCE IMMEDIATELY (`ReferenceProject/Verify/References/`), gate both configs, end on Debug. Delete the exe-dir `imgui.ini` before a golden run.
- **Process (plan 1's lessons):** implementers COMMIT before writing their report; never terminate an editor you did not launch (the user runs one); FOREGROUND suite runs only; never `git add -A` (untracked build dirs + `out.txt` are the user's).
- **Commit after every task**, message style `feat(scope): ...` / `fix(scope): ...` / `test(scope): ...`, ending with the session's attribution trailer.
- **Scope guard (spec §12):** no per-mesh frustum culling (F3), no shared depth / sprite depth / declarative clear (F5), no Top/Side views, no gizmo-size shortcuts (Alt+[ ]), no snap-to-grid affordances beyond the Ctrl snap, no "orbit around selection". The ~14 comment/test-pin minors plan 1's final review counted are NOT enumerated anywhere and are not in this plan; the Win10 `ID3D12InfoQueue1` fallback is outside F4.

**A deliberate two-commit seam:** Task 2 (the gizmo library + its tests) changes exported signatures that `ArcaneEditor` consumes, and Task 3 is the editor's side. Task 2's commit therefore leaves the `ArcaneEditor` project red until Task 3's commit. Execute Task 3 immediately after Task 2 in the same session; run no golden gate and no editor between them. Task 1 has the same shape internally (commit A = the emitter, commit B = the id pass) but every commit there BUILDS — between A and B the id pass merely projects world drawables through the old pixel map, which the [gpu][pick] cases in B are what fix.

---

## File structure

| Path | Responsibility |
|---|---|
| `ArcaneClient/src/Arcane/Render/PickEmit.{hpp,cpp}` | `PickDrawable` in WORLD space (`Quad` = 4 world corners, `Circle/Capsule/Box` = world centre + world angle + metres, NEW `Mesh` = world matrix + Guid); `CollectPickables(registry, out)` (no view); `PickIdVertex::pos` is `vec3` (36 B); `BuildPickIdGeometry` emits world corners and skips `Mesh`. `PickView` is DELETED. |
| `data/shaders/entity_id.hlsl` | VS: `mul(g_viewProj, float4(pos,1))`; 80-byte root block `{ float4x4 viewProj; float4 pad; }`; PS unchanged (analytic coverage now in metres) |
| `data/shaders/entity_id_mesh.hlsl` (new) | the mesh id pass: `MeshVertex` input, `{ float4x4 mvp; uint id; uint3 pad; }` root block, PS returns `id` |
| `data/shaders/compile-shaders.bat` | `entity_id_mesh_vs/_ps` lines |
| `ArcaneClient/src/Arcane/Render/Nri/nodes/PickOutlineNodes.{hpp,cpp}` | `PickNode` takes the view + the mesh buffer cache in `PrepareDrawables`; mints `pickdepth` (D32, supersampled); 2D pipeline depth-off against the attached depth; second (mesh) pipeline LESS/write; `RgPickHandles::depth` |
| `ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.{hpp,cpp}` | `FrameDesc::pickView` (`ViewTransform`, by value); `CurrentPickView()`; published beside `m_currentPickables` |
| `ArcaneEditor/src/App/EditorAppFrame.cpp`, `EditorApp.hpp` | pick: unconditional collect + `vp.pickView`; gates on `AsAffine2D` LIFTED (`wantOutline`, `HandleViewportPick`); gizmo: `ViewTransform`, handle mask by view mode, gizmo-size, 3D write-back, mirror re-homing; `GizmoToolsEnabled()` DELETED |
| `ArcaneEditor/src/Panels/EditorPanels.{hpp,cpp}` | `ViewportToolState::gizmoToolsEnabled` DELETED; Move/Rotate/Scale never greyed |
| `ArcaneRuntime/src/RuntimeFrame.cpp` | `--pick-probe` and `pick@x,y`: no affine gate, `graphFrame.pickView` |
| `ArcaneClient/src/Arcane/Edit/Gizmo.{hpp,cpp}` | THE 3D GIZMO: `GizmoTransform` 3D, `GizmoAxis` with X/Y/Z/XY/YZ/XZ/Center/Screen, `GizmoHandleMask`, `WorldUnitsPerPixel`, `ClosestLineParam`, `RayPlane`, `HitTest/Draw/ApplyDrag` over `ViewTransform`, `MakeGroupDelta/ApplyGroupDelta` 3D, `DecomposeTRS/ComposeTRS` 3D + `WithMirrorOn`; `IsPlanarBasis` DELETED |
| `ArcaneCore/src/Arcane/Plugin/PluginABI.hpp`, `ReferenceProject/ReferenceProject.arcproj` | ABI 33 |
| `scripts/golden-gate.ps1` | `ExtraArgs` per lane; two `editor-ui-perspective` lanes (dx12 + vulkan); `-SelfTest` expects six lanes |
| `ReferenceProject/Verify/References/vulkan/editor-ui-perspective.png` (new, if the Vulkan render differs from the shared slot) | the Vulkan perspective reference |
| `ArcaneTests/src/PickBufferTest.cpp` (rewritten), `RenderGraphTest.cpp` (pins), `NriGraphPixelTest.cpp` (pick cases + a mesh-pick case), `GizmoTest.cpp` (rewritten), `EntityOpsTest.cpp` (3D write-back; the planar case deleted) | the pins |
| `docs/specs/2026-09-17-f4-editor-3d-authoring-design.md`, `docs/plans/2026-08-21-nri-phase4-3d-slice.md`, `CLAUDE.md` | Status line; Task 10 desk items reworded; the editor's authoring loop stated |

---

### Task 1: Mesh picking — the world-space id pass

**Files:**
- Modify: `ArcaneClient/src/Arcane/Render/PickEmit.hpp`, `PickEmit.cpp`
- Modify: `data/shaders/entity_id.hlsl`; Create: `data/shaders/entity_id_mesh.hlsl`; Modify: `data/shaders/compile-shaders.bat`
- Modify: `ArcaneClient/src/Arcane/Render/Nri/nodes/PickOutlineNodes.hpp` (PickNode ~lines 165–350, `RgPickHandles` ~560), `PickOutlineNodes.cpp` (`Init` 148–225, `PrepareDrawables` 302, `Record` 308–420, `AddPickNodes` 1080–1125)
- Modify: `ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.hpp` (`FrameDesc` ~440–460, accessors ~996, members ~1461), `NriGraphContext.cpp` (~1705, ~1856)
- Modify: `ArcaneEditor/src/App/EditorAppFrame.cpp` (`ArmGraphViewportFrame` ~2110–2190; `HandleViewportPick` ~3540–3555), `ArcaneEditor/src/App/EditorApp.hpp` (`GizmoToolsEnabled` stays until Task 3 but its pick uses go)
- Modify: `ArcaneRuntime/src/RuntimeFrame.cpp` (~465–480, ~515–530)
- Test: `ArcaneTests/src/PickBufferTest.cpp` (rewrite the view-dependent cases), `ArcaneTests/src/RenderGraphTest.cpp` (~5601–5660 barriers; the `pick geometry` case ~6935), `ArcaneTests/src/NriGraphPixelTest.cpp` (~496–640 pick cases; add the mesh case beside the mesh cases ~850–1100)

**Interfaces:**
- Consumes (plan 1): `Arcane::ViewTransform` (`Scene/ViewTransform.hpp`: `view`, `projection`, `viewport`, `ViewProjection()`), `SpriteWorldQuad(world, baseSizeMetres, pivot)` (`Render/SpriteGeometry.hpp`, corners TL,TR,BR,BL), `MeshTable::Resolve(Guid) -> const MeshEntry*` (`Scene/SceneResources.hpp`; `entry->data.sections` empty ⇒ empty mesh), `NriMeshBufferCache::Resolve(Guid, frameCounter) -> const Resident*` (`vertexBuffer`, `indexBuffer`, `indexCount`, `ready`), `NriGraphContext::MeshBuffers()`, `PresentedFrames()`, `kGraphDepthFormat` (D32_SFLOAT), `MeshVertex { vec3 position; vec3 normal; vec2 uv; }` (32 B).
- Produces:
  ```cpp
  namespace Arcane {
  struct PickDrawable {
      Astra::Entity entity{};
      enum class Kind : uint8_t { Quad, Circle, Capsule, Box, Mesh };
      Kind kind = Kind::Quad;
      // Quad (sprites): the four WORLD corners, TL, TR, BR, BL (SpriteWorldQuad's order).
      std::array<glm::vec3, 4> corners{};
      // Circle / Capsule / Box (physics fixtures): a WORLD-space shape in the XY plane
      // at `center` (metres; z = the entity's world z, 0 without a WorldTransform),
      // turned by `angle` (radians, world sense, about +Z).
      glm::vec3 center{0.0f};
      glm::vec2 halfExtents{0.0f};   // Box
      float     radius  = 0.0f;      // Circle / Capsule
      float     halfLen = 0.0f;      // Capsule
      float     angle   = 0.0f;
      // Mesh: the whole resident mesh, rasterised by the pick node's second pipeline.
      glm::mat4 world{1.0f};
      Guid      mesh{};
  };
  ARCANE_API void CollectPickables(Astra::Registry& registry, std::vector<PickDrawable>& out);
  struct PickIdVertex { glm::vec3 pos; glm::vec2 local; float radius; float halfLen; uint32_t kind; uint32_t id; };   // 36 B
  ARCANE_API void BuildPickIdGeometry(std::span<const PickDrawable>, std::vector<PickIdVertex>&, std::vector<uint32_t>&);  // skips Mesh; ids still index+1 over ALL drawables
  // PickNode:
  void PrepareDrawables(std::span<const PickDrawable> drawables, const ViewTransform& view,
                        NriMeshBufferCache* meshBuffers, std::uint64_t frameCounter);
  struct RgPickHandles { RgTexture ids; RgTexture depth; RgBuffer readback; };
  // FrameDesc:
  ViewTransform pickView;   // the view the id pass projects through; default = no view (nothing rasterises)
  }
  ```

**Part A — the emitter goes world-space (commit A)**

- [ ] **Step A1: Rewrite the view-dependent cases in `PickBufferTest.cpp`.** Keep the fixture helpers (registry with `SpriteTable`, physics world, the collider helper at ~113). Replace `"CollectPickables gathers sprites and physics colliders, ordered"` (146) and `"CollectPickables projects +Y up and mirrors the canvas angle"` (199) and `"CollectPickables scales a collider silhouette by the body's baked scale"` (260) and `"CollectPickables orders sprites before colliders, deterministically"` (294) with these (the id-table case at 240 stays as is):

```cpp
#include <Arcane/Render/SpriteGeometry.hpp>   // SpriteWorldQuad -- THE corner rule the pick quad must match
#include <Arcane/Mesh/MeshBuilder.hpp>        // BuildCube for the mesh drawable case

TEST_CASE("CollectPickables emits sprites as WORLD quads from SpriteWorldQuad, colliders as world shapes, meshes last", "[pick]")
{
    // A sprite with an off-centre pivot, a mirrored X scale and a Z turn -- the
    // pose PickEmit's old "centre + angle" rule got wrong for a mirrored
    // off-centre pivot (plan 2 handoff). SpriteWorldQuad is the one rule.
    auto reg = MakeRegistryWithSpriteTable(/*sizeMeters=*/{2.0f, 1.0f}, /*pivot=*/{0.25f, 0.0f});
    const Astra::Entity sprite = AddSprite(*reg, glm::vec3(3.0f, 1.0f, 0.5f),
                                           Arcane::RotationAboutZ(0.7f), glm::vec3(-1.5f, 1.0f, 1.0f));
    const Astra::Entity body   = AddCircleCollider(*reg, glm::vec2(-2.0f, 4.0f), /*radius=*/0.5f);
    Arcane::PropagateWorldTransforms(*reg);   // whatever helper the fixture already uses to fill WorldTransform

    std::vector<Arcane::PickDrawable> out;
    Arcane::CollectPickables(*reg, out);
    REQUIRE(out.size() == 2);

    // 1. The sprite: kind Quad, corners == SpriteWorldQuad(world, base size, pivot).
    CHECK(out[0].entity == sprite);
    CHECK(out[0].kind == Arcane::PickDrawable::Kind::Quad);
    const glm::mat4 world = std::as_const(*reg).GetComponent<Arcane::WorldTransform>(sprite)->matrix;
    const Arcane::SpriteQuad expected = Arcane::SpriteWorldQuad(world, {2.0f, 1.0f}, {0.25f, 0.0f});
    for (int i = 0; i < 4; ++i)
        for (int c = 0; c < 3; ++c)
            CHECK_THAT(out[0].corners[i][c], WithinAbs(expected.corners[i][c], 1e-5f));

    // 2. The collider: a world-space circle at the body's pose, in METRES, no
    //    projection anywhere (the id pass projects).
    CHECK(out[1].entity == body);
    CHECK(out[1].kind == Arcane::PickDrawable::Kind::Circle);
    CHECK_THAT(out[1].center.x, WithinAbs(-2.0f, 1e-5f));
    CHECK_THAT(out[1].center.y, WithinAbs(4.0f, 1e-5f));
    CHECK_THAT(out[1].radius,   WithinAbs(0.5f, 1e-5f));
}

TEST_CASE("CollectPickables: a MeshRenderer entity emits ONE Mesh drawable carrying its world matrix and guid, after sprites and colliders", "[pick]")
{
    auto reg = MakeRegistryWithSpriteTable({1.0f, 1.0f}, {0.5f, 0.5f});
    // A MeshTable resource resolving one guid to a cube; an unresolved guid and
    // an empty mesh must NOT emit (nothing is drawn for them either).
    const Arcane::Guid cubeId{ 7, 7 };
    const Arcane::Guid emptyId{ 8, 8 };
    std::unordered_map<Arcane::Guid, Arcane::MeshEntry> meshes;
    meshes[cubeId].data  = Arcane::BuildCube(1.0f);
    meshes[emptyId].data = Arcane::MeshData{};            // sections empty == empty mesh
    Arcane::MeshTable table; table.meshes = &meshes;
    reg->AddResource<Arcane::MeshTable>(table);           // the fixture's resource-add call, as the SpriteTable is added

    const Astra::Entity sprite = AddSprite(*reg, glm::vec3(0.0f), glm::quat(1,0,0,0), glm::vec3(1.0f));
    const Astra::Entity cube   = AddMesh(*reg, cubeId, glm::vec3(1.0f, 2.0f, 3.0f));   // Transform + MeshRenderer{cubeId}
    const Astra::Entity empty  = AddMesh(*reg, emptyId, glm::vec3(0.0f));
    const Astra::Entity broken = AddMesh(*reg, Arcane::Guid{ 9, 9 }, glm::vec3(0.0f));
    const Astra::Entity hidden = AddMesh(*reg, cubeId, glm::vec3(5.0f, 0.0f, 0.0f));
    reg->AddComponent<Arcane::Hidden>(hidden);
    Arcane::PropagateWorldTransforms(*reg);

    std::vector<Arcane::PickDrawable> out;
    Arcane::CollectPickables(*reg, out);
    REQUIRE(out.size() == 2);   // the sprite, then the ONE drawable cube
    CHECK(out[0].entity == sprite);
    CHECK(out[1].entity == cube);
    CHECK(out[1].kind == Arcane::PickDrawable::Kind::Mesh);
    CHECK(out[1].mesh == cubeId);
    CHECK_THAT(out[1].world[3].x, WithinAbs(1.0f, 1e-6f));
    CHECK_THAT(out[1].world[3].y, WithinAbs(2.0f, 1e-6f));
    CHECK_THAT(out[1].world[3].z, WithinAbs(3.0f, 1e-6f));
    (void)empty; (void)broken;
    // ...and the id table still inverts through the same k+1 rule.
    CHECK(Arcane::PickPassIdOf(out, cube) == 2u);
    CHECK(Arcane::PickEntityForId(out, 2u) == cube);
}

TEST_CASE("CollectPickables scales a collider silhouette by the body's baked scale, in metres", "[pick]")
{
    // Same fixture as before (PhysicsBodyRef::appliedScale = (2, 3)); the box
    // half-extents come out as fixture metres times the baked scale -- no
    // pixels anywhere.
    auto reg = MakeRegistryWithSpriteTable({1.0f, 1.0f}, {0.5f, 0.5f});
    const Astra::Entity body = AddScaledBoxCollider(*reg, glm::vec2(0.0f), /*halfW=*/1.0f, /*halfH=*/0.5f, /*scale=*/{2.0f, 3.0f});
    std::vector<Arcane::PickDrawable> out;
    Arcane::CollectPickables(*reg, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].kind == Arcane::PickDrawable::Kind::Box);
    CHECK_THAT(out[0].halfExtents.x, WithinAbs(2.0f, 1e-5f));
    CHECK_THAT(out[0].halfExtents.y, WithinAbs(1.5f, 1e-5f));
}

TEST_CASE("CollectPickables orders sprites, then colliders, then meshes, deterministically", "[pick]")
{
    // Two of each, collected twice: identical order both times, and the three
    // groups in that sequence (id = index+1 is the contract every consumer inverts).
    auto reg = MakeRegistryWithSpriteTable({1.0f, 1.0f}, {0.5f, 0.5f});
    const Arcane::Guid cubeId{ 7, 7 };
    std::unordered_map<Arcane::Guid, Arcane::MeshEntry> meshes; meshes[cubeId].data = Arcane::BuildCube(1.0f);
    Arcane::MeshTable table; table.meshes = &meshes; reg->AddResource<Arcane::MeshTable>(table);
    AddMesh(*reg, cubeId, glm::vec3(0.0f)); AddSprite(*reg, glm::vec3(1.0f), glm::quat(1,0,0,0), glm::vec3(1.0f));
    AddCircleCollider(*reg, glm::vec2(2.0f), 0.5f); AddMesh(*reg, cubeId, glm::vec3(3.0f));
    AddSprite(*reg, glm::vec3(4.0f), glm::quat(1,0,0,0), glm::vec3(1.0f)); AddCircleCollider(*reg, glm::vec2(5.0f), 0.5f);
    Arcane::PropagateWorldTransforms(*reg);
    std::vector<Arcane::PickDrawable> a, b;
    Arcane::CollectPickables(*reg, a); Arcane::CollectPickables(*reg, b);
    REQUIRE(a.size() == 6); REQUIRE(b.size() == 6);
    for (std::size_t i = 0; i < 6; ++i) CHECK(a[i].entity == b[i].entity);
    CHECK(a[0].kind == Arcane::PickDrawable::Kind::Quad);   CHECK(a[1].kind == Arcane::PickDrawable::Kind::Quad);
    CHECK(a[2].kind == Arcane::PickDrawable::Kind::Circle); CHECK(a[3].kind == Arcane::PickDrawable::Kind::Circle);
    CHECK(a[4].kind == Arcane::PickDrawable::Kind::Mesh);   CHECK(a[5].kind == Arcane::PickDrawable::Kind::Mesh);
}
```
  Adapt the helper names (`MakeRegistryWithSpriteTable`, `AddSprite`, `AddCircleCollider`, `AddScaledBoxCollider`, `PropagateWorldTransforms`) to the fixture the file already has — read lines 40–145 first and reuse; add `AddMesh` (Transform + `MeshRenderer{ .mesh = id }`) beside `AddSprite`. Delete the `ViewTransform.hpp`/`PickView` include and every `view` argument.

- [ ] **Step A2: Update the `pick geometry` case in `RenderGraphTest.cpp` (~6935)** to world-space inputs: `drawables[0]` becomes a `Quad` with `corners = {{0,2,0},{4,2,0},{4,0,0},{0,0,0}}`, the circle/capsule keep `center` (now `glm::vec3`) and metre radii; add a 4th drawable of `Kind::Mesh` and assert it emits NO vertices while the ids of the others are still their index+1 (`vertices[d*4+v].id == d+1` for d in {0,1,2}, `vertices.size() == 12`); assert `vertices[0].pos == glm::vec3(0,2,0)` (the Quad's TL corner verbatim) and for the circle `vertices[4].local == glm::vec2(-7,-7)` with `pos == center + vec3(-7,-7,0)`. Keep the `PickBoundHalfExtents` and clear checks.

- [ ] **Step A3: Run the two files to see them fail to compile** — `msbuild Arcane.slnx /t:ArcaneTests /p:Configuration=Debug /m` is expected to FAIL on `PickDrawable::corners`, `Kind::Mesh`, the two-argument `CollectPickables`.

- [ ] **Step A4: Rewrite `PickEmit.hpp`.** Replace the header comment's "in canvas pixels" story and the `PickView` struct (delete it); the drawable becomes the struct in *Interfaces* above (include `<Arcane/Core/Guid.hpp>` or wherever `Arcane::Guid` lives — `grep -rn "struct Guid" ArcaneCore/src` — and `<array>`, `<glm/mat4x4.hpp>`). Document the ordering as `1. sprites (View<WorldTransform, SpriteRenderer, Not<Hidden>> — the drawn set, RenderSystems.hpp's own filter), 2. colliders (as today), 3. meshes (View<WorldTransform, MeshRenderer, Not<Hidden>>, resolved through the registry's MeshTable; nil/unresolved/empty emits nothing, exactly what CollectMeshInstances draws)`. Document the ORDER RULE for the id pass: "the 2D kinds draw first with the depth test OFF in submission order (later wins), then the meshes draw depth-tested against a cleared depth — so a mesh always owns a pixel it shares with a sprite, and meshes resolve among themselves by depth: the main pass's order (spec §7.1)". `PickIdVertex` becomes `{ glm::vec3 pos; glm::vec2 local; float radius; float halfLen; uint32_t kind; uint32_t id; }` with `static_assert(sizeof(PickIdVertex) == 36, "id vertex is the wire format")` and the comment "pos is WORLD space; entity_id.hlsl multiplies by the frame's view-projection; local/radius/halfLen are METRES (the PS coverage test is unit-agnostic)". `PickKindCode` gains `Mesh -> 4` (never emitted to the 2D path; the code exists so the switch is total). Signature: `ARCANE_API void CollectPickables(Astra::Registry& registry, std::vector<PickDrawable>& out);`.

- [ ] **Step A5: Rewrite `PickEmit.cpp`'s `CollectPickables`.** Pass 1 (sprites):
```cpp
const SpriteTable* spriteTable = registry.GetResource<SpriteTable>();
auto spriteView = registry.CreateView<const WorldTransform, const SpriteRenderer, Astra::Not<Hidden>>();
spriteView.ForEach([&](Astra::Entity e, const WorldTransform& xf, const SpriteRenderer& sp)
{
    const SpriteEntry* entry = (sp.shape == SpriteShape::Rect && spriteTable) ? spriteTable->Resolve(sp.sprite) : nullptr;
    const glm::vec2 baseSize = entry ? entry->sizeMeters : glm::vec2(1.0f);
    const glm::vec2 pivot    = entry ? entry->pivot      : glm::vec2(0.5f);
    // THE ONE CORNER RULE (SpriteGeometry.hpp): the same four world points the
    // sprite submission draws, so the silhouette matches the drawn quad under
    // ANY projection -- a mirrored or off-centre pivot included.
    PickDrawable d;
    d.entity  = e;
    d.kind    = PickDrawable::Kind::Quad;
    d.corners = SpriteWorldQuad(xf.matrix, baseSize, pivot).corners;
    out.push_back(d);
});
```
  Pass 2 (colliders): keep the body walk; `d.center = glm::vec3(bodyPos + RotateVec(localScaled, bodyAngle), worldZ)` where `worldZ` is `registry.GetComponent<WorldTransform>(entity) ? matrix[3].z : 0.0f` (look it up once per entity via `std::as_const(registry).GetComponent<const WorldTransform>(entity)` — use the read form the file already uses for components), `d.angle = fixtureAngle` (WORLD sense — no `AngleSign`), radii/half-extents in metres (`fx.radius * sMax`, `fx.halfLen * sx`, `fx.radius * sy`, `fx.halfW * sx`, `fx.halfH * sy`) — i.e. drop every `view.affine.Length(...)` wrapper. Pass 3 (meshes), after the physics early-return is restructured so meshes are still collected when there is no physics world:
```cpp
// ---- PASS 3: meshes -------------------------------------------------------
// One drawable per MeshRenderer entity that CollectMeshInstances would draw
// (Render/MeshSubmissionSystem.hpp: WorldTransform + MeshRenderer, not Hidden,
// resolved through the MeshTable to a mesh with sections). The whole mesh is
// one silhouette -- sections carry materials, not identity -- so an entity
// gets ONE id whatever its section count.
{
    const MeshTable* meshTable = registry.GetResource<MeshTable>();
    auto meshView = registry.CreateView<const WorldTransform, const MeshRenderer, Astra::Not<Hidden>>();
    meshView.ForEach([&](Astra::Entity e, const WorldTransform& xf, const MeshRenderer& mr)
    {
        const MeshEntry* entry = meshTable ? meshTable->Resolve(mr.mesh) : nullptr;
        if (!entry || entry->data.sections.empty())
            return;
        PickDrawable d;
        d.entity = e;
        d.kind   = PickDrawable::Kind::Mesh;
        d.world  = xf.matrix;
        d.mesh   = mr.mesh;
        out.push_back(d);
    });
}
```
  `BuildPickIdGeometry`: for `Kind::Quad` emit the four `corners` verbatim as `pos` with `local = {0,0}` (kind code 0 covers the whole quad; index order TL,TR,BR,BL is the existing `kSigns` order so the two triangles stay `base,base+1,base+2 / base,base+2,base+3`); for Circle/Capsule/Box rotate `local = signs * bound` by `angle` in XY and `pos = center + glm::vec3(rot, 0)`; for `Kind::Mesh` `continue` (no quad — the id is still `di + 1`, so the mesh path's ids and this path's ids share one numbering). Includes: `<Arcane/Render/SpriteGeometry.hpp>`, `<Arcane/Scene/SceneResources.hpp>` (already), `MeshRenderer` from `Components.hpp`.

- [ ] **Step A6: Fix the three callers to the new signature** so the tree builds (the view moves to the node in Part B): `EditorAppFrame.cpp` ~2161 becomes `Arcane::CollectPickables(m_runtime->Registry(), m_pickDrawables);` (drop the `pickAffine` guard and the `PickView`); `RuntimeFrame.cpp` ~471 and ~522 likewise (`probeAffine`/`pickAffine` guards become plain `if (io.config.pickProbe)` / `if (pickSpec)`). `NriGraphPixelTest.cpp`'s pick cases (~536–560, ~603) set `corners` instead of `center/halfExtents` — for now as `{{24,14,0},{56,14,0},{56,34,0},{24,34,0}}` (Part B gives them the pixel-identity view that makes these pixels).

- [ ] **Step A7: Build and run** — `msbuild Arcane.slnx /p:Configuration=Debug /m` → 0 errors, 0 warnings. From the ArcaneTests exe dir: `.\ArcaneTests.exe "[pick]"` and `.\ArcaneTests.exe "pick geometry*"` → all PASS.

- [ ] **Step A8: Commit A**
```bash
cd /d/dev/starworks/Arcane
git add ArcaneClient/src/Arcane/Render/PickEmit.hpp ArcaneClient/src/Arcane/Render/PickEmit.cpp ArcaneEditor/src/App/EditorAppFrame.cpp ArcaneRuntime/src/RuntimeFrame.cpp ArcaneTests/src/PickBufferTest.cpp ArcaneTests/src/RenderGraphTest.cpp ArcaneTests/src/NriGraphPixelTest.cpp
git commit -m "feat(render): the pick emitter goes WORLD-space -- sprite quads from SpriteWorldQuad, physics silhouettes in metres at the body pose, MeshRenderer entities emit a Mesh drawable (world matrix + guid) after sprites and colliders; PickView retired (F4 plan 2 T1a, spec s7.1)"
```

**Part B — the id pass projects, and rasterises meshes with its own depth (commit B)**

- [ ] **Step B1: Pin the frame shape first.** In `RenderGraphTest.cpp`'s `"the pick + outline chain lands between the tonemap and the capture"` (~5551) add after the `pickIds` checks:
```cpp
// The id pass owns a DEPTH transient of its own (spec s7.1): meshes resolve
// among themselves by depth inside the pick pass, and NOT against the mesh
// node's depth -- that one belongs to the canvas, and the pick pass is
// supersampled anyway.
REQUIRE(graph.IsHandleValid(handles.pickDepth));
CHECK(graph.IsTransient(handles.pickDepth));
CHECK(std::string(graph.NameOf(handles.pickDepth)) == "pickdepth");
```
  and in `"the pick chain's barriers are derived, including the readback copy"` (~5601) change the node-2 block to expect TWO pre-barriers (the id transient → colour attachment, the depth transient → depth write) — identify each by `isTexture` and by comparing `after` against `kColorState` / the depth-write state the mesh cases already use (grep `kDepthState` or the mesh node's barrier case ~6475 for the exact constant name). Add `RgTexture pickDepth{}` to `RgFrameHandles` (grep its definition in `NriGraphContext.hpp`) and assign it from `pick.depth` in `DeclareGraphFrame` (~1451). Also grep every `pickOutline = true` case for a `compiled.transients.size()` / `poolSlotCount` pin and bump it by one with a one-line reason.

- [ ] **Step B2: Pin the GPU contract.** In `NriGraphPixelTest.cpp`:
  - add a helper next to `ProbeAt`:
```cpp
// A pixel-identity ORTHOGRAPHIC view over the vehicle's kW x kH canvas: world
// (x, y, 0) lands on pixel (x, kH - y). The pick cases below author their
// silhouettes in these units so the old canvas-pixel expectations read
// through unchanged: a quad "at pixel (40, 24)" is a world quad centred on
// (40, kH - 24).
Arcane::ViewTransform PixelIdentityView()
{
    return Arcane::ViewTransform::Orthographic(glm::vec2(kW * 0.5f, kH * 0.5f), kH * 0.5f, { kW, kH });
}
Arcane::PickDrawable WorldQuadAtPixel(glm::vec2 pixelCentre, glm::vec2 halfPx)
{
    Arcane::PickDrawable d;
    d.kind = Arcane::PickDrawable::Kind::Quad;
    const float cx = pixelCentre.x, cy = float(kH) - pixelCentre.y;
    d.corners = { glm::vec3(cx - halfPx.x, cy + halfPx.y, 0.0f), glm::vec3(cx + halfPx.x, cy + halfPx.y, 0.0f),
                  glm::vec3(cx + halfPx.x, cy - halfPx.y, 0.0f), glm::vec3(cx - halfPx.x, cy - halfPx.y, 0.0f) };
    return d;
}
```
    `ProbeAt` sets `frame.pickView = PixelIdentityView();`; the two pick cases build `a = WorldQuadAtPixel({40,24},{16,10})`, `b = WorldQuadAtPixel({118,68},{16,10})`.
  - add a mesh-pick case beside the mesh cases (uses `SupplyOne`, `BuildCube`, `kEyeZ` etc.):
```cpp
namespace
{
    // THE PROPERTY THIS PLAN EXISTS FOR: a mesh drawable rasterises into the id
    // buffer through the frame's ViewTransform, depth-tested against the pick
    // pass's OWN depth -- the nearer of two overlapping cubes wins the centre
    // pixel whichever order they were emitted in, and a sprite quad under a
    // mesh loses the pixel to it (the main pass's order, reproduced).
    void CheckMeshPickThroughTheIdBuffer(Arcane::GraphicsBackend backend)
    {
        ARC_REQUIRE_BACKEND(backend);
        const std::uint64_t before = Arcane::RenderErrorCount();
        Arcane::NriGraphContext::NodeSet nodes;
        nodes.pickOutline = true;

        const Arcane::MeshData nearCube = Arcane::BuildCube(0.6f);
        const Arcane::MeshData farCube  = Arcane::BuildCube(3.0f);
        const Arcane::Guid nearId{ 1, 1 }, farId{ 2, 2 };

        Arcane::ViewTransform view = Arcane::ViewTransform::Perspective(
            glm::vec3(0.0f, 0.0f, kEyeZ), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f),
            kFovYDegrees, { kW, kH }, kNearZ, kFarZ);

        Arcane::PickDrawable nearD; nearD.kind = Arcane::PickDrawable::Kind::Mesh; nearD.mesh = nearId;
        nearD.world = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 1.0f));
        Arcane::PickDrawable farD;  farD.kind  = Arcane::PickDrawable::Kind::Mesh; farD.mesh  = farId;
        farD.world  = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -1.5f));
        // A sprite quad covering the whole view at z = +2 (NEARER than both cubes):
        // depth-off 2D drawables never occlude a mesh, by the pass's order rule.
        Arcane::PickDrawable sprite; sprite.kind = Arcane::PickDrawable::Kind::Quad;
        sprite.corners = { glm::vec3(-10, 10, 2), glm::vec3(10, 10, 2), glm::vec3(10, -10, 2), glm::vec3(-10, -10, 2) };

        // Order 1: sprite(1), near(2), far(3). Order 2: sprite(1), far(2), near(3).
        const Arcane::PickDrawable order1[] = { sprite, nearD, farD };
        const Arcane::PickDrawable order2[] = { sprite, farD, nearD };

        auto probe = [&](std::span<const Arcane::PickDrawable> drawables, glm::ivec2 pixel)
        {
            PixelVehicle v = MakeVehicle(backend, nodes);
            v.ctx->SetMeshSupply(SupplyTwo(nearId, nearCube, farId, farCube));
            Arcane::NriGraphContext::FrameDesc frame;
            frame.pickOutline = true; frame.pickables = drawables; frame.pickPixel = pixel; frame.pickView = view;
            for (std::uint32_t i = 0; i < Arcane::kSwapchainFramesInFlight; ++i) RenderOne(*v.ctx, frame);
            RenderOne(*v.ctx, frame);
            const auto id = v.ctx->ProbeId();
            REQUIRE(id.has_value());
            return *id;
        };

        // The centre pixel is the NEAR cube in both orders -- depth, not order.
        CHECK(probe(order1, glm::ivec2(kW / 2, kH / 2)) == 2u);
        CHECK(probe(order2, glm::ivec2(kW / 2, kH / 2)) == 3u);
        // x = 96 on the centre row is far-only (the mesh depth case's geometry).
        CHECK(probe(order1, glm::ivec2(96, kH / 2)) == 3u);
        // The corner is the SPRITE: no mesh there, and the sprite covers the view.
        CHECK(probe(order1, glm::ivec2(4, 4)) == 1u);
        CHECK(Arcane::RenderErrorCount() == before);
    }
}
TEST_CASE("pick: a mesh drawable rasterises depth-tested into the id buffer, over any 2D silhouette (d3d12)", "[gpu][pixel][pick][nri][d3d12]") { CheckMeshPickThroughTheIdBuffer(Arcane::GraphicsBackend::D3D12); }
TEST_CASE("pick: a mesh drawable rasterises depth-tested into the id buffer, over any 2D silhouette (vulkan)", "[gpu][pixel][pick][nri][vulkan]") { CheckMeshPickThroughTheIdBuffer(Arcane::GraphicsBackend::Vulkan); }
```
    (`SupplyTwo` lives above the mesh section; if it is declared after the pick section, move the new case BELOW the mesh cases so it is in scope.) Also the outline case (~700–760) sets `frame.pickView = PixelIdentityView()` and uses `WorldQuadAtPixel`.

- [ ] **Step B3: Build ArcaneTests and confirm the new cases fail** (`handles.pickDepth` does not exist; `frame.pickView` does not exist) — a compile failure is the expected "fail".

- [ ] **Step B4: The shaders.** `data/shaders/entity_id.hlsl` — replace the constants block and VS:
```hlsl
// Positions arrive in WORLD space (F4 plan 2); the root block carries the
// frame's view-projection. local/radius/halfLen are METRES -- the analytic
// coverage test below is unit-agnostic, so it is unchanged.
struct BatchConstants
{
    float4x4 viewProj;
    float4   pad;        // 80 bytes total: the same block size as entity_id_mesh.hlsl's, ONE pipeline layout serves both
};
#if SPIRV
[[vk::push_constant]] ConstantBuffer<BatchConstants> g_PC;
#define g_viewProj g_PC.viewProj
#else
cbuffer BatchConstantsCB : register(b0) { BatchConstants g_PCData; }
#define g_viewProj g_PCData.viewProj
#endif
struct VSInput
{
    float3 pos   : POSITION;     // WORLD-space corner of the bounding quad
    float2 local : LOCAL;        // shape-local coords (unrotated), metres
    float2 rl    : SHAPEPARAM;   // (radius, halfLen), metres
    uint2  ki    : KINDID;
};
// ...VSOutput unchanged...
VSOutput vs_main(VSInput input)
{
    VSOutput output;
    output.pos   = mul(g_viewProj, float4(input.pos, 1.0));
    output.local = input.local; output.rl = input.rl; output.ki = input.ki;
    return output;
}
```
  Create `data/shaders/entity_id_mesh.hlsl`:
```hlsl
// Entity-id pass, MESH half (F4 plan 2, spec s7.1): rasterise a resident mesh's
// triangles through mvp and write the drawable's 1-based id. Depth-tested
// (LESS, write) against the pick pass's own D32 transient, AFTER the 2D
// silhouettes drew depth-off -- so meshes resolve among themselves by depth
// and always over sprites, which is the main pass's order reproduced.
//
// The vertex input is MeshVertex (Mesh/MeshBuilder.hpp) verbatim -- the SAME
// resident buffers MeshNode draws, bound by PickNode -- so NORMAL and TEXCOORD
// are declared and unused. The 80-byte root block matches entity_id.hlsl's so
// one pipeline layout serves both pipelines.
struct MeshIdConstants
{
    float4x4 mvp;
    uint     id;
    uint     pad0;   // three SCALARS, not a uint3: a vector member would be 16-byte
    uint     pad1;   // aligned under cbuffer/push-constant packing and push the block
    uint     pad2;   // to 96 bytes, past the 80-byte root-constant range on both backends
};
#if SPIRV
[[vk::push_constant]] ConstantBuffer<MeshIdConstants> g_PC;
#define g_mvp g_PC.mvp
#define g_id  g_PC.id
#else
cbuffer MeshIdConstantsCB : register(b0) { MeshIdConstants g_PCData; }
#define g_mvp g_PCData.mvp
#define g_id  g_PCData.id
#endif
struct VSInput  { float3 position : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD; };
struct VSOutput { float4 pos : SV_Position; };
VSOutput vs_main(VSInput input) { VSOutput o; o.pos = mul(g_mvp, float4(input.position, 1.0)); return o; }
uint ps_main(VSOutput input) : SV_Target0 { return g_id; }
```
  `compile-shaders.bat`: after the two `entity_id` lines add
```bat
:: The id pass's MESH half (F4 plan 2, spec s7.1) -- MeshVertex in, id out, depth-tested inside the pick pass.
call :compile entity_id_mesh vs_main vs_6_5 entity_id_mesh_vs || exit /b 1
call :compile entity_id_mesh ps_main ps_6_5 entity_id_mesh_ps || exit /b 1
```

- [ ] **Step B5: `NriGraphContext` carries the view.** `FrameDesc` gains, beside `pickables`:
```cpp
// THE VIEW THE ID PASS PROJECTS THROUGH (F4 plan 2). The drawables are
// WORLD-space now, so the pass needs the same ViewTransform the scene render
// just used -- the editor's camera in Edit, the scene camera in Play. Copied,
// not borrowed (136 bytes; read at declaration time by PrepareDrawables). A
// default-constructed view (zero viewport) rasterises nothing, which is what
// a frame that armed the chain without a view must read back: background.
ViewTransform pickView{};
```
  (`#include <Arcane/Scene/ViewTransform.hpp>` at the top). Add member `ViewTransform m_currentPickView{};` beside `m_currentPickables`, accessor `[[nodiscard]] const ViewTransform& CurrentPickView() const noexcept { return m_currentPickView; }`, and publish `m_currentPickView = effective.pickView;` at BOTH sites (~1705 and ~1856) beside `m_currentPickables`. `RgFrameHandles` gains `RgTexture pickDepth{};`, set from `pick.depth` in `DeclareGraphFrame`.

- [ ] **Step B6: `PickNode`.** Header: `PrepareDrawables(std::span<const PickDrawable> drawables, const ViewTransform& view, NriMeshBufferCache* meshBuffers, std::uint64_t frameCounter);` with the doc "builds the 2D geometry AND resolves every Mesh drawable's resident buffers (declaration time, like MeshNode::Prepare — never at record time); `meshBuffers` nullable (no mesh picks then)". Replace `RootConstants` with:
```cpp
// entity_id.hlsl's BatchConstants and entity_id_mesh.hlsl's MeshIdConstants:
// BOTH 80 bytes, so ONE pipeline layout (root constants b0, vertex + fragment)
// serves the two pipelines. sizeof pinned because the layout registers the size.
struct IdRootConstants   { glm::mat4 viewProj{1.0f}; float pad[4]{}; };
struct MeshIdRootConstants { glm::mat4 mvp{1.0f}; std::uint32_t id = 0; std::uint32_t pad[3]{}; };
static_assert(sizeof(IdRootConstants) == 80 && sizeof(MeshIdRootConstants) == 80, "one layout, two shaders");
// (The HLSL pads are three scalar uints, deliberately -- see entity_id_mesh.hlsl.)
```
  New private members: `std::span<const std::uint8_t> m_meshVs, m_meshPs; static constexpr std::uint64_t kMeshShaderPairId = 0x4101; nri::VertexAttributeDesc m_meshAttributes[3]{}; nri::VertexStreamDesc m_meshStream{}; nri::VertexInputDesc m_meshVertexInput{}; glm::mat4 m_viewProj{1.0f}; struct MeshDraw { const NriMeshBufferCache::Resident* resident; glm::mat4 mvp; std::uint32_t id; }; std::vector<MeshDraw> m_meshDraws;` (include `<Arcane/Render/Nri/NriMeshBufferCache.hpp>`, `<Arcane/Mesh/MeshBuilder.hpp>` for `MeshVertex`). `RgPickHandles` gains `RgTexture depth{};   // the pass-local D32 transient (spec s7.1), supersampled like ids`.
  `Init`: load `entity_id_mesh_vs/ps` (ERROR + `return false` if missing, same wording as the 2D pair); the 2D `m_attributes[0].format = RGB32_SFLOAT` (POSITION is vec3 now), stride `sizeof(PickIdVertex)` (36); fill `m_meshAttributes` exactly as `MeshNode.cpp:206–217` (POSITION/NORMAL/TEXCOORD, `offsetof(MeshVertex, ...)`, stride `sizeof(MeshVertex)`); the root constant `size = sizeof(IdRootConstants)` and `shaderStages = VERTEX_SHADER | FRAGMENT_SHADER` (the mesh PS reads `id`).
  `PrepareDrawables`: `BuildPickIdGeometry(drawables, m_vertices, m_indices); m_viewProj = view.ViewProjection(); m_meshDraws.clear(); if (!meshBuffers) return; for (i, d) if (d.kind == Kind::Mesh) { const auto* r = meshBuffers->Resolve(d.mesh, frameCounter); if (r && r->ready && r->indexCount) m_meshDraws.push_back({ r, m_viewProj * d.world, uint32_t(i + 1) }); }`.
  `Record`: clear BOTH planes —
```cpp
nri::ClearAttachmentDesc clears[2] = {};
clears[0].planes = nri::PlaneBits::COLOR; clears[0].colorAttachmentIndex = 0; clears[0].value.color.ui = { 0u, 0u, 0u, 0u };
clears[1].planes = nri::PlaneBits::DEPTH; clears[1].value.depthStencil.depth = 1.0f; clears[1].value.depthStencil.stencil = 0;
core.CmdClearAttachments(context.cmd, clears, 2, nullptr, 0);
```
  then the 2D draw as today but `key.depthFormat = kGraphDepthFormat` and in the fill `desc.outputMerger.depth.compareOp = nri::CompareOp::NONE; desc.outputMerger.depth.write = false;` (comment: "the depth attachment is BOUND for the whole pass, so this pipeline must name its format; the 2D silhouettes neither test nor write it — later-wins by submission order, as before"), root constants `IdRootConstants push; push.viewProj = m_viewProj;`. Then, when `!m_meshDraws.empty()`: a second key `{ kMeshShaderPairId, m_layoutId, colorFormats[0] = kGraphPickIdFormat, colorCount 1, depthFormat = kGraphDepthFormat, TRIANGLE_LIST, Opaque }`, fill `vertexInput = &m_meshVertexInput; shaders = meshStages (entity_id_mesh_vs/ps); rasterization.cullMode = nri::CullMode::BACK (what MeshNode culls — pick what is drawn); outputMerger.depth.compareOp = nri::CompareOp::LESS; outputMerger.depth.write = true;`, then per draw:
```cpp
for (const MeshDraw& d : m_meshDraws)
{
    MeshIdRootConstants push; push.mvp = d.mvp; push.id = d.id;
    nri::SetRootConstantsDesc rc = {}; rc.rootConstantIndex = 0; rc.data = &push; rc.size = sizeof(push);
    core.CmdSetRootConstants(context.cmd, rc);
    nri::VertexBufferDesc vb = {}; vb.buffer = d.resident->vertexBuffer; vb.offset = 0; vb.stride = sizeof(MeshVertex);
    core.CmdSetVertexBuffers(context.cmd, 0, &vb, 1);
    core.CmdSetIndexBuffer(context.cmd, *d.resident->indexBuffer, 0, nri::IndexType::UINT32);
    nri::DrawIndexedDesc draw = {}; draw.indexNum = d.resident->indexCount; draw.instanceNum = 1;
    core.CmdDrawIndexed(context.cmd, draw);
}
m_meshDraws.clear();   // borrowed cache pointers: cleared at the last reader, as MeshNode::m_residents is
```
  The 2D early-return (`if (m_vertices.empty() || m_indices.empty()) return;`) must NOT skip the mesh draws: restructure as `if (!m_vertices.empty()) { ...2D draw... } if (!m_meshDraws.empty()) { ...mesh draw... }` after the clear. The empty-ring warning path stays for the 2D half only.
  `AddPickNodes`: `node->PrepareDrawables(context->CurrentPickables(), context->CurrentPickView(), context->MeshBuffers(), context->PresentedFrames());`; in the "pick" setup mint the depth beside the ids:
```cpp
RgTextureDesc depthDesc;
depthDesc.format       = kGraphDepthFormat;
depthDesc.width        = width  * PickNode::kSuperSample;
depthDesc.height       = height * PickNode::kSuperSample;
depthDesc.depthStencil = true;
*depth = builder.CreateTexture("pickdepth", depthDesc);
builder.Write(*depth, RgUsage::DepthWrite);
graph.SetDepthAttachment(*depth);
```
  (`auto depth = std::make_shared<RgTexture>();` like `ids`; returned in `RgPickHandles::depth`). Update the header's "WHAT IT OWNS" block and the file's top comment (the "no depth buffer anywhere on this path" sentences in `PickEmit.hpp` and `PickOutlineNodes.hpp` ~28/372 are now false — rewrite them to the order rule).

- [ ] **Step B7: The hosts hand over the view.** `EditorAppFrame.cpp` `ArmGraphViewportFrame`: after `vp.pickables = m_pickDrawables;` add `vp.pickView = m_runtime->View();` (comment: "the same ViewTransform the sprites, the mesh pass and the gizmo just used — Edit: the editor camera; Play: the scene camera SetView pushed"). Rewrite the `wantOutline` comment + predicate (~2110–2125) to drop `GizmoToolsEnabled()`: `const bool wantOutline = !InPlayMode() && (m_selection.HasSelection() || hoverLive);`, and the `m_pickDrawables.clear()` site's "Guarded on the view's Affine2D" comment (~2155). In `HandleViewportPick` (~3540–3552) delete the "FIFTH guard" and `&& GizmoToolsEnabled()` (the comment becomes: "a click in ANY view mode arms the pick: the id pass projects world drawables through FrameDesc::pickView (plan 2)"). `RuntimeFrame.cpp`: both sites set `graphFrame.pickView = io.runtime->View();` and lose the affine comments.

- [ ] **Step B8: Build and run.** `msbuild Arcane.slnx /p:Configuration=Debug /m` → 0/0. From the ArcaneTests exe dir: `.\ArcaneTests.exe "[nri]"` (frame-shape pins), `.\ArcaneTests.exe "[pick]"` (CPU + the [gpu][pick] cases — RUN them on the desk GPU, both backends), `.\ArcaneTests.exe "[witness]"` (E1/E2 still pass; the runtime's `pick@x,y` probe still answers the same sprite — grep VerifyReportTest / RuntimeWitnessTest for a `pick@` pin and run it). Expected: all PASS. If a Vulkan validation error appears about the depth attachment on the 2D pipeline, the pipeline's `depthFormat` was left UNKNOWN — it must be `kGraphDepthFormat`.

- [ ] **Step B9: Commit B**
```bash
git add data/shaders/entity_id.hlsl data/shaders/entity_id_mesh.hlsl data/shaders/compile-shaders.bat ArcaneClient/src/Arcane/Render/Nri/nodes/PickOutlineNodes.hpp ArcaneClient/src/Arcane/Render/Nri/nodes/PickOutlineNodes.cpp ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.hpp ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.cpp ArcaneClient/src/Arcane/Render/PickEmit.hpp ArcaneEditor/src/App/EditorAppFrame.cpp ArcaneRuntime/src/RuntimeFrame.cpp ArcaneTests/src/RenderGraphTest.cpp ArcaneTests/src/NriGraphPixelTest.cpp
git commit -m "feat(render): the id pass projects through FrameDesc::pickView and rasterises Mesh drawables with a pass-local D32 transient (LESS, after the depth-off 2D silhouettes) -- MeshRenderer entities are click-selectable and outlined in every view mode; the editor and runtime pick gates on AsAffine2D are lifted (F4 plan 2 T1b, spec s7.1, R8)"
```

---

### Task 2: The 3D gizmo — `Arcane::Gizmo` rewritten over `ViewTransform`

**Files:**
- Rewrite: `ArcaneClient/src/Arcane/Edit/Gizmo.hpp`, `Gizmo.cpp`
- Modify: `ArcaneCore/src/Arcane/Plugin/PluginABI.hpp` (~869–878: the v33 line, `kGamePluginABIVersion = 33`), `ReferenceProject/ReferenceProject.arcproj` (`"abi": 33`)
- Rewrite: `ArcaneTests/src/GizmoTest.cpp`
- Modify: `ArcaneTests/src/EntityOpsTest.cpp` (~392–560 the group-drag case's write-back; DELETE the `IsPlanarBasis` case at ~560–615 and its preamble comment)

**Interfaces:**
- Consumes: `ViewTransform` (`WorldToScreen`, `ScreenToRay`, `IsOrthographic`, `projection`, `view`, `viewport`), `Ray`, `Batcher2D::SetLayer/Line/Rect/Triangle` (overlay pixels).
- Produces (the WHOLE public surface of `Gizmo.hpp`):
  ```cpp
  namespace Arcane {
  class Batcher2D;
  enum class GizmoMode  { Translate, Rotate, Scale };
  enum class GizmoSpace { World, Local };
  // Translate: X/Y/Z arrows, XY/YZ/XZ plane squares, Center = camera-plane free move.
  // Rotate: X/Y/Z rings + Screen (the camera-facing ring). Scale: X/Y/Z boxes + Center = uniform.
  enum class GizmoAxis : std::uint8_t { None, X, Y, Z, XY, YZ, XZ, Center, Screen };
  struct GizmoTransform { glm::vec3 position{0}; glm::quat rotation{1,0,0,0}; glm::vec3 scale{1}; };
  struct GizmoHandleMask {           // which handles exist this frame -- HOST-SIDE (the 2D view masks, the gizmo has one code path)
      std::uint16_t bits = 0xFFFF;
      static GizmoHandleMask All() noexcept;
      static GizmoHandleMask Planar(GizmoMode mode) noexcept;   // Translate: X Y XY Center; Rotate: Z; Scale: X Y Center
      bool Has(GizmoAxis a) const noexcept; void Set(GizmoAxis a, bool on) noexcept;
  };
  struct GizmoSnap { bool enabled = false; float translate = 0.5f; float rotationDeg = 15.0f; float scale = 0.1f; };
  ARCANE_API float WorldUnitsPerPixel(const ViewTransform& view, glm::vec3 worldPoint) noexcept;
  ARCANE_API float ClosestLineParam(glm::vec3 lineOrigin, glm::vec3 lineDir, const Ray& ray) noexcept;
  ARCANE_API std::optional<glm::vec3> RayPlane(const Ray& ray, glm::vec3 planePoint, glm::vec3 planeNormal) noexcept;
  ARCANE_API GizmoAxis HitTest(GizmoMode, GizmoSpace, const GizmoTransform&, const ViewTransform&, GizmoHandleMask, float sizeScale, glm::vec2 mouseScreen);
  ARCANE_API void Draw(Batcher2D&, GizmoMode, GizmoSpace, const GizmoTransform&, const ViewTransform&, GizmoHandleMask, float sizeScale, GizmoAxis hovered, GizmoAxis active);
  ARCANE_API GizmoTransform ApplyDrag(GizmoMode, GizmoSpace, GizmoAxis, const GizmoTransform& start, const ViewTransform&, glm::vec2 mouseStartScreen, glm::vec2 mouseCurScreen, const GizmoSnap&);
  struct GizmoGroupDelta { glm::vec3 translate{0}; glm::quat rotate{1,0,0,0}; glm::vec3 scale{1}; glm::vec3 pivot{0}; };
  ARCANE_API GizmoGroupDelta MakeGroupDelta(const GizmoTransform& start, const GizmoTransform& end);
  ARCANE_API GizmoTransform  ApplyGroupDelta(const GizmoTransform& t, const GizmoGroupDelta& d);
  ARCANE_API GizmoTransform  DecomposeTRS(const glm::mat4& m);      // det < 0 => scale.x negated (UE's convention)
  ARCANE_API glm::mat4       ComposeTRS(const GizmoTransform& t);   // == Transform::ToMatrix
  ARCANE_API GizmoTransform  WithMirrorOn(const GizmoTransform& t, int axis);   // re-home a negative scale.x onto axis 1 or 2 (same matrix)
  }
  ```

- [ ] **Step 1: Write the new `GizmoTest.cpp`** (replace the file; `[gizmo]`, CPU-only; `#include <rapidcheck/catch.h>` for the properties — `WireRoundTripTest.cpp` is the pattern):

```cpp
// Arcane 3D transform-gizmo core ([gizmo], CPU-only): pure value tests over
// GizmoTransform + ViewTransform -- no Registry, no graphics device.
#include <cmath>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <rapidcheck/catch.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <Arcane/Edit/Gizmo.hpp>
#include <Arcane/Scene/Components.hpp>     // Transform::ToMatrix -- pins ComposeTRS against it
#include <Arcane/Scene/ViewTransform.hpp>

using Catch::Matchers::WithinAbs;
using namespace Arcane;

namespace
{
    constexpr float kPi = 3.14159265358979f;
    // The 2D view: orthographic, centred on the origin, 800x600 at 100 px/m
    // (halfH = 3 m). world (x, y) -> pixel (400 + 100x, 300 - 100y).
    ViewTransform Ortho() { return ViewTransform::Orthographic({0.0f, 0.0f}, 3.0f, {800u, 600u}); }
    // A perspective view on the same origin from +Z, 60 deg fov.
    ViewTransform Persp() { return ViewTransform::Perspective({0.0f, 0.0f, 6.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 60.0f, {800u, 600u}, 0.1f, 100.0f); }
    // An oblique perspective: above and to the side, so no world axis is edge-on.
    ViewTransform Oblique() { return ViewTransform::Perspective({4.0f, 3.0f, 6.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 60.0f, {800u, 600u}, 0.1f, 100.0f); }
    glm::vec2 Px(const ViewTransform& v, glm::vec3 p) { return glm::vec2(v.WorldToScreen(p)); }
    bool NearQuat(glm::quat a, glm::quat b, float eps = 1e-4f) { return std::abs(std::abs(glm::dot(a, b)) - 1.0f) < eps; }   // same rotation, either sign
}

TEST_CASE("Gizmo: WorldUnitsPerPixel is the ortho zoom in 2D and grows with distance in perspective", "[gizmo]")
{
    CHECK_THAT(WorldUnitsPerPixel(Ortho(), {0,0,0}), WithinAbs(0.01f, 1e-6f));      // 100 px per metre
    CHECK_THAT(WorldUnitsPerPixel(Ortho(), {5,5,-3}), WithinAbs(0.01f, 1e-6f));     // constant in ortho
    const float near = WorldUnitsPerPixel(Persp(), {0,0,3});    // 3 m from the eye
    const float far  = WorldUnitsPerPixel(Persp(), {0,0,-3});   // 9 m from the eye
    CHECK_THAT(far / near, WithinAbs(3.0f, 1e-3f));             // proportional to w (UnrealWidget's rule)
    // A handle of kAxisLenPx (80) at size 1 projects to 80 px in EITHER view.
    for (const ViewTransform& v : { Ortho(), Persp() })
    {
        const float R = 80.0f * WorldUnitsPerPixel(v, {0,0,0});
        CHECK_THAT(glm::length(Px(v, {R,0,0}) - Px(v, {0,0,0})), WithinAbs(80.0f, 0.5f));
    }
}

TEST_CASE("Gizmo: ClosestLineParam and RayPlane", "[gizmo]")
{
    Ray r; r.origin = {0, 0, 5}; r.direction = {0, 0, -1};
    // The ray passes 2 m along +X of the line origin: t = 2.
    CHECK_THAT(ClosestLineParam({-2, 0, 0}, {1, 0, 0}, r), WithinAbs(2.0f, 1e-5f));
    // Parallel: the projection of the ray origin onto the line.
    CHECK_THAT(ClosestLineParam({0, 0, 0}, {0, 0, 1}, r), WithinAbs(5.0f, 1e-5f));
    const auto hit = RayPlane(r, {0, 0, 1}, {0, 0, 1});
    REQUIRE(hit); CHECK_THAT(hit->z, WithinAbs(1.0f, 1e-5f));
    CHECK_FALSE(RayPlane(r, {0, 0, 0}, {1, 0, 0}));            // grazing
    Ray away = r; away.direction = {0, 0, 1};
    CHECK_FALSE(RayPlane(away, {0, 0, 1}, {0, 0, 1}));         // the plane is behind the ray
}

TEST_CASE("Gizmo ApplyDrag: translate along X, in the XY plane and in the camera plane -- 2D view", "[gizmo]")
{
    const ViewTransform v = Ortho();
    GizmoTransform start; start.position = {0, 0, 0.75f};    // an authored z: must come back UNTOUCHED
    const GizmoSnap noSnap;
    // Mouse (400,300)->(450,300) == world +0.5 along X.
    GizmoTransform rx = ApplyDrag(GizmoMode::Translate, GizmoSpace::World, GizmoAxis::X, start, v, {400,300}, {450,300}, noSnap);
    CHECK_THAT(rx.position.x, WithinAbs(0.5f, 1e-4f)); CHECK_THAT(rx.position.y, WithinAbs(0.0f, 1e-4f)); CHECK(rx.position.z == 0.75f);
    // XY plane: (400,300)->(450,350) == (+0.5, -0.5): screen DOWN is world -Y.
    GizmoTransform rp = ApplyDrag(GizmoMode::Translate, GizmoSpace::World, GizmoAxis::XY, start, v, {400,300}, {450,350}, noSnap);
    CHECK_THAT(rp.position.x, WithinAbs(0.5f, 1e-4f)); CHECK_THAT(rp.position.y, WithinAbs(-0.5f, 1e-4f)); CHECK(rp.position.z == 0.75f);
    // Center = the camera plane, which in the 2D view IS the XY plane.
    GizmoTransform rc = ApplyDrag(GizmoMode::Translate, GizmoSpace::World, GizmoAxis::Center, start, v, {400,300}, {450,350}, noSnap);
    CHECK_THAT(rc.position.x, WithinAbs(0.5f, 1e-4f)); CHECK_THAT(rc.position.y, WithinAbs(-0.5f, 1e-4f)); CHECK(rc.position.z == 0.75f);
    // Snap 0.5: a 0.37 m X drag lands on 0.5.
    GizmoSnap snap; snap.enabled = true; snap.translate = 0.5f;
    GizmoTransform rs = ApplyDrag(GizmoMode::Translate, GizmoSpace::World, GizmoAxis::X, start, v, {400,300}, {437,300}, snap);
    CHECK_THAT(rs.position.x, WithinAbs(0.5f, 1e-4f));
    // Rotation and scale untouched by a translate.
    CHECK(NearQuat(rx.rotation, start.rotation)); CHECK(rx.scale == start.scale);
}

TEST_CASE("Gizmo ApplyDrag: a LOCAL axis follows the rotation", "[gizmo]")
{
    const ViewTransform v = Ortho();
    GizmoTransform start; start.rotation = glm::angleAxis(kPi * 0.5f, glm::vec3(0, 0, 1));   // local X points +Y
    const GizmoSnap noSnap;
    // A screen drag of (+50, +30) px = world (+0.5, -0.3); only the local-X (world +Y) part projects.
    GizmoTransform r = ApplyDrag(GizmoMode::Translate, GizmoSpace::Local, GizmoAxis::X, start, v, {400,300}, {450,330}, noSnap);
    CHECK_THAT(r.position.x, WithinAbs(0.0f, 1e-4f)); CHECK_THAT(r.position.y, WithinAbs(-0.3f, 1e-4f));
}

TEST_CASE("Gizmo ApplyDrag: the Z ring turns about +Z, world sense, with snap", "[gizmo]")
{
    const ViewTransform v = Ortho();
    GizmoTransform start; GizmoSnap noSnap;
    // From world (1,0) [angle 0] to (0,-1) [100 px DOWN]: a clockwise screen sweep is a NEGATIVE world turn.
    GizmoTransform r = ApplyDrag(GizmoMode::Rotate, GizmoSpace::World, GizmoAxis::Z, start, v, {500,300}, {400,400}, noSnap);
    CHECK(NearQuat(r.rotation, glm::angleAxis(-kPi * 0.5f, glm::vec3(0, 0, 1)), 1e-3f));
    // Snap 15 deg: a ~20 deg clockwise sweep snaps to -15.
    GizmoSnap snap; snap.enabled = true; snap.rotationDeg = 15.0f;
    GizmoTransform rs = ApplyDrag(GizmoMode::Rotate, GizmoSpace::World, GizmoAxis::Z, start, v, {500,300}, {400 + 93.97f, 300 + 34.20f}, snap);
    CHECK(NearQuat(rs.rotation, glm::angleAxis(-kPi / 12.0f, glm::vec3(0, 0, 1)), 1e-3f));
    // The X ring in an oblique view: the result is a turn about WORLD X (its axis), whatever the amount.
    const ViewTransform o = Oblique();
    const glm::vec2 a = Px(o, {0, 1, 0}), b = Px(o, {0, 0, 1});   // two points on the X ring
    GizmoTransform rxr = ApplyDrag(GizmoMode::Rotate, GizmoSpace::World, GizmoAxis::X, start, o, a, b, noSnap);
    const glm::vec3 axis = glm::axis(rxr.rotation);
    CHECK_THAT(std::abs(axis.x), WithinAbs(1.0f, 1e-3f));
    CHECK_THAT(glm::angle(rxr.rotation), WithinAbs(kPi * 0.5f, 2e-2f));   // (0,1,0) -> (0,0,1) is a quarter turn about X
}

TEST_CASE("Gizmo ApplyDrag: scale by screen ratio along the projected axis, uniform, sign-preserving clamp, snap", "[gizmo]")
{
    const ViewTransform v = Ortho();
    GizmoTransform start; GizmoSnap noSnap;
    // X box at (500,300) dragged to (600,300): distance from the pivot doubles => x2 on X only.
    GizmoTransform rx = ApplyDrag(GizmoMode::Scale, GizmoSpace::Local, GizmoAxis::X, start, v, {500,300}, {600,300}, noSnap);
    CHECK_THAT(rx.scale.x, WithinAbs(2.0f, 1e-4f)); CHECK_THAT(rx.scale.y, WithinAbs(1.0f, 1e-4f)); CHECK_THAT(rx.scale.z, WithinAbs(1.0f, 1e-4f));
    // Uniform: |screen offset| 100 -> 200 px => (2,2,2).
    GizmoTransform rc = ApplyDrag(GizmoMode::Scale, GizmoSpace::Local, GizmoAxis::Center, start, v, {500,300}, {400,100}, noSnap);
    CHECK_THAT(rc.scale.x, WithinAbs(2.0f, 1e-4f)); CHECK_THAT(rc.scale.z, WithinAbs(2.0f, 1e-4f));
    // Onto the pivot: clamped to the minimum, never zero.
    GizmoTransform rz = ApplyDrag(GizmoMode::Scale, GizmoSpace::Local, GizmoAxis::X, start, v, {500,300}, {400,300}, noSnap);
    CHECK(rz.scale.x > 0.0f);
    // A MIRRORED start (scale.x = -1) keeps its sign under a scale drag.
    GizmoTransform mirrored = start; mirrored.scale.x = -1.0f;
    // (The local +X axis still projects to the RIGHT -- the scale sign lives in the
    // matrix, not the rotation -- so this grab on the negative side measures s0 = -100,
    // s1 = -200 along the projected axis: the ratio is 2 whichever side is grabbed.)
    GizmoTransform rm = ApplyDrag(GizmoMode::Scale, GizmoSpace::Local, GizmoAxis::X, mirrored, v, {300,300}, {200,300}, noSnap);
    CHECK_THAT(rm.scale.x, WithinAbs(-2.0f, 1e-4f));
    // Snap 0.1: 1.37 -> 1.4.
    GizmoSnap snap; snap.enabled = true; snap.scale = 0.1f;
    GizmoTransform rsn = ApplyDrag(GizmoMode::Scale, GizmoSpace::Local, GizmoAxis::X, start, v, {500,300}, {400 + 137.0f, 300}, snap);
    CHECK_THAT(rsn.scale.x, WithinAbs(1.4f, 1e-4f));
    // The same X drag in PERSPECTIVE gives the same factor: the ratio is taken in pixels.
    const ViewTransform p = Persp();
    const glm::vec2 pv = Px(p, {0,0,0}), tip = Px(p, {1,0,0});
    GizmoTransform rpx = ApplyDrag(GizmoMode::Scale, GizmoSpace::Local, GizmoAxis::X, start, p, tip, pv + (tip - pv) * 2.0f, noSnap);
    CHECK_THAT(rpx.scale.x, WithinAbs(2.0f, 1e-3f));
}

TEST_CASE("Gizmo HitTest: 2D view, planar mask -- axes, the XY square, the centre, the Z ring, and a miss", "[gizmo]")
{
    const ViewTransform v = Ortho();
    const GizmoTransform t;   // pivot at (400,300); R = 80 px at size 1
    const float size = 1.0f;
    const GizmoHandleMask tr = GizmoHandleMask::Planar(GizmoMode::Translate);
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, t, v, tr, size, {460, 302}) == GizmoAxis::X);
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, t, v, tr, size, {398, 240}) == GizmoAxis::Y);
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, t, v, tr, size, {440, 260}) == GizmoAxis::XY);   // the square spans 0.35R..0.65R on both axes
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, t, v, tr, size, {403, 297}) == GizmoAxis::Center);
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, t, v, tr, size, {600, 100}) == GizmoAxis::None);
    // The Z arrow is MASKED in 2D (it would project onto the pivot anyway).
    CHECK_FALSE(tr.Has(GizmoAxis::Z)); CHECK_FALSE(tr.Has(GizmoAxis::YZ)); CHECK_FALSE(tr.Has(GizmoAxis::XZ)); CHECK_FALSE(tr.Has(GizmoAxis::Screen));
    // Rotate: only the Z ring (radius 0.8R = 64 px) exists in 2D.
    const GizmoHandleMask ro = GizmoHandleMask::Planar(GizmoMode::Rotate);
    CHECK(HitTest(GizmoMode::Rotate, GizmoSpace::World, t, v, ro, size, {464, 300}) == GizmoAxis::Z);
    CHECK(HitTest(GizmoMode::Rotate, GizmoSpace::World, t, v, ro, size, {445, 300}) == GizmoAxis::None);   // 19 px inside the band
    // Scale: X box at the tip.
    const GizmoHandleMask sc = GizmoHandleMask::Planar(GizmoMode::Scale);
    CHECK(HitTest(GizmoMode::Scale, GizmoSpace::World, t, v, sc, size, {478, 301}) == GizmoAxis::X);
    // Gizmo size 2: the X tip is at 560 px; 460 is now mid-shaft and still X, 700 is a miss.
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, t, v, tr, 2.0f, {556, 300}) == GizmoAxis::X);
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, t, v, tr, 2.0f, {700, 300}) == GizmoAxis::None);
}

TEST_CASE("Gizmo HitTest: oblique perspective -- every handle is where it projects", "[gizmo]")
{
    const ViewTransform v = Oblique();
    const GizmoTransform t;
    const GizmoHandleMask all = GizmoHandleMask::All();
    const float R = 80.0f * WorldUnitsPerPixel(v, {0,0,0});
    // Each arrow's projected mid-shaft hits its axis.
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, t, v, all, 1.0f, Px(v, {R * 0.6f, 0, 0})) == GizmoAxis::X);
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, t, v, all, 1.0f, Px(v, {0, R * 0.6f, 0})) == GizmoAxis::Y);
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, t, v, all, 1.0f, Px(v, {0, 0, R * 0.6f})) == GizmoAxis::Z);
    // Each plane square's projected centre hits its plane.
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, t, v, all, 1.0f, Px(v, {R * 0.5f, R * 0.5f, 0})) == GizmoAxis::XY);
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, t, v, all, 1.0f, Px(v, {0, R * 0.5f, R * 0.5f})) == GizmoAxis::YZ);
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::World, t, v, all, 1.0f, Px(v, {R * 0.5f, 0, R * 0.5f})) == GizmoAxis::XZ);
    // A point on each ring hits that ring (ring radius 0.8R).
    CHECK(HitTest(GizmoMode::Rotate, GizmoSpace::World, t, v, all, 1.0f, Px(v, {0, 0.8f * R, 0})) == GizmoAxis::X || HitTest(GizmoMode::Rotate, GizmoSpace::World, t, v, all, 1.0f, Px(v, {0, 0.8f * R, 0})) == GizmoAxis::Z);   // (0,0.8R,0) lies on BOTH the X and Z rings; the more camera-facing wins
    CHECK(HitTest(GizmoMode::Rotate, GizmoSpace::World, t, v, all, 1.0f, Px(v, {0.8f * R * 0.7071f, 0, 0.8f * R * 0.7071f})) == GizmoAxis::Y);   // only the Y ring passes here
    // The screen ring: a pixel circle of kScreenRingRadiusPx (76) around the pivot.
    CHECK(HitTest(GizmoMode::Rotate, GizmoSpace::World, t, v, all, 1.0f, Px(v, {0,0,0}) + glm::vec2(76.0f, 0.0f)) == GizmoAxis::Screen);
    // LOCAL space with a turned entity: the X arrow follows the local X.
    GizmoTransform turned; turned.rotation = glm::angleAxis(kPi * 0.5f, glm::vec3(0, 0, 1));   // local X = world +Y
    CHECK(HitTest(GizmoMode::Translate, GizmoSpace::Local, turned, v, all, 1.0f, Px(v, {0, R * 0.6f, 0})) == GizmoAxis::X);
}

TEST_CASE("Gizmo group delta: translate shared, rotate ORBITS about the pivot, scale moves along the pivot ray, replay reproduces", "[gizmo]")
{
    GizmoTransform start; start.position = {1, 2, 3};
    GizmoTransform end = start; end.position = {4, 2, 3};
    end.rotation = glm::angleAxis(kPi * 0.5f, glm::vec3(0, 0, 1)); end.scale = {2, 2, 2};
    const GizmoGroupDelta d = MakeGroupDelta(start, end);
    CHECK(d.translate == glm::vec3(3, 0, 0)); CHECK(d.pivot == start.position); CHECK(d.scale == glm::vec3(2, 2, 2));
    // A member 1 m along +X from the pivot: scaled to 2 m, turned to +Y, then shifted.
    GizmoTransform other; other.position = {2, 2, 3};
    const GizmoTransform o = ApplyGroupDelta(other, d);
    CHECK_THAT(o.position.x, WithinAbs(1.0f + 3.0f, 1e-4f)); CHECK_THAT(o.position.y, WithinAbs(2.0f + 2.0f, 1e-4f)); CHECK_THAT(o.position.z, WithinAbs(3.0f, 1e-4f));
    CHECK(NearQuat(o.rotation, end.rotation)); CHECK(o.scale == glm::vec3(2, 2, 2));
    // Replaying onto the primary's own start reproduces `end` exactly.
    const GizmoTransform p = ApplyGroupDelta(start, d);
    CHECK_THAT(glm::length(p.position - end.position), WithinAbs(0.0f, 1e-5f)); CHECK(NearQuat(p.rotation, end.rotation)); CHECK(p.scale == end.scale);
    // A degenerate start scale yields ratio 1, not infinity.
    GizmoTransform z = start; z.scale = {0, 1, 1}; GizmoTransform ze = z; ze.scale = {5, 1, 1};
    CHECK(MakeGroupDelta(z, ze).scale.x == 1.0f);
}

TEST_CASE("Gizmo DecomposeTRS/ComposeTRS: 3D round trip, matches Transform::ToMatrix, determinant-aware for a mirror", "[gizmo]")
{
    GizmoTransform t; t.position = {1.5f, -2.0f, 0.25f}; t.rotation = glm::normalize(glm::quat(0.9f, 0.1f, 0.3f, -0.2f)); t.scale = {2.0f, 0.5f, 3.0f};
    const glm::mat4 m = ComposeTRS(t);
    Transform tf; tf.position = t.position; tf.rotation = t.rotation; tf.scale = t.scale;
    const glm::mat4 ref = tf.ToMatrix();
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) CHECK_THAT(m[c][r], WithinAbs(ref[c][r], 1e-5f));
    const GizmoTransform back = DecomposeTRS(m);
    CHECK_THAT(glm::length(back.position - t.position), WithinAbs(0.0f, 1e-5f));
    CHECK(NearQuat(back.rotation, t.rotation)); CHECK_THAT(glm::length(back.scale - t.scale), WithinAbs(0.0f, 1e-4f));
    // A Y-mirrored sprite (scale (1,-1,1)) decomposes to a NEGATIVE X (UE's convention) plus a half turn --
    // the same matrix -- and WithMirrorOn(1) re-homes the mirror onto Y with the authored rotation back.
    GizmoTransform my; my.scale = {1, -1, 1}; my.rotation = glm::angleAxis(0.4f, glm::vec3(0, 0, 1));
    const glm::mat4 mm = ComposeTRS(my);
    const GizmoTransform dm = DecomposeTRS(mm);
    CHECK(dm.scale.x < 0.0f); CHECK(dm.scale.y > 0.0f);
    const glm::mat4 again = ComposeTRS(dm);
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) CHECK_THAT(again[c][r], WithinAbs(mm[c][r], 1e-5f));
    const GizmoTransform rehomed = WithMirrorOn(dm, 1);
    CHECK_THAT(rehomed.scale.x, WithinAbs(1.0f, 1e-5f)); CHECK_THAT(rehomed.scale.y, WithinAbs(-1.0f, 1e-5f));
    CHECK(NearQuat(rehomed.rotation, my.rotation, 1e-4f));
    const glm::mat4 same = ComposeTRS(rehomed);
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) CHECK_THAT(same[c][r], WithinAbs(mm[c][r], 1e-5f));
    // WithMirrorOn is the identity when there is nothing to move (positive X) or the axis is 0.
    CHECK(WithMirrorOn(t, 1).scale == t.scale); CHECK(WithMirrorOn(dm, 0).scale == dm.scale);
}

// ---- rapidcheck: the spec s9 properties --------------------------------------
namespace
{
    glm::quat ArbQuat() { const auto ax = glm::normalize(glm::vec3(float(*rc::gen::inRange(-100, 100)) + 0.5f, float(*rc::gen::inRange(-100, 100)) + 0.25f, float(*rc::gen::inRange(-100, 100)) + 0.125f)); return glm::angleAxis(float(*rc::gen::inRange(-314, 314)) / 100.0f, ax); }
    glm::vec3 ArbVec(int lo, int hi, float div) { return { float(*rc::gen::inRange(lo, hi)) / div, float(*rc::gen::inRange(lo, hi)) / div, float(*rc::gen::inRange(lo, hi)) / div }; }
    glm::vec2 ArbPixel() { return { float(*rc::gen::inRange(50, 750)), float(*rc::gen::inRange(50, 550)) }; }
}

TEST_CASE("Gizmo property: an axis drag moves only along that axis, in both projections", "[gizmo]")
{
    rc::prop("axis-only motion", [] {
        GizmoTransform start; start.position = ArbVec(-200, 200, 100.0f); start.rotation = ArbQuat();
        const int which = *rc::gen::inRange(0, 3);
        const GizmoAxis axis = which == 0 ? GizmoAxis::X : which == 1 ? GizmoAxis::Y : GizmoAxis::Z;
        const GizmoSpace space = *rc::gen::arbitrary<bool>() ? GizmoSpace::World : GizmoSpace::Local;
        const glm::vec3 unit = axis == GizmoAxis::X ? glm::vec3(1,0,0) : axis == GizmoAxis::Y ? glm::vec3(0,1,0) : glm::vec3(0,0,1);
        const glm::vec3 dir = space == GizmoSpace::Local ? start.rotation * unit : unit;
        for (const ViewTransform& v : { Ortho(), Oblique() })
        {
            const GizmoTransform r = ApplyDrag(GizmoMode::Translate, space, axis, start, v, ArbPixel(), ArbPixel(), GizmoSnap{});
            const glm::vec3 delta = r.position - start.position;
            RC_ASSERT(glm::length(glm::cross(delta, dir)) <= 1e-3f * (1.0f + glm::length(delta)));   // parallel to the axis
            RC_ASSERT(NearQuat(r.rotation, start.rotation)); RC_ASSERT(r.scale == start.scale);
        }
    });
}

TEST_CASE("Gizmo property: a plane drag keeps the grabbed point under the cursor -- ortho and perspective agree", "[gizmo]")
{
    rc::prop("grabbed point follows the cursor", [] {
        GizmoTransform start; start.position = ArbVec(-100, 100, 100.0f);
        const glm::vec2 m0 = ArbPixel(), m1 = ArbPixel();
        for (const ViewTransform& v : { Ortho(), Oblique() })
        {
            const auto g0 = RayPlane(v.ScreenToRay(m0), start.position, glm::vec3(0, 0, 1));
            RC_PRE(g0.has_value());
            const auto g1 = RayPlane(v.ScreenToRay(m1), start.position, glm::vec3(0, 0, 1));
            RC_PRE(g1.has_value());
            const GizmoTransform r = ApplyDrag(GizmoMode::Translate, GizmoSpace::World, GizmoAxis::XY, start, v, m0, m1, GizmoSnap{});
            const glm::vec2 px = Px(v, *g0 + (r.position - start.position));
            RC_ASSERT(glm::length(px - m1) < 0.5f);   // half a pixel: inverse(VP) and back in float
            RC_ASSERT(std::abs(r.position.z - start.position.z) < 1e-6f);   // Z untouched, exactly
        }
    });
}

TEST_CASE("Gizmo property: Compose(Decompose(M)) == M for any TRS, mirrored or not; components round-trip for positive scale", "[gizmo]")
{
    rc::prop("decompose/compose", [] {
        GizmoTransform t; t.position = ArbVec(-1000, 1000, 10.0f); t.rotation = ArbQuat();
        t.scale = { float(*rc::gen::inRange(1, 500)) / 100.0f, float(*rc::gen::inRange(1, 500)) / 100.0f, float(*rc::gen::inRange(1, 500)) / 100.0f };
        const int mirror = *rc::gen::inRange(-1, 3);   // -1 none, else that axis negated
        if (mirror >= 0) t.scale[mirror] = -t.scale[mirror];
        const glm::mat4 m = ComposeTRS(t);
        const GizmoTransform d = DecomposeTRS(m);
        const glm::mat4 back = ComposeTRS(d);
        for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) RC_ASSERT(std::abs(back[c][r] - m[c][r]) < 1e-3f * (1.0f + std::abs(m[c][r])));
        if (mirror < 0) { RC_ASSERT(NearQuat(d.rotation, t.rotation, 1e-3f)); RC_ASSERT(glm::length(d.scale - t.scale) < 1e-3f * glm::length(t.scale)); }
        else { const GizmoTransform h = WithMirrorOn(d, mirror); RC_ASSERT(glm::length(h.scale - t.scale) < 1e-3f * glm::length(t.scale)); RC_ASSERT(NearQuat(h.rotation, t.rotation, 1e-3f)); }
    });
}

TEST_CASE("Gizmo property: replaying a group delta onto the primary reproduces the drag result", "[gizmo]")
{
    rc::prop("replay", [] {
        GizmoTransform s; s.position = ArbVec(-100, 100, 10.0f); s.rotation = ArbQuat(); s.scale = ArbVec(10, 300, 100.0f);
        GizmoTransform e; e.position = ArbVec(-100, 100, 10.0f); e.rotation = ArbQuat(); e.scale = ArbVec(10, 300, 100.0f);
        const GizmoTransform p = ApplyGroupDelta(s, MakeGroupDelta(s, e));
        RC_ASSERT(glm::length(p.position - e.position) < 1e-3f); RC_ASSERT(NearQuat(p.rotation, e.rotation, 1e-3f)); RC_ASSERT(glm::length(p.scale - e.scale) < 1e-3f);
    });
}
```

- [ ] **Step 2: Update `EntityOpsTest.cpp`.** In the group-drag case (~392–480) the lambda's write-back becomes fully 3D — `t->position = r.position; t->rotation = r.rotation; t->scale = r.scale;` — and `worldDelta` becomes a `glm::vec3` (`endPrimary.position += worldDelta` with `glm::vec3(5,0,0)`); the `glm::vec2 worldBeforeA(...)` reads become `glm::vec3`. DELETE the `"a tilted parent corrupts the planar decomposition, and IsPlanarBasis names it"` case (~560–615) and the comment block that introduces it; in its place add:
```cpp
TEST_CASE("a tilted parent is an ordinary parent for the 3D gizmo: a pure translate leaves the child's scale and tilt alone", "[outliner][gizmo]")
{
    World w;
    Astra::Entity parent = Edit::CreateEntity(w.reg, Astra::Entity::Invalid());
    Astra::Entity child  = Edit::CreateEntity(w.reg, parent);
    w.reg.GetComponent<Transform>(parent)->rotation = glm::angleAxis(glm::radians(45.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    Transform* tc = w.reg.GetComponent<Transform>(child);
    tc->position = glm::vec3(2.0f, 0.0f, 0.0f); tc->scale = glm::vec3(1.0f, 2.0f, 3.0f);
    tc->rotation = glm::angleAxis(0.3f, glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::quat authored = tc->rotation;
    const GizmoTransform startWorld = DecomposeTRS(Edit::WorldMatrix(w.reg, child));
    GizmoTransform moved = startWorld; moved.position += glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::mat4 localMat = glm::inverse(Edit::ParentWorldMatrix(w.reg, child)) * ComposeTRS(moved);
    const GizmoTransform r = DecomposeTRS(localMat);
    CHECK_THAT(r.scale.x, WithinAbs(1.0f, 1e-4f)); CHECK_THAT(r.scale.y, WithinAbs(2.0f, 1e-4f)); CHECK_THAT(r.scale.z, WithinAbs(3.0f, 1e-4f));
    CHECK(std::abs(std::abs(glm::dot(r.rotation, authored)) - 1.0f) < 1e-4f);   // the tilt survived
    // The world motion is the requested (1,0,0): the parent's tilt is undone by the demotion.
    const glm::vec3 worldAfter = glm::vec3((Edit::ParentWorldMatrix(w.reg, child) * ComposeTRS(r))[3]);
    CHECK_THAT(glm::length(worldAfter - (startWorld.position + glm::vec3(1.0f, 0.0f, 0.0f))), WithinAbs(0.0f, 1e-4f));
}
```
  (Ensure the file includes `<glm/gtc/quaternion.hpp>` and `<Arcane/Edit/Gizmo.hpp>` — it does for the group case.)

- [ ] **Step 3: Build ArcaneTests and confirm failure** — expected compile errors: `GizmoHandleMask`, `WorldUnitsPerPixel`, `RayPlane`, 3D `position`.

- [ ] **Step 4: Write the new `Gizmo.hpp`.** Replace the file with the *Interfaces* surface above plus these comments (keep the tone of the old header: WHY, not WHAT):
  - top: "Arcane/Edit: THE transform gizmo (ARCANE_API, editor-free, STATELESS, ONE code path for every view). Pure functions over value inputs: HitTest (which handle is under the cursor — in PIXELS on the projected handle geometry, so a projection change cannot change what is grabbable), ApplyDrag (new transform from the drag start — RAY-based through ViewTransform::ScreenToRay, so orthographic and perspective share the math and differ only in the ray constructor: Unreal's FViewportCursorLocation split), Draw (overlay pixels, top layer, no depth: ImGuizmo's posture). The editor owns all interaction state. (F4 plan 2, spec s7.2, R9.)"
  - `GizmoHandleMask`: "The 2D view is NOT a gizmo mode. It is the host hiding the handles that leave the XY plane — Translate: the Z arrow and the YZ/XZ squares; Rotate: the X and Y rings and the screen ring; Scale: the Z box — and nothing else changes: a masked-out handle cannot be hit, is not drawn, and the drags that remain leave Z exactly where it was (an axis drag along X or Y has no Z component; a plane drag in XY zeroes its normal component explicitly). The user's ruling 2026-09-17: the old 2D path is deleted, not kept as a fast path."
  - `WorldUnitsPerPixel`: "Screen-constant sizing, Unreal's rule (UnrealWidget.cpp): the handle radius in world units is kAxisLenPx × the gizmo-size setting × this — the clip-space w of the point over the projection's vertical scale and the viewport height. Collapses to the orthographic zoom in 2D (w = 1), grows with distance in perspective, so a handle is always the same size on screen and an axis pointing at the camera foreshortens the way a real object would."
  - `DecomposeTRS`: "Scale from the column lengths, rotation from the normalised basis (polar-decomposition-free; the same read ActivePerspectiveSceneCamera does). A NEGATIVE determinant is a mirror: one scale component is negative, and the decomposition cannot know which the author chose — it negates X, UE's convention (FMatrix::ExtractScaling / GetScaleVector). The editor re-homes it with WithMirrorOn onto the axis the entity's authored scale carries, so a Y-mirrored sprite dragged once does not come back as an X-mirror with a half turn in its Inspector. Assumes no shear (any product of TRS matrices). A zero-length column yields scale 0 on that axis and the identity direction for it."
  - `WithMirrorOn`: "Moves a negative X scale onto `axis` (1 = Y, 2 = Z) WITHOUT changing the matrix: S' = S·D and R' = R·D with D the diagonal flipping X and `axis` — a proper rotation (a half turn about the third axis), so the pose is identical. Identity when scale.x >= 0 or axis is 0."
  - `IsPlanarBasis` is GONE — its whole reason (a planar decomposition demoting through a tilted parent) no longer exists; state that in the changelog line, not in the header.

- [ ] **Step 5: Write the new `Gizmo.cpp`.** The complete file:

```cpp
#include <Arcane/Edit/Gizmo.hpp>
#include <Arcane/Render/Batcher2D.hpp>

#include <glm/gtc/matrix_access.hpp>     // glm::row
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace Arcane
{
    namespace
    {
        constexpr float kEps      = 1e-6f;
        constexpr float kMinScale = 0.01f;
        constexpr float kPi       = 3.14159265358979323846f;
        constexpr float kTau      = 2.0f * kPi;

        // Handle geometry in PIXELS at gizmo size 1 (the world radius follows
        // from WorldUnitsPerPixel at the pivot). Unreal's proportions.
        constexpr float kAxisLenPx          = 80.0f;   // arrow / scale-box reach
        constexpr float kRingFrac           = 0.8f;    // axis ring radius as a fraction of the reach (64 px)
        constexpr float kScreenRingRadiusPx = 76.0f;   // the camera-facing ring, a pixel circle
        constexpr float kPlaneMinFrac       = 0.35f;   // plane square from 0.35R to 0.65R along both axes
        constexpr float kPlaneMaxFrac       = 0.65f;
        constexpr float kHitThreshPx        = 8.0f;    // axis segment pick radius
        constexpr float kCenterHalfPx       = 8.0f;    // centre box half-extent
        constexpr float kRingBandPx         = 8.0f;    // ring pick band
        constexpr float kMinQuadAreaPx2     = 4.0f;    // an edge-on plane square is not a target
        constexpr float kShaftThicknessPx   = 2.0f;
        constexpr float kRingThicknessPx    = 2.0f;
        constexpr int   kRingSegments       = 48;
        constexpr float kArrowHeadLenPx     = 14.0f;
        constexpr float kArrowHeadHalfPx    = 6.0f;
        constexpr float kScaleBoxHalfPx     = 5.0f;

        glm::vec3 AxisUnit(GizmoAxis a) noexcept
        {
            switch (a)
            {
            case GizmoAxis::X: case GizmoAxis::YZ: return { 1.0f, 0.0f, 0.0f };   // a plane is named by its NORMAL here
            case GizmoAxis::Y: case GizmoAxis::XZ: return { 0.0f, 1.0f, 0.0f };
            case GizmoAxis::Z: case GizmoAxis::XY: return { 0.0f, 0.0f, 1.0f };
            default: return { 0.0f, 0.0f, 1.0f };
            }
        }

        // The axis (or plane normal) in WORLD space for the chosen frame.
        glm::vec3 AxisDir(GizmoSpace space, const glm::quat& rot, GizmoAxis a) noexcept
        {
            const glm::vec3 u = AxisUnit(a);
            return space == GizmoSpace::Local ? glm::normalize(rot * u) : u;
        }

        // The two axes a plane handle spans.
        std::pair<GizmoAxis, GizmoAxis> PlaneAxes(GizmoAxis plane) noexcept
        {
            switch (plane)
            {
            case GizmoAxis::XY: return { GizmoAxis::X, GizmoAxis::Y };
            case GizmoAxis::YZ: return { GizmoAxis::Y, GizmoAxis::Z };
            default:            return { GizmoAxis::X, GizmoAxis::Z };
            }
        }

        // The camera's forward in world space: the view matrix's rows are the
        // camera basis (right, up, BACK), so forward is minus the third row.
        glm::vec3 ViewForward(const ViewTransform& v) noexcept
        {
            return -glm::normalize(glm::vec3(glm::row(v.view, 2)));
        }

        // An orthonormal (u, v) in the plane normal to n with u x v == n, so a
        // positive angle from u toward v IS a right-hand turn about n.
        std::pair<glm::vec3, glm::vec3> PlaneBasis(glm::vec3 n) noexcept
        {
            const glm::vec3 a = std::abs(n.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
            const glm::vec3 u = glm::normalize(glm::cross(n, a));
            return { u, glm::cross(n, u) };
        }

        bool Finite(glm::vec2 p) noexcept { return std::isfinite(p.x) && std::isfinite(p.y); }

        glm::vec2 Px(const ViewTransform& v, glm::vec3 world) noexcept
        {
            return glm::vec2(v.WorldToScreen(world));
        }

        float SnapScalar(float v, float step) noexcept
        {
            return step > kEps ? std::round(v / step) * step : v;
        }

        float DistToSegment(glm::vec2 p, glm::vec2 a, glm::vec2 b) noexcept
        {
            const glm::vec2 ab = b - a;
            const float len2 = glm::dot(ab, ab);
            const float u = len2 > kEps ? glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
            return glm::length(p - (a + u * ab));
        }

        // Point-in-convex-quad in pixels (corners in order). A degenerate
        // (edge-on) quad is never inside.
        bool InsideQuad(glm::vec2 p, const std::array<glm::vec2, 4>& q) noexcept
        {
            float area = 0.0f;
            for (int i = 0; i < 4; ++i)
            {
                if (!Finite(q[i])) return false;
                const glm::vec2 a = q[i], b = q[(i + 1) % 4];
                area += a.x * b.y - b.x * a.y;
            }
            if (std::abs(area) * 0.5f < kMinQuadAreaPx2) return false;
            const float sign = area > 0.0f ? 1.0f : -1.0f;
            for (int i = 0; i < 4; ++i)
            {
                const glm::vec2 a = q[i], b = q[(i + 1) % 4];
                const glm::vec2 e = b - a, d = p - a;
                if ((e.x * d.y - e.y * d.x) * sign < 0.0f) return false;
            }
            return true;
        }

        // World radius of the handle set at the pivot.
        float Reach(const ViewTransform& v, glm::vec3 pivot, float sizeScale) noexcept
        {
            return kAxisLenPx * sizeScale * WorldUnitsPerPixel(v, pivot);
        }

        // The four world corners of a plane square (fractions of the reach).
        std::array<glm::vec3, 4> PlaneSquareWorld(glm::vec3 pivot, glm::vec3 a, glm::vec3 b, float R) noexcept
        {
            const float lo = kPlaneMinFrac * R, hi = kPlaneMaxFrac * R;
            return { pivot + a * lo + b * lo, pivot + a * hi + b * lo, pivot + a * hi + b * hi, pivot + a * lo + b * hi };
        }

        // The ring's world polyline (kRingSegments + 1 points, closed).
        std::array<glm::vec3, kRingSegments + 1> RingWorld(glm::vec3 pivot, glm::vec3 n, float radius) noexcept
        {
            const auto [u, w] = PlaneBasis(n);
            std::array<glm::vec3, kRingSegments + 1> pts{};
            for (int i = 0; i <= kRingSegments; ++i)
            {
                const float a = kTau * static_cast<float>(i) / static_cast<float>(kRingSegments);
                pts[static_cast<std::size_t>(i)] = pivot + (u * std::cos(a) + w * std::sin(a)) * radius;
            }
            return pts;
        }

        glm::vec4 Brighten(glm::vec4 c) noexcept
        {
            return { std::min(c.x * 1.4f, 1.0f), std::min(c.y * 1.4f, 1.0f), std::min(c.z * 1.4f, 1.0f), c.w };
        }

        // X red, Y green, Z blue; a plane takes the colour of its NORMAL axis
        // (Unreal / Blender); Center yellow; Screen light grey.
        glm::vec4 HandleColor(GizmoAxis a, GizmoAxis hovered, GizmoAxis active) noexcept
        {
            glm::vec4 base(0.85f, 0.2f, 0.2f, 1.0f);
            switch (a)
            {
            case GizmoAxis::Y: case GizmoAxis::XZ: base = { 0.2f, 0.85f, 0.2f, 1.0f }; break;
            case GizmoAxis::Z: case GizmoAxis::XY: base = { 0.25f, 0.4f, 0.95f, 1.0f }; break;
            case GizmoAxis::Center:                base = { 0.9f, 0.85f, 0.2f, 1.0f }; break;
            case GizmoAxis::Screen:                base = { 0.85f, 0.85f, 0.85f, 1.0f }; break;
            default: break;
            }
            return (a == hovered || a == active) ? Brighten(base) : base;
        }

        void Polyline(Batcher2D& b, const ViewTransform& v, std::span<const glm::vec3> pts, float thickness, glm::vec4 color)
        {
            for (std::size_t i = 0; i + 1 < pts.size(); ++i)
            {
                const glm::vec2 p0 = Px(v, pts[i]), p1 = Px(v, pts[i + 1]);
                if (Finite(p0) && Finite(p1)) b.Line(p0, p1, thickness, color);
            }
        }
    }

    // ---- GizmoHandleMask -------------------------------------------------------
    GizmoHandleMask GizmoHandleMask::All() noexcept { return {}; }

    GizmoHandleMask GizmoHandleMask::Planar(GizmoMode mode) noexcept
    {
        GizmoHandleMask m; m.bits = 0;
        switch (mode)
        {
        case GizmoMode::Translate: m.Set(GizmoAxis::X, true); m.Set(GizmoAxis::Y, true); m.Set(GizmoAxis::XY, true); m.Set(GizmoAxis::Center, true); break;
        case GizmoMode::Rotate:    m.Set(GizmoAxis::Z, true); break;
        case GizmoMode::Scale:     m.Set(GizmoAxis::X, true); m.Set(GizmoAxis::Y, true); m.Set(GizmoAxis::Center, true); break;
        }
        return m;
    }

    bool GizmoHandleMask::Has(GizmoAxis a) const noexcept { return ((bits >> static_cast<unsigned>(a)) & 1u) != 0u; }
    void GizmoHandleMask::Set(GizmoAxis a, bool on) noexcept
    {
        const std::uint16_t bit = static_cast<std::uint16_t>(1u << static_cast<unsigned>(a));
        bits = on ? static_cast<std::uint16_t>(bits | bit) : static_cast<std::uint16_t>(bits & ~bit);
    }

    // ---- geometry ----------------------------------------------------------------
    float WorldUnitsPerPixel(const ViewTransform& view, glm::vec3 worldPoint) noexcept
    {
        const glm::vec4 clip = view.projection * (view.view * glm::vec4(worldPoint, 1.0f));
        const float w  = std::max(std::abs(clip.w), 1e-4f);
        const float py = view.projection[1][1];
        if (view.viewport.y == 0u || std::abs(py) < kEps) return 1.0f;
        return (2.0f * w) / (py * static_cast<float>(view.viewport.y));
    }

    float ClosestLineParam(glm::vec3 lineOrigin, glm::vec3 lineDir, const Ray& ray) noexcept
    {
        // Two-lines closest points (Ericson 5.1.8) with both directions unit.
        const glm::vec3 w0 = lineOrigin - ray.origin;
        const float b  = glm::dot(lineDir, ray.direction);
        const float d0 = glm::dot(lineDir, w0);
        const float e0 = glm::dot(ray.direction, w0);
        const float denom = 1.0f - b * b;
        if (denom < 1e-5f)
            return -d0;   // parallel: the projection of the ray origin onto the line
        return (b * e0 - d0) / denom;
    }

    std::optional<glm::vec3> RayPlane(const Ray& ray, glm::vec3 planePoint, glm::vec3 planeNormal) noexcept
    {
        const float denom = glm::dot(planeNormal, ray.direction);
        if (std::abs(denom) < 1e-5f) return std::nullopt;                 // grazing
        const float s = glm::dot(planeNormal, planePoint - ray.origin) / denom;
        if (s < 0.0f) return std::nullopt;                                 // behind the ray
        return ray.origin + ray.direction * s;
    }

    // ---- HitTest ---------------------------------------------------------------
    GizmoAxis HitTest(GizmoMode mode, GizmoSpace space, const GizmoTransform& t, const ViewTransform& view,
                      GizmoHandleMask handles, float sizeScale, glm::vec2 mouse)
    {
        const glm::vec2 pivotPx = Px(view, t.position);
        if (!Finite(pivotPx)) return GizmoAxis::None;
        const float R = Reach(view, t.position, sizeScale);
        const GizmoSpace axisSpace = (mode == GizmoMode::Scale) ? GizmoSpace::Local : space;

        if (mode == GizmoMode::Rotate)
        {
            // Rings first, the most camera-facing first: an edge-on ring is a
            // line through the pivot that would otherwise steal every hit.
            const glm::vec3 fwd = ViewForward(view);
            std::array<GizmoAxis, 3> order{ GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z };
            std::sort(order.begin(), order.end(), [&](GizmoAxis a, GizmoAxis b)
            {
                return std::abs(glm::dot(AxisDir(space, t.rotation, a), fwd)) > std::abs(glm::dot(AxisDir(space, t.rotation, b), fwd));
            });
            for (GizmoAxis a : order)
            {
                if (!handles.Has(a)) continue;
                const auto ring = RingWorld(t.position, AxisDir(space, t.rotation, a), R * kRingFrac);
                for (int i = 0; i < kRingSegments; ++i)
                {
                    const glm::vec2 p0 = Px(view, ring[static_cast<std::size_t>(i)]), p1 = Px(view, ring[static_cast<std::size_t>(i + 1)]);
                    if (Finite(p0) && Finite(p1) && DistToSegment(mouse, p0, p1) <= kRingBandPx) return a;
                }
            }
            if (handles.Has(GizmoAxis::Screen) &&
                std::abs(glm::length(mouse - pivotPx) - kScreenRingRadiusPx * sizeScale) <= kRingBandPx)
                return GizmoAxis::Screen;
            return GizmoAxis::None;
        }

        // Centre wins on overlap.
        if (handles.Has(GizmoAxis::Center) &&
            std::abs(mouse.x - pivotPx.x) <= kCenterHalfPx && std::abs(mouse.y - pivotPx.y) <= kCenterHalfPx)
            return GizmoAxis::Center;

        if (mode == GizmoMode::Translate)
        {
            for (GizmoAxis plane : { GizmoAxis::XY, GizmoAxis::YZ, GizmoAxis::XZ })
            {
                if (!handles.Has(plane)) continue;
                const auto [a, b] = PlaneAxes(plane);
                const auto sq = PlaneSquareWorld(t.position, AxisDir(axisSpace, t.rotation, a), AxisDir(axisSpace, t.rotation, b), R);
                const std::array<glm::vec2, 4> q{ Px(view, sq[0]), Px(view, sq[1]), Px(view, sq[2]), Px(view, sq[3]) };
                if (InsideQuad(mouse, q)) return plane;
            }
        }

        for (GizmoAxis a : { GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z })
        {
            if (!handles.Has(a)) continue;
            const glm::vec2 tip = Px(view, t.position + AxisDir(axisSpace, t.rotation, a) * R);
            if (Finite(tip) && DistToSegment(mouse, pivotPx, tip) <= kHitThreshPx) return a;
        }
        return GizmoAxis::None;
    }

    // ---- Draw --------------------------------------------------------------------
    void Draw(Batcher2D& batcher, GizmoMode mode, GizmoSpace space, const GizmoTransform& t, const ViewTransform& view,
              GizmoHandleMask handles, float sizeScale, GizmoAxis hovered, GizmoAxis active)
    {
        batcher.SetLayer(0xFFFF, 0xFFFF);   // on top of the scene (max layer/order); overlay pixels, no depth
        const glm::vec2 pivotPx = Px(view, t.position);
        if (!Finite(pivotPx)) return;
        const float R = Reach(view, t.position, sizeScale);
        const GizmoSpace axisSpace = (mode == GizmoMode::Scale) ? GizmoSpace::Local : space;

        if (mode == GizmoMode::Rotate)
        {
            for (GizmoAxis a : { GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z })
            {
                if (!handles.Has(a)) continue;
                const auto ring = RingWorld(t.position, AxisDir(space, t.rotation, a), R * kRingFrac);
                Polyline(batcher, view, ring, kRingThicknessPx, HandleColor(a, hovered, active));
            }
            if (handles.Has(GizmoAxis::Screen))
            {
                const glm::vec4 c = HandleColor(GizmoAxis::Screen, hovered, active);
                const float r = kScreenRingRadiusPx * sizeScale;
                for (int i = 0; i < kRingSegments; ++i)
                {
                    const float a0 = kTau * static_cast<float>(i) / kRingSegments, a1 = kTau * static_cast<float>(i + 1) / kRingSegments;
                    batcher.Line(pivotPx + glm::vec2(std::cos(a0), std::sin(a0)) * r, pivotPx + glm::vec2(std::cos(a1), std::sin(a1)) * r, kRingThicknessPx, c);
                }
            }
            return;
        }

        if (mode == GizmoMode::Translate)
        {
            for (GizmoAxis plane : { GizmoAxis::XY, GizmoAxis::YZ, GizmoAxis::XZ })
            {
                if (!handles.Has(plane)) continue;
                const auto [a, b] = PlaneAxes(plane);
                const auto sq = PlaneSquareWorld(t.position, AxisDir(axisSpace, t.rotation, a), AxisDir(axisSpace, t.rotation, b), R);
                const std::array<glm::vec2, 4> q{ Px(view, sq[0]), Px(view, sq[1]), Px(view, sq[2]), Px(view, sq[3]) };
                if (!Finite(q[0]) || !Finite(q[1]) || !Finite(q[2]) || !Finite(q[3])) continue;
                glm::vec4 c = HandleColor(plane, hovered, active);
                c.w = (plane == hovered || plane == active) ? 0.6f : 0.35f;
                batcher.Triangle(q[0], q[1], q[2], c);
                batcher.Triangle(q[0], q[2], q[3], c);
            }
        }

        for (GizmoAxis a : { GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z })
        {
            if (!handles.Has(a)) continue;
            const glm::vec2 tip = Px(view, t.position + AxisDir(axisSpace, t.rotation, a) * R);
            if (!Finite(tip)) continue;
            const glm::vec4 c = HandleColor(a, hovered, active);
            batcher.Line(pivotPx, tip, kShaftThicknessPx, c);
            const glm::vec2 d = tip - pivotPx;
            const float len = glm::length(d);
            if (len < 2.0f) continue;   // pointing at the camera: a dot, no head
            const glm::vec2 dir = d / len, perp(-dir.y, dir.x);
            if (mode == GizmoMode::Translate)
            {
                const glm::vec2 base = tip - dir * kArrowHeadLenPx;
                batcher.Triangle(tip, base + perp * kArrowHeadHalfPx, base - perp * kArrowHeadHalfPx, c);
            }
            else
            {
                const glm::vec2 half(kScaleBoxHalfPx, kScaleBoxHalfPx);
                batcher.Rect(tip - half, half * 2.0f, c);
            }
        }

        if (handles.Has(GizmoAxis::Center))
        {
            const glm::vec2 half(kCenterHalfPx, kCenterHalfPx);
            batcher.Rect(pivotPx - half, half * 2.0f, HandleColor(GizmoAxis::Center, hovered, active));
        }
    }

    // ---- ApplyDrag ---------------------------------------------------------------
    GizmoTransform ApplyDrag(GizmoMode mode, GizmoSpace space, GizmoAxis axis, const GizmoTransform& start,
                             const ViewTransform& view, glm::vec2 mouseStart, glm::vec2 mouseCur, const GizmoSnap& snap)
    {
        GizmoTransform r = start;
        const Ray ray0 = view.ScreenToRay(mouseStart);
        const Ray ray1 = view.ScreenToRay(mouseCur);
        const GizmoSpace axisSpace = (mode == GizmoMode::Scale) ? GizmoSpace::Local : space;

        switch (mode)
        {
        case GizmoMode::Translate:
        {
            if (axis == GizmoAxis::X || axis == GizmoAxis::Y || axis == GizmoAxis::Z)
            {
                // Closest point between the mouse ray and the axis LINE, before and after.
                const glm::vec3 dir = AxisDir(axisSpace, start.rotation, axis);
                const float d = ClosestLineParam(start.position, dir, ray1) - ClosestLineParam(start.position, dir, ray0);
                if (snap.enabled && axisSpace == GizmoSpace::Local)
                    r.position = start.position + SnapScalar(d, snap.translate) * dir;
                else
                {
                    r.position = start.position + d * dir;
                    if (snap.enabled)   // world axis: snap the moved COMPONENT so it lands on the grid
                    {
                        const int i = axis == GizmoAxis::X ? 0 : axis == GizmoAxis::Y ? 1 : 2;
                        r.position[i] = SnapScalar(r.position[i], snap.translate);
                    }
                }
            }
            else if (axis == GizmoAxis::XY || axis == GizmoAxis::YZ || axis == GizmoAxis::XZ || axis == GizmoAxis::Center)
            {
                // Ray-plane, before and after. Center is the CAMERA plane (in the
                // 2D view that is the XY plane, so it is the old free move).
                const glm::vec3 n = axis == GizmoAxis::Center ? ViewForward(view) : AxisDir(axisSpace, start.rotation, axis);
                const auto p0 = RayPlane(ray0, start.position, n);
                const auto p1 = RayPlane(ray1, start.position, n);
                if (!p0 || !p1) break;   // grazing / behind the eye: hold the start
                glm::vec3 delta = *p1 - *p0;
                delta -= n * glm::dot(delta, n);   // EXACTLY in the plane: a 2D drag leaves z untouched to the bit
                if (snap.enabled)
                {
                    if (axis == GizmoAxis::Center || axisSpace == GizmoSpace::World)
                    {
                        glm::vec3 p = start.position + delta;
                        for (int i = 0; i < 3; ++i)
                            if (axis == GizmoAxis::Center || std::abs(n[i]) < 0.5f)   // the components the plane spans
                                p[i] = SnapScalar(p[i], snap.translate);
                        r.position = p;
                    }
                    else
                    {
                        const auto [a, b] = PlaneAxes(axis);
                        const glm::vec3 da = AxisDir(axisSpace, start.rotation, a), db = AxisDir(axisSpace, start.rotation, b);
                        r.position = start.position + SnapScalar(glm::dot(delta, da), snap.translate) * da
                                                    + SnapScalar(glm::dot(delta, db), snap.translate) * db;
                    }
                }
                else
                    r.position = start.position + delta;
            }
            break;
        }
        case GizmoMode::Rotate:
        {
            const glm::vec3 n = axis == GizmoAxis::Screen ? ViewForward(view) : AxisDir(space, start.rotation, axis);
            const auto p0 = RayPlane(ray0, start.position, n);
            const auto p1 = RayPlane(ray1, start.position, n);
            if (!p0 || !p1) break;
            const auto [u, v] = PlaneBasis(n);
            const glm::vec3 d0 = *p0 - start.position, d1 = *p1 - start.position;
            const float a0 = std::atan2(glm::dot(d0, v), glm::dot(d0, u));
            const float a1 = std::atan2(glm::dot(d1, v), glm::dot(d1, u));
            float delta = a1 - a0;   // world-sense: u x v == n, so + is a right-hand turn about n
            if (snap.enabled) delta = SnapScalar(delta, snap.rotationDeg * kPi / 180.0f);
            // n is already the WORLD direction of the chosen axis (local or not),
            // so the turn pre-multiplies in every case.
            r.rotation = glm::normalize(glm::angleAxis(delta, n) * start.rotation);
            break;
        }
        case GizmoMode::Scale:
        {
            const glm::vec2 pivotPx = Px(view, start.position);
            if (!Finite(pivotPx)) break;
            if (axis == GizmoAxis::Center)
            {
                const float l0 = glm::length(mouseStart - pivotPx), l1 = glm::length(mouseCur - pivotPx);
                const float f = l0 > kEps ? l1 / l0 : 1.0f;
                r.scale = start.scale * f;
            }
            else if (axis == GizmoAxis::X || axis == GizmoAxis::Y || axis == GizmoAxis::Z)
            {
                // Screen delta along the PROJECTED local axis, as a ratio of the
                // grab distance -- projection-independent, and the box the user
                // grabbed stays under the cursor along that axis.
                const glm::vec3 dir = AxisDir(GizmoSpace::Local, start.rotation, axis);
                const glm::vec2 tip = Px(view, start.position + dir * (10.0f * WorldUnitsPerPixel(view, start.position)));
                if (!Finite(tip)) break;
                const glm::vec2 d = tip - pivotPx;
                const float len = glm::length(d);
                if (len < kEps) break;   // the axis points at the camera: no screen direction to measure along
                const glm::vec2 dpx = d / len;
                const float s0 = glm::dot(mouseStart - pivotPx, dpx), s1 = glm::dot(mouseCur - pivotPx, dpx);
                const float f = std::abs(s0) > kEps ? s1 / s0 : 1.0f;
                const int i = axis == GizmoAxis::X ? 0 : axis == GizmoAxis::Y ? 1 : 2;
                r.scale[i] = start.scale[i] * f;
            }
            for (int i = 0; i < 3; ++i)
            {
                if (snap.enabled) r.scale[i] = SnapScalar(r.scale[i], snap.scale);
                // Clamp the MAGNITUDE and keep the sign: a mirrored entity (a
                // negative authored scale) stays mirrored through a scale drag.
                const float sign = r.scale[i] < 0.0f ? -1.0f : 1.0f;
                r.scale[i] = sign * std::max(std::abs(r.scale[i]), kMinScale);
            }
            break;
        }
        }
        return r;
    }

    // ---- group delta ---------------------------------------------------------------
    GizmoGroupDelta MakeGroupDelta(const GizmoTransform& start, const GizmoTransform& end)
    {
        GizmoGroupDelta d;
        d.translate = end.position - start.position;
        d.rotate    = glm::normalize(end.rotation * glm::inverse(start.rotation));   // the WORLD turn that takes start to end
        d.pivot     = start.position;
        for (int i = 0; i < 3; ++i)
            d.scale[i] = std::abs(start.scale[i]) > 1e-6f ? end.scale[i] / start.scale[i] : 1.0f;
        return d;
    }

    GizmoTransform ApplyGroupDelta(const GizmoTransform& t, const GizmoGroupDelta& d)
    {
        // Scale then turn the member's offset from the pivot, then shift: T*R*S about the pivot.
        const glm::vec3 rel = d.rotate * ((t.position - d.pivot) * d.scale);
        GizmoTransform r;
        r.position = d.pivot + rel + d.translate;
        r.rotation = glm::normalize(d.rotate * t.rotation);
        r.scale    = t.scale * d.scale;
        return r;
    }

    // ---- decompose / compose ------------------------------------------------------
    GizmoTransform DecomposeTRS(const glm::mat4& m)
    {
        GizmoTransform t;
        t.position = glm::vec3(m[3]);
        glm::mat3 basis(glm::vec3(m[0]), glm::vec3(m[1]), glm::vec3(m[2]));
        for (int i = 0; i < 3; ++i)
        {
            const float len = glm::length(basis[i]);
            t.scale[i] = len;
            basis[i] = len > kEps ? basis[i] / len : glm::vec3(i == 0, i == 1, i == 2);   // a dead axis keeps its identity direction
        }
        if (glm::determinant(basis) < 0.0f)
        {
            // A mirror. Negate X (UE's convention); the caller re-homes it if the
            // author put the mirror elsewhere (WithMirrorOn).
            t.scale.x = -t.scale.x;
            basis[0]  = -basis[0];
        }
        t.rotation = glm::normalize(glm::quat_cast(basis));
        return t;
    }

    glm::mat4 ComposeTRS(const GizmoTransform& t)
    {
        // translate * rotate * scale -- Transform::ToMatrix's order (pinned in GizmoTest.cpp).
        return glm::translate(glm::mat4(1.0f), t.position) * glm::mat4_cast(t.rotation) * glm::scale(glm::mat4(1.0f), t.scale);
    }

    GizmoTransform WithMirrorOn(const GizmoTransform& t, int axis)
    {
        if (t.scale.x >= 0.0f || axis < 1 || axis > 2) return t;
        // S' = S * D, R' = R * D, D = diag with -1 on X and on `axis`: a half turn
        // about the remaining axis, so R * D is still a rotation and the matrix is unchanged.
        GizmoTransform r = t;
        r.scale.x     = -t.scale.x;
        r.scale[axis] = -t.scale[axis];
        const glm::vec3 third = axis == 1 ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
        r.rotation = glm::normalize(t.rotation * glm::angleAxis(kPi, third));
        return r;
    }
}
```
  (`Polyline` takes a `std::span<const glm::vec3>` — include `<span>`; the `std::array<glm::vec3, 49>` converts.) Note for the LOCAL-space rotation identity: `AxisDir(Local, rot, a)` returns the local axis in world coordinates, so `angleAxis(delta, n) * start.rotation` is right for both spaces — write that in the comment, it is the one place a reader will doubt.

- [ ] **Step 6: The ABI line.** In `PluginABI.hpp` after the v32 block add:
```cpp
    // v33 (2026-09-17, F4 plan 2): Edit/Gizmo.hpp is 3D. GizmoTransform is
    //     { vec3, quat, vec3 }; GizmoView is REMOVED (HitTest/Draw/ApplyDrag take
    //     a ViewTransform, a GizmoHandleMask and the size scale); GizmoAxis gained
    //     Z/XY/YZ/XZ/Screen; DecomposeTRS/ComposeTRS/MakeGroupDelta/ApplyGroupDelta
    //     changed signature; IsPlanarBasis is REMOVED (its whole reason -- a planar
    //     decomposition demoting through a tilted parent -- no longer exists);
    //     WithMirrorOn, WorldUnitsPerPixel, ClosestLineParam, RayPlane are NEW.
    //     PickView (Render/PickEmit.hpp) is REMOVED and PickDrawable is world-space
    //     (same bump, plan 2 Task 1). A module built against v32 references removed
    //     exports -- refuse. ReferenceProject.arcproj restamped.
    inline constexpr uint32_t kGamePluginABIVersion = 33;
```
  and `ReferenceProject/ReferenceProject.arcproj` `"abi": 33`. Grep `kGamePluginABIVersion == 32` / `"abi": 32` / `ABI 32` across `ArcaneTests/src` and `docs` for a pinned expectation (`PluginAbiTest`?) and update it.

- [ ] **Step 7: Build the library + tests ONLY and run.** `msbuild Arcane.slnx /t:ArcaneTests /p:Configuration=Debug /m` (this builds `Arcane` + `ArcaneTests`, not the editor). Expected: 0 warnings. From the exe dir: `.\ArcaneTests.exe "[gizmo]"` → all PASS (rapidcheck runs 100 cases each; a failure prints the shrunk case — read it, the expectation may be the wrong one). `.\ArcaneTests.exe "[outliner]"` → PASS. If the `oblique` hit-test expectations miss by a few pixels, adjust the PROBE POINTS (they are approximate mid-handle points), not the thresholds.

- [ ] **Step 8: Commit** (the editor is red until Task 3 — say so in the message):
```bash
git add ArcaneClient/src/Arcane/Edit/Gizmo.hpp ArcaneClient/src/Arcane/Edit/Gizmo.cpp ArcaneCore/src/Arcane/Plugin/PluginABI.hpp ReferenceProject/ReferenceProject.arcproj ArcaneTests/src/GizmoTest.cpp ArcaneTests/src/EntityOpsTest.cpp
git commit -m "feat(client): THE 3D gizmo -- GizmoTransform {vec3, quat, vec3} over ViewTransform; pixel hit-tests on projected handles (axes, plane squares, centre, axis rings, screen ring), ray-based drags (closest-point-on-line, ray-plane), screen-constant size by projected w x the gizmo-size setting, GizmoHandleMask::Planar for the 2D view, determinant-aware decompose + WithMirrorOn; IsPlanarBasis and GizmoView retired; ABI 33 (F4 plan 2 T2, spec s7.2, R9; ArcaneEditor builds again at T3)"
```

---

### Task 3: The editor drives the one gizmo; the Perspective gates come down

**Files:**
- Modify: `ArcaneEditor/src/App/EditorAppFrame.cpp` (`UpdateGizmoInteraction` 1105–1390; the gizmo draw in `SubmitSceneToBatcher` ~1927–1950; `DrawViewportPanel`'s tool state ~3370–3380)
- Modify: `ArcaneEditor/src/App/EditorApp.hpp` (~855–892 `GizmoLive`/`GizmoToolsEnabled`; ~1080–1110 `GizmoDrag`)
- Modify: `ArcaneEditor/src/Panels/EditorPanels.hpp` (~260–277 `ViewportToolState`), `EditorPanels.cpp` (~1220–1240)

**Interfaces:**
- Consumes (Task 2): everything in `Gizmo.hpp`'s surface; (plan 1) `m_runtime->View()`, `m_camera.mode` (`Arcane::Editor::ViewMode::TwoD/Perspective`), `m_viewSettings.gizmoSize`, `Arcane::Edit::WorldMatrix/ParentWorldMatrix/SelectionRoots`, `m_undo->Begin/SnapshotComponent/Commit/Cancel`.
- Produces: no new API. `GizmoToolsEnabled()` and `ViewportToolState::gizmoToolsEnabled` are DELETED.

- [ ] **Step 1: Header state.** In `EditorApp.hpp` replace the `GizmoLive`/`GizmoToolsEnabled` block (855–892) with:
```cpp
        // "The transform gizmo may interact and draw." HoverLive()'s sibling
        // (see that predicate for why a NAMED predicate rather than "m_gizmoEnabled
        // defaults to false"): the gizmo draws INTO THE SCENE BATCH and its hover
        // comes from the live cursor, so both consequences are gated here. Since F4
        // plan 2 the gizmo works in every view mode -- the 2D view only masks its
        // Z handles (GizmoHandles() below) -- so this is the tool state alone.
        [[nodiscard]] bool GizmoLive() const noexcept { return m_gizmoEnabled; }

        // The handles the current view offers: everything in Perspective; in the
        // 2D view the planar set for the current mode (spec s7.2 -- the Z arrow,
        // the YZ/XZ squares, the X/Y rings, the Z box and the screen ring hide,
        // and Z components pass through the drags untouched). A HOST decision,
        // not a gizmo mode: Arcane::Gizmo has one code path.
        [[nodiscard]] Arcane::GizmoHandleMask GizmoHandles() const noexcept
        {
            return m_camera.mode == Arcane::Editor::ViewMode::TwoD
                 ? Arcane::GizmoHandleMask::Planar(m_gizmoMode)
                 : Arcane::GizmoHandleMask::All();
        }
```
  and the `GizmoDrag` struct's `targets` becomes
```cpp
            // Every selection ROOT carrying a Transform, with its pre-drag WORLD
            // pose and the axis (1 = Y, 2 = Z, else 0) its AUTHORED scale carries a
            // mirror on -- DecomposeTRS reads any mirror as a negative X, and the
            // write-back re-homes it (WithMirrorOn) so a Y-mirrored sprite's
            // Inspector numbers survive a drag. Roots only: a selected child rides
            // its selected parent through WorldTransform propagation.
            struct Target { Astra::Entity entity; Arcane::GizmoTransform startWorld; int mirrorAxis; };
            std::vector<Target> targets;
```
  Grep `GizmoToolsEnabled` across `ArcaneEditor/src` — every remaining use goes (Task 1 already removed the pick ones).

- [ ] **Step 2: `UpdateGizmoInteraction`.** Replace the affine gate and view construction (~1150–1165) with:
```cpp
        if (lt)
        {
            const Astra::Entity sel = m_selection.Primary();
            // WORLD pose (Transform is parent-local; the gizmo anchors at the primary's world location).
            const Arcane::GizmoTransform gt = Arcane::DecomposeTRS(Arcane::Edit::WorldMatrix(*regPtr, sel));
            const Arcane::ViewTransform& view = m_runtime->View();
            const Arcane::GizmoHandleMask handles = GizmoHandles();
            const float gizmoSize = m_viewSettings.gizmoSize;
```
  `HitTest(m_gizmoMode, m_gizmoSpace, gt, view, handles, gizmoSize, mouseScreen)`. In the targets loop DELETE the whole planar-refusal block (the `IsPlanarBasis` guard and its ~60-line comment) — the loop body becomes:
```cpp
                                const glm::mat4 worldMat = Arcane::Edit::WorldMatrix(*regPtr, e);
                                m_undo->SnapshotComponent(e, ed);
                                // The mirror axis the AUTHOR chose, if any (first negative component).
                                int mirrorAxis = 0;
                                if      (et->scale.y < 0.0f && et->scale.x >= 0.0f) mirrorAxis = 1;
                                else if (et->scale.z < 0.0f && et->scale.x >= 0.0f && et->scale.y >= 0.0f) mirrorAxis = 2;
                                m_gizmoDrag.targets.push_back({ e, Arcane::DecomposeTRS(worldMat), mirrorAxis });
```
  The drag branch: `ApplyDrag(m_gizmoMode, m_gizmoSpace, m_gizmoDrag.axis, m_gizmoDrag.start, view, m_gizmoDrag.mouseStartScreen, dragMouse, gsnap)`; the write-back loop becomes:
```cpp
                for (const auto& [e, startPose, mirrorAxis] : m_gizmoDrag.targets)
                {
                    Arcane::Transform* et = regPtr->GetComponent<Arcane::Transform>(e);
                    if (!et) continue;   // destroyed mid-drag
                    // startPose/gd are WORLD; demote through the parent's inverse
                    // before writing the LOCAL Transform (Unreal's SetWorldTransform).
                    const Arcane::GizmoTransform w = Arcane::ApplyGroupDelta(startPose, gd);
                    const glm::mat4 localMat = glm::inverse(Arcane::Edit::ParentWorldMatrix(*regPtr, e)) * Arcane::ComposeTRS(w);
                    // FULLY 3D since F4 plan 2: every component is the gizmo's to
                    // write. The only massaging is the mirror's home axis.
                    const Arcane::GizmoTransform r = Arcane::WithMirrorOn(Arcane::DecomposeTRS(localMat), mirrorAxis);
                    et->position = r.position;
                    et->rotation = r.rotation;
                    et->scale    = r.scale;
                }
```
  Delete the F1-era "Task 3 (F1): Transform is 3D but THE GIZMO IS STILL 2D" comment wholesale. Rewrite the function's opening comment: "mouseScreen is viewport-local px, the space `ViewTransform::WorldToScreen` projects into, so the gizmo aligns pixel-for-pixel with the scene in EVERY view mode (the id pass projects through the same view)".

- [ ] **Step 3: The draw site (~1927–1950):** drop `drawAffine`; `Arcane::Draw(b, m_gizmoMode, m_gizmoSpace, gt, m_runtime->View(), GizmoHandles(), m_viewSettings.gizmoSize, m_gizmoHovered, m_gizmoDrag.active ? m_gizmoDrag.axis : Arcane::GizmoAxis::None);`. Since the handle mask depends on `m_gizmoMode`, `HitTest` and `Draw` must be called with the SAME mask in one frame — they are (`GizmoHandles()` reads the same members).

- [ ] **Step 4: The tool overlay.** `EditorPanels.hpp`: remove `bool gizmoToolsEnabled;` from `ViewportToolState` and its comment (~264–266, 276). `EditorPanels.cpp` ~1220–1240: remove `toolsOff`, `offSuffix`, `BeginDisabled/EndDisabled`; the tooltips are plain `"Move (W)"`, `"Rotate (E)"`, `"Scale (R)"`. `EditorAppFrame.cpp` ~3376: drop the `GizmoToolsEnabled(),` initialiser and the sentence "gizmoToolsEnabled greys Move/Rotate/Scale in Perspective" from the comment above it. Grep `ViewportToolState` in `ArcaneTests/src` for an aggregate-init that names the field.

- [ ] **Step 5: Build everything and run.** `msbuild Arcane.slnx /p:Configuration=Debug /m` → 0/0. From the exe dir: `.\ArcaneTests.exe "[editor]"`, `"[gizmo]"`, `"[outliner]"`, `"[pick]"` → PASS. Then the WHOLE suite: `.\ArcaneTests.exe "~[gpu]"` → PASS (note the count; the close re-books it).

- [ ] **Step 6: A scripted smoke of the editor (headless).** From `bin\Debug-windows-x86_64-md\ArcaneEditor` (delete `imgui.ini` first): `.\ArcaneEditor.exe --project ReferenceProject --headless --backend dx12 --frames 60 --settle 30 --view-mode perspective --report smoke.json --compare editor-ui-perspective` → exit 0, `compare.passed == true` (nothing selected, gizmo off: the picture is unchanged). Then the same in `--view-mode 2d` against `editor-ui`. Delete `smoke.json` afterwards (never commit it).

- [ ] **Step 7: Commit**
```bash
git add ArcaneEditor/src/App/EditorAppFrame.cpp ArcaneEditor/src/App/EditorApp.hpp ArcaneEditor/src/Panels/EditorPanels.hpp ArcaneEditor/src/Panels/EditorPanels.cpp
git commit -m "feat(editor): the viewport drives the one 3D gizmo -- ViewTransform in, the 2D view masks the Z handles (GizmoHandles), gizmo size from the view settings, fully 3D write-back with the authored mirror axis re-homed; the planar refusal, GizmoToolsEnabled and the greyed Move/Rotate/Scale are gone (F4 plan 2 T3)"
```

- [ ] **Step 8: DESK PASS (USER) — hand off, do not simulate.** The items spec §9 names, now performable, for the user to run in the editor on ReferenceProject:
  - Perspective: click-select the cube (outline appears); W, drag it along X (only X changes in the Inspector); Ctrl+Z reverts the drag in ONE step; E, drag the X ring (a turn about X); R, drag the Z box.
  - Alt+LMB orbit: the gizmo stays the same size on screen; the axis pointing at the camera foreshortens.
  - 2D: the Z handles are absent; the XY square and X/Y arrows work as before; a sprite with a NEGATIVE Y scale keeps `scale = (1, -1, 1)` in the Inspector after a translate drag.
  - Click-select a sprite in Perspective (it is a world quad now); click a mesh sitting in front of a sprite: the mesh wins.
  - Gizmo size slider (view settings) scales the handles live.
  Findings come back as theories to DISPROVE (memory: desk evidence overturns hypotheses).

---

### Task 4: The `editor-ui-perspective` golden lanes (dx12 + vulkan) — Ruling P

**Files:**
- Modify: `scripts/golden-gate.ps1` (`$combos` ~307–312; `$exeArgs` ~840–849; every "four lanes"/`4 lane(s)` string and the `-SelfTest` assertions ~140–170, ~1300–1360)
- Create (only if the Vulkan render differs from the shared slot): `ReferenceProject/Verify/References/vulkan/editor-ui-perspective.png`

**Interfaces:**
- Consumes: the host's `--view-mode perspective --compare editor-ui-perspective` (plan 1 T12; `EditorWitnessTest.cpp` E2 is the existing dx12 proof); `ExpectedLevel` semantics (`shared` = resolves `References/<name>.png`, `backend` = `References/<backend>/<name>.png`).
- Produces: a lane table with an `ExtraArgs` array field; six lanes.

- [ ] **Step 1: Add `ExtraArgs` to the lane table** — each existing combo gains `ExtraArgs = @()`, and two new rows:
```powershell
    @{ Host = 'ArcaneEditor';  Exe = 'ArcaneEditor.exe';  Reference = 'editor-ui-perspective'; Backend = 'dx12';   ExpectedLevel = 'shared'; ExtraArgs = @('--view-mode', 'perspective') }
    @{ Host = 'ArcaneEditor';  Exe = 'ArcaneEditor.exe';  Reference = 'editor-ui-perspective'; Backend = 'vulkan'; ExpectedLevel = 'shared'; ExtraArgs = @('--view-mode', 'perspective') }
```
  with the comment: "The perspective editor lane (F4 plan 1's witness E2, promoted to the gate by plan 2). Keyed by BACKEND like every other lane and never by build config -- UE keys screenshot references by Platform/RHI (Ruling P, docs/plans/2026-09-17-f4-plan1-rulings-ue-check.md)." In the `$exeArgs` block append `$exeArgs += $combo.ExtraArgs` (after the `--compare` pair, before `Start-Process`). Update the header comment's lane list (lines 12–15) to six lines.

- [ ] **Step 2: Run the gate once, Debug:** `powershell -File scripts/golden-gate.ps1 -Configuration Debug`. Expected: the four old lanes PASS as before; the dx12 perspective lane PASSES (E2's slot); the vulkan perspective lane either PASSES on the shared slot (done — leave `ExpectedLevel = 'shared'`) or FAILS with a diff. If it fails: inspect the staged diff image the host writes beside the report; if the difference is the backend's own rasterisation (the editor-ui lanes are 'shared' so it is unlikely, but the perspective scene has a depth-tested grid), bless it: from the editor exe dir `.\ArcaneEditor.exe --project ReferenceProject --headless --backend vulkan --frames 60 --settle 30 --report bless.json --compare editor-ui-perspective --bless` into the STAGED slot, copy the resulting PNG to `ReferenceProject/Verify/References/vulkan/editor-ui-perspective.png` IMMEDIATELY, set that lane's `ExpectedLevel = 'backend'`, and re-run the gate. Delete `bless.json`.

- [ ] **Step 3: `-SelfTest`.** Every "all 4 lane(s)" / "four lanes" assertion and message becomes six (grep `4 lane`, `four lanes`, `-eq 4`); the self-test breaks the scene and must see ALL SIX go FAIL. Run `powershell -File scripts/golden-gate.ps1 -Configuration Debug -SelfTest` → "SELF-TEST PASSED -- all 6 lane(s) launched and caught the broken scene", exit 0, and `git status` shows the scene restored (no diff under `ReferenceProject/Content`).

- [ ] **Step 4: Commit**
```bash
git add scripts/golden-gate.ps1
# plus ReferenceProject/Verify/References/vulkan/editor-ui-perspective.png if Step 2 blessed one
git commit -m "ci(golden-gate): editor-ui-perspective joins the gate on dx12 AND vulkan (--view-mode perspective via a per-lane ExtraArgs; six lanes; -SelfTest expects six) -- Ruling P: references keyed by backend, never by build config (F4 plan 2 T4)"
```

---

### Task 5: Close — both configs, the gate, baselines, the comment sweep, docs

**Files:**
- Modify: `scripts/automation-baselines.json` (via `scripts/check-baselines.ps1`), `docs/specs/2026-09-17-f4-editor-3d-authoring-design.md` (Status line), `docs/plans/2026-08-21-nri-phase4-3d-slice.md` (Task 10's desk items), `CLAUDE.md` (the editor section: the authoring loop), every source comment that still says "plan 2" / "until plan 2" / "F4 PLAN 2" / "THE GIZMO STAYS 2D" / "no depth buffer anywhere on this path"

- [ ] **Step 1: The comment sweep (zero-legacy grep).** `grep -rn "plan 2\|Plan 2\|PLAN 2\|GIZMO STAYS 2D\|IsPlanarBasis\|PickView\|GizmoView\|gizmoToolsEnabled\|GizmoToolsEnabled\|AngleSign" --include=*.cpp --include=*.hpp --include=*.hlsl ArcaneClient/src ArcaneEditor/src ArcaneRuntime/src ArcaneCore/src data/shaders` — every hit is either (a) a comment that said "plan 2 will..." → rewrite to the present tense with what landed (e.g. `PickEmit.hpp`'s header, `SpriteGeometry.hpp`'s "in plan 2 the pick emitter", `EditorApp.hpp:1257`'s `m_pickDrawables` comment, `EditorAppFrame.cpp:1114`'s "mirrors the click-pick's PickView"), or (b) `AngleSign` still used by the physics debug overlay / the sprite path — leave those (they are not this plan; F5 owns them). Expected: zero hits of the first five patterns when done. Then `grep -rn "affine\|Affine2D" ArcaneClient/src/Arcane/Edit ArcaneClient/src/Arcane/Render/PickEmit.* ArcaneClient/src/Arcane/Render/Nri/nodes/PickOutlineNodes.*` → zero.

- [ ] **Step 2: Docs.** Spec Status line: append "plan 2 closed at <commit> (mesh picking + the 3D gizmo)". `docs/plans/2026-08-21-nri-phase4-3d-slice.md` Task 10: reword the desk items that F4 made performable ("orbit a cube", "click-select a mesh in perspective", "move it along X with the gizmo") to point at spec §9's desk pass — one sentence, no deletion of history. `CLAUDE.md` (engine): in the editor/viewport paragraph state "meshes are click-selectable in every view mode; the gizmo is 3D (2D view = planar handles)". Do NOT touch the Aphelyon repo.

- [ ] **Step 3: Both configs green.** `msbuild Arcane.slnx /p:Configuration=Release /m` → 0/0, then from the RELEASE ArcaneTests exe dir `.\ArcaneTests.exe "~[gpu]"` → PASS; then `.\ArcaneTests.exe "[gpu][pick]"` and `"[gpu][pixel]"` on the desk GPU → PASS. Rebuild Debug last.

- [ ] **Step 4: The gate, both configs, END ON DEBUG.** Delete the exe-dir `imgui.ini` for each host; `powershell -File scripts/golden-gate.ps1 -Configuration Release` then `-Configuration Debug` → all six lanes PASS. If a lane diffs: name the pixel cause (this plan expects NONE — the headless runs select nothing and the gizmo is off); a re-bless follows the STAGED-then-COPY-TO-SOURCE rule and both configs are gated again.

- [ ] **Step 5: Baselines.** `powershell -File scripts/check-baselines.ps1 -Update` (or the flag the script documents — read its header) in BOTH configs; commit the new numbers in `scripts/automation-baselines.json`. Derive, never recall.

- [ ] **Step 6: Commit the close**
```bash
git add scripts/automation-baselines.json docs/specs/2026-09-17-f4-editor-3d-authoring-design.md docs/plans/2026-08-21-nri-phase4-3d-slice.md CLAUDE.md <every swept source file>
git commit -m "chore(f4): plan 2 close -- baselines re-booked (both configs), six-lane gate green, the 'plan 2 owns this' comments retired, NRI Phase 4 Task 10's desk items point at spec s9, spec Status names the close (F4 plan 2 T5)"
```

- [ ] **Step 7: The close report** (to the user, not a commit): commits landed; the ABI is 33 — Aphelyon's `Game/` module must rebuild (`arcbuild build --project Game --config Debug`) before its editor loads it; the desk pass (Task 3 Step 8) is OWED to the user; baselines (numbers); what the gate showed; anything skipped and why.

---

## Self-review (run against the spec, fixed inline)

**Spec coverage.** §7.1: mesh drawable ✔ (T1 A5), sprite quads on the world path ✔ (T1 A5 — and every 2D kind, which is the spec's sentence generalised: the emitter no longer projects at all), pass-local depth transient after the 2D drawables ✔ (T1 B6), 2×/outline/DeferredPick unchanged ✔, hover/click semantics unchanged and `MeshRenderer` selectable + outlined ✔ (B7 lifts the gates; the outline seed reads the same ids). §7.2: `GizmoTransform` 3D + `ViewTransform` ✔ (T2), decompose/compose column-length + normalised basis ✔, `IsPlanarBasis`/bridges retired FOR THE GIZMO ✔ (physics keeps `RotationZ` — not touched), handles (3 arrows, 3 squares, centre / 3 rings + screen / 3 boxes + uniform) ✔, screen-constant size by projected `w` × the setting ✔ (`WorldUnitsPerPixel`, `sizeScale`), pixel hit-tests ✔, ray drags with the stated formulas ✔, overlay draw on top ✔, 2D = Z hidden + Z untouched ✔ (`Planar` mask; the exact `delta -= n·dot` line), undo one step / off-viewport continuation / snap ✔ (T3 keeps the transaction and `ToViewportLocal` extrapolation verbatim). §9: rapidcheck properties (axis-only, ortho/persp agreement, decompose∘compose) ✔ (T2 Step 1), desk pass ✔ (T3 Step 8, user), witness ✔ (T4 promotes E2 to the gate + Vulkan), baselines ✔ (T5). §11 plan 2 line: "retire the two 'F4 owns this' comments and reword NRI Phase 4 Task 10" ✔ (T5). Ruling P ✔ (T4). The user's ruling ✔ throughout (no 2D path survives: `Gizmo.cpp` is replaced whole).

**Gaps stated, not hidden.** Physics colliders in the id pass sit at the entity's world z (or 0) in the XY plane — in Perspective they are honest silhouettes of the 2D bodies; their DRAW overlay stays affine-gated (not this plan). The gizmo-size shortcuts, snap-to-grid coupling and "orbit around selection" stay in §12. The ~14 unenumerated minors are not here.

**Placeholder scan.** No TBD/TODO; every code step has code; the test helper names in T1 A1 are marked "adapt to the fixture the file already has" with the line range to read — that is a lookup, not a placeholder. T4 Step 2's "if it fails" branch is a real either/or with both arms written.

**Type consistency.** `PickDrawable::Kind::Mesh` (T1) ↔ `PickKindCode` `Mesh -> 4` ↔ `BuildPickIdGeometry` skips it ↔ `PickNode::PrepareDrawables` resolves it. `FrameDesc::pickView` (B5) ↔ `CurrentPickView()` ↔ `AddPickNodes` (B6) ↔ hosts (B7) ↔ tests (B2). `RgPickHandles::depth` ↔ `RgFrameHandles::pickDepth` (B1/B5). `GizmoHandleMask::Planar(GizmoMode)` (T2) ↔ `EditorApp::GizmoHandles()` (T3). `HitTest(mode, space, t, view, handles, sizeScale, mouse)` and `Draw(batcher, mode, space, t, view, handles, sizeScale, hovered, active)` and `ApplyDrag(mode, space, axis, start, view, m0, m1, snap)` are used with exactly those argument orders in T2's tests and T3's editor code. `GizmoDrag::Target { entity, startWorld, mirrorAxis }` ↔ the structured binding in the write-back. `WithMirrorOn(t, axis)` axis ∈ {1, 2} ↔ the editor's `mirrorAxis` derivation.
