# Entity-ID (Hit-Proxy) Viewport Picking — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** GPU hit-proxy picking — render each pickable entity's silhouette into an `R32_UINT` id buffer, read back the pixel under the cursor to select it. Replaces the CPU sprite-OBB pick; covers sprites + physics colliders; 3D-forward.

**Architecture:** A new `Arcane::PickBuffer` (`ARCANE_API`, sibling of `OffscreenCanvas`) owns an `R32_UINT` render target + a 1×1 staging texture + the `entity_id` pipeline. `Pick(registry, view, pixel)` collects pickable entities into an ID→Entity table while emitting their geometry, renders the id pass on-demand, copies the 1×1 pixel to staging, maps it, and returns `table[id]`. Grimoire owns one beside its `OffscreenCanvas` and calls it on viewport click.

**Tech Stack:** C++23, NVRHI (D3D12 + Vulkan), HLSL (`entity_id.hlsl`), Astra ECS, Manifold2D (collider shapes), Catch2. Spec: `docs/superpowers/specs/2026-07-19-arcane-entity-id-picking-design.md`.

## Global Constraints

- **`ARCANE_API`, editor-free engine:** `PickBuffer` lives in Arcane; Grimoire only consumes it. No editor hooks in Arcane.
- **Determinism/validation:** GPU-touching tests are `[gpu]`-tagged and MUST assert `Arcane::RenderErrorCount() == 0` (NVRHI + VK validation noise fails the suite). No `/fp:fast`. UTF-8 no BOM, ASCII comments.
- **Shaders are data:** `entity_id.hlsl` in `Arcane/shaders/`, compiled (DXIL+SPIR-V) by the Arcane prebuild into the gitignored `Arcane/shaders/generated/`; SPIR-V register shifts match `nvrhi::VulkanBindingOffsets` (t=0 s=128 b=256 u=384), same conventions as `msdf.hlsl`/`imgui.hlsl`.
- **Build (PowerShell, VS18 MSBuild):** `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "Arcane\Arcane.slnx" /p:Configuration=Debug /m`. New files (shader, .hpp/.cpp, test) → run `& "Arcane\GenerateProjects.bat"` once (premake globs), and the shader prebuild picks up `entity_id.hlsl` automatically.
- **`[gpu]` tests are DESK-DRIVEN:** the red/green loop needs a real GPU and hits the Parsec GPU-driver crash hazard headless. Run `[gpu]` tests at the desk: `Arcane\bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe "[pick]"`. The `~[gpu]` dev-loop excludes them. Headless (non-`[gpu]`) tests run anywhere.
- **Commits:** `type(scope): summary`, NO AI trailers.
- **Baseline:** `~[gpu]` 27757/326 (CPU floor, must not drop); `[gpu]` count grows by the new `[pick]` cases.

---

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `Arcane/Arcane/src/Arcane/Render/PickBuffer.hpp` | Create | `ARCANE_API PickBuffer` interface + `PickView`. |
| `Arcane/Arcane/src/Arcane/Render/PickBuffer.cpp` | Create | Impl: targets, entity collection + ID table, id pass, readback. |
| `Arcane/Arcane/src/Arcane/Render/PickEmit.hpp` | Create | `PickDrawable` (entity + geometry) + `CollectPickables(registry, view)` — the CPU emitter seam (sprite + collider), headless-testable. |
| `Arcane/shaders/entity_id.hlsl` | Create | VS (position via view) + PS (write per-instance uint id to `R32_UINT`). |
| `Arcane/Grimoire/src/GrimoireApp.cpp` | Modify | Own a `PickBuffer`; viewport click → `Pick` → selection; drop `PickEntitiesAt`. |
| `Arcane/Arcane/src/Arcane/Scene/EntityPick.{hpp,cpp}` | Delete | Superseded (after grep confirms no other consumer). |
| `Arcane/Tests/src/EntityPickTest.cpp` | Delete | Tested the retired CPU pick. |
| `Arcane/Tests/src/PickBufferTest.cpp` | Create | `[pick]` GPU tests + headless collection/table tests. |

---

## Task 1: `PickBuffer` GPU-resource skeleton

Create `PickBuffer` owning the `R32_UINT` render target + 1×1 staging texture, with `Create`/`Resize`/`~`. No rendering or picking yet — just the resources, proven by a clear+readback.

**Files:** Create `PickBuffer.hpp`, `PickBuffer.cpp`, `PickBufferTest.cpp`. Modify Arcane premake include if needed (Render is already globbed).

**Interfaces:**
- Produces: `class ARCANE_API PickBuffer { static std::unique_ptr<PickBuffer> Create(nvrhi::IDevice*, ShaderLibrary&, uint32_t w, uint32_t h); void Resize(uint32_t,uint32_t); uint32_t Width()/Height(); }` (Pick added in Task 4).

- [ ] **Step 1: Write the failing `[gpu]` test** — `PickBufferTest.cpp`. Follow the device-fixture pattern in `TonemapTest.cpp`/`OffscreenCanvasTest.cpp` (read one to copy the `RenderDevice` setup + `[gpu]` tag). Test:

```cpp
TEST_CASE("PickBuffer creates and clears its id target to 0", "[gpu][pick]")
{
    // ... obtain Arcane::RenderDevice& device via the shared GPU fixture ...
    auto pick = Arcane::PickBuffer::Create(device.Nvrhi(), shaders, 64, 64);
    REQUIRE(pick != nullptr);
    CHECK(pick->Width() == 64);
    pick->Resize(128, 72);
    CHECK(pick->Width() == 128);
    CHECK(Arcane::RenderErrorCount() == 0);
}
```

- [ ] **Step 2: Run it, verify it fails** (link error: no `PickBuffer`). Desk: `ArcaneTests.exe "[pick]"`.

- [ ] **Step 3: Implement the skeleton.** `PickBuffer.hpp` public interface as above. `PickBuffer.cpp`: on `Create`/`Resize`, build an `R32_UINT` render-target texture (`setIsRenderTarget(true).setInitialState(ResourceStates::RenderTarget).setKeepInitialState(true)`) and a 1×1 `R32_UINT` staging texture (`createStagingTexture(desc, CpuAccessMode::Read)`) — same idiom as `GpuTestHelpers.hpp`. Store `nvrhi::IDevice*`, `ShaderLibrary&`, dims. `Resize` rebuilds the target (no-op on unchanged/zero). Guard `Create` to return null + `ARC_ERROR` on any failed resource.

- [ ] **Step 4: Run at desk, verify PASS.** Regenerate projects first (new files): `& "Arcane\GenerateProjects.bat"`, then build, then `ArcaneTests.exe "[pick]"`.

- [ ] **Step 5: Commit** — `feat(arcane): PickBuffer skeleton (R32_UINT id target + staging)`.

---

## Task 2: Pickable collection + ID↔Entity table (headless)

The CPU emitter seam: walk the registry for pickable entities (sprites + physics colliders), producing an ordered list whose k-th element gets id `k+1`. Pure, headless-testable — the 3D-forward seam.

**Files:** Create `PickEmit.hpp` (+ impl inline or a small `.cpp`). Modify `PickBufferTest.cpp` (append headless tests).

**Interfaces:**
- Produces: `struct PickView { glm::vec2 cameraOffset; float pixelsPerMeter; uint32_t width, height; }` (the world→canvas mapping Grimoire feeds the scene render — confirm exact fields against `RenderContext2D`/the Sandbox camera at impl time).
- Produces:
```cpp
struct PickDrawable {
    Astra::Entity entity;
    // geometry in CANVAS pixels (y-down), ready for the id VS:
    enum class Kind { Quad, Circle, Capsule, Box } kind;
    glm::vec2 center; glm::vec2 halfExtents; float radius; float halfLen; float angle;
};
// Ordered back-to-front (front-most LAST) so the id pass's later draws win the pixel.
// out[k].entity has pass id (k+1). Physics colliders read from the registry's PhysicsResource.
ARCANE_API void CollectPickables(Astra::Registry& registry, const PickView& view,
                                 std::vector<PickDrawable>& out);
```

- [ ] **Step 1: Write failing headless tests** (no `[gpu]` tag) in `PickBufferTest.cpp`:

```cpp
TEST_CASE("CollectPickables gathers sprites and physics colliders, ordered", "[pick]")
{
    // Build a registry: RegisterScene+PhysicsComponents; a SpriteRenderer entity and a
    // physics Aabb body (mint via PhysicsSystem stepWorld=false), known camera view.
    // Assert: out has 2 entries; each entity appears once; a Quad emitter for the sprite
    // and a Box emitter for the collider; ids are 1-based by index (id == k+1 convention
    // is applied by PickBuffer, so here assert the ORDER/kinds/entities).
}
TEST_CASE("id->entity table maps 1-based, 0 is background", "[pick]")
{
    // Given a collected vector, the mapping fn: id 0 -> invalid entity; id k -> out[k-1];
    // out-of-range -> invalid.
}
```

- [ ] **Step 2: Run headless, verify fail.** `ArcaneTests.exe "[pick]~[gpu]"` (runs here, no GPU).

- [ ] **Step 3: Implement `CollectPickables` + the table map.** Sprite pass: view over `WorldTransform, SpriteRenderer` → `Quad` (center+halfExtents+angle from the world matrix, projected to canvas via `view`). Collider pass: for each `PhysicsResource.entityToBody` entry, read `Collider2D.fixtures` + the body pose → a `PickDrawable` per fixture shape (Circle/Box/Capsule; polygon → its Aabb for v1, per spec §8.2), projected to canvas. Ordering: append sprites then colliders (or by a stable draw-order key — match the scene's front-most convention; document the choice). Provide `inline Astra::Entity PickEntityForId(const std::vector<PickDrawable>&, uint32_t id)`.

- [ ] **Step 4: Run headless, verify PASS.**

- [ ] **Step 5: Commit** — `feat(arcane): pickable collection + id<->entity table (sprite + collider emitters)`.

---

## Task 3: `entity_id.hlsl` + the id render pass

Render the collected drawables into the `R32_UINT` target, each with its 1-based id, front-most winning. **This task needs Arcane's pipeline idiom — Step 0 reads it.**

**Files:** Create `entity_id.hlsl`. Modify `PickBuffer.cpp` (add the pass), `PickBufferTest.cpp`.

**Interfaces:**
- Consumes: `CollectPickables` (Task 2), the target (Task 1).
- Produces (internal): `void PickBuffer::RenderIdPass(const std::vector<PickDrawable>&, const PickView&)` — clears the target to `0` (`clearTextureUInt`), draws each drawable's silhouette with id `k+1`.

- [ ] **Step 0 (read-first — this replaces guessing renderer internals):** read `Arcane/Arcane/src/Arcane/Render/Batcher2D.*` and `TonemapPass.*` for the exact Arcane idiom: how a pass creates its `nvrhi::GraphicsPipeline` from a `ShaderLibrary` shader, sets up its `BindingLayout`/`BindingSet` + constant buffer, builds a framebuffer over a target, and issues draws. `entity_id.hlsl` and the pass below follow that idiom verbatim. Also read `msdf.hlsl` for the HLSL entry-point + register-binding conventions.

- [ ] **Step 1: Write the failing `[gpu]` test** (append to `PickBufferTest.cpp`):

```cpp
TEST_CASE("id pass writes front-most entity id at a covered pixel", "[gpu][pick]")
{
    // Two non-overlapping Aabb bodies at known canvas pixels A and B (via a known PickView).
    // Render the id pass; read back the FULL target (copyTexture whole -> staging -> map).
    // Assert target[A] == id(bodyA), target[B] == id(bodyB), a gap pixel == 0.
    // Then an overlapping pair: the covered pixel == the front-most (later-drawn) id.
    CHECK(Arcane::RenderErrorCount() == 0);
}
```

- [ ] **Step 2: Run at desk, verify fail.**

- [ ] **Step 3: Write `entity_id.hlsl`.** VS: transform a per-instance drawable (position/size/angle in canvas px) to clip space (canvas ortho, y-down, matching Batcher2D). PS: output the per-instance `uint id` to `SV_Target` (target is `R32_UINT`). Thread `id` as a per-instance vertex attribute (preferred) or a per-draw constant — pick per the Batcher2D idiom from Step 0. Circle/capsule silhouettes: tessellate to triangles (reuse the physics-debug tessellation entrypoint from Step 0's reading, or a small fan) — or, v1-simplest, draw each shape's bounding quad and discard fragments outside the analytic shape in the PS (clean, avoids tessellation). Document which.

- [ ] **Step 4: Implement `RenderIdPass`.** Build the pipeline from `entity_id.hlsl` (once, cached), a framebuffer over the target, per-instance buffer of `{center, halfExtents, radius, halfLen, angle, id, kind}`; `clearTextureUInt(target, AllSubresources, 0u)`; draw all instances (front-most last so it wins, or enable depth/`GREATER` — match Task 2's ordering decision).

- [ ] **Step 5: Run at desk, verify PASS.** (Regenerate projects for the new shader/HLSL if the prebuild list is explicit; it globs `shaders/*.hlsl` — confirm.)

- [ ] **Step 6: Commit** — `feat(arcane): entity_id id-pass renders pickable silhouettes to the id buffer`.

---

## Task 4: `Pick()` + 1×1 readback (the public API)

Add the on-demand `Pick`: render the pass, copy the 1×1 pixel under the cursor to staging, map, look up the table.

**Files:** Modify `PickBuffer.hpp` (add `Pick`), `PickBuffer.cpp`, `PickBufferTest.cpp`.

**Interfaces:**
- Produces: `virtual Astra::Entity Pick(Astra::Registry&, const PickView&, glm::vec2 pixel) = 0;` — invalid `Astra::Entity` == background.

- [ ] **Step 1: Write the failing `[gpu]` test:**

```cpp
TEST_CASE("Pick returns the entity under a pixel, invalid on background", "[gpu][pick]")
{
    // Two bodies at known pixels; CHECK(pick->Pick(reg, view, pxA) == entityA);
    // CHECK(pick->Pick(reg, view, pxB) == entityB);
    // CHECK(!reg.IsValid(pick->Pick(reg, view, gapPx)));   // background -> invalid
    // Overlap: Pick(overlapPx) == front-most entity.
    CHECK(Arcane::RenderErrorCount() == 0);
}
```

- [ ] **Step 2: Run at desk, verify fail.**

- [ ] **Step 3: Implement `Pick`.** `CollectPickables` → keep the vector; `RenderIdPass`; `copyTexture(staging1x1, TextureSlice(), target, TextureSlice().setX((uint)pixel.x).setY((uint)pixel.y).setWidth(1).setHeight(1))`; `executeCommandList` + `waitForIdle`; `mapStagingTexture` → read the `uint32` → `PickEntityForId(vec, id)` → unmap. Clamp `pixel` to `[0,w) × [0,h)`; out-of-range → invalid. `runGarbageCollection` after.

- [ ] **Step 4: Run at desk, verify PASS.**

- [ ] **Step 5: Commit** — `feat(arcane): PickBuffer::Pick — on-demand id render + 1x1 readback`.

---

## Task 5: Grimoire integration + retire the CPU pick

Wire the viewport click to `Pick`; delete the superseded `EntityPick`.

**Files:** Modify `GrimoireApp.cpp`. Delete `EntityPick.{hpp,cpp}` + `EntityPickTest.cpp`.

- [ ] **Step 1: Grep for `PickEntitiesAt` / `EntityPick` consumers** (`Grep "PickEntitiesAt|EntityPick"`). Expect only `GrimoireApp.cpp` + the test. If any other consumer exists, keep the file and only drop Grimoire's use (note it); else proceed to delete.

- [ ] **Step 2: Own a `PickBuffer` in Grimoire.** Beside `m_viewport` (`OffscreenCanvas`): `m_pick = Arcane::PickBuffer::Create(device, shaders, w, h)`; `Resize` it wherever `m_viewport` resizes. Include `<Arcane/Render/PickBuffer.hpp>`; drop `<Arcane/Scene/EntityPick.hpp>`.

- [ ] **Step 3: On viewport left-click** (the existing viewport-active, viewport-local-pixel branch): build the `PickView` from the same camera Grimoire feeds the scene render, call `Astra::Entity e = m_pick->Pick(runtime.Registry(), view, {lx, ly});` set the editor selection to `e` (invalid → clear selection). Replace the `PickEntitiesAt(...)` call. Confirm the selection state field already exists (from the Grimoire shell); reuse it.

- [ ] **Step 4: Delete** `EntityPick.hpp`, `EntityPick.cpp`, `EntityPickTest.cpp`. Regenerate projects.

- [ ] **Step 5: Build + desk-verify** — build clean; at desk, run Grimoire, click a physics body in the viewport → it selects (Inspector shows it); click empty space → deselects. Run `ArcaneTests.exe "[pick]"` + `"[grimoire]"`.

- [ ] **Step 6: Commit** — `feat(grimoire): GPU hit-proxy viewport pick; retire CPU EntityPick`.

---

## Task 6: Gate + desk-verify

- [ ] **Step 1: Headless dev-loop gate** (runs here): `ArcaneTests.exe "~[gpu]"` — the `[pick]` headless cases (Task 2) pass; existing count unchanged otherwise (floor 27757/326 + the new headless `[pick]` cases).
- [ ] **Step 2: Desk `[gpu]` gate:** at the desk, `ArcaneTests.exe "[gpu]"` (or at least `"[pick][gpu]"` + `"[grimoire]"`) → all pass, `RenderErrorCount()==0`. Record the new `[gpu]` count.
- [ ] **Step 3: Desk interactive:** Grimoire — click-select physics bodies + sprites (if any), overlap picks front-most, empty deselects, Play/Stop still fine. Then append a completion note to `.superpowers/sdd/progress.md`.

---

## Self-Review

**Spec coverage:** §3a id pass → T3; §3b emitter seam (sprite+collider) → T2; §3c id↔entity table → T2; §3d readback → T4; §4 `PickBuffer`/`PickView` API → T1/T2/T4; §5 Grimoire integration + retire EntityPick → T5; §6 tests → T2 (headless) + T1/T3/T4 (`[gpu]`); §7 non-goals (hover/3D/cycling/marquee) untouched; §8 verification points → T2 (`PickView` fields), T3 Step 0 (shader/pipeline idiom + collider tessellation), T5 Step 1 (retirement grep). Covered.

**Placeholder scan:** the render-pass task (T3) intentionally leads with a read-first step and specifies the shader behavior + the two silhouette options (tessellate vs analytic-discard) with a "document which" — this is a real, bounded decision at impl time, not a deferred TODO. The `PickView` exact fields (T2) and the per-instance-vs-constant id plumbing (T3) are the only impl-time picks, both flagged with the file to read.

**Type consistency:** `PickBuffer::Create/Resize/Pick`, `PickView{cameraOffset, pixelsPerMeter, width, height}`, `PickDrawable{entity, kind, center, halfExtents, radius, halfLen, angle}`, `CollectPickables(registry, view, out)`, `PickEntityForId(vec, id)` — consistent across tasks.
