# 2D physics events — Box2D-v3 event arrays upstream, pull-model windows in Arcane

**Date:** 2026-10-08 · **Status:** Spec (awaiting user review) · **Arc:** physics events + joints, spec 1 of 3 (2: Astra typed relationship edges, upstream · 3: Joint2D on typed edges + editor, incl. joint-break events) · **Follows:** 2D physics wiring (docs/specs/2026-09-11-physics-2d-wiring-design.md), settings + cvar arc (merged 4db21698) · **ABI:** game module 54 → 55

## 1. What this is

Game code cannot react to collisions or triggers. Manifold2D produces contact and sensor events, but Arcane never surfaces them, and what Manifold2D produces is not what a game needs (measured 2026-10-08):

1. **Dynamic-vs-static pairs are excluded by design** (`ContactManager.hpp` header: the touched pairs exclude dynamic-vs-staticBody). A ball landing on the ground fires nothing.
2. **Events are body pairs**, not fixture pairs: a foot-sensor fixture or a hitbox fixture cannot be told apart from the body's other fixtures.
3. **No hit events** (no impact point, normal or approach speed).
4. **Delivery is a `std::function` listener** (`ContactManager::Listener`, `PhysicsWorld::OnContact`) — a callback, which the events direction rules out for gameplay.

Separately, Arcane's vendored Manifold2D (pin `5523f77`, 2026-08-11) is 28 commits behind upstream work that spec 3 needs (joint limits, motors, springs, local anchors, reaction force) and that has never reached Manifold2D master.

This spec: (a) consolidates Manifold2D's unmerged branches onto master and re-vendors; (b) adds Box2D-v3.1.1-parity event arrays upstream; (c) exposes them to game code as two pull windows on `PhysicsResource`, keyed by entity + GUID + fixture index.

## 2. Direction this spec implements (binding, user 2026-10-08)

- **Pull model, no callback bus** (memory `project_events_pull_model_direction`): engine facts are data read after the step. No listener registration, nothing stored in module code, nothing runs mid-step. Reasons: determinism, hot-reload safety, no reentrancy, ECS-friendly iteration.
- **Upstream first:** Manifold2D changes land in the Manifold2D repo, then sync into Arcane.
- **Game modules never see Manifold2D** (wiring spec R1): every game-facing type is an Arcane type.
- **Anything outliving the frame keys entities by `Identity` GUID** (memory `feedback_guid_not_astra_handles`).
- **New tunables are settings** (memory `feedback_new_systems_expose_settings`).

## 3. Rulings ledger (user-decided 2026-10-08)

| # | Ruling | Alternative rejected |
|---|---|---|
| E1 | **Three specs, events first.** Events have no Astra dependency and are the smallest useful piece. | Joints first; one umbrella spec. |
| E2 | **Event granularity = entity + fixture index** on each side (Box2D v3 shape ids). Foot sensors and hitboxes work without extra entities. | Entity only (a sensor fixture would need its own child body). |
| E3 | **Two fixed windows:** `StepEvents()` (the previous physics step; Box2D/Unity timing) and `FrameEvents()` (every step since the last Update; Unreal's per-frame batch). No per-reader state. | Bevy-style per-reader cursors (module-held state; an "oldest unread" retention rule). |
| E4 | **Approach A: Box2D v3 parity built upstream.** | B: deriving events in Arcane by diffing contact constraints (misses sleepers, duplicates library logic, violates upstream-first). C: patching the Lua-era `ContactManager` (stay events, callback, body pairs). |
| E5 | **Fold every unmerged Manifold2D branch into step zero** (`feat/wasm-scene-api`, which contains `feat/wasm-step-trace`, plus `origin/ci/tri-platform`). | Re-vendor from the feature branch only. |
| E6 | **Arcane's defaults are friendlier than Box2D's:** contact events on, sensor events on, hit events off. Manifold2D itself keeps Box2D's all-off defaults; Arcane's `Fixture` sets its own. | Box2D's opt-in everything (the common failure is "my trigger does nothing"). |

## 4. Scope and non-goals

**In scope:** Manifold2D branch consolidation + Mosaic reconcile + re-vendor through one `sync-vendor.ps1`; upstream event arrays, per-fixture flags, hit threshold, per-body contact listing; Arcane event types, the two windows, clearing rules, `ContactsOf`; `Fixture` flags; the `physics.events` settings; ABI 55; tests at all three levels and a witness.

**Non-goals (recorded, not built):**
- Joint-break events — spec 3, built on upstream `Joint::ReactionForce()`.
- Pre-solve / contact-modification callbacks (Box2D's `b2PreSolveFcn`) — they are callbacks by nature; revisit only with a concrete need.
- The game-fact typed queue (`EventReader<T>`-style "player died") — its own later spec.
- The `Arcane::ECS` / `Arcane::Physics2D` naming aliases — deferred to render foundation (ruling of 2026-10-08).
- Events across the network — replication v1 decides.
- Mover-vs-tile contacts — tiles are not bodies in Manifold2D (unchanged).
- Event visualisation in the editor overlay.

## 5. Step zero — consolidate Manifold2D and re-vendor

### 5.1 Branch state (measured 2026-10-08, `D:\dev\starworks\Manifold2D`, working tree clean)

| Ref | Ahead of master | Content |
|---|---|---|
| `feat/wasm-scene-api` | 28 | revolute limits/spring/motor; prismatic limits + force-limited motor; distance local anchors, length limits, spring; joint reaction force/torque; collision groups; runtime filters; continuous collision for fast non-bullet bodies (on by default); stiffer static contacts (`staticSoftness`); the speculative two-point polygon manifold; gravity well + per-body gravity scale; `CastRayClosest`; `StepTrace`; `bodyAABB`; the wasm scene surface |
| `feat/wasm-step-trace` | 7 | contained in `feat/wasm-scene-api`. The Starworks website's `manifold.rev` is `8e4174a`, this branch's tip — so the live page is covered |
| `origin/ci/tri-platform` (remote only) | 10 | tri-platform CI matrix, `tests/CrossPlatformDeterminismTest.cpp`, `scripts/compare-determinism.py`, per-shell build/test scripts, premake FMA on Linux + a macOS arm64 filter. `git merge-tree` against `feat/wasm-scene-api`: no conflicts |

`D:\dev\github\Manifold2D` is an unrelated stale clone (at "Initial commit", 1065 staged files). Not touched by this spec; its disposal is the user's call.

### 5.2 Procedure

1. Manifold2D: fast-forward `master` to `feat/wasm-scene-api`; merge `origin/ci/tri-platform`. Build Debug + Release; run the full suite including the cross-platform determinism fixture. Pushing `origin/main` and deleting merged branches happen **only on the user's word**.
2. **One vendoring script for all three libraries** (amendment A6, user 2026-10-08). `scripts/sync-vendor.ps1 -Library Astra|Manifold2D|Mosaic|All` replaces `sync-astra.ps1` (kept as a one-line shim that calls it):
   - **Astra:** `include/` only.
   - **Manifold2D:** `include/` + `src/` + `LICENSE`; never Manifold2D's own `ThirdParty/Mosaic`, never Arcane's consumer `premake5.lua`.
   - **Mosaic:** `include/` + `src/` + `LICENSE`.

   Every run mirrors (orphans deleted), stamps `ThirdParty/<Library>/VENDORED.txt` with the source commit, and keeps the CRLF discipline (memory `reference_astra_sync_crlf_fanout`). **Drift check:** the script reads which Mosaic commit each upstream library pins (Manifold2D's `ThirdParty/Mosaic/VENDORED.txt`, Astra's `vendor/Mosaic`) and warns when it differs from Arcane's Mosaic stamp. Arcane keeps exactly one copy of each library (it already does: vendored Astra and Manifold2D compile against `ThirdParty/Mosaic` through `IncludeDir["Mosaic"]`). Pointing the build at sibling checkouts is rejected: builds and CI would depend on local working-tree state.
3. **Mosaic reconcile first.** Measured 2026-10-08: Arcane's `Simd/Wide_AVX2.inl`, `Wide_NEON.inl` and `Wide_Scalar.inl` differ from upstream Mosaic (`784f066`), and Manifold2D's copy carries an `__EMSCRIPTEN__` arm in `Platform.hpp` that upstream lacks. Every copy-only change is committed to upstream Mosaic first, then all three copies are synced from it. The ARM `Simd/Bits.hpp` fix from the macOS runs also belongs upstream; a branch that patched a copy (Arcane `mac/port`) takes upstream's version when it rebases. The new FMA/arm64 premake flags remain a follow-up for the Linux/mac port.
4. Build Arcane both configs. Run the physics tests.
5. **Re-record the trajectory fixture deliberately** (`ArcaneTests/data/trajectory/reference_player.json`): the motion changes (continuous collision, static softness, the new manifold). The re-vendor commit carries a before/after trajectory comparison, and the new motion is reviewed as correct before the fixture is replaced. Never silently.
6. Golden gate. Re-bless only if a moved golden is confirmed to be correct motion (procedure: memory `project_arcane_golden_rebless_procedure`).

The event work (§6) then lands upstream on the consolidated master and is synced by a second run of the script.

## 6. Upstream: Manifold2D event arrays

Reference: Box2D v3.1.1 (`D:\dev\starworks\Manifold2D\.reference\box2d-3.1.1`). Every parity claim in the plan cites file:line there.

### 6.1 The arrays

After each `Step`, the world exposes five arrays, valid until the next `Step`:

```cpp
struct ContactBeginEvent { FixtureHandle a, b; BodyHandle bodyA, bodyB; };
struct ContactEndEvent   { FixtureHandle a, b; BodyHandle bodyA, bodyB; };
struct ContactHitEvent   { FixtureHandle a, b; BodyHandle bodyA, bodyB; Vec2 point, normal; Real approachSpeed; };  // normal A -> B
struct SensorBeginEvent  { FixtureHandle sensor, visitor; BodyHandle sensorBody, visitorBody; };
struct SensorEndEvent    { FixtureHandle sensor, visitor; BodyHandle sensorBody, visitorBody; };

struct ContactEvents { std::span<const ContactBeginEvent> begin; std::span<const ContactEndEvent> end; std::span<const ContactHitEvent> hit; };
struct SensorEvents  { std::span<const SensorBeginEvent> begin;  std::span<const SensorEndEvent> end; };

ContactEvents PhysicsWorld::GetContactEvents() const;   // b2World_GetContactEvents
SensorEvents  PhysicsWorld::GetSensorEvents() const;    // b2World_GetSensorEvents
```

(`FixtureHandle` is Manifold2D's existing type, `Fixture.hpp:36`; `BodyHandle`, `PhysicsTypes.hpp:101`.)

- **Contact events cover every solver contact:** any touching fixture pair with at least one dynamic body and no sensor, including dynamic-vs-static and dynamic-vs-kinematic. This is Box2D's set: Box2D creates no contact for kinematic-vs-static or kinematic-vs-kinematic pairs, so those never produce contact events. (Amended 2026-10-08: the earlier text also listed kinematic-vs-static.)
- **Begin/End:** a fixture pair's `Contact::touching` (`manifold.pointCount > 0`, speculative points included, as Box2D's `touching`) flips this step (Box2D `b2_simStartedTouching` / `b2_simStoppedTouching`, `world.c:629-668`). Whether a contact reports is decided **once, when it is created** (Box2D `contact.c:253-256`): `contactEvents` on **either** fixture by default, or on **both** when `WorldDef::contactEventsRequireBoth` is set (amendment A2). A pair evaluates `touching` once per step, so it cannot both begin and end in one step: a graze gives Begin at step k and End at step k+1.
- **Hit:** reported for a touching pair whose approach speed exceeds `WorldDef::hitEventThreshold` (default 1 m/s, Box2D `types.c:14`). Requires `hitEvents` on **either** fixture (Box2D `contact.c:535`), also fixed at creation. One hit per pair per step: the manifold point with the largest approach speed among points that received normal impulse (Box2D `solver.c:1758-1814`). Manifold2D sources: the approach speed is `-ContactConstraintPoint::relativeVelocity` (`SoftStep.cpp:252-274`). The impulse test is the post-solve `ManifoldPoint::normalImpulse > 0`, because Manifold2D keeps no `totalNormalImpulse` (amendment A4: a documented deviation).
- **Sensor Begin/End** come from a **sensor pass at the end of the step**, independent of contacts, as Box2D v3.1.1's `sensor.c` does it:
  - Every sensor fixture whose `sensorEvents` is on is tested against fixtures on bodies of **every type, static included** (Box2D queries all three trees, `sensor.c:179-181`).
  - A candidate is skipped if: it is on the same body (`sensor.c:72-75`); the collision filter rejects the pair (`:77-81`); its own `sensorEvents` is off (`:66-68`); or it is itself a sensor. The last rule is a deliberate deviation: Box2D 3.1.1 does not apply it, but the approved design does (amendment A1).
  - Overlap means the narrowphase reports a point with `separation > 0`.
  - Sensor flags are read every pass, not fixed at creation. Turning a sensor's `sensorEvents` off ends its overlaps on the next step (Box2D `sensor.c:158-165`).
- **Destroy-time ends:**
  - Destroying a touching contact emits its End. That covers `RemoveBody`, `DropFixture`, `SetBodyFilter`, and a contact dropped because the fat boxes separated.
  - Destroying a sensor or a visitor that was overlapping emits the sensor End.
  - End arrays are double-buffered (Box2D `endEventArrayIndex`, `contact.c:354-364`, `world.c:581`, `:807-810`): an End caused between steps is delivered with the next step, never dropped.
- **Runtime flag changes** (`SetFixtureEvents`): contact and hit flags apply to contacts created afterwards, so a Begin always pairs with an End. A sensor flag applies from the next sensor pass. No synthetic Begin is emitted. (Amendment A3 replaces the earlier "re-enabling emits a fresh Begin".)
- **Every event carries both bodies' handles** beside the fixture handles. This is an additive deviation from Box2D (amendment A5): a destroy-time End names a fixture that no longer exists, and the consumer still needs to know whose it was.

### 6.2 Flags and threshold

- `FixtureDef` and `BodyDef` (whose auto-fixture copies them) gain `contactEvents`, `sensorEvents` and `hitEvents`, all default `false` (Box2D `b2DefaultShapeDef`, `types.c:55-65`). One runtime setter: `SetFixtureEvents(FixtureHandle, bool contact, bool sensor, bool hit)`, with the semantics in §6.1.
- `WorldDef::hitEventThreshold` (`Real`, m/s, default 1) + `SetHitEventThreshold`; `WorldDef::contactEventsRequireBoth` (default `false`, Box2D's either-fixture rule) + `SetContactEventsRequireBoth`.
- The world-level gate `SetEventsEnabled` gates all five arrays and the destroy-time ends. The per-body gate (`BodyDef::eventsEnabled`, `SetBodyEvents`, `Body::SetEventsEnabled`) is removed along with the legacy listener (§6.5); nothing in Arcane uses it.

### 6.3 Determinism

Each array is emitted in ascending (fixture A, fixture B) order (sensor arrays: (sensor, visitor)), sorted after the step — never in hash-map iteration order. Identical inputs produce byte-identical arrays, single-threaded and with the multithreaded solver.

### 6.4 Per-body contact listing

`PhysicsWorld::GetBodyContacts(BodyHandle, std::vector<BodyContact>& out)` lists the body's currently-touching solver contacts (Box2D `b2Body_GetContactData`). Each entry carries the body's own fixture and the other fixture, both body handles, the normal pointing from this body outward to the other, and the point count. It reflects the end of the last step. Sleeping bodies' persistent contacts are included, because they stay in the pool (`ConstraintGraph.cpp:574-578`).

### 6.5 The old listener

Measured 2026-10-08: the wasm export does not use the listener (it reads `ForEachContactConstraint`). Its only callers are Manifold2D's own tests. So the legacy `ContactManager` is removed in this work: `OnContact`, `ContactEvent`, the per-body gate, `CollectTouchedEventPairs` and stage 6's flush. The tests move to the arrays. `PhysicsWorld::ForEachContact(fn(slotA, slotB))` stays, because Arcane's `PhysicsDebugDraw.cpp:486` draws with it. It is reimplemented over the contact pool: every touching body-to-body pool contact, in ascending id order. That newly includes dynamic-vs-static pairs, which the old version excluded. Pool creation rules (`eventRelevant`, event-only contacts) are left untouched, so the simulation stays bit-identical; pruning them is a follow-up. The Stay event type has no replacement (Box2D v3 dropped it; `GetBodyContacts` answers "touching now").

## 7. Arcane: the game-facing surface

### 7.1 Types (`ArcaneCore/src/Arcane/Scene/PhysicsEvents2D.hpp`, new, SDK-visible)

```cpp
struct ContactSide2D  { Arcane::Entity entity; Guid guid; std::uint32_t fixture; };  // fixture = index into Collider2D::fixtures
struct ContactBegin2D { ContactSide2D a, b; };
struct ContactEnd2D   { ContactSide2D a, b; };
struct ContactHit2D   { ContactSide2D a, b; glm::vec2 point, normal; float approachSpeed; };
struct SensorBegin2D  { ContactSide2D sensor, visitor; };
struct SensorEnd2D    { ContactSide2D sensor, visitor; };
struct ContactPoint2D { ContactSide2D self, other; glm::vec2 normal; std::uint32_t pointCount; };

struct PhysicsEvents2D
{
    std::span<const ContactBegin2D> contactBegin;
    std::span<const ContactEnd2D>   contactEnd;
    std::span<const ContactHit2D>   contactHit;
    std::span<const SensorBegin2D>  sensorBegin;
    std::span<const SensorEnd2D>    sensorEnd;
};
```

Pure data, no Manifold2D types: the header is includable from a game module. `normal` points from `a` to `b` (Box2D convention). `entity` is valid for the frame it is read in **unless** that entity was destroyed after the step — readers check `registry.Valid(entity)` or use `guid`. Game code must not store `entity` past the frame.

### 7.2 The windows (members of `PhysicsResource`)

```cpp
ARC_CORE_API PhysicsEvents2D StepEvents() const;   // the most recent physics step
ARC_CORE_API PhysicsEvents2D FrameEvents() const;  // every step since the last Update
ARC_CORE_API void ContactsOf(Arcane::Entity entity, std::vector<ContactPoint2D>& out) const;
```

Storage: two `std::vector`-per-kind buffers on `PhysicsResource` (step + frame), capacity retained across clears (no per-step allocation at steady state).

**Timing** (RunLoop: each frame runs 0..N fixed steps, then Update once; a module's fixed callback runs before the engine's fixed systems):
- At the end of `PhysicsSystem`'s stepping pass, the step buffer is **replaced** with the translated arrays of that step, and the same events are **appended** to the frame buffer.
- A module's fixed callback at step k therefore reads step k−1's events through `StepEvents()` (Unity's `OnCollision*` timing).
- A fixed-update game system ordered `After<PhysicsSystem>` reads the current step's events; one ordered `Before<PhysicsSystem>` reads the previous step's.
- `FrameEvents()` read in Update, in `OnUpdate` or in render holds every step of the current frame, in step order. A frame with zero fixed steps has an empty `FrameEvents()`; `StepEvents()` is left unchanged by it.
- **Where the frame window is cleared** (amendment A7). `RunLoop` gains a physics-agnostic hook, `SetFrameBeginHook(std::function<void(Astra::Registry&)>)`, invoked first thing in both `Advance` overloads. `Runtime`'s constructor installs it to call `PhysicsResource::BeginFrame()`, which clears the frame window. Clearing at frame begin rather than "after Update" is equivalent for every reader (Update, `OnUpdate` and render all run before the next frame begins), and it keeps `RunLoop.hpp` free of physics includes (`Runtime.hpp` includes it, and so do game modules).
- Edit mode (`PhysicsEditPass`, `stepWorld = false`) never steps and never produces events.

**Clearing:** both windows are cleared whenever the world is minted or re-minted — scene open, a gravity-change re-mint, `RestoreRegistry`, hot reload — and at Play and Stop. Pairs touching at a re-mint are **not** reported as ended (the world was replaced, not simulated); this is documented on `StepEvents()`, and game code tracking touching state re-reads `ContactsOf` after a re-mint. Since the windows live on the transient `PhysicsResource`, a restored registry starts with none.

**Hot reload:** nothing points into module code; the windows are plain data cleared at the reload.

### 7.3 Translation

`PhysicsResource` keeps body records keyed by the packed `BodyHandle` (index + generation). Each record holds `{Arcane::Entity, Guid, fixture handles in Collider2D order}` and is filled when PASS 2 mints the body: fixture 0 is `GetBodyFixture(handle, 0)` after `AddBody`, and fixtures 1..N are the `AddFixture` returns.
- When PASS 1 removes a body, its record is **retired, not erased**. Retired records are erased after the next stepping pass's translation, so the destroy-time End (delivered with the next step) still resolves to the entity and GUID. The `entity` is then dead, per §7.1.
- Events carry body handles (amendment A5), so translation never asks the world about a fixture that no longer exists.
- Translation preserves the upstream order exactly.
- An event naming a body with no record (a body minted outside `PhysicsSystem`) is dropped. A Debug assert fires only for the record-missing-at-mint case.

### 7.4 Authoring

`Fixture` (`PhysicsComponents.hpp`) gains three reflected fields, shown in the Inspector through reflection:

| Field | Default | Meaning |
|---|---|---|
| `contactEvents` | `true` | Begin/End for this fixture (both fixtures must allow it — see below) |
| `sensorEvents` | `true` | Sensor Begin/End, as sensor or visitor (both must allow it) |
| `hitEvents` | `false` | Hit events above the threshold (either fixture suffices) |

**Contact opt-out needs both fixtures in Arcane** (amendment A2 supersedes the earlier translation-time filter). Box2D's either-fixture rule, combined with Arcane's default of `true`, would make turning `contactEvents` off on one fixture almost useless. So `PhysicsSystem` sets `SetContactEventsRequireBoth(true)` on the world at the top of every pass. That makes it the single owner of the policy, and tests that mint a world directly get it too. Because enablement is fixed when a contact is created, a Begin always pairs with an End. Result: a fixture with `contactEvents = false` never appears in a contact event. Hits keep the either-fixture rule (opt-in, default off).

Scenes saved before this spec load with the defaults (absent fields keep their initialiser); no scene schema migration. A flag edit flows through the existing `Changed<Collider2D>` re-mint path in Edit/paused mode, like every other fixture field. **Play-time component edits are not pushed to live fixtures**, again like every other fixture field today. The earlier promise to push them through setters is withdrawn (amendment A3). The upstream setter exists for code that drives Manifold2D directly.

### 7.5 Settings

`PhysicsEventSettings` (`PhysicsQuerySettings.hpp`, beside `PhysicsGroundSettings`):

```cpp
ARC_REFLECT_TYPE_ATTR(Settings, "physics.events", SettingScope::Project, ApplyMode::Live, Audience::Game)
float hitThreshold = 1.0f;   // m/s, Range 0..100, Deterministic
```

`PhysicsSystem` applies it with `SetHitEventThreshold(Settings<PhysicsEventSettings>().hitThreshold)` at the top of every pass, which gives Live semantics with no callback (the `PhysicsGroundSettings` read-at-use pattern).

### 7.6 ABI

`Fixture`'s layout and `PhysicsResource`'s exported members change: `kGamePluginABIVersion` 54 → 55 (`PluginABI.hpp:1131`), with the ledger entry written in the bump commit and ReferenceProject + Aphelyon restamped/rebuilt.

## 8. Edge cases (consolidated)

| Case | Behaviour |
|---|---|
| A graze | Begin at step k, End at step k+1; `FrameEvents()` holds both, in step order, when both steps ran in one frame |
| Entity destroyed while touching | End in the next step's window; `guid` valid, `entity` dead |
| Re-mint / Play / Stop / hot reload / scene open | Both windows cleared; no synthetic Ends |
| Contact or hit flag changed while touching (upstream setter) | Applies to contacts created afterwards; the existing contact keeps reporting until it ends |
| Sensor flag turned off while overlapping | End on the next step |
| Sensor vs sensor | Never reported |
| Sensor vs a static fixture / a sensor on a static body | Both detected (Box2D queries every tree) |
| Kinematic vs static, kinematic vs kinematic | No contact events (Box2D creates no such contact); sensor events still apply |
| Body type changed | Existing re-mint path → clearing rule |
| Zero fixed steps in a frame | `FrameEvents()` empty; `StepEvents()` unchanged |
| Hit below threshold / threshold changed live | Not reported / takes effect next step |

## 9. Testing

1. **Manifold2D (upstream suite):** one test per event kind:
   - begin/end for dynamic-vs-static and dynamic-vs-dynamic, and no contact events for kinematic-vs-static;
   - hits above and below the threshold, the hit normal pointing A→B, and a parity case for approach speed;
   - sensor enter/exit with a static sensor and with a static visitor; sensor-vs-sensor exclusion; same-body exclusion; the sensor flag turned off ends overlaps;
   - destroy-time Ends delivered next step (body, fixture, filter change), and not lost when no step runs in between;
   - the either/both contact rule; flags fixed at creation; the world gate;
   - `GetBodyContacts`, including a sleeping body;
   - the rewritten `ForEachContact`;
   - a determinism test (two runs, byte-identical arrays, MT executor on).
2. **Arcane (ArcaneTests):**
   - window semantics across 0, 1 and N fixed steps per frame (`StepEvents` = last step; `FrameEvents` = all, in order, cleared after Update);
   - clearing on re-mint, gravity change, `RestoreRegistry`, Play/Stop, hot reload;
   - fixture-index mapping on a multi-fixture body;
   - the both-fixtures rule: a fixture with `contactEvents = false` touching a default fixture produces no Begin/End;
   - GUID survives on a destroyed entity, `entity` invalid;
   - `ContactsOf`;
   - `Fixture` flags round-trip through scene JSON; a pre-spec scene loads with the defaults;
   - `physics.events.hitThreshold` reaches the world;
   - `PhysicsEvents2D.hpp` includes no Manifold2D header (a test reads its `#include` lines) and joins the spelling guard's game-facing header list (`ArcaneSpellingGuardTest.cpp` `kGameFacingHeaders`). The SDK include path carries Manifold2D anyway (`build/arcane.lua:193`), so this guards the header itself, not the include path.
3. **End to end (witness):** a ReferenceProject Update-phase game system reads `FrameEvents()`. When a `ContactBegin2D` names an entity carrying a new `ReferenceProject::TintOnContact` component, it sets that entity's `SpriteRenderer::tint` to the component's colour.
   - `physics.arcscene` (guid `4f6a1c2e-7b3d-4e8a-9c1f-2d5b6e7a8f90`, not the boot scene) gives the Crate a red `TintOnContact`.
   - The Crate landing on the static Ground is exactly the dynamic-vs-static case the old events missed.
   - A new witness, W5, runs the runtime host headless and reads `rgba@640,405` (the Crate's resting centre, as W4 picks it). It asserts the pixel is red, not the authored orange.
   - The trajectory fixture (Pill) and the existing goldens stay untouched: the system writes only `tint`.
4. **Step zero gates:** Manifold2D full suite + determinism fixture green; Arcane both configs green; trajectory fixture re-recorded with before/after review; golden gate.

## 10. Follow-ups recorded

- Manifold2D premake FMA (Linux) + macOS arm64 filter → Arcane's consumer wrapper, at the Linux/mac port.
- Prune Manifold2D's event-only pool contacts (`eventRelevant`) now that nothing consumes them — a simulation-neutral cleanup kept out of this spec so the step-zero bit-identity check stays meaningful.
- A sensor pass that queries the broadphases instead of `QueryAABB`'s linear scan, if sensor counts grow (2D game scale does not need it).

## 11. Plan-time amendments (2026-10-08)

Made while writing the implementation plan, from a read-only survey of Manifold2D (`feat/wasm-scene-api`) and Arcane at `4bfaacd9`:

| # | Amendment | Why |
|---|---|---|
| A1 | Sensor-vs-sensor exclusion kept, but recorded as a deliberate deviation | Box2D v3.1.1's `sensor.c` callback does not exclude sensors; the earlier citation was wrong |
| A2 | The both-fixtures contact rule moves upstream as `WorldDef::contactEventsRequireBoth`; Arcane's translation filter is dropped | Enablement fixed at creation keeps Begin/End paired; a translation filter could orphan one when a flag changes |
| A3 | Runtime flag changes affect new contacts (contact/hit) or the next pass (sensor); no synthetic Begin; no Play-time push from Arcane | Box2D parity; Play-time component edits are not pushed for any fixture field today |
| A4 | Hit impulse test uses the post-solve `normalImpulse > 0` | Manifold2D has no `totalNormalImpulse` |
| A5 | Events carry body handles beside fixture handles | A destroy-time End names a dead fixture; the consumer still needs its body |
| A6 | One `sync-vendor.ps1` (Astra, Manifold2D, Mosaic) with stamps and a Mosaic drift warning; Mosaic reconciled upstream first | User, 2026-10-08; the copies had already drifted |
| A7 | The frame window clears at frame begin through a physics-agnostic `RunLoop` hook | Keeps `RunLoop.hpp` free of physics includes; equivalent for every reader |
| A8 | Manifold2D's manifold normal points B→A; events report A→B | `Manifold.hpp:33` |
| A9 | Contact events = Box2D's contact set (needs a dynamic body, no sensor) | Box2D creates no kinematic-static contact; the earlier text listed it |
- Joint-break events → spec 3.
- A per-reader cursor facility, if the typed game-event queue spec wants one.
