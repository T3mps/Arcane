# Astra Adoption — Plan 2: Interpolation on `PhysicsInterpBuffer`, `PreviousTransform` deleted, ABI 27

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move sprite render interpolation onto the physics-side `PhysicsInterpBuffer` (world-slot indexed, generation-guarded), delete `PreviousTransform` and `LerpPose` end to end, bump the ABI to 27, close. Neither interpolation path is production-wired today (spec §8's finding), so this is mechanism + tests.

**Architecture:** Read-side first (the buffer-driven lerp lands with its two rebuilt cases while `PreviousTransform` still exists, so the old and new consumers overlap for one commit and the tests prove the new one alone) → the deletion (struct, reflect block, both registration lists, the physics trait + PASS 4 stash, the catalog sets and their pins, `LerpPose` and its spine cases) → ABI 27 with the B9 build order → closeout with the zero-legacy sweep.

**Tech Stack:** C++23, Astra, Catch2, msbuild `Arcane.slnx` / `ReferenceProject.slnx`.

**Spec:** `docs/specs/2026-09-11-astra-adoption-design.md` §8 (R3), §9. Plan 1 (`2026-09-11-astra-adoption-plan1-residency-change-detection.md`) must be complete: ABI 26, `Transform` tracked, the Runtime roster module-owned.

## Global Constraints

- **Repo:** `D:\dev\starworks\Arcane`, `main`, at Plan 1's closeout commit (ABI **26**). Gacha `D:\dev\starworks\Gacha` is at ABI 26 after Plan 1 Task 9; its 27 restamp is that repo's follow-up, recorded not actioned. **`out.txt` at the repo root is the user's — never stage it**; nor the untracked `ArcaneAssetPipeline/ArcaneAs.25D4CEF5/` / `ArcaneEditor/ArcaneEditor/` strays.
- **Commit per task, do NOT push.** Trailer on every commit, exactly:
  ```
  Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01Djskfodj1WnyR2g5v9V1Qc
  ```
- **The ABI bumps ONCE, in Task 3 (26 → 27).** Byte discipline: `PreviousTransform`'s reflect block and registration are compiled into every plugin (`Components.hpp` via `RenderSystems.hpp`); deleting a reflected component and shifting the roster's first-touch ids after it is a header change every plugin bakes in — a v26 `ReferenceGame.dll` under a v27 host must be refused, not loaded. Tail-append the v27 ledger entry; `ReferenceProject.arcproj` → 27.
- **Build order after ANY change to a header a game module compiles** (`Components.hpp`, `RenderSystems.hpp`, `SceneModule.hpp`, `SceneResources.hpp`, `PhysicsComponents.hpp`, `build/arcane.lua`): `GenerateProjects.bat` → **`ReferenceProject.slnx` FIRST for every configuration the run targets** (`cd ReferenceProject && ..\ThirdParty\premake5\premake5.exe vs2026 && msbuild ReferenceProject.slnx /p:Configuration=<cfg> /m`; single-slot `Binaries\`) → `msbuild Arcane.slnx /p:Configuration=<cfg> /m`. Launch hosts with an ABSOLUTE `--project`.
- **Run tests FROM the exe dir, in the FOREGROUND:** `cd bin/Debug-windows-x86_64-md/ArcaneTests && ./ArcaneTests.exe "<filter>"`; capture the seed banner. Never background a suite or a build. No new test TU in this plan (every test lands in an existing file), so `GenerateProjects.bat` is needed only for the file-list-neutral reasons above.
- **Baseline at plan start: Plan 1's derived closeout counts** (expected 1632 cases; take the number from Plan 1's ledger, not from here). Per-task deltas attributed to named cases. **Derive counts from the run, never recall them.**
- **Every task ends green** (Debug); Task 3 and Task 4 also Release; Task 3 runs the full unfiltered suite (`[witness][gpu]` proves the staged DLL is v27).
- **Anchors drift** — every `file:line` is orientation against Plan 1's end state; **re-locate by SYMBOL before editing.**
- **No render change ⇒ golden lanes untouched, no re-bless.** The buffer-driven lerp is test-only until a game module schedules `PhysicsSystem` and sets the resource (spec §2's trigger).

---

### Task 1: `RenderSubmissionSystem` reads `PhysicsInterpBuffer` (spec §8)

`PhysicsInterpBuffer` gains an entity → slot map (`slotOf`), rebuilt by PASS 2.5 alongside `prev` from `PhysicsResource::entityToBody`; `RenderSubmissionSystem` looks its entity up there, and when the buffer resource exists, is `captured`, the entity has an entry, `slot.index < prev.size()` and `prev[index].generation == slot.generation`, position (XY) and angle blend from `prev[index]` to the current WORLD pose with `Lerp`/`AngleLerp` (`SceneResources.hpp:34-49`, the debug draw's helpers); any miss snaps. World-slot poses are world poses — more correct than the old local-as-world approximation. The view is UNCHANGED from Plan 1 Task 4's const shape — no `PhysicsBodyRef` term.

**One contradiction, resolved per the controller's ruling:** spec §8 says the view gains `Astra::Optional<const PhysicsBodyRef>`, but `PhysicsBodyRef` lives in `PhysicsComponents.hpp`, which includes `<Manifold2D/Physics/PhysicsTypes.hpp>`/`Shapes.hpp`, and the SDK include surface a game module compiles `RenderSystems.hpp` under (`build/arcane.lua:66-76`, mirrored in `ReferenceGame.vcxproj` and Gacha's `Aphelyon.vcxproj`) has NO Manifold2D row — the spec's view as written breaks both game modules at compile time. Ruling: do NOT widen the game-module include surface; the entity → slot map on the buffer (Manifold2D-free: `SceneResources.hpp` already includes `<Astra/Entity/Entity.hpp>`, and `Astra::FlatMap` is what `TransformOrder` uses for `rowOf`, beside the `Astra::FlatSet` it already used for `visited`) carries the slot + generation the render side needs. Cost: one map rebuild per capture, proportional to live bodies. Staleness class: while paused, PASS 2.5 does not run, so `slotOf` is exactly as frozen as `prev` — the same "a paused scene renders static" contract the debug overlay already has.

**Files:**
- Modify: `ArcaneClient/src/Arcane/Scene/SceneResources.hpp` (`InterpSlot`, `PhysicsInterpBuffer::slotOf`, include), `ArcaneClient/src/Arcane/Scene/PhysicsSystem.hpp` (PASS 2.5, `:387-411`), `ArcaneClient/src/Arcane/Scene/RenderSystems.hpp` (the resource fetch; the lerp block `:69-94`)
- Test: `ArcaneTests/src/RenderInterpolationTest.cpp` (the two `PreviousTransform` cases at `:182-241` rebuilt over the buffer; one new snap-on-miss case; two assertions added to the capture case at `:64-109`)

**Interfaces:**

```cpp
    // SceneResources.hpp -- beside InterpPose; add #include <Astra/Container/FlatMap.hpp>

    // One entity's address into `prev`: the body SLOT it occupied at capture and
    // the handle generation it had then. Manifold2D-free on purpose -- this
    // header is compiled by every game module, whose include surface has no
    // Manifold2D row, so Phys::BodyHandle cannot appear here (and that is why
    // RenderSubmissionSystem reads THIS map rather than PhysicsBodyRef).
    struct InterpSlot
    {
        std::uint32_t index      = 0;
        std::uint32_t generation = 0;
    };

    struct PhysicsInterpBuffer
    {
        std::vector<InterpPose> prev;
        // entity -> its slot at capture. Rebuilt by PhysicsSystem PASS 2.5 from
        // PhysicsResource::entityToBody in the same pass that fills `prev`, so the
        // two are exactly as fresh as each other. Read by RenderSubmissionSystem:
        // a miss (no entry, slot past `prev`, generation mismatch) snaps.
        Astra::FlatMap<Astra::Entity, InterpSlot> slotOf;
        bool                    captured = false;   // false until the first capture

        template<typename Archive>
        void Serialize(Archive& /*ar*/) {}
    };
```

The trait stays `Astra::SystemTraits<Astra::Reads<WorldTransform, SpriteRenderer, PreviousTransform, Hidden>>` until Task 2 drops `PreviousTransform`; the view stays `reg.CreateView<const WorldTransform, const SpriteRenderer, Astra::Not<Hidden>>()` with lambda `(Astra::Entity e, const WorldTransform& world, const SpriteRenderer& sprite)`; `RenderSystems.hpp` gains NO include.

- [ ] **Step 1: Rewrite the two cases + add one** (replace `:182-241`), and extend the capture case:

```cpp
namespace
{
    // A sprite entity addressed by a hand-built PhysicsInterpBuffer -- the exact
    // shape PASS 2.5 leaves behind (prev[slot] + slotOf[e]), with no PhysicsWorld
    // and no PhysicsBodyRef involved: the buffer is world-SLOT indexed and the
    // map IS the entity's address. Current world pose from `lt`; previous `prev`.
    Astra::Entity SpriteWithPrev(Astra::Registry& reg, const Arcane::Transform& lt,
                                 Arcane::InterpPose prev, std::uint32_t slot, std::uint32_t generation)
    {
        Astra::Entity e = reg.CreateEntity();
        Arcane::WorldTransform wt; wt.matrix = lt.ToMatrix();
        reg.AddComponent<Arcane::WorldTransform>(e, wt);
        reg.AddComponent<Arcane::SpriteRenderer>(e, Arcane::SpriteRenderer{});

        Arcane::PhysicsInterpBuffer buf;
        buf.prev.resize(slot + 1);
        prev.generation = generation;
        buf.prev[slot] = prev;
        buf.slotOf[e] = Arcane::InterpSlot{ slot, generation };
        buf.captured = true;
        reg.SetResource<Arcane::PhysicsInterpBuffer>(std::move(buf));
        return e;
    }
}

TEST_CASE("RenderSubmissionSystem interpolates a sprite by PhysicsInterpBuffer + alpha", "[interp]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);

    // Current world pose at x=10; previous world-slot pose at x=0. Untextured Rect
    // sprite: nil .arcsprite -> a 1x1 m quad, so the scale IS the 4x4 size.
    Arcane::Transform lt; lt.position = glm::vec3(10.0f, 0.0f, 0.0f); lt.scale = glm::vec3(4.0f, 4.0f, 1.0f);
    SpriteWithPrev(reg, lt, Arcane::InterpPose{ glm::vec2(0.0f, 0.0f), 0.0f, 0 }, /*slot*/ 3, /*gen*/ 7);

    RecBatcher rec;
    Arcane::RenderContext2D ctx{ &rec, glm::vec2(0.0f, 0.0f), 1.0f, 0.5f };  // alpha 0.5
    reg.SetResource<Arcane::RenderContext2D>(std::move(ctx));
    Arcane::RenderSubmissionSystem{}(reg);

    REQUIRE(rec.rectCalls == 1);
    CHECK(rec.lastRectCenter().x == Approx(5.0f));   // lerp(0, 10, 0.5) at identity zoom
    CHECK(rec.lastRectCenter().y == Approx(0.0f));
    CHECK(rec.lastRotation == Approx(0.0f).margin(1e-5));
}

TEST_CASE("RenderSubmissionSystem interpolates sprite rotation on the shortest arc", "[interp]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);

    Arcane::Transform lt; lt.position = glm::vec3(0.0f);
    lt.rotation = Arcane::RotationAboutZ(10.0f * kPi / 180.0f);   // current 10deg about +Z
    lt.scale    = glm::vec3(4.0f, 4.0f, 1.0f);
    SpriteWithPrev(reg, lt, Arcane::InterpPose{ glm::vec2(0.0f), 350.0f * kPi / 180.0f, 0 }, 0, 1);

    RecBatcher rec;
    reg.SetResource<Arcane::RenderContext2D>(
        Arcane::RenderContext2D{ &rec, glm::vec2(0.0f, 0.0f), 1.0f, 0.5f });
    Arcane::RenderSubmissionSystem{}(reg);

    REQUIRE(rec.rectCalls == 1);
    // Shortest arc 350 -> 10 midpoint is 0deg, NOT 180deg.
    CHECK(std::sin(rec.lastRotation) == Approx(0.0f).margin(1e-5));
    CHECK(std::cos(rec.lastRotation) == Approx(1.0f).margin(1e-5));
}

TEST_CASE("RenderSubmissionSystem snaps to the current pose on any buffer miss", "[interp]")
{
    // Every miss path takes the snap: a generation mismatch (recycled slot), a
    // slot past the buffer, an uncaptured buffer, no entry for the entity.
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);
    Arcane::Transform lt; lt.position = glm::vec3(10.0f, 0.0f, 0.0f); lt.scale = glm::vec3(4.0f, 4.0f, 1.0f);
    const Astra::Entity e = SpriteWithPrev(reg, lt, Arcane::InterpPose{ glm::vec2(0.0f), 0.0f, 0 }, 2, 5);
    (void)e;
    reg.SetResource<Arcane::RenderContext2D>(Arcane::RenderContext2D{ nullptr, glm::vec2(0.0f), 1.0f, 0.5f });

    auto submit = [&]
    {
        RecBatcher rec;
        reg.GetResource<Arcane::RenderContext2D>()->batcher = &rec;
        Arcane::RenderSubmissionSystem{}(reg);
        REQUIRE(rec.rectCalls == 1);
        return rec.lastRectCenter().x;
    };
    CHECK(submit() == Approx(5.0f));                                              // the hit, for contrast

    reg.GetResource<Arcane::PhysicsInterpBuffer>()->prev[2].generation = 6;        // recycled slot
    CHECK(submit() == Approx(10.0f));
    reg.GetResource<Arcane::PhysicsInterpBuffer>()->prev[2].generation = 5;
    reg.GetResource<Arcane::PhysicsInterpBuffer>()->prev.resize(2);               // slot past the end
    CHECK(submit() == Approx(10.0f));
    reg.GetResource<Arcane::PhysicsInterpBuffer>()->prev.resize(3);
    reg.GetResource<Arcane::PhysicsInterpBuffer>()->prev[2] = Arcane::InterpPose{ glm::vec2(0.0f), 0.0f, 5 };
    reg.GetResource<Arcane::PhysicsInterpBuffer>()->captured = false;             // never captured
    CHECK(submit() == Approx(10.0f));
    reg.GetResource<Arcane::PhysicsInterpBuffer>()->captured = true;
    reg.GetResource<Arcane::PhysicsInterpBuffer>()->slotOf.Clear();               // no entry for the entity
    CHECK(submit() == Approx(10.0f));
}
```

And in `"PhysicsInterpBuffer captures the pre-step pose each fixed step"` (`:64-109`), after the `pp.generation == h.generation` check, pin that PASS 2.5 fills the map from the real world:

```cpp
    const Arcane::InterpSlot* slot = buf->slotOf.TryGet(e);
    REQUIRE(slot != nullptr);
    CHECK(slot->index == h.index);
    CHECK(slot->generation == h.generation);
```

- [ ] **Step 2: Run** `./ArcaneTests.exe "[interp]"` — **FAIL** (compile: no `InterpSlot` / `slotOf`).
- [ ] **Step 3: `SceneResources.hpp`** — `InterpSlot` + the `slotOf` member per Interfaces; `#include <Astra/Container/FlatMap.hpp>`. `PhysicsSystem.hpp` PASS 2.5 (`:387-411`): after the `for` that fills `prev` and before `interp->captured = true;` insert:

```cpp
                    // The entity -> slot map the sprite path reads (RenderSystems.hpp
                    // carries no PhysicsBodyRef term: PhysicsComponents.hpp would drag
                    // Manifold2D into every game module's include surface). Rebuilt
                    // from entityToBody in the SAME capture that filled `prev`, so the
                    // two are exactly as fresh as each other; a body PASS 1 removed
                    // this pass is already gone from the map (no stale address).
                    interp->slotOf.Clear();
                    interp->slotOf.Reserve(entityToBody.size());
                    for (const auto& [entity, handle] : entityToBody)
                    {
                        if (!world.IsValid(handle))
                            continue;
                        interp->slotOf[entity] = InterpSlot{ handle.index, handle.generation };
                    }
```

(`entityToBody` is the `auto& entityToBody = res->entityToBody;` alias at the top of `operator()`; `handle` is `Phys::BodyHandle`.)
- [ ] **Step 4: `RenderSystems.hpp`.** No new include. Hoist `const PhysicsInterpBuffer* interp = reg.GetResource<PhysicsInterpBuffer>();` beside the `SpriteTable`/`SpriteMaterialTable` fetches (`:44-45`). The view and lambda are Plan 1 Task 4's, untouched. Replace the lerp block (`:69-94`) with:

```cpp
                // Render interpolation (Epic 04.2, re-based 2026-09-11 spec s8): a
                // physics body's PREVIOUS world-slot pose lives in PhysicsInterpBuffer
                // (captured by PhysicsSystem PASS 2.5 before each step, indexed by
                // PhysicsWorld body SLOT; slotOf is this entity's address into it,
                // rebuilt by the same capture). Blend position (XY) and angle from it
                // to the current WORLD pose by alpha -- Lerp/AngleLerp, the debug
                // overlay's own helpers, so sprite and overlay agree to the bit.
                // World-slot poses are world poses, MORE correct than the retired
                // PreviousTransform path's local-as-world approximation. ANY miss --
                // no buffer, not yet captured, no entry for this entity, slot past
                // the buffer, or a recycled slot (generation mismatch) -- is the
                // unchanged snap-to-step.
                if (interp && interp->captured)
                {
                    if (const InterpSlot* slot = interp->slotOf.TryGet(e))
                    {
                        if (slot->index < interp->prev.size()
                            && interp->prev[slot->index].generation == slot->generation)
                        {
                            const InterpPose& pp = interp->prev[slot->index];
                            worldPos = glm::vec2(Lerp(pp.position.x, worldPos.x, ctx->alpha),
                                                 Lerp(pp.position.y, worldPos.y, ctx->alpha));
                            worldRot = AngleLerp(pp.angle, worldRot, ctx->alpha);
                        }
                    }
                }
```

Drop the now-unused `#include <glm/gtc/quaternion.hpp>` (`:31`) if nothing else in the TU needs it.

- [ ] **Step 5: Build order** (`SceneResources.hpp` and `RenderSystems.hpp` are game-module headers): `ReferenceProject.slnx` Debug → `Arcane.slnx` Debug. Run `[interp]` — **PASS** (all seven cases; the debug-draw one unchanged).
- [ ] **Step 6: Full `~[gpu]` — green.** Delta: **+1 case** (two rebuilt in place, one new; +3 assertions on the capture case — one REQUIRE and two CHECKs). Commit — `feat(scene): sprite interpolation reads PhysicsInterpBuffer through its entity->slot map (Lerp/AngleLerp, generation-guarded)`.

---

### Task 2: Delete `PreviousTransform` and `LerpPose` end to end (spec §8)

**Files:**
- Modify: `ArcaneClient/src/Arcane/Scene/Components.hpp` (struct `:103-114`, `LerpPose` `:116-140`, reflect block `:331-339`, the `:25` and `:126` comment mentions), `Scene/SceneModule.hpp` (`:23`), `Scene/SceneResources.hpp` (comment `:29-33`), `Scene/PhysicsSystem.hpp` (header comment `:38-41`; trait `:239`; PASS 4 stash `:493-512`), `Scene/RenderSystems.hpp` (trait), `Base/Runtime.cpp` (the `Register<…>()` list from Plan 1 Task 3), `ArcaneEditor/src/Scene/ComponentCatalog.cpp` (`:50`) + `.hpp` (`:31`)
- Test: `ArcaneTests/src/EditorComponentCatalogTest.cpp` (`:101,:124,:154,:322` pins deleted), `TransformSpineTest.cpp` (`:216-262` case deleted), `RenderInterpolationTest.cpp` (header comment `:1-5`)

**Interfaces:** `PreviousTransform`, `LerpPose` cease to exist. `RegisterSceneComponents` and Runtime's `Register<…>()` list both drop the slot — later ids shift by one, harmless in-process (`SceneModule.hpp:28-32` says so; ids are never persisted).

- [ ] **Step 1: Delete the pins first (RED by absence).** `EditorComponentCatalogTest.cpp`: remove the four `PreviousTransform` lines. `TransformSpineTest.cpp`: remove the whole `"PreviousTransform: the render blend slerps on the shortest arc across +-180 degrees"` case. Build — still compiles (nothing else referenced yet); the suite passes. (The deletion below is what makes the tree's own greps go quiet — Step 4 is the assertion.)
- [ ] **Step 2: Engine deletion.** `Components.hpp`: delete the struct + its banner, `LerpPose` + its banner, the reflect block; `:25` "(see PreviousTransform below)" → "(see the render interpolation note in RenderSystems.hpp)". `SceneModule.hpp:23` line deleted (keep the APPEND comment). `SceneResources.hpp:29-33`: the two-sentence "The COMPONENT-side pose (Arcane::PreviousTransform)…" → "The sprite path (RenderSubmissionSystem) blends through these same two helpers since the Astra adoption (2026-09-11), so overlay and sprite agree to the bit." `PhysicsSystem.hpp`: header comment `:38-41` deleted; trait → `Astra::Writes<Transform, PhysicsBodyRef, RigidBody2D>`; PASS 4's `if (m_stepWorld) { if (PreviousTransform* pt = …) {…} }` block and its comment (`:493-512`) deleted. `RenderSystems.hpp` trait → `Astra::Reads<WorldTransform, SpriteRenderer, Hidden>`. `Runtime.cpp`: the `Register<…>()` list drops `PreviousTransform` (order otherwise unchanged; update the order comment to name the shift). `ComponentCatalog.cpp:50` line deleted; `.hpp:31` row deleted. `RenderInterpolationTest.cpp:1-5` comment: "LerpPose's SLERP for the 3D component pose" → "PhysicsInterpBuffer's slot poses for the sprite path".
- [ ] **Step 3: Build order** (`Components.hpp` changed): `ReferenceProject.slnx` → `Arcane.slnx`. Run `[interp]`, `[scene]`, `[transform]`, `[editor]`, `[outliner]`, `[physics]`, `[transform-sync]` — PASS.
- [ ] **Step 4: Sweep** — `git grep -riw "PreviousTransform\|LerpPose" -- ':!docs' ':!ThirdParty' ':!bin' ':!out.txt'` returns ONLY `PluginABI.hpp:215` (the v16 ledger mention, kept as history). Anything else is a miss — fix it here.
- [ ] **Step 5: Full `~[gpu]` — green.** Delta: **−1 case** (`TransformSpineTest`'s slerp case); the four catalog assertions leave their cases' counts lower, not the case count. Commit — `refactor(scene)!: delete PreviousTransform and LerpPose -- PhysicsInterpBuffer is the one interpolation history`.

---

### Task 3: ABI 27 (spec §8, §9)

**Files:** Modify: `ArcaneClient/src/Arcane/Plugin/PluginABI.hpp` (v27 ledger + constant), `ReferenceProject/ReferenceProject.arcproj` (`26` → `27`). Test: the whole suite + a host launch.

- [ ] **Step 1: The v27 ledger entry**, appended directly above `kGamePluginABIVersion` (after Plan 1's v26 entry):

```cpp
    // v27 (2026-09-11, Astra adoption Plan 2): PreviousTransform DELETED --
    //     struct, reflect block, its RegisterSceneComponents slot and its slot
    //     in Runtime's Resident ComponentModule roster -- together with LerpPose
    //     (Components.hpp). Sprite render interpolation now reads the physics
    //     side's PhysicsInterpBuffer through its new entity->slot map
    //     (RenderSubmissionSystem, spec docs/specs/2026-09-11-astra-adoption-
    //     design.md s8 -- the map rather than the spec's Optional<PhysicsBodyRef>
    //     term, because PhysicsComponents.hpp drags Manifold2D headers a game
    //     module's include surface does not carry; controller ruling 2026-09-11),
    //     and PhysicsSystem's PASS 4 no longer stashes a previous local pose.
    //     WHY A BUMP: every plugin compiles Components.hpp and SceneResources.hpp
    //     (via RenderSystems.hpp) and so (a) bakes the reflect roster into its
    //     own drained baselines, (b) inlines PhysicsInterpBuffer's LAYOUT (it
    //     grew an Astra::FlatMap member), and (c) instantiates
    //     RenderSubmissionSystem's body -- which reads that resource -- INSIDE
    //     the DLL; and the roster's first-touch ids after the deleted slot shift
    //     by one. A v26 DLL under a v27 host would register a roster the host no
    //     longer knows and read a resource the host lays out differently. Reject
    //     the pairing. The v16 entry above still names PreviousTransform: that is
    //     history, and stays. The game-module include surface (build/arcane.lua)
    //     is UNCHANGED.
    //     MEASURED, not assumed: `grep -rn -E "PreviousTransform|LerpPose|
    //     PhysicsBodyRef|PhysicsInterpBuffer|InterpPose|InterpSlot"` over BOTH game modules
    //     -- ReferenceProject/Source/ and Gacha's Game/Source/ -- returns NOTHING
    //     in either tree, so neither breaks at compile time; the gate, not the
    //     compiler, refuses the stale DLL.
    //     ReferenceProject.arcproj restamped with this change. Gacha's Game
    //     restamp (26 -> 27) is that repo's own follow-up, tracked there -- the
    //     grep is what proves it safe to defer, not evidence it was done.
    inline constexpr uint32_t kGamePluginABIVersion = 27;
```

Run that grep over both trees before committing and paste the (empty) result into the entry in the v25/v26 form. Delete the `= 26;` line. `ReferenceProject.arcproj` → `"abi": 27`.
- [ ] **Step 2: Build order, Debug:** `ReferenceProject.slnx` → `Arcane.slnx`. Full UNFILTERED suite — all pass, `[witness][gpu]` included (the staged `ReferenceGame.dll` is v27). Then `"~[gpu]"`.
- [ ] **Step 3: Release, same order;** `"~[gpu]"` — same counts as Debug.
- [ ] **Step 4: One observable host launch:** `bin\Debug-windows-x86_64-md\ArcaneEditor\ArcaneEditor.exe --project D:\dev\starworks\Arcane\ReferenceProject --frames 60 --screenshot D:\dev\starworks\Arcane\bin\t3-frame.png` (absolute paths; `--screenshot` writes the last rendered frame, `HostConfig.cpp:19`). Two concrete checks: (1) the log carries NO `AbiMismatch` and NO `plugin: initial load failed` line and the process exits 0 — the v27 `ReferenceGame.dll` loaded; (2) open the PNG: the reference cube and the scene's sprites are present, drawn where the pre-plan build draws them (no render change is claimed, so compare against a PNG taken the same way at Plan 1's closeout commit if there is any doubt). Delete both PNGs afterwards (never stage them).
- [ ] **Step 5: Commit** — `chore(abi)!: ABI 27 -- PreviousTransform deleted, ReferenceProject restamped` (+ trailer). Delta: **+0 cases**.

---

### Task 4: Plan 2 close — sweep, final counts, handoff

- [ ] **Step 1: Zero-legacy sweep** (path-exclude + `-riw`): `PreviousTransform`, `LerpPose`, `shadowValid`, `SamePose`, `FrameSceneIfPending`, `m_frameOnSceneOpen` across `ArcaneClient ArcaneEditor ArcaneRuntime ArcaneTests ReferenceProject scripts build` — hits only in `docs/` history and in `PluginABI.hpp`'s ledger: the v16 entry (history, `:215`) AND the v27 entry Task 3 just wrote (it names `PreviousTransform` and `LerpPose` as the things deleted). Anything outside those two ledger entries is a miss. (Task 2's pre-Task-3 sweep expected the v16 line alone; that was correct at that point.)
- [ ] **Step 2: Build order, both configs, foreground** (0 warnings / 0 errors). Suites: Debug unfiltered + `~[gpu]`; Release `~[gpu]`. Derive the final counts and attribute against Plan 1's closeout: T1 +1, T2 −1 = **net +0 cases**, assertions changed by the rebuilt/deleted cases (state the measured delta). Debug and Release must agree. Ledger seed banners. `scripts/check-baselines.ps1` on a `-r json` Debug report: rise over the committed baseline, exit 0.
- [ ] **Step 3: Hand off.** The ABI is **27**; Gacha's `Game/Aphelyon.arcproj` is at 26 and its `Aphelyon.dll` was built against the Plan 1 SDK — **that repo owes a 27 restamp + rebuild before it opens in a v27 host; recorded here, not actioned** (no include-surface change: the game module needs no regenerate beyond the ordinary rebuild). `PhysicsSystem` is still unscheduled and `PhysicsInterpBuffer` still unset in production (spec §2's trigger unchanged). `scripts/automation-baselines.json` reads Plan 1's closeout figure (its Task 10 catch-up); this plan's net case delta is 0, and its assertion delta is recorded in Step 2, not rewritten into the file.
- [ ] **Step 4: Commit** — `docs: Astra adoption plan 2 closeout notes` (only if a doc changed; otherwise no commit, said so in the handoff).

---

## Self-review record (run at authoring time)

**Spec coverage — every clause to a task:**

| Spec | Task |
|---|---|
| §3 R3 (delete `PreviousTransform`, lerp onto `PhysicsInterpBuffer`, `LerpPose` goes) | T1 (lerp), T2 (deletions) |
| §3 R4 (ABI 27 at the deletion) | T3 |
| §8 `RenderSubmissionSystem` (captured + slot + generation guard, Lerp/AngleLerp on XY + angle, snap on any miss, world-slot = world pose) | T1 — the entity → slot map on the buffer stands in for the spec's `Optional<const PhysicsBodyRef>` term (controller ruling; see Contradictions) |
| §8 delete list (struct + reflect, `RegisterSceneComponents`, the `ComponentModule` list, PhysicsSystem trait + PASS 4 stash, ComponentCatalog sets + `EditorComponentCatalogTest` pins; v16 ledger stays) | T2 |
| §8 `LerpPose` + `TransformSpineTest` cases | T2 |
| §8 `RenderInterpolationTest` two cases rebuilt by hand (α=0.5 midpoint; 350°→10° lands at 0°; generation mismatch snaps) | T1 |
| §8 ABI 27 + v27 ledger + `ReferenceProject.arcproj`; Gacha as follow-up | T3, T4 |
| §9 build order / absolute `--project` / baseline discipline | Global Constraints, T3, T4 |
| §2 non-goal: scheduling `PhysicsSystem` / wiring the buffer | Untouched; T4's handoff restates the trigger |

**Type consistency across tasks:** `InterpPose{position, angle, generation}`, `InterpSlot{index, generation}` and `PhysicsInterpBuffer{prev, slotOf, captured}` (`SceneResources.hpp`) are named in T1 and consumed unchanged by T1's system, PASS 2.5 and the tests; `PhysicsResource::entityToBody`'s `Phys::BodyHandle{index, generation}` is copied field-for-field into `InterpSlot` at capture, so the render side never sees a Manifold2D type. `RenderSubmissionSystem`'s trait keeps `PreviousTransform` through T1 and drops it in T2 — the one-commit overlap is deliberate so T1's tests prove the buffer path alone. The Runtime roster list edited in T2 is Plan 1 Task 3's.

**Contradictions found in the code, resolved per the controller's ruling:**
1. **Spec §8's `Optional<const PhysicsBodyRef>` view term vs the SDK include surface:** `PhysicsBodyRef` drags Manifold2D headers into `RenderSystems.hpp`, which game modules compile without a Manifold2D include row (`build/arcane.lua:66-76`). Ruled: do not widen the include surface; T1 gives `PhysicsInterpBuffer` an entity → slot map filled by PASS 2.5, and the view stays `PhysicsBodyRef`-free.

**Known intentional gaps, each with its owner:**
- **Gacha's 27 restamp + rebuild** is that repo's follow-up (T4 handoff), per spec §8's own wording.
- **Nothing production-wires the buffer:** `PhysicsSystem` is still unscheduled by both game modules and no host sets `PhysicsInterpBuffer`; the sprite lerp is test-proven mechanism until spec §2's trigger fires.
- **`slotOf` is only as fresh as the last STEPPING pass** (PASS 2.5 is gated on `m_stepWorld`): a body destroyed and its slot recycled by a paused mint keeps its entity's stale entry until the next step. Same frozen-while-paused contract `prev` already has; the generation guard is a consistency check between the two, not a live recycled-slot detector.
- `scripts/automation-baselines.json`: Plan 1 Task 10 catches it up; this plan's closeout re-runs `check-baselines.ps1` against that figure and leaves a further rise un-rewritten (net +0 cases here).
