# Physics Debug Visualization Suite — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Interactive debug rendering of the physics broadphases (per-fixture `DynamicTree`, `SpatialGrid` static+residency) and a narrowphase inspector (separating axes + GJK/EPA simplex/polytope in a Minkowski-space inset), driven by ImGui checkboxes + click-to-pin.

**Architecture:** Read-only/opt-in **Core** seams (iteration accessors + a `NarrowphaseKind` tag + an opt-in `NarrowphaseTrace` recorder threaded through `Collide`) feed **Sandbox** drawing. A new reusable **engine** `OffscreenCanvas` renders the Minkowski inset into a texture shown via `ImGui::Image`. Two slices: A = broadphase + manifold overlay (Tasks 1-2); B = narrowphase inspector (Tasks 3-5).

**Tech Stack:** C++23, Core (static-CRT + /MD dual build), NVRHI/Batcher2D/Canvas/TonemapPass, ImGui (`ImGuiNvrhi`), Catch2 (`Arcane/Tests`), premake5 (`vs2026`), MSBuild. SPEC: `docs/superpowers/specs/2026-06-23-arcane-physics-debug-viz-design.md`.

---

## Conventions (read once)

- **Build (Debug):** `"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo` (Release/Dist: swap config).
- **Tests** from the exe dir: `cd "Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe "[physics]"`. GPU tests: `"[gpu]"`. Full gate = no filter, Debug AND Release.
- **ArcaneCore static-CRT:** `"<msbuild>" "D:/dev/starworks/Gacha/Server/ArcaneCore/ArcaneCore.vcxproj" -p:Configuration=Debug|Release ...`. Core stays presentation-free (std+glm+Physics only) + builds both CRTs.
- **New source files** → regen BOTH workspaces: `cd Arcane && "<root>/ThirdParty/premake5/premake5.exe" vs2026` AND `cd Server && "<root>/ThirdParty/premake5/premake5.exe" vs2026` (ArcaneCore globs `Arcane/Core/src/**.cpp`). NOT GenerateProjects.bat (hangs on `pause`). `VCPKG_ROOT` is set.
- **clangd/IDE diagnostics are FALSE POSITIVES** (bogus "compiler version"/file-not-found). MSVC is the source of truth.
- **Kill stray Loom** before building on a plugin-lock error: `powershell -c "Get-Process Loom -ErrorAction SilentlyContinue | Stop-Process -Force"`.
- Branch: `feature/arcane-physics-debug-viz` (already created; spec committed at `e330c4c`). Commit per task; trailer `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`. Do NOT push.
- **Determinism contract holds:** every Core addition is read-only or opt-in (`nullptr` recorder on the Step path); nothing here feeds the simulation. Full `[physics]` suite must stay green after each Core task.

## Architecture facts (from the codebase map — rely on these; locate exact lines by grep)

- **Existing overlay:** `PhysicsDebugRenderSystem` (inline in `Arcane/Sandbox/src/SandboxApp.hpp` ~109-142) reads `RenderContext2D` (`ctx->batcher`, `ctx->cameraOffset`, `ctx->zoom`) + `PhysicsResource` (`phys->world`) + `SandboxDebugDraw` resource, copies flags into `PhysicsDebugDrawOptions opts`, calls `DrawPhysicsDebug(*phys->world, *ctx->batcher, opts)`.
- **`DrawPhysicsDebug`** in `Arcane/Arcane/src/Arcane/Render/PhysicsDebugDraw.cpp` (+ `PhysicsDebugDrawOptions` in `.hpp`). Helper `ToScreen(wpos, offset, zoom) = wpos*zoom + offset`. Draws via `Batcher2D::Line(a,b,thick,color)`, `Circle(center,radius,color)`, `Rect(pos,size,color,rot)` (all SCREEN space). Reads `world.Count()`, `Alive(i)`, `ShapeSlot(i)`, `PosSlot(i)`, `TypeSlot(i)`, `SlotAabb(i)`, `VelSlot(i)`, `ForEachContact(fn)` etc.
- **Toggle pattern:** add a `bool` to `SandboxDebugDraw` (struct in `SandboxApp.hpp`) → `SandboxApp::PublishDebug(reg)` (SandboxApp.cpp ~93-103) mirrors `m_debug` into the `SandboxDebugDraw` registry resource each FixedUpdate → `PhysicsDebugRenderSystem` copies into `PhysicsDebugDrawOptions` → gated block in `DrawPhysicsDebug`. HUD: `Hud.cpp` "Debug draw" `CollapsingHeader` writes `app.DebugOptionsMut()` via `ImGui::Checkbox`.
- **Broadphase accessors today:** `world.FixtureBroadphase() const -> const IBroadphase&` (has `QueryAABB`, `Pairs`, `UpdatePairs`); `world.LiveFixtureAabbs(fxOut, boxOut)`. `m_staticGrid`/`m_residencyGrid` are PRIVATE (no getters). `DynamicTree` (Broadphase/DynamicTree.hpp): private `m_nodes` (`Node{ Aabb2 fat; Aabb2 tight; ... uint32_t id; bool IsLeaf(); }`) + `m_leafOfId`. `SpatialGrid` (Broadphase/SpatialGrid.hpp): private `m_cells` (`unordered_map<uint64_t key, vector<uint32_t>>`), public `TileSize()`, `Origin()`, `CellCoord(Vec2,int&,int&)`; key packs `(cx<<32)|cy` via private `Key`.
- **Contacts:** `world.ForEachContact(fn)` yields only body-slot pairs (a,b). `world.ActiveContactCount()`. The manifold pool `m_contactConstraints` (`vector<ContactConstraint>`) is PRIVATE; `ContactConstraint` has `bodyA/bodyB`, `normal`, `pointCount`, `points[2]{ anchorA, anchorB, baseSeparation }`. Filled by `GenerateContacts`, valid until next `Step`.
- **Narrowphase:** `Collide(const Shape& a, const Transform& xfA, const Shape& b, const Transform& xfB, Real speculativeMargin=0) -> Manifold` in `Arcane/Core/src/Arcane/Physics/Narrowphase/Collide.hpp/.cpp`. Dispatches to SAT (poly-poly), GJK/EPA (deep), MPR (fallback), circle/capsule (analytic). `Manifold` struct (grep its definition — likely `Narrowphase/Manifold.hpp` or in Collide.hpp): `pointCount`, `points[]{ separation, normal, id }`.
- **Render building blocks (engine, host-owned):** `Batcher2D` (`Arcane/Arcane/src/Arcane/Render/Batcher2D.hpp`, `Create(device->Nvrhi(), shaders)`, `Begin(cmdList, fb, w, h)`, `Line/Circle/Rect`, `End()`), `Canvas` (`CreateCanvas(nvrhi, w, h)` → RGBA16F target + framebuffer), `TonemapPass` (`Create(nvrhi, shaders)`, `Run(cmdList, srcTex, dstFb)`), `ShaderLibrary`. `ImGuiNvrhi` (`Arcane/Arcane/src/Arcane/ImGui/`) binds user textures: `tex->SetTexID((ImTextureID)(intptr_t)nvrhiTexHandle.Get())`; per-`ITexture*` binding-set cache.

## File Structure

- Modify `Arcane/Core/src/Arcane/Physics/Broadphase/DynamicTree.hpp/.cpp` — `ForEachLeaf`.
- Modify `Arcane/Core/src/Arcane/Physics/Broadphase/SpatialGrid.hpp/.cpp` — `ForEachCell`.
- Modify `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp/.cpp` — `StaticGrid()`/`ResidencyGrid()`, `ForEachContactConstraint`, `DebugCollide`.
- Modify `Arcane/Core/src/Arcane/Physics/Narrowphase/Manifold.hpp` (or where `Manifold`/`ContactConstraint` live) — `NarrowphaseKind kind`.
- Modify `Arcane/Core/src/Arcane/Physics/Narrowphase/Collide.hpp/.cpp` (+ SAT/GJK/EPA/MPR sub-TUs) — set `kind`; thread optional `NarrowphaseTrace*`.
- Create `Arcane/Core/src/Arcane/Physics/Narrowphase/NarrowphaseTrace.hpp` — the recorder struct + `NarrowphaseKind` enum.
- Create `Arcane/Arcane/src/Arcane/Render/OffscreenCanvas.hpp/.cpp` — the engine helper.
- Modify `Arcane/Arcane/src/Arcane/Render/PhysicsDebugDraw.hpp/.cpp` — broadphase/manifold/inspector-overlay draw blocks.
- Modify `Arcane/Sandbox/src/SandboxApp.hpp/.cpp`, `Hud.cpp`, `Interaction.hpp/.cpp` — toggles, click-to-pin, the inspector window.
- Tests: `Arcane/Tests/src/PhysicsDebugAccessorsTest.cpp` (Core accessors + DebugCollide), `Arcane/Tests/src/OffscreenCanvasTest.cpp` (`[gpu]`).

---

### Task 1: Core read accessors + `NarrowphaseKind` tag (Slice A foundation)

**Files:** DynamicTree.hpp/.cpp, SpatialGrid.hpp/.cpp, PhysicsWorld.hpp/.cpp, the `Manifold`/`ContactConstraint` header, Collide.cpp; Create `NarrowphaseTrace.hpp` (enum only this task); Test `PhysicsDebugAccessorsTest.cpp`.

- [ ] **Step 1: Write the failing test**

Create `Arcane/Tests/src/PhysicsDebugAccessorsTest.cpp`:

```cpp
// Read-only debug accessors: enumerate exactly the live structures, and tag each
// contact with the narrowphase that produced it. Determinism/behavior unchanged.
#include <algorithm>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Body.hpp>
#include <Arcane/Physics/Broadphase/Broadphase.hpp>

using namespace Arcane::Physics;

TEST_CASE("Debug accessors enumerate broadphase + contacts", "[physics][debugviz]")
{
    PhysicsWorld w;
    auto addBox = [&](Real x, Real y, BodyType t){ BodyDef d; d.type=t; d.position=Vec2(x,y);
        d.fixedRotation=true; d.shape=MakeAabb(Real(10),Real(10)); return w.AddBody(d); };
    addBox(0, 100, BodyType::Static);     // a static -> static grid
    addBox(0,  79, BodyType::Dynamic);    // overlaps the static -> a contact
    w.Step(Real(1)/Real(60));

    // (a) ForEachLeaf yields live mover fixtures, tight inside fat.
    std::size_t leaves = 0;
    w.FixtureBroadphaseTree().ForEachLeaf([&](std::uint32_t, const Aabb2& tight, const Aabb2& fat){
        ++leaves;
        REQUIRE(fat.min.x <= tight.min.x); REQUIRE(fat.max.x >= tight.max.x);
        REQUIRE(fat.min.y <= tight.min.y); REQUIRE(fat.max.y >= tight.max.y);
    });
    REQUIRE(leaves >= 1);

    // (b) static grid has >=1 occupied cell.
    std::size_t cells = 0;
    w.StaticGrid().ForEachCell([&](int, int, const std::vector<std::uint32_t>& ids){
        REQUIRE(!ids.empty()); ++cells; });
    REQUIRE(cells >= 1);

    // (c) ForEachContactConstraint count == ActiveContactCount, and kind is set.
    std::size_t n = 0; bool kindSet = false;
    w.ForEachContactConstraint([&](const ContactConstraint& cc){
        ++n; if (cc.kind != NarrowphaseKind::Separated) kindSet = true; });
    REQUIRE(n == w.ActiveContactCount());
    if (n > 0) REQUIRE(kindSet);
}
```

VERIFY symbol names by reading the headers: the per-fixture tree accessor name (`FixtureBroadphaseTree()` — add it as a `const DynamicTree&` getter since `FixtureBroadphase()` returns the `IBroadphase` base which lacks `ForEachLeaf`), `ContactConstraint`, `NarrowphaseKind`. Adjust the test to the real names you settle on, but keep the three assertions.

- [ ] **Step 2: Regen both workspaces + build + verify COMPILE FAIL** (`[debugviz]`). New test file + new symbols → fail.

- [ ] **Step 3: Add `NarrowphaseKind` + the accessors**

Create `Arcane/Core/src/Arcane/Physics/Narrowphase/NarrowphaseTrace.hpp` with (this task: the enum only; the trace struct lands in Task 3):
```cpp
#pragma once
#include <cstdint>
namespace Arcane { namespace Physics {
    enum class NarrowphaseKind : std::uint8_t
    { Separated = 0, CircleCircle, CircleVsPolygon, Capsule, SatPolygon, Epa, Mpr };
}}
```
- Add `NarrowphaseKind kind = NarrowphaseKind::Separated;` to the `Manifold` struct (include NarrowphaseTrace.hpp there) and carry it to `ContactConstraint` (set in `emit`/`GenerateContacts` from the manifold's `kind`).
- In `Collide` (and each dispatch branch), set the result manifold's `kind` at the point each path resolves (CircleCircle, CircleVsPolygon, Capsule, SatPolygon; the EPA path sets `Epa`; the MPR fallback sets `Mpr`; no contact → leave `Separated`). One field write per branch.
- `DynamicTree::ForEachLeaf(const std::function<void(std::uint32_t,const Aabb2&,const Aabb2&)>& fn) const` — iterate `m_leafOfId` (skip `kNull`), or `m_nodes` where `IsLeaf()`, yielding `(node.id, node.tight, node.fat)`.
- `PhysicsWorld::FixtureBroadphaseTree() const -> const DynamicTree&` (return the concrete `m_fixtureBroadphase`; needs `static_cast`/`dynamic_cast` from `unique_ptr<IBroadphase>` OR store/return the concrete type — simplest: a `const DynamicTree*` getter that returns the tree only when the strategy is Tree, else nullptr; the test uses the default Tree). Decide + document; the Sandbox checks for null.
- `SpatialGrid::ForEachCell(const std::function<void(int,int,const std::vector<std::uint32_t>&)>& fn) const` — iterate `m_cells`, split key `cx = int(k>>32), cy = int(k & 0xFFFFFFFF)`.
- `PhysicsWorld::StaticGrid() const -> const SpatialGrid&` and `ResidencyGrid() const -> const SpatialGrid&`.
- `PhysicsWorld::ForEachContactConstraint(const std::function<void(const ContactConstraint&)>& fn) const` — walk `m_contactConstraints`.

- [ ] **Step 4: Build Debug + `[debugviz]` pass; then full `[physics]` green** (behavior unchanged — these are read-only + a field default). Paste both summaries.

- [ ] **Step 5: ArcaneCore static-CRT Debug clean** (regen Server first — new Core FILE `NarrowphaseTrace.hpp` is header-only; if ArcaneCore globs only `.cpp`, a header needs no regen, but `Collide.cpp`/`PhysicsWorld.cpp` changes recompile — confirm clean).

- [ ] **Step 6: Commit**
```bash
git add Arcane/Core/src/Arcane/Physics Arcane/Tests/src/PhysicsDebugAccessorsTest.cpp
git commit -m "feat(arcane/physics): debug read accessors (ForEachLeaf/ForEachCell/ForEachContactConstraint) + NarrowphaseKind tag

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: Slice A overlay — broadphase + manifold checkboxes

**Files:** `SandboxApp.hpp` (`SandboxDebugDraw` + `PhysicsDebugRenderSystem` opts copy), `PhysicsDebugDraw.hpp/.cpp` (opts + draw blocks), `Hud.cpp` (checkboxes). No new Core. Visual gate (no unit test — Sandbox draw is smoke-tested via build + the existing `[sandbox]`/visuals suite staying green).

- [ ] **Step 1: Add the toggles (mirror the existing `drawAabbs` pattern EXACTLY)**

In `SandboxDebugDraw` (SandboxApp.hpp): add `bool drawFixtureTree=false, drawStaticGrid=false, drawResidencyGrid=false, drawManifolds=false;`. In `PhysicsDebugDrawOptions` (PhysicsDebugDraw.hpp): add the same four. In `PhysicsDebugRenderSystem::operator()` opts-copy block: copy the four. In `Hud.cpp` "Debug draw" header: four `ImGui::Checkbox` lines following the `Contacts`/`AABBs` pattern. (`PublishDebug` already mirrors the whole `m_debug` struct — no change there.)

- [ ] **Step 2: Add the draw blocks in `DrawPhysicsDebug` (PhysicsDebugDraw.cpp)**

Add four gated blocks (use the existing `ToScreen` + `batcher.Line/Rect/Circle`; reuse `opts.lineThickness`):
- `if (opts.drawFixtureTree)`: if `world.FixtureBroadphaseTree()` is available (non-null), `ForEachLeaf([&](id,tight,fat){ DrawAabbOutline(tight, ...); DrawAabbOutline(fat, dimmerColor, ...); })`; then `world.FixtureBroadphase().Pairs(scratch)` and for each pair draw a `Line` between the two fixtures' AABB centers (recover centers via `world.LiveFixtureAabbs` into a `slot->center` lookup, or via `ForEachLeaf` building an `id->center` map first).
- `if (opts.drawStaticGrid)`: `world.StaticGrid().ForEachCell([&](cx,cy,ids){ ... })` → cell world AABB = `min = Origin + (cx,cy)*TileSize`, `max = min + TileSize`; draw a tinted `Rect` (or 4 `Line`s) in color StaticGridColor.
- `if (opts.drawResidencyGrid)`: same on `world.ResidencyGrid()` in a distinct tint.
- `if (opts.drawManifolds)`: `world.ForEachContactConstraint([&](cc){ for each point: world pt = bodyCOM + anchor; draw Circle(pt, r); draw Line(pt, pt + normal*kArrowLen) for the normal; color = ManifoldColor(cc.kind) })`. Add a `ManifoldColor(NarrowphaseKind)` palette helper in PhysicsDebugDraw.cpp.

(Recover world COM from the body: read the spec §5 — `anchor + body-COM`; use the existing body accessors `PosSlot`/`LocalCenterSlot`/`GetAngle` as `DrawPhysicsDebug` already does for COM markers.)

- [ ] **Step 2.5: Premake regen NOT needed** (no new files). Build Debug.

- [ ] **Step 3: Build + `[physics]` + the Sandbox visuals suite green** (`./ArcaneTests.exe "[sandbox]"` or the visuals tag — confirm the existing Sandbox tests still pass; the new toggles default off so behavior is unchanged).

- [ ] **Step 4: Visual smoke (Dist or Debug Loom):** build, run Loom scene 8, toggle each of the four checkboxes, confirm: fixture proxy boxes + fat boxes + pair lines; static cells; residency cells; colored manifold points+normals. (Report what you saw; this is a visual gate — no assertion.)

- [ ] **Step 5: Commit**
```bash
git add Arcane/Sandbox/src/SandboxApp.hpp Arcane/Arcane/src/Arcane/Render/PhysicsDebugDraw.hpp Arcane/Arcane/src/Arcane/Render/PhysicsDebugDraw.cpp Arcane/Sandbox/src/Hud.cpp
git commit -m "feat(arcane/sandbox): broadphase + manifold debug overlay toggles (Slice A)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

**SLICE A COMPLETE after Task 2 — broadphase + manifold viz is usable.**

---

### Task 3: `NarrowphaseTrace` recorder + threading + `DebugCollide` (Slice B Core)

**Files:** `NarrowphaseTrace.hpp` (the struct), Collide.hpp/.cpp + SAT/GJK/EPA/MPR sub-TUs, PhysicsWorld.hpp/.cpp (`DebugCollide`); Test: extend `PhysicsDebugAccessorsTest.cpp`.

- [ ] **Step 1: Write the failing reproduction + recorder test**

Append to `PhysicsDebugAccessorsTest.cpp`:
```cpp
#include <Arcane/Physics/Narrowphase/NarrowphaseTrace.hpp>

TEST_CASE("DebugCollide reproduces the manifold + records a trace", "[physics][debugviz]")
{
    PhysicsWorld w;
    BodyDef d; d.type=BodyType::Dynamic; d.fixedRotation=true; d.shape=MakeAabb(Real(10),Real(10));
    d.position=Vec2(0,0);  BodyHandle a = w.AddBody(d);
    d.position=Vec2(15,0); BodyHandle b = w.AddBody(d); // overlapping boxes -> SAT/EPA
    w.Step(Real(1)/Real(60));

    // The fixture slots of a,b: each body has its back-compat fixture (slot via the world).
    // Use the body's primary fixture handle accessor (grep: GetBodyFixture / FixtureOf).
    FixtureHandle fa = w.GetBodyFixture(a, 0);
    FixtureHandle fb = w.GetBodyFixture(b, 0);

    NarrowphaseTrace trace;
    Manifold m = w.DebugCollide(fa, fb, trace);
    REQUIRE(m.pointCount >= 1);                 // they overlap
    REQUIRE(trace.kind == m.kind);              // trace tags the algorithm
    REQUIRE(trace.kind != NarrowphaseKind::Separated);
    // For a poly-poly overlap, SAT recorded >=1 candidate axis OR EPA >=1 polytope snapshot.
    REQUIRE((trace.satAxes.size() >= 1 || trace.epaSnapshots.size() >= 1));
}
```
VERIFY `GetBodyFixture`/equivalent exists (grep PhysicsWorld.hpp; the map noted `GetBodyFixture(bh, i)`); adjust to the real accessor. Settle the `NarrowphaseTrace` field names (`satAxes`, `epaSnapshots`, `gjkSnapshots`, `mprSnapshots`, `kind`, plus the final manifold + world shapes) — keep them consistent with Task 5's drawing.

- [ ] **Step 2: Build + verify compile fail** (`DebugCollide`/`NarrowphaseTrace` struct undefined).

- [ ] **Step 3: Define `NarrowphaseTrace` (NarrowphaseTrace.hpp)** — the recorder per spec §6: `kind`; final manifold copy + the two world `Shape`+`Transform`; `std::vector<SatAxis>` (`{ Vec2 dir; Real minA,maxA,minB,maxB; bool chosen; }`); `std::vector<SimplexSnapshot>` (`{ Vec2 verts[3]; int count; Vec2 searchDir; bool containsOrigin; }`) for GJK; `std::vector<PolytopeSnapshot>` (`{ std::vector<Vec2> verts; int edgeA, edgeB; Vec2 edgeNormal; Real edgeDist; }`) for EPA; `std::vector<MprSnapshot>` for MPR; a `Clear()`. All `Vec2`/POD — presentation-free.

- [ ] **Step 4: Thread the recorder** — add a trailing `NarrowphaseTrace* trace = nullptr` to `Collide` and its SAT/GJK/EPA/MPR/circle/capsule sub-functions. When non-null, append snapshots at the natural points (each candidate axis in SAT; end of each GJK/EPA/MPR iteration). When null: nothing (the Step path is unchanged — confirm `GenerateContacts` calls `Collide(...)` with NO trace arg, so it defaults null). Set `trace->kind` + copy the final manifold into the trace.

- [ ] **Step 5: `PhysicsWorld::DebugCollide(FixtureHandle a, FixtureHandle b, NarrowphaseTrace& out) const -> Manifold`** — compose the two fixtures' world transforms (`ComposeFixtureXf`, the same as `GenerateContacts`/`FixtureAabb`), `out.Clear()`, call `Collide(shapeA, xfA, shapeB, xfB, /*specMargin*/0, &out)`, return the manifold. Pure (reads world, writes only `out`).

- [ ] **Step 6: Build + `[debugviz]` pass + full `[physics]` green** (the null-default keeps the Step path byte-identical; the reproduction test proves DebugCollide matches). ArcaneCore static-CRT Debug clean (regen Server — new header is fine; Collide.cpp recompiles).

- [ ] **Step 7: Commit**
```bash
git add Arcane/Core/src/Arcane/Physics Arcane/Tests/src/PhysicsDebugAccessorsTest.cpp
git commit -m "feat(arcane/physics): opt-in NarrowphaseTrace recorder + DebugCollide (re-runs the real narrowphase)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 4: `OffscreenCanvas` engine helper

**Files:** Create `Arcane/Arcane/src/Arcane/Render/OffscreenCanvas.hpp/.cpp`; Test `Arcane/Tests/src/OffscreenCanvasTest.cpp` (`[gpu]`).

- [ ] **Step 1: Write the failing `[gpu]` test**

Create `Arcane/Tests/src/OffscreenCanvasTest.cpp`:
```cpp
// OffscreenCanvas: render a Batcher2D pass into an offscreen texture, get an
// ImTextureID for ImGui::Image. No NVRHI validation errors.
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Render/OffscreenCanvas.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>
#include <Arcane/Render/RenderError.hpp> // RenderErrorCount (grep the real include)

using namespace Arcane;

TEST_CASE("OffscreenCanvas renders a pass to a texture", "[gpu][debugviz]")
{
    // Use the test's shared device/shaders harness (grep how other [gpu] tests
    // get a RenderDevice + ShaderLibrary -- mirror that setup).
    auto dev = /* shared device per existing [gpu] tests */;
    auto shaders = /* shared ShaderLibrary */;
    auto oc = OffscreenCanvas::Create(dev->Nvrhi(), *shaders, 256, 256);
    REQUIRE(oc);
    oc->Draw([](Batcher2D& b){ b.Line({10,10},{200,200},2.0f,{1,1,1,1}); b.Circle({128,128},40,{1,0,0,1}); },
             /*clear*/{0,0,0,1});
    REQUIRE(oc->TextureId() != 0);
    REQUIRE(Arcane::RenderErrorCount() == 0);
}
```
VERIFY the `[gpu]` device/shaders setup pattern from an existing GPU test (e.g. the Batcher2D/Canvas tests) and mirror it; adjust `RenderErrorCount` include.

- [ ] **Step 2: Regen both workspaces + build + verify compile fail.**

- [ ] **Step 3: Implement `OffscreenCanvas`** (per spec §7): `Create(nvrhi::IDevice*, ShaderLibrary&, uint32 w, uint32 h) -> unique_ptr<OffscreenCanvas>` owning a `Canvas` (RGBA16F via `CreateCanvas`), a `Batcher2D`, a `TonemapPass`, and an sRGB8 output texture + framebuffer. `Draw(const std::function<void(Batcher2D&)>& fn, glm::vec4 clear)`: open/get a command list, clear the canvas, `batcher.Begin(cmd, canvas->Framebuffer(), w, h)`, `fn(batcher)`, `batcher.End()`, `tonemap.Run(cmd, canvas->Texture(), outputFb)`, execute. (Mirror how `Loom/main.cpp` drives canvas→batcher→tonemap.) `TextureId() -> ImTextureID` = `(ImTextureID)(intptr_t)outputTex.Get()`. `Resize(w,h)` recreates targets. Add the project (regen) so ArcaneTests links it. NOTE: this is engine (Arcane.dll), NOT Core — it's fine to use NVRHI here.

- [ ] **Step 4: Build + `[gpu]` test passes** (`RenderErrorCount()==0`, valid TextureId). Confirm both backends if the harness runs them.

- [ ] **Step 5: Commit**
```bash
git add Arcane/Arcane/src/Arcane/Render/OffscreenCanvas.hpp Arcane/Arcane/src/Arcane/Render/OffscreenCanvas.cpp Arcane/Tests/src/OffscreenCanvasTest.cpp
git commit -m "feat(arcane/render): OffscreenCanvas -- Batcher2D pass to a texture for ImGui::Image

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 5: Slice B inspector — click-to-pin + world overlay + Minkowski inset + step control

**Files:** `Interaction.hpp/.cpp` (click-to-pin), `SandboxApp.hpp/.cpp` (pinned pair + `OffscreenCanvas` + per-frame `DebugCollide`), `Hud.cpp` (the inspector window + step UI), `PhysicsDebugDraw.cpp` (world-space trace overlay). Visual gate.

- [ ] **Step 1: Click-to-pin (Interaction)** — add an "inspect mode" bool (a HUD toggle gating it vs spawn/drag). On an un-captured LMB press in inspect mode, scan `world.ForEachContactConstraint` for the contact whose world point is nearest the click (within ~12 px screen); store the pinned `(FixtureHandle a, FixtureHandle b)` (generation-stamped). Click empty / a "Clear" button → unpin. Keep the pin across steps while both handles are valid.

- [ ] **Step 2: Per-frame trace + world overlay** — in `SandboxApp` (render or DrawUI path), if pinned + valid, call `world.DebugCollide(a,b,m_trace)` into a member `NarrowphaseTrace m_trace`. Publish it (a `SandboxInspectorResource` holding the trace + the pinned ids + the current step index) so `PhysicsDebugDraw` can read it. Add a `DrawNarrowphaseWorldOverlay(trace, stepIndex, batcher, ...)` block: draw the two shapes highlighted, SAT axes as world lines (chosen axis bold), support points, the final normal arrow, contact points.

- [ ] **Step 3: Minkowski inset + step control (Hud.cpp + OffscreenCanvas)** — when pinned, draw an ImGui "Narrowphase Inspector" window: header (ids, `kind`, normal, depth, pointCount); a step `ImGui::SliderInt` over the recorded iteration count + play/pause + step buttons (store the index in the inspector resource); the inset `ImGui::Image(app.Inspector().Offscreen().TextureId(), size)`. Each frame BEFORE ImGui renders, call `offscreen.Draw(fn)` where `fn` draws the Minkowski geometry at the current step via a fit-to-bounds camera (compute bounds from the trace's Minkowski points + padding): the origin crosshair; GJK simplex (point/segment/tri) or EPA polytope (closed poly + highlighted closest edge + edge-normal) or MPR portal for the current snapshot; analytic kinds draw the closest-point construction + disable the slider with an "analytic — no iterations" note. `offscreen.Resize` on content-region change. (Timing: the offscreen `Draw` must run in the render phase before the host's ImGui render — wire it as an early render-phase step or call it from the plugin render hook; mirror how `PhysicsDebugRenderSystem` is ordered.)

- [ ] **Step 4: Build + visual gate (Loom)** — scene 8 (or a simpler 2-box scene): enable inspect mode, click a contact, confirm the world overlay (axes/normal) + the Minkowski inset render, and the step slider scrubs the simplex/polytope. Confirm `[physics]` + Sandbox suites still green (inspector defaults off/unpinned).

- [ ] **Step 5: Commit**
```bash
git add Arcane/Sandbox/src Arcane/Arcane/src/Arcane/Render/PhysicsDebugDraw.cpp Arcane/Arcane/src/Arcane/Render/PhysicsDebugDraw.hpp
git commit -m "feat(arcane/sandbox): narrowphase inspector -- click-to-pin + world overlay + stepped Minkowski inset (Slice B)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 6: Full gate + visual gate + memory

**Files:** none (verification).

- [ ] **Step 1: Full ArcaneTests Debug + Release** (no filter, `[gpu]` both backends). Expected: all pass (baseline 77570/375 + the new `[debugviz]`/`[gpu]` cases).
- [ ] **Step 2: ArcaneCore static-CRT Debug + Release** clean (`ArcaneCore.lib`). Core stayed presentation-free + opt-in.
- [ ] **Step 3: Visual gate (Dist Loom):** all four Slice-A toggles + the Slice-B inspector (click-to-pin, step slider) work; nothing regressed in the base sim.
- [ ] **Step 4: Update memory** — add a `project_arcane_physics_debug_viz` memory (broadphase + narrowphase viz; the OffscreenCanvas helper; click-to-pin + stepped Minkowski inset; NarrowphaseKind tag + opt-in NarrowphaseTrace) + MEMORY.md line.

---

## Self-Review Notes (addressed)

- **Spec coverage:** §5 accessors = Task 1; §6 NarrowphaseKind = Task 1 (tag) + Task 3 (trace/threading/DebugCollide); §7 OffscreenCanvas = Task 4; §8 Slice A overlay = Task 2; §9 Slice B inspector = Task 5; §10 determinism = the opt-in/read-only design enforced in Tasks 1/3 + the `[physics]`-green gate each task; §11 testing = the `[debugviz]` accessor/reproduction tests (Tasks 1,3), the `[gpu]` OffscreenCanvas test (Task 4), visual gates (Tasks 2,5,6).
- **Determinism:** every Core task re-runs full `[physics]` to prove the Step path is byte-unchanged (read-only accessors + null-default recorder). DebugCollide is a side-effect-free re-run.
- **Type consistency:** `NarrowphaseKind`, `NarrowphaseTrace` (+ `satAxes`/`gjkSnapshots`/`epaSnapshots`/`mprSnapshots`/`kind`), `ForEachLeaf`/`ForEachCell`/`ForEachContactConstraint`/`StaticGrid`/`ResidencyGrid`/`FixtureBroadphaseTree`/`DebugCollide`, `OffscreenCanvas::Create/Draw/TextureId/Resize` are referenced consistently across tasks. The implementer settles the few `VERIFY`-flagged existing-symbol names (the per-fixture tree getter, `GetBodyFixture`, the `Manifold`/`ContactConstraint`/`RenderErrorCount` locations) by reading headers — flagged at each use.
- **Slices:** A (Tasks 1-2) ships independently; B (Tasks 3-5) layers on. Task 6 is the joint gate.
- **Known soft spots for the executor:** (1) `FixtureBroadphaseTree()` returning the concrete `DynamicTree` from a `unique_ptr<IBroadphase>` — pick a clean mechanism (a concrete getter, or a `DynamicTree*`-or-null) and document. (2) The OffscreenCanvas command-list timing (must render before ImGui) — mirror the host's render ordering. (3) Threading the recorder through SAT/GJK/EPA/MPR is the most invasive part — keep the null path branch-free/zero-cost and re-run `[physics]` to prove it.
