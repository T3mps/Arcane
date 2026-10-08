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

**In scope:** Manifold2D branch consolidation + re-vendor + a sync script; upstream event arrays, per-fixture flags, hit threshold, per-body contact listing; Arcane event types, the two windows, clearing rules, `ContactsOf`; `Fixture` flags; the `physics.events` settings; ABI 55; tests at all three levels and a witness.

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
2. Arcane: add `scripts/sync-manifold2d.ps1`, mirroring `sync-astra.ps1`: copy `include/` + `src/` + `LICENSE` only, stamp `ThirdParty/Manifold2D/VENDORED.txt` with the commit and date, normalise line endings to the repo's convention (the CRLF fan-out lesson, memory `reference_astra_sync_crlf_fanout`). Arcane's `ThirdParty/Manifold2D/premake5.lua` is the consumer wrapper and is never synced; the new FMA/arm64 flags are recorded as a follow-up for the Linux/mac port, not applied here.
3. Carry Manifold2D's `ThirdParty/Mosaic/Platform.hpp` `__EMSCRIPTEN__` arm into Arcane's `ThirdParty/Mosaic` in the same commit (additive).
4. Build Arcane both configs. Run the physics tests.
5. **Re-record the trajectory fixture deliberately** (`ArcaneTests/data/trajectory/reference_player.json`): the motion changes (continuous collision, static softness, the new manifold). The re-vendor commit carries a before/after trajectory comparison, and the new motion is reviewed as correct before the fixture is replaced. Never silently.
6. Golden gate. Re-bless only if a moved golden is confirmed to be correct motion (procedure: memory `project_arcane_golden_rebless_procedure`).

The event work (§6) then lands upstream on the consolidated master and is synced by a second run of the script.

## 6. Upstream: Manifold2D event arrays

Reference: Box2D v3.1.1 (`D:\dev\starworks\Manifold2D\.reference\box2d-3.1.1`). Every parity claim in the plan cites file:line there.

### 6.1 The arrays

After each `Step`, the world exposes five arrays, valid until the next `Step`:

```cpp
struct ContactBeginEvent { FixtureHandle a, b; };
struct ContactEndEvent   { FixtureHandle a, b; };
struct ContactHitEvent   { FixtureHandle a, b; Vec2 point, normal; Real approachSpeed; };
struct SensorBeginEvent  { FixtureHandle sensor, visitor; };
struct SensorEndEvent    { FixtureHandle sensor, visitor; };

struct ContactEvents { std::span<const ContactBeginEvent> begin; std::span<const ContactEndEvent> end; std::span<const ContactHitEvent> hit; };
struct SensorEvents  { std::span<const SensorBeginEvent> begin;  std::span<const SensorEndEvent> end; };

ContactEvents PhysicsWorld::GetContactEvents() const;   // b2World_GetContactEvents
SensorEvents  PhysicsWorld::GetSensorEvents() const;    // b2World_GetSensorEvents
```

(`FixtureHandle` is Manifold2D's existing type, `Fixture.hpp:36`; `BodyHandle`, `PhysicsTypes.hpp:101`.)

- **All body-type pairs are reported**, including dynamic-vs-static and kinematic-vs-static, wherever at least one side is non-static (static-vs-static never touches).
- **Begin/End:** a fixture pair whose manifold gains / loses touching points this step (Box2D `b2_simStartedTouching` / `b2_simStoppedTouching`). Requires `contactEvents` on **either** fixture (Box2D `contact.c:253`: `shapeA->enableContactEvents || shapeB->enableContactEvents`). Sensors never produce contact events.
- **Hit:** reported for a touching pair whose approach speed exceeds `WorldDef::hitEventThreshold` (default 1 m/s, Box2D `types.c:14`). Requires `hitEvents` on **either** fixture (Box2D `contact.c:535`). One hit per pair per step: the manifold point with the largest approach speed (`-normalVelocity`, captured at contact prepare) among points that received normal impulse (Box2D `solver.c:1777-1801`).
- **Sensor Begin/End:** computed at the end of the step (Box2D `sensor.c`). A sensor fixture never detects another sensor. A sensor on a static body detects visitors. Requires `sensorEvents` on **both** the sensor and the visitor (Box2D `sensor.c:66`, `:158`).
- **Destroy-time ends:** destroying a body or fixture that is touching (or overlapping a sensor) emits the End event. End arrays are double-buffered (Box2D `endEventArrayIndex`, `contact.c:364`, `world.c:581`): an End caused between steps is delivered with the next step, never dropped.
- **Disabling a flag at runtime** emits no End; re-enabling while overlapping emits a fresh Begin (Manifold2D's existing level-triggered re-arm, preserved).

### 6.2 Flags and threshold

- `FixtureDef` (and a runtime setter per flag): `contactEvents`, `sensorEvents`, `hitEvents`, all default `false` (Box2D `b2DefaultShapeDef`, `types.c:55-65`).
- `WorldDef::hitEventThreshold` (`Real`, m/s, default 1) + `PhysicsWorld::SetHitEventThreshold`.
- The existing world-level gate (`SetEventsEnabled`) gates all five arrays.

### 6.3 Determinism

Each array is emitted in ascending (fixture A, fixture B) order (sensor arrays: (sensor, visitor)), sorted after the step — never in hash-map iteration order. Identical inputs produce byte-identical arrays, single-threaded and with the multithreaded solver.

### 6.4 Per-body contact listing

`PhysicsWorld::GetBodyContacts(BodyHandle, std::vector<BodyContact>& out)` lists the body's currently-touching contacts (Box2D `b2Body_GetContactData`): both fixture handles, normal, point count. It reflects the end of the last step; sleeping bodies' persistent contacts are included.

### 6.5 The old listener

`ContactManager::Listener` / `PhysicsWorld::OnContact` stay until their callers (the wasm contacts export) move to the arrays, then are removed upstream in the same spec's work. The Stay event type has no replacement (Box2D v3 dropped it; `GetBodyContacts` answers "touching now").

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
- `FrameEvents()` read in Update holds every step of the current frame, in step order. The frame buffer is cleared after the Update phase. A frame with zero fixed steps has an empty `FrameEvents()`; `StepEvents()` is left unchanged by it.
- Edit mode (`PhysicsEditPass`, `stepWorld = false`) never steps and never produces events.

**Clearing:** both windows are cleared whenever the world is minted or re-minted — scene open, a gravity-change re-mint, `RestoreRegistry`, hot reload — and at Play and Stop. Pairs touching at a re-mint are **not** reported as ended (the world was replaced, not simulated); this is documented on `StepEvents()`, and game code tracking touching state re-reads `ContactsOf` after a re-mint. Since the windows live on the transient `PhysicsResource`, a restored registry starts with none.

**Hot reload:** nothing points into module code; the windows are plain data cleared at the reload.

### 7.3 Translation

`PhysicsSystem` keeps a body → `{Arcane::Entity, Guid}` record filled when it mints the body (beside `entityToBody`), so a destroy-time End for an entity removed by PASS 1 still carries its GUID (the `entity` is then dead, per §7.1). Fixture handles map to the `Collider2D::fixtures` index through the order PASS 2 adds fixtures (recorded at mint). Translation preserves the upstream order exactly.

### 7.4 Authoring

`Fixture` (`PhysicsComponents.hpp`) gains three reflected fields, shown in the Inspector through reflection:

| Field | Default | Meaning |
|---|---|---|
| `contactEvents` | `true` | Begin/End for this fixture (both fixtures must allow it — see below) |
| `sensorEvents` | `true` | Sensor Begin/End, as sensor or visitor (both must allow it) |
| `hitEvents` | `false` | Hit events above the threshold (either fixture suffices) |

**Contact opt-out is AND in Arcane, OR upstream.** Upstream keeps Box2D's either-fixture rule (§6.1). With Arcane's default of `true`, that rule would make turning `contactEvents` off on one fixture almost useless (every partner still has it on). So Arcane pushes the flag OR-wise to Manifold2D and then drops, at translation (§7.3), any Begin/End where either side's `Fixture::contactEvents` is `false`. Result: a fixture with `contactEvents = false` never appears in a contact event. Hits keep the upstream either-fixture rule (opt-in, default off).

Scenes saved before this spec load with the defaults (absent fields keep their initialiser); no scene schema migration. A runtime change to a flag flows through the existing `Changed<Collider2D>` re-mint path in Edit/paused mode; in Play the flags are pushed to the live fixtures through the upstream setters without a re-mint.

### 7.5 Settings

`PhysicsEventSettings` (`PhysicsQuerySettings.hpp`, beside `PhysicsGroundSettings`):

```cpp
ARC_REFLECT_TYPE_ATTR(Settings, "physics.events", SettingScope::Project, ApplyMode::Live, Audience::Game)
float hitThreshold = 1.0f;   // m/s, Range 0..100, Deterministic
```

Applied to the world at mint and on change (`SetHitEventThreshold`).

### 7.6 ABI

`Fixture`'s layout and `PhysicsResource`'s exported members change: `kGamePluginABIVersion` 54 → 55 (`PluginABI.hpp:1131`), with the ledger entry written in the bump commit and ReferenceProject + Aphelyon restamped/rebuilt.

## 8. Edge cases (consolidated)

| Case | Behaviour |
|---|---|
| Begin and End in one step (a graze) | Both reported, Begin first (order within a step: begin array, then end array; the reader sees both) |
| Entity destroyed while touching | End in the next step's window; `guid` valid, `entity` dead |
| Re-mint / Play / Stop / hot reload / scene open | Both windows cleared; no synthetic Ends |
| Flag turned off while touching | No End; turned back on while overlapping → fresh Begin |
| Sensor vs sensor | Never reported |
| Static sensor | Detects visitors |
| Body type changed | Existing re-mint path → clearing rule |
| Zero fixed steps in a frame | `FrameEvents()` empty; `StepEvents()` unchanged |
| Hit below threshold / threshold changed live | Not reported / takes effect next step |

## 9. Testing

1. **Manifold2D (upstream suite):** one test per event kind — dynamic-vs-static begin/end; dynamic-vs-dynamic; kinematic-vs-static; hits above and below threshold; sensor enter/exit with a static sensor; sensor-vs-sensor exclusion; destroy-time End delivered next step (body and fixture); per-fixture flags (either-fixture for contact and hit, both-fixture for sensor — Box2D parity); same-step begin+end; flag off/on re-arm; `GetBodyContacts` including a sleeping body; a determinism test (two runs, byte-identical arrays, MT solver on); a Box2D v3.1.1 parity case for hit approach speed (cited).
2. **Arcane (ArcaneTests):**
   - window semantics across 0, 1 and N fixed steps per frame (`StepEvents` = last step; `FrameEvents` = all, in order, cleared after Update);
   - clearing on re-mint, gravity change, `RestoreRegistry`, Play/Stop, hot reload;
   - fixture-index mapping on a multi-fixture body;
   - the AND filter: a fixture with `contactEvents = false` touching a default fixture produces no Begin/End;
   - GUID survives on a destroyed entity, `entity` invalid;
   - `ContactsOf`;
   - `Fixture` flags round-trip through scene JSON; a pre-spec scene loads with the defaults;
   - `physics.events.hitThreshold` reaches the world;
   - the game-module surface compiles without Manifold2D (the SDK include check).
3. **End to end (witness):** a ReferenceProject game system reads `FrameEvents()` and makes an observable change when the ball hits the crate in `physics.arcscene` (guid `4f6a1c2e-7b3d-4e8a-9c1f-2d5b6e7a8f90`, not the boot scene). A new witness scenario runs it headless and asserts the change. The existing goldens stay untouched.
4. **Step zero gates:** Manifold2D full suite + determinism fixture green; Arcane both configs green; trajectory fixture re-recorded with before/after review; golden gate.

## 10. Follow-ups recorded

- Manifold2D premake FMA (Linux) + macOS arm64 filter → Arcane's consumer wrapper, at the Linux/mac port.
- Joint-break events → spec 3.
- A per-reader cursor facility, if the typed game-event queue spec wants one.
