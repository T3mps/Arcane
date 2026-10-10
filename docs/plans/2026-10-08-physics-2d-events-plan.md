# 2D Physics Events Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Game code can read collision, hit and trigger events as data after each physics step: Box2D-v3-style event arrays added upstream in Manifold2D, then exposed by Arcane as two pull windows (`StepEvents()`, `FrameEvents()`) on `PhysicsResource`, keyed by entity + GUID + fixture index.

**Architecture:** Three repos, upstream first.
- **Mosaic** is reconciled so it is the single source of truth.
- **Manifold2D** gains per-fixture event flags, contact begin/end arrays derived from `Contact::touching` transitions, hit events read from the solved constraints, a Box2D-v3.1.1-style sensor pass at the end of the step, double-buffered destroy-time ends and `GetBodyContacts`. The legacy `ContactManager` listener is removed.
- **Arcane** gets one `sync-vendor.ps1`, re-vendors twice (once for the motion change, once for events), then:
  - records each minted body (entity, GUID, fixture handles);
  - translates the world's arrays after every step;
  - clears the per-frame window through a physics-agnostic `RunLoop` hook;
  - proves the chain end to end with a witness, where a game system tints the Crate when it lands.

**Tech Stack:** C++23, premake5, MSBuild (VS 2026), Catch2 v3, Astra ECS, Manifold2D, PowerShell.

**Spec:** `docs/specs/2026-10-08-physics-2d-events-design.md`, which includes its §11 plan-time amendments A1–A9. Read it before any task. Where this plan and the spec disagree, the spec wins; stop and report.

## Global Constraints

- **Repos:**
  - Mosaic `D:\dev\starworks\Mosaic`;
  - Manifold2D `D:\dev\starworks\Manifold2D`;
  - Astra `D:\dev\starworks\Astra` (V1 only, its `vendor/Mosaic`);
  - Arcane `D:\dev\starworks\Arcane`.

  Never touch `D:\dev\github\Manifold2D` (a stale clone; the user's call).
- **Branches:** Manifold2D work happens on `feat/events`, off `main` after the user's branch merge. Arcane work happens on `feat/physics-events`, off `main`. Never push, never force-push, never merge to a main branch without the user's word.
- **Every commit message ends with:**
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
  ```
- **Manifold2D style:** C++23; `namespace Manifold2D { namespace Physics { ... } }`; ASCII comments; "PRESENTATION-FREE" headers (no SDL/NRI/ImGui); every Box2D parity claim cites `.reference/box2d-3.1.1` file:line.
- **Determinism:** no event array is ever filled in `std::unordered_map` iteration order. Every array is sorted before it becomes visible, and identical inputs give byte-identical arrays with the serial and the MT executor.
- **Simulation neutrality:** nothing in this plan changes a body trajectory, except Task A1's re-vendor (the reviewed motion change). After A1, `ArcaneTests "[trajectory]"` must stay bit-identical in every later task.
- **Build Manifold2D:** from the repo root, run `scripts\generate_vs2022.bat`, then `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" Manifold2D.sln /p:Configuration=Debug /p:Platform=x64 /m /v:m`. Tests: `bin\Debug-windows-x86_64\Manifold2DTests\Manifold2DTests.exe "[physics][events]"`. Use Release the same way, with `bin\Release-windows-x86_64`.
- **Build Arcane:** use the **PowerShell tool** (Bash's MSYS path conversion mangles `/p:`):
  - generate: `cmd /c call "D:\dev\starworks\Arcane\GenerateProjects.bat"`;
  - build: `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" D:\dev\starworks\Arcane\Arcane.slnx /p:Configuration=Debug /m /v:m > build.log` and check the log with `Select-String 'error|Warning\(s\)|Error\(s\)'`.
  - Tests run **from the exe dir**: `cd bin\Debug-windows-x86_64-md\ArcaneTests; .\ArcaneTests.exe "[physics]"`. Note the `Randomness seeded to: N` line.
- **ReferenceProject build:** `ARCANE_SDK=D:\dev\starworks\Arcane`, then `%ARCANE_SDK%\bin\Debug-windows-x86_64-md\arcbuild\arcbuild.exe build --project ReferenceProject --config Debug`.
  - Build `ReferenceProject` **before** `Arcane.slnx` for each config, or Arcane's post-build stages the other config's DLL.
  - A Debug-CRT game DLL inside a Release host crashes.
- **Settings rule:** new tunables are settings (`physics.events`).
- **GUID rule:** nothing stores an `Astra::Entity` past the frame. Body records key on the Manifold2D handle and carry the `Identity` GUID.
- **Never edit vendored copies in place:** `ThirdParty/Astra`, `ThirdParty/Manifold2D/include|src` and `ThirdParty/Mosaic/include|src` change only through `scripts/sync-vendor.ps1`.

## Review Focus

These are the five inputs most likely to bite a user that no other task's tests would naturally cover. Each one has a pinning test in the task named.

1. **A body slot is reused inside one frame** (a body is removed and a new one minted before the next step, and Manifold2D recycles the index with a new generation). The destroy-time End must carry the old generation and resolve to the old entity and GUID. The new body must never resolve to the old record. Pinned in A3 (`"a recycled body slot never resolves to the retired record"`).
2. **A compound body whose fixture 0 is a sensor and fixture 1 is solid.** Fixture 0 goes through `BodyDef`'s auto-fixture, the others through `AddFixture`. The event flags and the fixture-index mapping must survive both paths. Pinned in A3 (`"fixture indices map through the auto-fixture and AddFixture paths"`).
3. **A sensor's visitor is destroyed and a new fixture takes its slot before the next pass.** The sensor must report End for the old visitor and Begin for the new one, never "nothing changed". Pinned in M5 (`"a recycled visitor slot ends the old overlap and begins the new"`).
4. **The registry is replaced mid-frame** (Stop's `RestoreRegistry`, a hot reload) between fixed steps. The frame hook must tolerate a registry with no `PhysicsResource`, and nothing from the old world may leak into the new frame window. Pinned in A4 (`"the frame hook tolerates a registry without PhysicsResource"`).
5. **The world event gate is turned off and back on while pairs are touching.** No burst of Begins on re-enable, and no End for pairs that never reported a Begin. Pinned in M2 (`"the world gate drops events without a burst on re-enable"`).

---

## File map

**Mosaic (V1):** whatever files the reconcile finds; `docs/` gets a short note on the reconcile.

**Manifold2D (M0–M7):**

| File | Responsibility |
|---|---|
| `include/Manifold2D/Physics/Events.hpp` (new) | Event structs, `ContactEvents` / `SensorEvents` spans, `BodyContact`, `kEv*` flags, handle ordering |
| `include/Manifold2D/Physics/Fixture.hpp` | `FixtureDef` event flags |
| `include/Manifold2D/Physics/PhysicsWorld.hpp` / `src/Physics/PhysicsWorld.cpp` | `BodyDef` flags, `WorldDef` fields, SoA flag columns, setters, event buffers, stage 6 (hits, sensor pass, sort, flip), `GetContactEvents` / `GetSensorEvents` / `GetBodyContacts`, rewritten `ForEachContact`, legacy removal |
| `include/Manifold2D/Physics/Contact.hpp` | `Contact::eventFlags`, `genA`, `genB` |
| `src/Physics/ConstraintGraph.cpp` (+ header) | flag capture at create, transitions for every solver contact, begin/end push in the serial tail, the End on destroy, a pool accessor |
| `include/Manifold2D/Physics/ContactManager.hpp`, `src/Physics/ContactManager.cpp` | **deleted** in M7 |
| `tests/PhysicsEventTestHelpers.hpp` (new) | `EventLog` collector + scene builders |
| `tests/PhysicsContactEventsTest.cpp`, `tests/PhysicsHitEventsTest.cpp`, `tests/PhysicsSensorEventsTest.cpp`, `tests/PhysicsBodyContactsTest.cpp` (new) | upstream event tests |
| `tests/PhysicsWorldTest.cpp`, `PhysicsCollisionFilterTest.cpp`, `PhysicsQueryRotationTest.cpp`, `PhysicsPhase1HarnessTest.cpp` | migrated off `OnContact` (M7) |

**Arcane (A1–A8):**

| File | Responsibility |
|---|---|
| `scripts/sync-vendor.ps1` (new), `scripts/sync-astra.ps1` (shim) | one vendoring path with stamps + a Mosaic drift warning |
| `ThirdParty/{Mosaic,Manifold2D}/...` | re-vendored by script only |
| `ArcaneCore/src/Arcane/Scene/PhysicsEvents2D.hpp` (new) | game-facing event types, no Manifold2D |
| `ArcaneCore/src/Arcane/Scene/PhysicsEvents2D.cpp` (new) | exported `PhysicsResource` event members (record, retire, capture, windows, `ContactsOf`) |
| `ArcaneCore/src/Arcane/Scene/PhysicsSystem.hpp` | resource members, policy setters, record/retire in PASS 1/2, capture after PASS 3 |
| `ArcaneCore/src/Arcane/Scene/PhysicsComponents.hpp` | `Fixture` flags |
| `ArcaneCore/src/Arcane/Scene/PhysicsQuerySettings.{hpp,cpp}` | `PhysicsEventSettings` |
| `ArcaneCore/src/Arcane/Sim/RunLoop.hpp`, `ArcaneCore/src/Arcane/Base/Runtime.cpp` | frame-begin hook |
| `ArcaneCore/src/Arcane/Plugin/PluginABI.hpp`, `ReferenceProject/ReferenceProject.arcproj`, `D:\dev\starworks\Aphelyon\Aphelyon.arcproj` | ABI 55 |
| `ArcaneTests/src/PhysicsEvents2DTest.cpp`, `PhysicsFrameEventsTest.cpp` (new) | Arcane event tests |
| `ArcaneTests/src/ArcaneSpellingGuardTest.cpp` | add the new header |
| `ReferenceProject/Source/Game/TintOnContact.{hpp,cpp}`, `TintOnContactSystem.{hpp,cpp}` (new); `Content/scenes/physics.arcscene` | witness game code + data |
| `ArcaneTests/src/WitnessScenariosTest.cpp` | W5 |

---

## Phase V — Mosaic is the source of truth

### Task V1: Reconcile Mosaic upstream and the Manifold2D / Astra copies

**Files:**
- Modify: files under `D:\dev\starworks\Mosaic\include\Mosaic\` that a copy changed (expected: `Simd/Wide_AVX2.inl`, `Simd/Wide_NEON.inl`, `Simd/Wide_Scalar.inl`, `Platform.hpp`, `Simd/Bits.hpp`)
- Modify: `D:\dev\starworks\Manifold2D\ThirdParty\Mosaic\**` and `D:\dev\starworks\Astra\vendor\Mosaic\**` (copied from upstream after the reconcile)
- Create: `D:\dev\starworks\Manifold2D\ThirdParty\Mosaic\VENDORED.txt`, `D:\dev\starworks\Astra\vendor\Mosaic\VENDORED.txt`

**Interfaces:**
- Produces: a Mosaic commit `M_RECON` that every copy matches byte for byte (`include/`, `src/`, `LICENSE`). A1's drift check reads the `commit :` line of each copy's `VENDORED.txt`.

- [ ] **Step 1: Inventory the drift (read-only).**

```bash
cd /d/dev/starworks
for c in Arcane/ThirdParty/Mosaic Manifold2D/ThirdParty/Mosaic Astra/vendor/Mosaic; do
  echo "== $c"; diff -rq Mosaic/include "$c/include"; diff -rq Mosaic/src "$c/src"; done
```
Expected (measured 2026-10-08): Arcane's `Simd/Wide_AVX2.inl`, `Wide_NEON.inl` and `Wide_Scalar.inl` differ; Manifold2D's `Platform.hpp` differs (the `__EMSCRIPTEN__` arm). Astra: whatever it prints. Save the full output to the task report.

- [ ] **Step 2: Decide the direction for every differing file.** For each file, run `git log -3 --format='%h %ci %s' -- <path>` in Mosaic and in the copy's repo, and diff the content. The rule:
  - The copy has a change upstream lacks → port that change into Mosaic.
  - Upstream is a strict superset (the copy is merely stale) → nothing to port.
  - Both sides changed → merge by hand and write down why.

  Also fetch the ARM fix from Astra's macOS branch: `git -C D:\dev\starworks\Astra fetch origin mac/port` then `git -C D:\dev\starworks\Astra diff origin/linux/gcc-clang origin/mac/port -- vendor/Mosaic`. Port its `Simd/Bits.hpp` change (`__crc32cd`, `__builtin_prefetch` constant argument) into Mosaic.

- [ ] **Step 3: Commit the ports in Mosaic**, one commit per source. For example: `fix(simd): the ARM Bits paths compile under Apple Clang (from Astra mac/port)`, `feat(platform): the __EMSCRIPTEN__ arm (from Manifold2D)`, `fix(simd): Wide_* changes made in Arcane's copy`. Each message names the copy it came from.

- [ ] **Step 4: Build and test Mosaic.** Follow `D:\dev\starworks\Mosaic\README.md`'s build section (generate, MSBuild Debug + Release, run its test exe). Expected: all tests pass. If the README has no test step, build `Mosaic.sln` both configs and record "no test suite".

- [ ] **Step 5: Sync the two library copies from upstream and stamp them.** Run in PowerShell:

```powershell
$src = 'D:\dev\starworks\Mosaic'; $sha = git -C $src rev-parse HEAD
foreach ($dst in 'D:\dev\starworks\Manifold2D\ThirdParty\Mosaic','D:\dev\starworks\Astra\vendor\Mosaic') {
  foreach ($d in 'include','src') { robocopy "$src\$d" "$dst\$d" /MIR /NFL /NDL /NJH /NJS /NP | Out-Null; if ($LASTEXITCODE -ge 8) { throw "robocopy $d -> $dst failed" } }
  Copy-Item "$src\LICENSE" "$dst\LICENSE" -Force
  Set-Content "$dst\VENDORED.txt" -Encoding utf8 -Value "Mosaic vendored from $src`ncommit : $sha`nscope  : include/ + src/ + LICENSE`nsynced : $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
}
```
Keep each copy's own `premake5.lua`, if one exists; the script never touches it. Then build and run Manifold2D's suite (Debug) and Astra's suite (`AstraTest.exe`, Debug), and commit in each repo: `chore(vendor): Mosaic synced from upstream <sha> with a VENDORED stamp`. Astra: commit on its current branch (`dev`). Do not touch Astra's other unpushed work.

Expected: both suites pass with the same counts as before the sync. Record the counts.

---

## Phase M — Manifold2D upstream (branch `feat/events`)

### Task M0: Baseline on the consolidated main

**Files:** none modified.

- [ ] **Step 1: Confirm the user's merge.** `git -C D:\dev\starworks\Manifold2D log --oneline -1 main`. Then check that `git merge-base --is-ancestor feat/wasm-scene-api main` and `git merge-base --is-ancestor origin/ci/tri-platform main` both exit 0. If either fails, STOP and report: the user is merging these by hand.
- [ ] **Step 2: Branch.** `git switch -c feat/events main`. Record the base SHA as `M2D_BASE` (A1 vendors exactly this commit).
- [ ] **Step 3: Baseline.** Build Debug and Release (Global Constraints). Run the full exe in both configs and the cross-platform determinism fixture `"[xplat]"`. Record `test cases` / `assertions` per config. Every later Manifold2D task compares against these numbers.

### Task M1: Event flags, definitions, and capture at contact creation

**Files:**
- Create: `include/Manifold2D/Physics/Events.hpp`
- Modify: `include/Manifold2D/Physics/Fixture.hpp:190-223` (FixtureDef)
- Modify: `include/Manifold2D/Physics/PhysicsWorld.hpp` (BodyDef ~`:103-140`, WorldDef `:223-283`, SoA columns beside `m_fxSensor` `:1383`, setters near `SetBodyFilter` `:611`)
- Modify: `src/Physics/PhysicsWorld.cpp` (`AllocFixtureSlot` `:506-522`, the AddBody auto-fixture `autoFd` near `:1169`, setters)
- Modify: `include/Manifold2D/Physics/Contact.hpp:63-117` (Contact)
- Modify: `src/Physics/ConstraintGraph.cpp:494-506` (TryCreateContact `r.created` block)
- Create: `tests/PhysicsEventTestHelpers.hpp`, `tests/PhysicsContactEventsTest.cpp`

**Interfaces:**
- Produces (exact names, used by M2–M7 and Arcane):
  - `FixtureDef::contactEvents`, `::sensorEvents`, `::hitEvents` (`bool`, default `false`), plus the same three on `BodyDef`.
  - `WorldDef::hitEventThreshold` (`Real`, default `Real(1)`) and `WorldDef::contactEventsRequireBoth` (`bool`, default `false`).
  - `void PhysicsWorld::SetFixtureEvents(FixtureHandle, bool contact, bool sensor, bool hit)`, `void SetHitEventThreshold(Real)`, `void SetContactEventsRequireBoth(bool)`.
  - `[[nodiscard]] std::uint8_t PhysicsWorld::DebugContactEventFlags(FixtureHandle a, FixtureHandle b) const` (test seam: the pool contact's `eventFlags`, or `0xFF` when no contact exists).
  - In `Events.hpp`: `kEvContact = 1`, `kEvHit = 2`; the five event structs; `ContactEvents`, `SensorEvents`, `BodyContact`.

- [ ] **Step 1: Create `Events.hpp`.**

```cpp
#pragma once

// Events.hpp: per-step contact / hit / sensor event records (Box2D v3
// b2ContactEvents / b2SensorEvents shape, spec 2026-10-08 s6). Arrays are owned
// by PhysicsWorld and valid until the next Step. Every record carries BOTH
// fixture handles and BOTH body handles (amendment A5: a destroy-time End names
// a fixture that no longer exists).
//
// PRESENTATION-FREE, ASCII comments, C++23.

#include <cstdint>
#include <span>

#include <Manifold2D/Physics/PhysicsTypes.hpp>
#include <Manifold2D/Physics/Fixture.hpp>

namespace Manifold2D
{
    namespace Physics
    {
        // Contact::eventFlags bits, fixed when the pool contact is created
        // (Box2D contact.c:253-256, :535-541).
        inline constexpr std::uint8_t kEvContact = 1u;
        inline constexpr std::uint8_t kEvHit     = 2u;

        struct ContactBeginEvent { FixtureHandle a{}, b{}; BodyHandle bodyA{}, bodyB{}; };
        struct ContactEndEvent   { FixtureHandle a{}, b{}; BodyHandle bodyA{}, bodyB{}; };
        // normal points from A to B (Box2D convention; Manifold2D's manifold
        // normal points B -> A, Manifold.hpp:33, so the producer negates it).
        struct ContactHitEvent   { FixtureHandle a{}, b{}; BodyHandle bodyA{}, bodyB{};
                                   Vec2 point{}; Vec2 normal{}; Real approachSpeed = Real(0); };
        struct SensorBeginEvent  { FixtureHandle sensor{}, visitor{}; BodyHandle sensorBody{}, visitorBody{}; };
        struct SensorEndEvent    { FixtureHandle sensor{}, visitor{}; BodyHandle sensorBody{}, visitorBody{}; };

        struct ContactEvents
        {
            std::span<const ContactBeginEvent> begin;
            std::span<const ContactEndEvent>   end;
            std::span<const ContactHitEvent>   hit;
        };
        struct SensorEvents
        {
            std::span<const SensorBeginEvent> begin;
            std::span<const SensorEndEvent>   end;
        };

        // One touching solver contact of a body (b2Body_GetContactData). normal
        // points from `self` outward to `other`.
        struct BodyContact
        {
            FixtureHandle self{}, other{};
            BodyHandle    selfBody{}, otherBody{};
            Vec2          normal{};
            int           pointCount = 0;
        };

        // Deterministic order for event arrays: (index, generation) lexicographic.
        [[nodiscard]] constexpr bool FixtureLess(FixtureHandle l, FixtureHandle r) noexcept
        {
            return l.index != r.index ? l.index < r.index : l.generation < r.generation;
        }
    } // namespace Physics
} // namespace Manifold2D
```

- [ ] **Step 2: Add the flags.** Add to `FixtureDef` after `isSensor`:

```cpp
            // Event opt-ins (Box2D b2ShapeDef enableContactEvents / enableSensorEvents
            // / enableHitEvents, types.h:385-392). All default OFF, like Box2D's
            // b2DefaultShapeDef (types.c:55-65). contact + hit are captured when a
            // contact is created; sensor is read every sensor pass (spec s6.1).
            bool contactEvents = false;
            bool sensorEvents  = false;
            bool hitEvents     = false;
```
Add the same three fields to `BodyDef` after `isSensor`, with the comment `// copied onto the auto-fixture, like isSensor`. Add to `WorldDef` after `sleepThreshold`:

```cpp
            // Approach speed (m/s) a touching point must EXCEED to report a hit
            // (Box2D b2WorldDef::hitEventThreshold, types.c:14: 1 m/s).
            Real          hitEventThreshold = Real(1);
            // false: a contact reports begin/end if EITHER fixture opts in (Box2D
            // contact.c:253). true: BOTH must (Arcane's policy, spec amendment A2).
            bool          contactEventsRequireBoth = false;
```

- [ ] **Step 3: Add the SoA columns and fill them.** Beside `m_fxSensor`, add `std::vector<std::uint8_t> m_fxContactEvents, m_fxSensorEvents, m_fxHitEvents;`. Add world members `Real m_hitEventThreshold = Real(1); bool m_contactEventsRequireBoth = false;`, initialised from the `WorldDef` in the constructor next to the other `WorldDef` copies. Resize the new columns wherever `m_fxSensor` is resized. In `AllocFixtureSlot` (`PhysicsWorld.cpp:517`), add:

```cpp
            m_fxContactEvents[fi] = def.contactEvents ? std::uint8_t(1) : std::uint8_t(0);
            m_fxSensorEvents[fi]  = def.sensorEvents  ? std::uint8_t(1) : std::uint8_t(0);
            m_fxHitEvents[fi]     = def.hitEvents     ? std::uint8_t(1) : std::uint8_t(0);
```
Where `AddBody` builds `autoFd` (the `autoFd.isSensor = def.isSensor` line), add `autoFd.contactEvents = def.contactEvents; autoFd.sensorEvents = def.sensorEvents; autoFd.hitEvents = def.hitEvents;`.

- [ ] **Step 4: Add the setters** (definitions in `PhysicsWorld.cpp`, following `SetGravityScale`'s shape):

```cpp
        void PhysicsWorld::SetFixtureEvents(FixtureHandle fh, bool contact, bool sensor, bool hit)
        {
            if (!IsValid(fh))
            {
                MOSAIC_LOG_WARN("operation on a stale/invalid FixtureHandle ignored");
                return;
            }
            // contact/hit apply to contacts CREATED afterwards (captured at create,
            // Box2D contact.c:253); sensor is read every sensor pass (spec s6.1, A3).
            m_fxContactEvents[fh.index] = contact ? std::uint8_t(1) : std::uint8_t(0);
            m_fxSensorEvents[fh.index]  = sensor  ? std::uint8_t(1) : std::uint8_t(0);
            m_fxHitEvents[fh.index]     = hit     ? std::uint8_t(1) : std::uint8_t(0);
        }

        void PhysicsWorld::SetHitEventThreshold(Real threshold)
        {
            if (!std::isfinite(threshold) || threshold < Real(0))
            {
                MOSAIC_LOG_WARN("SetHitEventThreshold: negative or non-finite threshold ignored");
                return;
            }
            m_hitEventThreshold = threshold;
        }

        void PhysicsWorld::SetContactEventsRequireBoth(bool both) noexcept { m_contactEventsRequireBoth = both; }
```
`SetContactEventsRequireBoth` may be inline in the header, declared `noexcept`.

- [ ] **Step 5: Capture at create.** Add to `Contact` (after `eventRelevant`):

```cpp
            std::uint8_t  eventFlags = 0;   // kEvContact | kEvHit, fixed at create (Events.hpp)
            std::uint32_t genA = 0;         // body generations at create: an End emitted after
            std::uint32_t genB = 0;         // RemoveBody bumped m_gen still names the old handle
```
In `TryCreateContact`'s `if (r.created)` block (`ConstraintGraph.cpp:501-506`), after `c.eventRelevant = eventRelevant;`, add:

```cpp
                // Event opt-ins, decided ONCE here (Box2D contact.c:253-256 contact
                // events, :535-541 hit events). Only solver contacts report -- a
                // dynamic body present, no sensor -- which is Box2D's contact set
                // (it makes no kinematic-static contact; sensors go through the
                // sensor pass). Spec s6.1, amendments A2/A9.
                c.genA = w.m_gen[ia];
                c.genB = w.m_gen[ib];
                if (solverRelevant)
                {
                    const bool evA = w.m_fxContactEvents[fia] != 0u;
                    const bool evB = w.m_fxContactEvents[fib] != 0u;
                    const bool contactOn = w.m_contactEventsRequireBoth ? (evA && evB) : (evA || evB);
                    const bool hitOn = (w.m_fxHitEvents[fia] | w.m_fxHitEvents[fib]) != 0u;
                    c.eventFlags = static_cast<std::uint8_t>((contactOn ? kEvContact : 0u) | (hitOn ? kEvHit : 0u));
                }
```
Include `<Manifold2D/Physics/Events.hpp>` in `ConstraintGraph.cpp`. Then implement `DebugContactEventFlags`: walk `m_graph`'s pool (add `const Contact* ConstraintGraph::FindContact(FixtureHandle a, FixtureHandle b) const`, which looks up by the same order-independent key `EnsurePair` uses, via `ContactPool`) and return `c->eventFlags` or `0xFF`.

- [ ] **Step 6: Write the test helpers.** Create `tests/PhysicsEventTestHelpers.hpp`:

```cpp
#pragma once
// Shared helpers for the [physics][events] suites: an EventLog that copies each
// step's arrays (they are only valid until the next Step) and tiny scene builders.
// +Y DOWN, gravity 10 (WorldDef defaults).
#include <vector>
#include <Manifold2D/Physics/PhysicsWorld.hpp>
#include <Manifold2D/Physics/Events.hpp>

namespace EventTest
{
    using namespace Manifold2D::Physics;
    inline constexpr Real kStep = Real(1) / Real(60);

    struct EventLog
    {
        std::vector<ContactBeginEvent> begin;
        std::vector<ContactEndEvent>   end;
        std::vector<ContactHitEvent>   hit;
        std::vector<SensorBeginEvent>  sensorBegin;
        std::vector<SensorEndEvent>    sensorEnd;

        void Collect(const PhysicsWorld& w)
        {
            const ContactEvents c = w.GetContactEvents();
            begin.insert(begin.end(), c.begin.begin(), c.begin.end());
            end.insert(end.end(), c.end.begin(), c.end.end());
            hit.insert(hit.end(), c.hit.begin(), c.hit.end());
            const SensorEvents s = w.GetSensorEvents();
            sensorBegin.insert(sensorBegin.end(), s.begin.begin(), s.begin.end());
            sensorEnd.insert(sensorEnd.end(), s.end.begin(), s.end.end());
        }
        void StepAndCollect(PhysicsWorld& w, int steps)
        {
            for (int i = 0; i < steps; ++i) { w.Step(kStep); Collect(w); }
        }
    };

    // Static ground slab whose top face is y = 0, 20 m wide.
    inline BodyHandle AddGround(PhysicsWorld& w, bool contactEvents = true, bool sensorEvents = true)
    {
        BodyDef d;
        d.type = BodyType::Static;
        d.position = Vec2(Real(0), Real(0.5));
        d.shape = MakeAabb(Real(10), Real(0.5));
        d.contactEvents = contactEvents;
        d.sensorEvents = sensorEvents;
        return w.AddBody(d);
    }

    // Dynamic 1 m box whose centre starts at (x, y).
    inline BodyHandle AddBox(PhysicsWorld& w, Real x, Real y, bool contactEvents = true,
                             bool hitEvents = false, bool sensorEvents = true)
    {
        BodyDef d;
        d.type = BodyType::Dynamic;
        d.position = Vec2(x, y);
        d.shape = MakeAabb(Real(0.5), Real(0.5));
        d.contactEvents = contactEvents;
        d.hitEvents = hitEvents;
        d.sensorEvents = sensorEvents;
        return w.AddBody(d);
    }
}
```
`GetContactEvents` / `GetSensorEvents` do not exist yet: in M1, declare them in `PhysicsWorld.hpp` returning empty spans, so the helper compiles. M2 and M5 fill them in.

- [ ] **Step 7: Write the failing capture tests** in `tests/PhysicsContactEventsTest.cpp`:

```cpp
// PhysicsContactEventsTest.cpp
// [physics][events]: contact begin/end arrays (spec 2026-10-08 s6.1).
#include <catch2/catch_test_macros.hpp>
#include "PhysicsEventTestHelpers.hpp"

using namespace EventTest;

TEST_CASE("Event flags default off and are captured when a contact is created", "[physics][events]")
{
    PhysicsWorld w{ WorldDef{} };
    FixtureDef fd;                       // defaults
    CHECK_FALSE(fd.contactEvents);
    CHECK_FALSE(fd.sensorEvents);
    CHECK_FALSE(fd.hitEvents);
    CHECK(WorldDef{}.hitEventThreshold == Real(1));
    CHECK_FALSE(WorldDef{}.contactEventsRequireBoth);

    const BodyHandle g = AddGround(w, /*contactEvents*/ false);
    const BodyHandle b = AddBox(w, Real(0), Real(-0.49), /*contactEvents*/ true, /*hit*/ true);
    w.Step(kStep);
    const std::uint8_t flags = w.DebugContactEventFlags(w.GetBodyFixture(b, 0), w.GetBodyFixture(g, 0));
    REQUIRE(flags != 0xFF);              // the pair exists
    CHECK(flags == (kEvContact | kEvHit)); // either-fixture rule (Box2D contact.c:253, :535)
}

TEST_CASE("contactEventsRequireBoth needs both fixtures; hit stays either", "[physics][events]")
{
    WorldDef wd; wd.contactEventsRequireBoth = true;
    PhysicsWorld w{ wd };
    const BodyHandle g = AddGround(w, /*contactEvents*/ false);
    const BodyHandle b = AddBox(w, Real(0), Real(-0.49), /*contactEvents*/ true, /*hit*/ true);
    w.Step(kStep);
    CHECK(w.DebugContactEventFlags(w.GetBodyFixture(b, 0), w.GetBodyFixture(g, 0)) == kEvHit);
}

TEST_CASE("A sensor pair captures no contact or hit flags", "[physics][events]")
{
    PhysicsWorld w{ WorldDef{} };
    AddGround(w);
    BodyDef d; d.type = BodyType::Dynamic; d.position = Vec2(Real(0), Real(-2));
    d.shape = MakeAabb(Real(0.5), Real(0.5)); d.isSensor = true; d.contactEvents = true; d.hitEvents = true;
    const BodyHandle s = w.AddBody(d);
    const BodyHandle o = AddBox(w, Real(0), Real(-2), true, true);   // overlapping mover pair
    w.Step(kStep);
    const std::uint8_t f = w.DebugContactEventFlags(w.GetBodyFixture(s, 0), w.GetBodyFixture(o, 0));
    CHECK((f == 0xFF || f == 0u));       // either no pool contact or a non-solver one
}
```

- [ ] **Step 8: Run the tests and see them fail before Step 5, pass after.** Build Debug, run `Manifold2DTests.exe "[physics][events]"`. Expected after Steps 1–5: 3 test cases pass. Then run the full suite: the same counts as M0 plus these.
- [ ] **Step 9: Commit.** `git add -A include src tests` then `git commit -m "feat(events): per-fixture contact/sensor/hit flags, the hit threshold and the either/both contact rule, captured when a contact is created (Box2D contact.c:253, :535)"` (plus the trailer).

### Task M2: Contact begin/end arrays, double-buffered ends, the world gate

**Files:**
- Modify: `src/Physics/ConstraintGraph.cpp`: `UpdateOneContact` `:581-607` and the serial tail `:1036-1075`
- Modify: `include/Manifold2D/Physics/PhysicsWorld.hpp`, `src/Physics/PhysicsWorld.cpp`: buffers, push helpers, `StepImpl` start and end, `GetContactEvents`, `SetEventsEnabled`
- Test: `tests/PhysicsContactEventsTest.cpp`

**Interfaces:**
- Consumes: M1's `Contact::eventFlags`, `genA`, `genB`, `kEvContact`.
- Produces: `[[nodiscard]] ContactEvents PhysicsWorld::GetContactEvents() const noexcept`, and (private, `ConstraintGraph` is a friend) `void PushContactBegin(const Contact&)` / `void PushContactEnd(const Contact&)`. Buffer members: `m_contactBeginEvents`, `m_contactHitEvents`, `m_contactEndEvents[2]`, `m_endEventIndex` (initialised to 0).

- [ ] **Step 1: Write the failing tests** (append to `PhysicsContactEventsTest.cpp`):

```cpp
TEST_CASE("A box landing on static ground begins once and ends when lifted", "[physics][events]")
{
    PhysicsWorld w{ WorldDef{} };
    const BodyHandle g = AddGround(w);
    const BodyHandle b = AddBox(w, Real(0), Real(-2));
    EventLog log;
    log.StepAndCollect(w, 120);
    REQUIRE(log.begin.size() == 1);                 // dynamic-vs-static now reports (the old events skipped it)
    CHECK(log.begin[0].bodyA == b);                 // canonical: A is the dynamic side (Contact.hpp:65)
    CHECK(log.begin[0].bodyB == g);
    CHECK(log.end.empty());
    w.SetPosition(b, Vec2(Real(0), Real(-5)));      // teleport away
    log.StepAndCollect(w, 2);
    CHECK(log.end.size() == 1);
}

TEST_CASE("Two dynamic boxes report begin; kinematic-vs-static reports nothing", "[physics][events]")
{
    PhysicsWorld w{ WorldDef{} };
    AddGround(w);
    AddBox(w, Real(0), Real(-0.5));
    AddBox(w, Real(0), Real(-1.6));
    BodyDef k; k.type = BodyType::Kinematic; k.position = Vec2(Real(5), Real(-0.4));
    k.shape = MakeAabb(Real(0.5), Real(0.5)); k.contactEvents = true;
    w.AddBody(k);                                   // overlaps the ground: no solver contact in Box2D terms
    EventLog log;
    log.StepAndCollect(w, 90);
    CHECK(log.begin.size() == 2);                   // box-ground, box-box; never kinematic-ground (A9)
}

TEST_CASE("Opting out on both fixtures silences the pair", "[physics][events]")
{
    PhysicsWorld w{ WorldDef{} };
    AddGround(w, false);
    AddBox(w, Real(0), Real(-2), false);
    EventLog log;
    log.StepAndCollect(w, 120);
    CHECK(log.begin.empty());
}

TEST_CASE("Begin arrays are sorted by fixture pair", "[physics][events]")
{
    PhysicsWorld w{ WorldDef{} };
    AddGround(w);
    for (int i = 0; i < 6; ++i) AddBox(w, Real(-5 + 2 * i), Real(-0.49));   // all land in step 1
    w.Step(kStep);
    const ContactEvents c = w.GetContactEvents();
    REQUIRE(c.begin.size() == 6);
    for (std::size_t i = 1; i < c.begin.size(); ++i)
        CHECK((FixtureLess(c.begin[i - 1].a, c.begin[i].a) ||
               (c.begin[i - 1].a == c.begin[i].a && FixtureLess(c.begin[i - 1].b, c.begin[i].b))));
}

TEST_CASE("the world gate drops events without a burst on re-enable", "[physics][events]")
{
    PhysicsWorld w{ WorldDef{} };
    AddGround(w);
    AddBox(w, Real(0), Real(-2));
    w.SetEventsEnabled(false);
    EventLog log;
    log.StepAndCollect(w, 120);                     // lands while gated
    CHECK(log.begin.empty());
    w.SetEventsEnabled(true);
    log.StepAndCollect(w, 10);                      // still touching
    CHECK(log.begin.empty());                       // no burst
    CHECK(log.end.empty());                         // and no orphan End
}
```

- [ ] **Step 2: Run them to see them fail** (they see empty spans). `Manifold2DTests.exe "[physics][events]"`: FAIL on the begin counts.

- [ ] **Step 3: Transitions for every solver contact.** In `UpdateOneContact`, replace the dyn-dyn-only block (`ConstraintGraph.cpp:599-607`) with:

```cpp
            // Touching transitions for EVERY solver contact (events, spec s6.1);
            // the island consumers in the serial tail re-check dynamic-dynamic.
            if (c.solverRelevant && c.bIsBody && c.bodyB != kInvalidSlot)
            {
                if (!wasTouching && c.touching)      { c.npState |= kNpStarted; }
                else if (wasTouching && !c.touching) { c.npState |= kNpStopped; }
            }
```

- [ ] **Step 4: Push in the serial tail.** In the tail's `ForEachSetBit` lambda (`:1041-1072`), replace the two `else if` arms with:

```cpp
                    else if (c.npState & kNpStarted)
                    {
                        if (c.eventFlags & kEvContact) { w.PushContactBegin(c); }
                        if (IsDynDynSolver(w, c))
                        {
                            const std::uint32_t lo = c.bodyA < c.bodyB ? c.bodyA : c.bodyB;
                            const std::uint32_t hi = c.bodyA < c.bodyB ? c.bodyB : c.bodyA;
                            m_pendingMerges.push_back(BroadphasePair{ lo, hi });
                        }
                    }
                    else if (c.npState & kNpStopped)
                    {
                        if (c.eventFlags & kEvContact) { w.PushContactEnd(c); }
                        if (IsDynDynSolver(w, c)) { w.MarkSplitCandidate(w.IslandOf(c.bodyA)); }
                    }
```
Then add a file-local helper above `UpdateContacts`. It must carry the exact predicate the island code used before (bounds checks included):

```cpp
        // The island merge/split edge predicate (unchanged semantics): a solver
        // contact between two DYNAMIC bodies.
        bool IsDynDynSolver(const PhysicsWorld& w, const Contact& c) noexcept
        {
            return c.solverRelevant && c.bIsBody &&
                   c.bodyA != kInvalidSlot && c.bodyB != kInvalidSlot &&
                   w.TypeSlot(c.bodyA) == BodyType::Dynamic &&
                   w.TypeSlot(c.bodyB) == BodyType::Dynamic;
        }
```

- [ ] **Step 5: Buffers, push helpers, getter, flip.** In `PhysicsWorld.hpp` (private), declare the buffers named in Interfaces as `std::vector<...>`, plus `std::uint32_t m_endEventIndex = 0;`. In `PhysicsWorld.cpp`:

```cpp
        void PhysicsWorld::PushContactBegin(const Contact& c)
        {
            if (!m_eventsEnabled) return;
            m_contactBeginEvents.push_back(ContactBeginEvent{ c.a, c.b,
                BodyHandle{ c.bodyA, c.genA }, BodyHandle{ c.bodyB, c.genB } });
        }

        void PhysicsWorld::PushContactEnd(const Contact& c)
        {
            if (!m_eventsEnabled) return;
            // Into the CURRENT end buffer (Box2D world.c:668 / contact.c:364): a step
            // writes it, then flips at its end; a destroy between steps writes the
            // buffer the NEXT step will flip and deliver.
            m_contactEndEvents[m_endEventIndex].push_back(ContactEndEvent{ c.a, c.b,
                BodyHandle{ c.bodyA, c.genA }, BodyHandle{ c.bodyB, c.genB } });
        }

        ContactEvents PhysicsWorld::GetContactEvents() const noexcept
        {
            return ContactEvents{ m_contactBeginEvents,
                                  m_contactEndEvents[1u - m_endEventIndex],
                                  m_contactHitEvents };
        }
```
At the very start of `StepImpl` (before stage 1), clear `m_contactBeginEvents` and `m_contactHitEvents` (Box2D `world.c:710-712`). Add a new **stage 6** at the end of `StepImpl`. Leave the legacy stage-6 lines in place until M7; put the new block after them:

```cpp
            // ---- stage 6b: event arrays (spec 2026-10-08 s6) -----------------
            // Sort for determinism (s6.3), then flip the end buffers exactly as
            // Box2D world.c:807-810: the buffer this step wrote becomes readable,
            // the other is cleared for the next step and for destroys before it.
            {
                const auto pairLess = [](const auto& l, const auto& r) noexcept
                {
                    if (l.a != r.a) return FixtureLess(l.a, r.a);
                    return FixtureLess(l.b, r.b);
                };
                std::sort(m_contactBeginEvents.begin(), m_contactBeginEvents.end(), pairLess);
                std::sort(m_contactEndEvents[m_endEventIndex].begin(), m_contactEndEvents[m_endEventIndex].end(), pairLess);
                m_endEventIndex = 1u - m_endEventIndex;
                m_contactEndEvents[m_endEventIndex].clear();
            }
```
`SetEventsEnabled(bool on)` becomes `m_eventsEnabled = on;`. Keep its legacy `Disarm`/`Rearm` calls until M7 removes the `ContactManager`, but make sure the new arrays depend only on `m_eventsEnabled`.

- [ ] **Step 6: Run the tests.** `Manifold2DTests.exe "[physics][events]"`: all pass. Full suite in Debug and Release: M0 counts plus the new cases, and `"[determinism]"`, `"[xplat]"`, `"[solvermt]"` unchanged (the island predicate is identical).
- [ ] **Step 7: Commit.** `feat(events): contact begin/end arrays from touching transitions of every solver contact, sorted, with Box2D's double-buffered ends and the world gate`

### Task M3: Destroy-time ends

**Files:**
- Modify: `src/Physics/ConstraintGraph.cpp:1511-1516` (`ReleaseAndDestroyContact`)
- Test: `tests/PhysicsContactEventsTest.cpp`

**Interfaces:**
- Consumes: M2's `PushContactEnd`.

- [ ] **Step 1: Write the failing tests.**

```cpp
TEST_CASE("Removing a touching body ends the contact on the next step, once", "[physics][events]")
{
    PhysicsWorld w{ WorldDef{} };
    AddGround(w);
    const BodyHandle b = AddBox(w, Real(0), Real(-2));
    EventLog settle; settle.StepAndCollect(w, 120);
    REQUIRE(settle.begin.size() == 1);
    w.RemoveBody(b);
    EventLog log;
    log.StepAndCollect(w, 1);
    REQUIRE(log.end.size() == 1);
    CHECK(log.end[0].bodyA == b);           // the OLD generation (Contact::genA), not the bumped one
    log.StepAndCollect(w, 1);
    CHECK(log.end.size() == 1);             // delivered once
}

TEST_CASE("DropFixture and a filter change end a touching contact", "[physics][events]")
{
    PhysicsWorld w{ WorldDef{} };
    AddGround(w);
    const BodyHandle b1 = AddBox(w, Real(-3), Real(-2));
    const BodyHandle b2 = AddBox(w, Real(3), Real(-2));
    EventLog settle; settle.StepAndCollect(w, 120);
    REQUIRE(settle.begin.size() == 2);
    // Keep b1 a body: add a second fixture first, then drop fixture 0.
    FixtureDef extra; extra.shape = MakeCircle(Real(0.1)); extra.localPos = Vec2(Real(0), Real(-3));
    w.AddFixture(b1, extra);
    w.DropFixture(w.GetBodyFixture(b1, 0));
    w.SetBodyFilter(b2, 2u, 0u);            // collides with nothing now
    EventLog log; log.StepAndCollect(w, 1);
    CHECK(log.end.size() == 2);
}

TEST_CASE("A contact that separates by fat box while touching still ends", "[physics][events]")
{
    PhysicsWorld w{ WorldDef{} };
    AddGround(w);
    const BodyHandle b = AddBox(w, Real(0), Real(-2));
    EventLog settle; settle.StepAndCollect(w, 120);
    w.SetPosition(b, Vec2(Real(0), Real(-50)));   // far beyond the fat AABB in one move
    EventLog log; log.StepAndCollect(w, 1);
    CHECK(log.end.size() == 1);
}
```
Note: if `GetBodyFixture(b1, 0)` after `AddFixture` is not the original fixture (the order is swap-remove on drop, not on add), keep the handle in a local *before* calling `AddFixture`.

- [ ] **Step 2: Run them to see them fail** (no End on destroy).
- [ ] **Step 3: Emit the End on destroy.** Replace `ReleaseAndDestroyContact`'s body with:

```cpp
        void ConstraintGraph::ReleaseAndDestroyContact(PhysicsWorld& w, std::uint32_t id, const Contact& c) noexcept
        {
            // Destroy-time End (Box2D contact.c:354-364): a touching contact that
            // reports, destroyed for ANY reason (RemoveBody, DropFixture,
            // SetBodyFilter, fat-box separation), ends here. Read c before the pool
            // frees the slot -- the frozen order below still holds.
            if (c.touching && (c.eventFlags & kEvContact) != 0u)
            {
                w.PushContactEnd(c);
            }
            w.m_islandMgr.DetachContactAdjacency(w, id, c); // reads c before the pool frees the slot
            ReleaseContactColor(w, id); // free the color while c still holds it
            m_contactPool.Destroy(id);
        }
```
`PushContactEnd` is not `noexcept`-safe if `push_back` throws. Either make `ReleaseAndDestroyContact` drop its `noexcept` (check its header declaration and every override), or wrap the push in a `try { } catch (...) { }` with a `MOSAIC_LOG_WARN`. Prefer dropping `noexcept`, and say which you chose in the report.

- [ ] **Step 4: Run the tests and the full suite.** All pass; determinism suites unchanged.
- [ ] **Step 5: Commit.** `feat(events): destroying a touching contact ends it (Box2D contact.c:354), delivered with the next step through the double-buffered end array`

### Task M4: Hit events

**Files:**
- Modify: `src/Physics/PhysicsWorld.cpp` (stage 6b), `src/Physics/ConstraintGraph.cpp` + header (a pool accessor)
- Create: `tests/PhysicsHitEventsTest.cpp`

**Interfaces:**
- Consumes: `ContactConstraint::sourceContactId`, `ContactConstraintPoint::relativeVelocity` (`Solver.hpp:88`), the post-writeback `ManifoldPoint::normalImpulse`, `kEvHit`.
- Produces: `const Contact& ConstraintGraph::PoolContact(std::uint32_t id) const` (returns `m_contactPool.Get(id)`; asserts the id is alive).

- [ ] **Step 1: Write the failing tests.**

```cpp
// PhysicsHitEventsTest.cpp
// [physics][events]: hit events (spec s6.1, Box2D solver.c:1758-1814; amendment A4).
#include <cmath>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "PhysicsEventTestHelpers.hpp"

using namespace EventTest;

TEST_CASE("A dropped box reports one hit with the free-fall approach speed", "[physics][events]")
{
    PhysicsWorld w{ WorldDef{} };
    AddGround(w);
    // Bottom face 2 m above the ground: v = sqrt(2 g h) = sqrt(40) ~ 6.32 m/s.
    AddBox(w, Real(0), Real(-2.5), true, /*hit*/ true);
    EventLog log;
    log.StepAndCollect(w, 90);
    REQUIRE(log.hit.size() >= 1);
    const ContactHitEvent& h = log.hit.front();
    CHECK(h.approachSpeed == Catch::Approx(std::sqrt(40.0)).margin(0.35));  // one step of g*dt slack
    CHECK(h.normal.y > Real(0.99));          // A (box) -> B (ground) is +y in a +Y-down world (A8)
    CHECK(h.point.y == Catch::Approx(0.0).margin(0.05));
}

TEST_CASE("No hit below the threshold, and none without the opt-in", "[physics][events]")
{
    PhysicsWorld w{ WorldDef{} };
    w.SetHitEventThreshold(Real(50));        // above any speed here
    AddGround(w);
    AddBox(w, Real(-2), Real(-2.5), true, true);
    AddBox(w, Real(2), Real(-2.5), true, false);   // never opted in
    EventLog log; log.StepAndCollect(w, 90); // a 2 m drop lands at ~step 38 (t = sqrt(2h/g) = 0.63 s)
    w.SetHitEventThreshold(Real(1));
    log.StepAndCollect(w, 60);               // both now resting: no approach speed above 1 m/s
    // The first box landed while the threshold was 50; the second never opted in.
    CHECK(log.hit.empty());
}

TEST_CASE("A resting box produces no hits", "[physics][events]")
{
    PhysicsWorld w{ WorldDef{} };
    AddGround(w);
    AddBox(w, Real(0), Real(-0.5), true, true);
    EventLog settle; settle.StepAndCollect(w, 120);
    EventLog log; log.StepAndCollect(w, 60);
    CHECK(log.hit.empty());
}
```

- [ ] **Step 2: Run them to see them fail.**
- [ ] **Step 3: Produce hits in stage 6b**, before the sort:

```cpp
            // Hits (Box2D solver.c:1758-1814): per solver contact that opted in, the
            // point with the largest approach speed above the threshold among points
            // that took normal impulse. Manifold2D sources: approach speed is
            // -ContactConstraintPoint::relativeVelocity (prepare-time, SoftStep.cpp:252-274);
            // the impulse test is the post-solve ManifoldPoint::normalImpulse > 0 (no
            // totalNormalImpulse exists here -- spec amendment A4). Constraint point p
            // was emitted from manifold point p (ConstraintGraph.cpp:1213-1231) and
            // written back by index (WritebackImpulses), so p indexes both.
            if (m_eventsEnabled)
            {
                for (const ContactConstraint& cc : m_contactConstraints)
                {
                    if (cc.sourceContactId == ContactConstraint::kNoContact) continue;   // tile span
                    const Contact& c = m_graph.PoolContact(cc.sourceContactId);
                    if ((c.eventFlags & kEvHit) == 0u) continue;
                    Real best = m_hitEventThreshold;
                    int  bestP = -1;
                    for (int p = 0; p < cc.pointCount; ++p)
                    {
                        const Real approach = -cc.points[p].relativeVelocity;
                        if (approach > best && c.manifold.points[p].normalImpulse > Real(0))
                        {
                            best = approach;
                            bestP = p;
                        }
                    }
                    if (bestP < 0) continue;
                    ContactHitEvent e;
                    e.a = c.a; e.b = c.b;
                    e.bodyA = BodyHandle{ c.bodyA, c.genA };
                    e.bodyB = BodyHandle{ c.bodyB, c.genB };
                    e.point = c.manifold.points[bestP].point;
                    e.normal = Vec2(-c.manifold.normal.x, -c.manifold.normal.y);   // B->A stored; A->B reported (A8)
                    e.approachSpeed = best;
                    m_contactHitEvents.push_back(e);
                }
            }
```
Sort `m_contactHitEvents` with the same `pairLess` as the begin array. Implement `ConstraintGraph::PoolContact`. Check that `c.manifold` really is the manifold `EmitContactConstraints` reads for a pool contact (`ConstraintGraph.cpp` around `:1150-1175`, where `m` is bound). If it is a copy taken at a different time, STOP and report.

- [ ] **Step 4: Run the tests and the full suite** (Debug + Release). All pass; determinism unchanged.
- [ ] **Step 5: Commit.** `feat(events): hit events from the solved constraints -- largest approach speed above the threshold among impulse-bearing points, normal A->B (Box2D solver.c:1758; totalNormalImpulse deviation recorded)`

### Task M5: The sensor pass

**Files:**
- Modify: `include/Manifold2D/Physics/PhysicsWorld.hpp`, `src/Physics/PhysicsWorld.cpp`
- Create: `tests/PhysicsSensorEventsTest.cpp`

**Interfaces:**
- Produces:
  - `[[nodiscard]] SensorEvents PhysicsWorld::GetSensorEvents() const noexcept`;
  - private `void RunSensorPass()`;
  - per-fixture-slot state `std::vector<SensorState> m_sensorState`, where `struct SensorState { std::uint32_t gen = 0; BodyHandle body{}; std::vector<SensorOverlap> overlaps; }` and `struct SensorOverlap { FixtureHandle visitor; BodyHandle visitorBody; }`;
  - buffers `m_sensorBeginEvents` and `m_sensorEndEvents[2]` (the latter shares `m_endEventIndex`);
  - reused scratch members `m_sensorSlotScratch` (`std::vector<std::uint32_t>`), `m_sensorVisitedScratch` (`std::vector<std::uint8_t>`) and `m_sensorBodyScratch` (`std::vector<BodyHandle>`).

- [ ] **Step 1: Write the failing tests.**

```cpp
// PhysicsSensorEventsTest.cpp
// [physics][events]: the end-of-step sensor pass (spec s6.1; Box2D 3.1.1 sensor.c,
// with the sensor-vs-sensor exclusion as amendment A1).
#include <catch2/catch_test_macros.hpp>
#include "PhysicsEventTestHelpers.hpp"

using namespace EventTest;

namespace
{
    BodyHandle AddStaticSensor(PhysicsWorld& w, Real x, Real y)
    {
        BodyDef d; d.type = BodyType::Static; d.position = Vec2(x, y);
        d.shape = MakeAabb(Real(1), Real(1)); d.isSensor = true; d.sensorEvents = true;
        return w.AddBody(d);
    }
}

TEST_CASE("A static sensor sees a falling box enter and leave", "[physics][events]")
{
    PhysicsWorld w{ WorldDef{} };
    const BodyHandle s = AddStaticSensor(w, Real(0), Real(-6));   // a zone in mid-air
    const BodyHandle b = AddBox(w, Real(0), Real(-10));
    EventLog log; log.StepAndCollect(w, 120);
    REQUIRE(log.sensorBegin.size() == 1);
    REQUIRE(log.sensorEnd.size() == 1);
    CHECK(log.sensorBegin[0].sensorBody == s);
    CHECK(log.sensorBegin[0].visitorBody == b);
}

TEST_CASE("A dynamic sensor detects a static fixture; sensors never detect sensors", "[physics][events]")
{
    PhysicsWorld w{ WorldDef{} };
    AddGround(w);                                                   // static visitor (y 0..1), sensorEvents on
    BodyDef d; d.type = BodyType::Dynamic; d.position = Vec2(Real(0), Real(-2));
    d.shape = MakeAabb(Real(0.5), Real(0.5)); d.isSensor = true; d.sensorEvents = true;
    w.AddBody(d);                                                   // falls THROUGH the ground (no response)
    AddStaticSensor(w, Real(0), Real(4));                           // another sensor on its path (y 3..5), clear of the ground
    EventLog log; log.StepAndCollect(w, 120);                       // ~20 m of fall: passes both
    REQUIRE(log.sensorBegin.size() == 1);                           // the ground only; never sensor-vs-sensor (A1)
}

TEST_CASE("Same-body, filtered and opted-out visitors are skipped", "[physics][events]")
{
    PhysicsWorld w{ WorldDef{} };
    BodyDef d; d.type = BodyType::Dynamic; d.position = Vec2(Real(0), Real(-2));
    d.shape = MakeAabb(Real(1), Real(1)); d.isSensor = true; d.sensorEvents = true;
    const BodyHandle s = w.AddBody(d);
    FixtureDef solid; solid.shape = MakeCircle(Real(0.2)); solid.sensorEvents = true;
    w.AddFixture(s, solid);                                         // same body: never a visitor (sensor.c:72)
    AddBox(w, Real(0), Real(-2), true, false, /*sensorEvents*/ false);   // opted out (sensor.c:66)
    const BodyHandle f = AddBox(w, Real(0.5), Real(-2));
    w.SetBodyFilter(f, 2u, 0u);                                     // filtered out (sensor.c:77)
    EventLog log; log.StepAndCollect(w, 1);
    CHECK(log.sensorBegin.empty());
}

TEST_CASE("Turning a sensor's flag off ends its overlaps on the next step", "[physics][events]")
{
    PhysicsWorld w{ WorldDef{} };
    const BodyHandle s = AddStaticSensor(w, Real(0), Real(0));
    AddBox(w, Real(0), Real(0));
    EventLog log; log.StepAndCollect(w, 1);
    REQUIRE(log.sensorBegin.size() == 1);
    w.SetFixtureEvents(w.GetBodyFixture(s, 0), false, false, false);
    log.StepAndCollect(w, 1);
    CHECK(log.sensorEnd.size() == 1);                               // Box2D sensor.c:158-165
}

TEST_CASE("a recycled visitor slot ends the old overlap and begins the new", "[physics][events]")
{
    PhysicsWorld w{ WorldDef{} };
    AddStaticSensor(w, Real(0), Real(0));
    const BodyHandle old = AddBox(w, Real(0), Real(0));
    EventLog log; log.StepAndCollect(w, 1);
    REQUIRE(log.sensorBegin.size() == 1);
    w.RemoveBody(old);
    const BodyHandle neu = AddBox(w, Real(0), Real(0));             // LIFO recycle: same index, new generation
    REQUIRE(neu.index == old.index);
    log.StepAndCollect(w, 1);
    REQUIRE(log.sensorEnd.size() == 1);
    CHECK(log.sensorEnd[0].visitorBody == old);
    REQUIRE(log.sensorBegin.size() == 2);
    CHECK(log.sensorBegin[1].visitorBody == neu);
}

TEST_CASE("Removing a sensor ends its overlaps", "[physics][events]")
{
    PhysicsWorld w{ WorldDef{} };
    const BodyHandle s = AddStaticSensor(w, Real(0), Real(0));
    AddBox(w, Real(0), Real(0));
    EventLog log; log.StepAndCollect(w, 1);
    w.RemoveBody(s);
    log.StepAndCollect(w, 1);
    CHECK(log.sensorEnd.size() == 1);
}
```
If `REQUIRE(neu.index == old.index)` fails because the body free-list is not LIFO, make the test find the recycled slot by adding bodies until the index matches. Never weaken the End/Begin assertions.

- [ ] **Step 2: Run them to see them fail.**
- [ ] **Step 3: Implement `RunSensorPass`**, called in stage 6b before the sort and flip:

```cpp
        // End-of-step sensor pass (Box2D 3.1.1 sensor.c b2OverlapSensors, spec s6.1):
        // every live sensor fixture with sensorEvents tests fixtures on bodies of
        // EVERY type (sensor.c:179-181 queries all trees). Skip: same body (:72),
        // filter (:77), visitor sensorEvents off (:66), visitor is a sensor
        // (amendment A1 -- not in 3.1.1). Overlap = a narrowphase point with
        // separation > 0 (margin 0, Collide). Overlaps are kept sorted per sensor and
        // diffed against last pass: new -> Begin, missing -> End. A dead or recycled
        // sensor slot (gen mismatch) ends everything it held. Ascending fixture-slot
        // order + sorted visitors = deterministic arrays.
        void PhysicsWorld::RunSensorPass()
        {
            // LIVENESS: a freed fixture slot keeps a NON-zero generation (RemoveBody
            // and DropFixture bump m_fxGen and push the slot on m_fxFree), so
            // m_fxGen != 0 is NOT a live test. The authoritative live set is each
            // alive body's m_bodyFixtures list; collect live sensor slots from it,
            // ascending, and treat every other slot holding overlaps as dead.
            if (m_sensorState.size() < m_fxCount) m_sensorState.resize(m_fxCount);
            std::vector<std::uint32_t>& liveSensors = m_sensorSlotScratch;   // member scratch, reused
            std::vector<std::uint8_t>&  visited     = m_sensorVisitedScratch;
            std::vector<BodyHandle>&    candidates  = m_sensorBodyScratch;
            liveSensors.clear();
            visited.assign(m_fxCount, std::uint8_t(0));
            for (std::uint32_t b = 0; b < m_count; ++b)
            {
                if (m_alive[b] == 0) continue;
                for (const std::uint32_t fi : m_bodyFixtures[b])
                    if (m_fxSensor[fi] != 0u) liveSensors.push_back(fi);
            }
            std::sort(liveSensors.begin(), liveSensors.end());
            std::vector<SensorOverlap> now;
            for (const std::uint32_t fi : liveSensors)
            {
                visited[fi] = 1;
                SensorState& st = m_sensorState[fi];
                if (st.gen != m_fxGen[fi] && !st.overlaps.empty())       // slot recycled into a new sensor
                {
                    for (const SensorOverlap& o : st.overlaps)
                        PushSensorEnd(FixtureHandle{ fi, st.gen }, st.body, o);
                    st.overlaps.clear();
                }
                const std::uint32_t sb = m_fxBody[fi];
                st.gen  = m_fxGen[fi];
                st.body = HandleOf(sb);
                if (m_fxSensorEvents[fi] == 0u)                          // flag off: end what it held (sensor.c:158)
                {
                    for (const SensorOverlap& o : st.overlaps)
                        PushSensorEnd(FixtureHandle{ fi, st.gen }, st.body, o);
                    st.overlaps.clear();
                    continue;
                }
                now.clear();
                QueryAABB(FixtureAabb(fi), candidates);                   // index-ordered, all body types
                for (const BodyHandle bh : candidates)
                {
                    if (bh.index == sb) continue;                         // same body (sensor.c:72)
                    for (const std::uint32_t vf : m_bodyFixtures[bh.index])   // live fixtures only
                    {
                        if (m_fxSensor[vf] != 0u || m_fxSensorEvents[vf] == 0u) continue;
                        if (!FixturesCollide(fi, vf)) continue;           // filter (sensor.c:77)
                        if (!FixturesOverlapExact(fi, vf)) continue;
                        now.push_back(SensorOverlap{ FixtureHandle{ vf, m_fxGen[vf] }, bh });
                    }
                }
                std::sort(now.begin(), now.end(), [](const SensorOverlap& l, const SensorOverlap& r) noexcept
                          { return FixtureLess(l.visitor, r.visitor); });
                // Diff two sorted lists (sensor.c:270-340). A handle differs if index OR generation does.
                std::size_t i = 0, j = 0;
                while (i < st.overlaps.size() || j < now.size())
                {
                    if (j == now.size() || (i < st.overlaps.size() && FixtureLess(st.overlaps[i].visitor, now[j].visitor)))
                        PushSensorEnd(FixtureHandle{ fi, st.gen }, st.body, st.overlaps[i++]);
                    else if (i == st.overlaps.size() || FixtureLess(now[j].visitor, st.overlaps[i].visitor))
                        PushSensorBegin(FixtureHandle{ fi, st.gen }, st.body, now[j++]);
                    else { ++i; ++j; }
                }
                st.overlaps.swap(now);
            }
            // Dead sensors (body removed, fixture dropped): their slot was not in
            // the live set but still holds overlaps -> End each, with the OLD handles.
            for (std::uint32_t fi = 0; fi < m_fxCount; ++fi)
            {
                if (visited[fi] != 0u || m_sensorState[fi].overlaps.empty()) continue;
                SensorState& st = m_sensorState[fi];
                for (const SensorOverlap& o : st.overlaps)
                    PushSensorEnd(FixtureHandle{ fi, st.gen }, st.body, o);
                st.overlaps.clear();
            }
        }
```
Confirm the body-slot liveness array and count names (`m_alive`, `m_count`) against `PhysicsWorld.hpp`; the report cites `m_alive[idx] = 0` in `RemoveBody` and `w.m_count` in `ConstraintGraph.cpp:1149`.
Also add:
- `FixturesOverlapExact(fi, vf)`: compose both fixture world transforms with the same `ComposeFixtureXf` that `DebugCollide` uses (`PhysicsWorld.hpp:930-945`), call `Collide(shapeA, xfA, shapeB, xfB, /*specMargin*/ 0, nullptr)`, and return true if any point has `separation > 0`. This is the same exact-overlap rule the legacy events used (`ConstraintGraph.cpp:1416-1426`).
- `PushSensorBegin` / `PushSensorEnd`: return early when `!m_eventsEnabled`. Otherwise build the event from (sensor handle, sensor body, overlap) and push to `m_sensorBeginEvents` / `m_sensorEndEvents[m_endEventIndex]`.
- `GetSensorEvents()` returns `{ m_sensorBeginEvents, m_sensorEndEvents[1u - m_endEventIndex] }`.
- Clear `m_sensorBeginEvents` at the start of `StepImpl`. Sort both sensor arrays by (sensor, visitor). Clear `m_sensorEndEvents[m_endEventIndex]` in the flip block.

The gate drops events, but the overlap state still updates, so re-enabling causes no burst (matching M2).

- [ ] **Step 4: Run the tests and the full suite** (Debug + Release); determinism unchanged (the pass writes no simulation state).
- [ ] **Step 5: Commit.** `feat(events): an end-of-step sensor pass over every body type with sorted overlap diffs, generation-aware ends for dead or recycled slots, and the sensor-vs-sensor exclusion (Box2D 3.1.1 sensor.c; A1 deviation recorded)`

### Task M6: GetBodyContacts and the pool-backed ForEachContact

**Files:**
- Modify: `include/Manifold2D/Physics/PhysicsWorld.hpp`, `src/Physics/PhysicsWorld.cpp` (`ForEachContact` definition `:2844-2848`), `ConstraintGraph` (a pool `ForEach` accessor)
- Create: `tests/PhysicsBodyContactsTest.cpp`

**Interfaces:**
- Produces: `void PhysicsWorld::GetBodyContacts(BodyHandle h, std::vector<BodyContact>& out) const`, and `void ConstraintGraph::ForEachPoolContact(Mosaic::FunctionRef<void(std::uint32_t, const Contact&)> fn) const` (forwards to `m_contactPool.ForEach`). `ForEachContact` keeps its signature `Mosaic::FunctionRef<void(std::uint32_t a, std::uint32_t b)>`.

- [ ] **Step 1: Write the failing tests.**

```cpp
// PhysicsBodyContactsTest.cpp
// [physics][events]: GetBodyContacts (b2Body_GetContactData) and the pool-backed
// ForEachContact (spec s6.4, s6.5).
#include <catch2/catch_test_macros.hpp>
#include "PhysicsEventTestHelpers.hpp"

using namespace EventTest;

TEST_CASE("GetBodyContacts lists a sleeping box's ground contact, normal outward", "[physics][events]")
{
    PhysicsWorld w{ WorldDef{} };
    const BodyHandle g = AddGround(w);
    const BodyHandle b = AddBox(w, Real(0), Real(-0.5));
    EventLog settle; settle.StepAndCollect(w, 300);
    REQUIRE_FALSE(w.IsAwake(b));                    // asleep: its contact is out of the solver feed
    std::vector<BodyContact> out;
    w.GetBodyContacts(b, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].selfBody == b);
    CHECK(out[0].otherBody == g);
    CHECK(out[0].normal.y > Real(0.99));            // from the box down to the ground (+Y down)
    w.GetBodyContacts(g, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].normal.y < Real(-0.99));           // from the ground up to the box
    w.GetBodyContacts(BodyHandle{}, out);
    CHECK(out.empty());                              // invalid handle -> cleared, no crash
}

TEST_CASE("ForEachContact visits a dynamic-vs-static touching pair", "[physics][events]")
{
    PhysicsWorld w{ WorldDef{} };
    AddGround(w);
    AddBox(w, Real(0), Real(-0.5));
    EventLog settle; settle.StepAndCollect(w, 30);
    int visits = 0;
    w.ForEachContact([&](std::uint32_t, std::uint32_t) { ++visits; });
    CHECK(visits == 1);                              // the old begun-pair version skipped dyn-static
}
```

- [ ] **Step 2: Run them to see them fail.**
- [ ] **Step 3: Implement.**

```cpp
        void PhysicsWorld::GetBodyContacts(BodyHandle h, std::vector<BodyContact>& out) const
        {
            out.clear();
            if (!IsValid(h)) return;
            // Touching solver contacts of this body, ascending pool id. Sleeping
            // bodies keep their pool contacts (ConstraintGraph.cpp:574-578), so they
            // are listed too. Stored normal points B -> A (Manifold.hpp:33).
            m_graph.ForEachPoolContact([&](std::uint32_t, const Contact& c)
            {
                if (!c.solverRelevant || !c.bIsBody || !c.touching) return;
                const Vec2 n = c.manifold.normal;
                if (c.bodyA == h.index && c.genA == h.generation)
                    out.push_back(BodyContact{ c.a, c.b, h, BodyHandle{ c.bodyB, c.genB },
                                               Vec2(-n.x, -n.y), c.manifold.pointCount });
                else if (c.bodyB == h.index && c.genB == h.generation)
                    out.push_back(BodyContact{ c.b, c.a, h, BodyHandle{ c.bodyA, c.genA },
                                               n, c.manifold.pointCount });
            });
        }

        void PhysicsWorld::ForEachContact(Mosaic::FunctionRef<void(std::uint32_t a, std::uint32_t b)> fn) const
        {
            // Every touching body-to-body pool contact, ascending id (spec s6.5).
            // Replaces the ContactManager begun-pair walk; includes dyn-static now.
            m_graph.ForEachPoolContact([&](std::uint32_t, const Contact& c)
            {
                if (c.bIsBody && c.touching && c.bodyB != kInvalidSlot) fn(c.bodyA, c.bodyB);
            });
        }
```
Update `ForEachContact`'s header comment to match.

- [ ] **Step 4: Run the tests and the full suite** (Debug + Release). Tests that pinned the old `ForEachContact` (grep `ForEachContact(` in `tests/`) must be reviewed. If one asserted the old exclusion, change its expectation and name the test in the report.
- [ ] **Step 5: Commit.** `feat(events): GetBodyContacts (b2Body_GetContactData, sleepers included) and ForEachContact backed by the contact pool`

### Task M7: Retire the legacy ContactManager

**Files:**
- Delete: `include/Manifold2D/Physics/ContactManager.hpp`, `src/Physics/ContactManager.cpp`
- Modify: `PhysicsWorld.hpp/.cpp` (remove `m_contacts`, `OnContact`, `SetBodyEvents`, `m_evtOn`, `EvtOn`, `BodyDef::eventsEnabled`, `m_touchedEventPairs`, the legacy stage-6 lines, `m_contacts.DropBody` in `RemoveBody`, the `Disarm`/`Rearm` calls in `SetEventsEnabled`), `Body.hpp:138-141` (remove `Body::SetEventsEnabled`), `ConstraintGraph` (remove `CollectTouchedEventPairs`), stale comments (`PhysicsWorld.hpp:30-36`, `:684-686`, `:1660`; `PhysicsWorld.cpp:7-8`)
- Modify tests: `tests/PhysicsWorldTest.cpp` (`:218`, `:271`, `:298`, `:317-364`), `tests/PhysicsCollisionFilterTest.cpp:70`, `tests/PhysicsQueryRotationTest.cpp:136`, `tests/PhysicsPhase1HarnessTest.cpp:162`, `tests/PhysicsRotationTest.cpp:204` (comment only)
- Modify: `README.md` / `docs/` wherever they describe `OnContact` (grep)

- [ ] **Step 1: Migrate each `OnContact` test before deleting anything.** For each listed test, read what property it pins, then:
  - It used the listener to detect that two bodies **overlap** → assert with `ForEachContact` (touching) or `GetBodyContacts`, or with sensor events when a sensor is involved.
  - It pinned Begin/End **timing** → assert on `GetContactEvents()` through `EventLog`.
  - It pinned a removed feature (Stay events, the per-body gate, the Rearm burst) → delete that case and list it in the report with one line saying why (spec §6.5 removes the feature).
  - A kinematic-vs-static listener case → it now has **no contact event** (A9). Switch it to a sensor (`isSensor` + `sensorEvents` on both fixtures) if the test is about detection, or to `ForEachContact`.

  Never weaken an assertion silently. If a property can't be preserved, STOP and report.
- [ ] **Step 2: Delete the legacy code** listed under Files. `SetEventsEnabled(bool on)` keeps its signature and becomes the plain gate from M2. `EventsEnabled()` stays.
- [ ] **Step 3: Check that nothing references the old names:**

```bash
cd /d/dev/starworks/Manifold2D && grep -rn "ContactManager\|OnContact\|SetBodyEvents\|EvtOn\|eventsEnabled\|CollectTouchedEventPairs\|ForEachBegunPair" include src tests bindings
```
Expected: no hits (the `SetEventsEnabled` / `EventsEnabled` world gate is allowed).
- [ ] **Step 4: Full verification.** Regenerate (a file was deleted): run `scripts\generate_vs2022.bat`. Build Debug, Release and Dist. Run the full suite in each, plus `"[xplat]"`, `"[determinism]"` and `"[solvermt]"`. Expected: green. Counts = M0 + new event cases − deleted legacy cases, with each delta listed. Then write a determinism case into `PhysicsContactEventsTest.cpp`:

```cpp
TEST_CASE("Event arrays are byte-identical across runs", "[physics][events][determinism]")
{
    const auto run = []
    {
        PhysicsWorld w{ WorldDef{} };
        AddGround(w);
        for (int i = 0; i < 20; ++i) AddBox(w, Real(-5 + (i % 10)), Real(-2 - 1.1 * (i / 10)), true, true);
        std::vector<std::uint32_t> trace;
        for (int s = 0; s < 240; ++s)
        {
            w.Step(kStep);
            const ContactEvents c = w.GetContactEvents();
            for (const auto& e : c.begin) { trace.push_back(1); trace.push_back(e.a.index); trace.push_back(e.b.index); }
            for (const auto& e : c.end)   { trace.push_back(2); trace.push_back(e.a.index); trace.push_back(e.b.index); }
            for (const auto& e : c.hit)   { trace.push_back(3); trace.push_back(e.a.index); trace.push_back(std::bit_cast<std::uint32_t>(static_cast<float>(e.approachSpeed))); }
        }
        return trace;
    };
    CHECK(run() == run());
}
```
Add an MT variant that mirrors how `tests/SolverMtInvarianceTest.cpp` installs a parallel executor (`SetExecutor`), and assert the serial trace == the MT trace. Include `<bit>`.

- [ ] **Step 5: Commit.** `refactor(events)!: retire the Lua-era ContactManager listener -- OnContact, Stay events and the per-body gate go; the event arrays replace them; legacy tests migrated (spec s6.5)`
- [ ] **Step 6: Report for the merge.** The controller asks the user before merging `feat/events` into Manifold2D `main` (or pushing). Record the `feat/events` tip SHA as `M2D_EVENTS`.

---

## Phase A — Arcane (branch `feat/physics-events`)

### Task A1: sync-vendor.ps1, then re-vendor Mosaic + Manifold2D at M2D_BASE (the reviewed motion change)

**Files:**
- Create: `scripts/sync-vendor.ps1`
- Modify: `scripts/sync-astra.ps1` (becomes a shim)
- Modify (by script only): `ThirdParty/Mosaic/{include,src,LICENSE,VENDORED.txt}`, `ThirdParty/Manifold2D/{include,src,LICENSE,VENDORED.txt}`
- Modify: `ArcaneTests/data/trajectory/reference_player.json` (deliberate re-record)

**Interfaces:**
- Produces: `powershell -ExecutionPolicy Bypass -File scripts\sync-vendor.ps1 -Library <Astra|Manifold2D|Mosaic|All> [-Source <path>] [-Commit <sha>] [-DryRun]`. `-Commit` vendors a specific commit through a temporary `git worktree` of the source repo, so the working tree being synced need not be checked out at it.

- [ ] **Step 1: Branch.** `git -C D:\dev\starworks\Arcane switch -c feat/physics-events main`.
- [ ] **Step 2: Write `scripts/sync-vendor.ps1`.** Base it on `sync-astra.ps1` (same robocopy `/MIR` flags, rc ≥ 8 = failure, `VENDORED.txt` stamp format, DryRun semantics). Table-drive it:

```powershell
$libs = @{
  Astra      = @{ Source = 'D:\dev\starworks\Astra';      Dest = 'ThirdParty\Astra';      Dirs = @('include');        Files = @() }
  Manifold2D = @{ Source = 'D:\dev\starworks\Manifold2D'; Dest = 'ThirdParty\Manifold2D'; Dirs = @('include','src'); Files = @('LICENSE') }
  Mosaic     = @{ Source = 'D:\dev\starworks\Mosaic';     Dest = 'ThirdParty\Mosaic';     Dirs = @('include','src'); Files = @('LICENSE') }
}
```
Requirements:
- `-Library All` runs all three.
- It never copies a library's `premake5.lua`, `ThirdParty/`, `vendor/`, `tests/` or `docs/`.
- With `-Commit`, it runs `git -C <Source> worktree add --detach <tmp> <sha>`, copies from `<tmp>`, and removes the worktree in a `finally`.
- The stamp records source, commit, branch (or `detached`), subject, the synced time and the scope.
- **Drift warning:** after syncing, read `commit :` from `ThirdParty\Mosaic\VENDORED.txt`, from `<Manifold2D source>\ThirdParty\Mosaic\VENDORED.txt` and from `<Astra source>\vendor\Mosaic\VENDORED.txt`. `Write-Warning` names every library whose Mosaic commit differs from Arcane's. A missing stamp warns "unstamped".
- Line endings: copy bytes as-is (robocopy). Then run `git -C <repo> diff --stat` and fail loudly if more than 50% of the touched files differ only in line endings (`git diff --ignore-cr-at-eol --stat` empty for them). That is the CRLF fan-out guard (memory `reference_astra_sync_crlf_fanout`).

Make `sync-astra.ps1` a shim: keep its param block, then `& "$PSScriptRoot\sync-vendor.ps1" -Library Astra -Source $Source -DryRun:$DryRun; exit $LASTEXITCODE`.

- [ ] **Step 3: Prove the Astra path is equivalent without changing Astra.** `scripts\sync-vendor.ps1 -Library Astra -DryRun`. Expected: rc 0 ("already identical") if the vendored Astra matches the Astra checkout. If it reports changes, do **not** sync Astra in this plan. Record what differs (Astra syncs are their own decision).
- [ ] **Step 4: Vendor Mosaic (the reconciled `M_RECON`) and Manifold2D at `M2D_BASE`:**

```powershell
cd D:\dev\starworks\Arcane
powershell -ExecutionPolicy Bypass -File scripts\sync-vendor.ps1 -Library Mosaic -Commit <M_RECON>
powershell -ExecutionPolicy Bypass -File scripts\sync-vendor.ps1 -Library Manifold2D -Commit <M2D_BASE>
```
Expected: two stamps, and no drift warning for Manifold2D (V1 synced its copy). Check: `git diff --stat ThirdParty` shows Mosaic's reconciled files plus Manifold2D's ~15 files.
- [ ] **Step 5: Build both configs.** `GenerateProjects.bat` (Manifold2D added no `.cpp` at `M2D_BASE`, but Mosaic may have changed `src/`), then `Arcane.slnx` Debug and Release. Fix nothing in vendored code; report compile errors in Arcane's own code.
- [ ] **Step 6: Run the physics tests and the trajectory test** from the exe dir: `ArcaneTests.exe "[physics]"`, `"[physics2d]"`, `"[trajectory]"`. Expected: `[trajectory]` **FAILS** (motion changed: continuous collision, static softness, the polygon manifold). Anything else red is a regression; investigate before going on.
- [ ] **Step 7: Review, then re-record the trajectory.** Write `scratchpad/a1-trajectory-before-after.md`: dump the old and new Pill samples (x, y, vx, vy per frame from the old JSON and from a recording run), and note where they diverge and why (expected: the landing frames, from static softness and CCD). Present it to the controller. **Only after the controller accepts**, record:

```powershell
cd D:\dev\starworks\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests
$env:ARCANE_RECORD_TRAJECTORY = '1'; .\ArcaneTests.exe "[trajectory]"; Remove-Item Env:ARCANE_RECORD_TRAJECTORY
.\ArcaneTests.exe "[trajectory]"   # now passes
```
Then run the same test in Release: it must pass bit-identically against the Debug recording.
- [ ] **Step 8: Full gates.**
  1. ReferenceProject build (Debug), then `Arcane.slnx` Debug.
  2. Delete `imgui.ini` in both host exe dirs.
  3. Run `ArcaneTests "~[gpu]"` and `"[witness]~[shell]"`.
  4. Run `scripts\golden-gate.ps1 -Configuration Debug`. Physics is not in the boot scene, so expect 14/14; any moved golden goes to the controller (re-bless only on confirmed-correct motion, per `.claude/skills/arcane-verify/SKILL.md:49-65`).
  5. Repeat for Release.
- [ ] **Step 9: Commit** (two commits):
  - `build(vendor): one sync-vendor.ps1 for Astra, Manifold2D and Mosaic -- mirrored dirs, VENDORED stamps, a Mosaic drift warning, a CRLF fan-out guard; sync-astra.ps1 becomes a shim`
  - `build(vendor): Mosaic <M_RECON> and Manifold2D <M2D_BASE> -- continuous collision, stiffer static contacts, the clipped polygon manifold; trajectory fixture re-recorded after review` (with the before/after summary in the body).

### Task A2: Re-vendor Manifold2D at M2D_EVENTS; adapt the debug draw

**Files:**
- Modify (script): `ThirdParty/Manifold2D/**`
- Modify: `ArcaneClient/src/Arcane/Render/PhysicsDebugDraw.cpp:480-492` (comment only; the signature is unchanged)

**Interfaces:**
- Consumes: M7's `M2D_EVENTS`.

- [ ] **Step 1: Sync.** `scripts\sync-vendor.ps1 -Library Manifold2D -Commit <M2D_EVENTS>`. Expected: `Events.hpp` added; `ContactManager.{hpp,cpp}` deleted (`/MIR` purges orphans).
- [ ] **Step 2: Regenerate and build both configs** (a `.cpp` was removed: run `GenerateProjects.bat` first).
- [ ] **Step 3: Update the debug-draw comment** at `PhysicsDebugDraw.cpp:480-483`. It now visits every touching body-to-body contact (dynamic-vs-static included), not "begun pairs". The code is unchanged.
- [ ] **Step 4: Run tests.** `ArcaneTests "[physics]"`, `"[physics2d]"`, `"[trajectory]"` (**must pass bit-identically**: events are simulation-neutral) and the physics debug tests (`PhysicsDebugRichTest`, `SweepPhysicsDebugTest`). If a debug test counted the old pairs, update its expectation and name it in the report.
- [ ] **Step 5: Commit.** `build(vendor): Manifold2D <M2D_EVENTS> -- event arrays, the sensor pass, GetBodyContacts; the legacy ContactManager is gone (debug draw keeps ForEachContact, now pool-backed)`

### Task A3: Game-facing types, Fixture flags, body records, StepEvents, policy, ABI 55

**Files:**
- Create: `ArcaneCore/src/Arcane/Scene/PhysicsEvents2D.hpp`, `ArcaneCore/src/Arcane/Scene/PhysicsEvents2D.cpp`
- Modify: `ArcaneCore/src/Arcane/Scene/PhysicsComponents.hpp` (`Fixture` `:99-122`, reflect `:229-243`)
- Modify: `ArcaneCore/src/Arcane/Scene/PhysicsSystem.hpp` (`PhysicsResource` `:128-173`, `MakeFixtureDef` `:232`, PASS 1 `:324-378`, PASS 2 `:395-510`, PASS 3 `:564-569`)
- Modify: `ArcaneCore/src/Arcane/Scene/PhysicsQuerySettings.hpp` / `.cpp`
- Modify: `ArcaneCore/src/Arcane/Plugin/PluginABI.hpp:1131` (+ ledger), `ReferenceProject/ReferenceProject.arcproj:6`, `D:\dev\starworks\Aphelyon\Aphelyon.arcproj:6`
- Modify: `ArcaneTests/src/ArcaneSpellingGuardTest.cpp:34-50` (add the header; the test name's "15" becomes "16")
- Create: `ArcaneTests/src/PhysicsEvents2DTest.cpp`

**Interfaces:**
- Produces, in `PhysicsEvents2D.hpp` (namespace `Arcane`): `ContactSide2D`, `ContactBegin2D`, `ContactEnd2D`, `ContactHit2D`, `SensorBegin2D`, `SensorEnd2D`, `ContactPoint2D`, `PhysicsEvents2D` (spec §7.1, field names exact).
- Produces, on `PhysicsResource`:
  - `BodyRecord2D` and `std::unordered_map<std::uint64_t, BodyRecord2D> bodyRecords`;
  - the event buffers `stepEvents` and `frameEvents` (type `PhysicsEventBuffers2D`);
  - `ARC_CORE_API void RecordBody(Arcane::Entity, Guid, Phys::BodyHandle, std::vector<Phys::FixtureHandle>)`;
  - `ARC_CORE_API void RetireBody(Phys::BodyHandle)`;
  - `ARC_CORE_API void CaptureStep()`;
  - `ARC_CORE_API PhysicsEvents2D StepEvents() const`;
  - `ARC_CORE_API PhysicsEvents2D FrameEvents() const`;
  - `ARC_CORE_API void BeginFrame()`.
- Produces: `PhysicsEventSettings { float hitThreshold = 1.0f; }`, registered as `"physics.events"`.

- [ ] **Step 1: Write the failing tests** in `ArcaneTests/src/PhysicsEvents2DTest.cpp`. Use `PhysicsSystemTest.cpp`'s pattern (a bare registry, a `PhysicsResource` minted directly, `PhysicsSystem(kDt)` called N times). The world here is **+Y down** with `gravityY = 10`, matching `PhysicsSystemTest`.

```cpp
// PhysicsEvents2DTest.cpp -- [physics][events]: the Arcane event surface (spec
// 2026-10-08 s7). Bare registry + PhysicsSystem, +Y DOWN, g = 10 (the
// PhysicsSystemTest convention).
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <string>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsEvents2D.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/TransformSystems.hpp>
#include <Arcane/Serialization/SceneSerializer.hpp>
#include "Helpers/TestPaths.hpp"   // FindReferenceProjectDir; adjust to the real helper header name

namespace
{
    constexpr float kDt = 1.0f / 60.0f;

    struct World
    {
        std::shared_ptr<Astra::ComponentRegistry> components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{ components };
        World()
        {
            Arcane::RegisterSceneComponents(reg);
            Arcane::RegisterPhysicsComponents(reg);
            Manifold2D::Physics::WorldDef wd; wd.gravityY = 10.0f;
            reg.SetResource(Arcane::PhysicsResource{ std::make_unique<Manifold2D::Physics::PhysicsWorld>(wd), {} });
        }
        Astra::Entity Body(const char* name, Manifold2D::Physics::BodyType type, glm::vec2 pos,
                           std::vector<Arcane::Fixture> fixtures, glm::vec3 scale = glm::vec3(1.0f))
        {
            Astra::Entity e = reg.CreateEntity();
            Arcane::Identity id; id.id = Arcane::Guid::Generate(); id.name = name;
            reg.AddComponent<Arcane::Identity>(e, id);
            Arcane::Transform t; t.position = glm::vec3(pos, 0.0f); t.scale = scale;
            reg.AddComponent<Arcane::Transform>(e, t);
            reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});
            Arcane::RigidBody2D rb; rb.type = type;
            reg.AddComponent<Arcane::RigidBody2D>(e, rb);
            Arcane::Collider2D col; col.fixtures = std::move(fixtures);
            reg.AddComponent<Arcane::Collider2D>(e, col);
            reg.AddComponent<Arcane::PhysicsBodyRef>(e, Arcane::PhysicsBodyRef{});
            return e;
        }
        Arcane::PhysicsResource& Res() { return *reg.GetResource<Arcane::PhysicsResource>(); }
        Arcane::Guid GuidOf(Astra::Entity e) { return std::as_const(reg).GetComponent<Arcane::Identity>(e)->id; }
    };

    Arcane::Fixture Box(float hw, float hh)
    {
        Arcane::Fixture f; f.kind = Manifold2D::Physics::ShapeKind::Aabb; f.halfW = hw; f.halfH = hh; return f;
    }
}

TEST_CASE("Fixture event flags default contact+sensor on, hit off", "[physics][events]")
{
    Arcane::Fixture f;
    CHECK(f.contactEvents);
    CHECK(f.sensorEvents);
    CHECK_FALSE(f.hitEvents);
}

TEST_CASE("StepEvents reports a crate landing on static ground, by entity + GUID + fixture", "[physics][events]")
{
    World w;
    const Astra::Entity ground = w.Body("Ground", Manifold2D::Physics::BodyType::Static, { 0.0f, 0.5f }, { Box(10.0f, 0.5f) });
    const Astra::Entity crate  = w.Body("Crate",  Manifold2D::Physics::BodyType::Dynamic, { 0.0f, -2.0f }, { Box(0.5f, 0.5f) });
    Arcane::PhysicsSystem physics(kDt);
    int begins = 0;
    for (int i = 0; i < 120; ++i)
    {
        physics(w.reg);
        for (const Arcane::ContactBegin2D& e : w.Res().StepEvents().contactBegin)
        {
            ++begins;
            CHECK(e.a.entity == crate);           // A = the dynamic side
            CHECK(e.a.guid == w.GuidOf(crate));
            CHECK(e.a.fixture == 0u);
            CHECK(e.b.entity == ground);
            CHECK(e.b.guid == w.GuidOf(ground));
        }
    }
    CHECK(begins == 1);
}

TEST_CASE("fixture indices map through the auto-fixture and AddFixture paths", "[physics][events]")
{
    World w;
    Arcane::Fixture sensor = Box(2.0f, 2.0f); sensor.isSensor = true;    // fixture 0: the auto-fixture path
    Arcane::Fixture solid  = Box(0.3f, 0.3f); solid.localPos = { 0.0f, 5.0f };   // fixture 1: AddFixture
    const Astra::Entity zone = w.Body("Zone", Manifold2D::Physics::BodyType::Static, { 0.0f, -6.0f }, { sensor, solid });
    const Astra::Entity crate = w.Body("Crate", Manifold2D::Physics::BodyType::Dynamic, { 0.0f, -12.0f }, { Box(0.5f, 0.5f) });
    Arcane::PhysicsSystem physics(kDt);
    int sensorBegins = 0;
    for (int i = 0; i < 90; ++i)
    {
        physics(w.reg);
        for (const Arcane::SensorBegin2D& e : w.Res().StepEvents().sensorBegin)
        {
            ++sensorBegins;
            CHECK(e.sensor.entity == zone);
            CHECK(e.sensor.fixture == 0u);
            CHECK(e.visitor.entity == crate);
        }
    }
    CHECK(sensorBegins == 1);
}

TEST_CASE("Opting one fixture out silences the pair (both-fixtures rule)", "[physics][events]")
{
    World w;
    Arcane::Fixture quiet = Box(10.0f, 0.5f); quiet.contactEvents = false;
    w.Body("Ground", Manifold2D::Physics::BodyType::Static, { 0.0f, 0.5f }, { quiet });
    w.Body("Crate", Manifold2D::Physics::BodyType::Dynamic, { 0.0f, -2.0f }, { Box(0.5f, 0.5f) });   // default on
    Arcane::PhysicsSystem physics(kDt);
    int begins = 0;
    for (int i = 0; i < 120; ++i) { physics(w.reg); begins += static_cast<int>(w.Res().StepEvents().contactBegin.size()); }
    CHECK(begins == 0);
}

TEST_CASE("A destroyed entity's End still carries its GUID", "[physics][events]")
{
    World w;
    w.Body("Ground", Manifold2D::Physics::BodyType::Static, { 0.0f, 0.5f }, { Box(10.0f, 0.5f) });
    const Astra::Entity crate = w.Body("Crate", Manifold2D::Physics::BodyType::Dynamic, { 0.0f, -0.49f }, { Box(0.5f, 0.5f) });
    const Arcane::Guid guid = w.GuidOf(crate);
    Arcane::PhysicsSystem physics(kDt);
    for (int i = 0; i < 10; ++i) physics(w.reg);
    w.reg.DestroyEntity(crate);
    physics(w.reg);                                // PASS 1 removes + retires; the step delivers the End
    const auto ends = w.Res().StepEvents().contactEnd;
    REQUIRE(ends.size() == 1);
    CHECK(ends[0].a.guid == guid);
    CHECK_FALSE(w.reg.IsValid(ends[0].a.entity));
    physics(w.reg);
    CHECK(w.Res().bodyRecords.size() == 1);        // retired record erased after its delivery
}

TEST_CASE("a recycled body slot never resolves to the retired record", "[physics][events]")
{
    World w;
    w.Body("Ground", Manifold2D::Physics::BodyType::Static, { 0.0f, 0.5f }, { Box(10.0f, 0.5f) });
    const Astra::Entity first = w.Body("First", Manifold2D::Physics::BodyType::Dynamic, { 0.0f, -0.49f }, { Box(0.5f, 0.5f) });
    Arcane::PhysicsSystem physics(kDt);
    for (int i = 0; i < 5; ++i) physics(w.reg);
    const Arcane::Guid firstGuid = w.GuidOf(first);
    w.reg.DestroyEntity(first);
    const Astra::Entity second = w.Body("Second", Manifold2D::Physics::BodyType::Dynamic, { 0.0f, -0.49f }, { Box(0.5f, 0.5f) });
    physics(w.reg);                                // PASS 1 retires First, PASS 2 mints Second (same slot)
    bool sawFirstEnd = false, sawSecondBegin = false;
    for (const auto& e : w.Res().StepEvents().contactEnd)   if (e.a.guid == firstGuid) sawFirstEnd = true;
    for (const auto& e : w.Res().StepEvents().contactBegin) if (e.a.entity == second && e.a.guid == w.GuidOf(second)) sawSecondBegin = true;
    CHECK(sawFirstEnd);
    CHECK(sawSecondBegin);
}

TEST_CASE("physics.events.hitThreshold reaches the world", "[physics][events]")
{
    World w;
    Arcane::Fixture hitty = Box(0.5f, 0.5f); hitty.hitEvents = true;
    w.Body("Ground", Manifold2D::Physics::BodyType::Static, { 0.0f, 0.5f }, { Box(10.0f, 0.5f) });
    w.Body("Crate", Manifold2D::Physics::BodyType::Dynamic, { 0.0f, -2.5f }, { hitty });
    Arcane::PhysicsSystem physics(kDt);
    int hits = 0;
    for (int i = 0; i < 90; ++i) { physics(w.reg); hits += static_cast<int>(w.Res().StepEvents().contactHit.size()); }
    CHECK(hits >= 1);                              // the default 1 m/s threshold; a 6 m/s landing hits
}

TEST_CASE("PhysicsEvents2D.hpp includes no Manifold2D header", "[physics][events][guard]")
{
    const auto path = Arcane::Test::FindReferenceProjectDir().parent_path() / "ArcaneCore/src/Arcane/Scene/PhysicsEvents2D.hpp";
    std::ifstream in(path);
    REQUIRE(in.good());
    std::string line;
    while (std::getline(in, line))
        if (line.rfind("#include", 0) == 0) CHECK(line.find("Manifold2D") == std::string::npos);
}

TEST_CASE("Fixture event flags round-trip through scene JSON; absent keys keep the defaults", "[physics][events][json]")
{
    nlohmann::json doc;
    {
        World w;
        Arcane::Fixture f = Box(0.5f, 0.5f); f.contactEvents = false; f.hitEvents = true;
        const Astra::Entity root = w.reg.CreateEntity();
        w.reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});
        Arcane::Collider2D col; col.fixtures.push_back(f);
        w.reg.AddComponent<Arcane::Collider2D>(root, col);
        w.reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{ root });
        doc = Arcane::Scene::SaveJson(w.reg);
    }
    auto& fx = doc["entities"][0]["components"]["Arcane::Collider2D"]["fixtures"][0];
    CHECK(fx["contactEvents"] == false);
    CHECK(fx["hitEvents"] == true);
    fx.erase("sensorEvents");                      // a pre-spec scene has no key
    World loaded;
    REQUIRE(Arcane::Scene::LoadJson(loaded.reg, doc));
    loaded.reg.CreateView<Arcane::Collider2D>().ForEach([&](Astra::Entity, Arcane::Collider2D& c)
    {
        REQUIRE(c.fixtures.size() == 1);
        CHECK_FALSE(c.fixtures[0].contactEvents);
        CHECK(c.fixtures[0].hitEvents);
        CHECK(c.fixtures[0].sensorEvents);         // default kept
    });
}
```
Confirm the JSON key path (`doc["entities"][0]["components"]["Arcane::Collider2D"]["fixtures"]`) against `SceneJsonTest.cpp:584-621` and adjust the path if the serializer nests differently. The assertions stay as written. Confirm the `FindReferenceProjectDir` header name with `grep -rn "FindReferenceProjectDir" ArcaneTests/src/Helpers`.

- [ ] **Step 2: Build and run the tests to see them fail** (compile errors first: the members don't exist).
- [ ] **Step 3: Write `PhysicsEvents2D.hpp`** (spec §7.1 verbatim, plus):

```cpp
#pragma once

// PhysicsEvents2D: the game-facing 2D physics events (spec 2026-10-08 s7). Pure
// data: NO Manifold2D include, so a game module reads it without the physics
// library (a [guard] test reads these #include lines). Read through
// PhysicsResource::StepEvents() / FrameEvents() (PhysicsSystem.hpp).
//
// `entity` is valid for the frame it is read in UNLESS that entity was destroyed
// after the step -- check registry.IsValid(entity) or use `guid`. Never store
// `entity` past the frame (GUID rule). `normal` points from a to b. `fixture` is
// the index into Collider2D::fixtures.

#include <cstdint>
#include <span>
#include <glm/vec2.hpp>
#include <Arcane/Ecs.hpp>
#include <Arcane/Guid.hpp>

namespace Arcane
{
    struct ContactSide2D  { Arcane::Entity entity = Arcane::Entity::Invalid(); Guid guid{}; std::uint32_t fixture = 0; };
    struct ContactBegin2D { ContactSide2D a, b; };
    struct ContactEnd2D   { ContactSide2D a, b; };
    struct ContactHit2D   { ContactSide2D a, b; glm::vec2 point{ 0.0f }; glm::vec2 normal{ 0.0f }; float approachSpeed = 0.0f; };
    struct SensorBegin2D  { ContactSide2D sensor, visitor; };
    struct SensorEnd2D    { ContactSide2D sensor, visitor; };
    struct ContactPoint2D { ContactSide2D self, other; glm::vec2 normal{ 0.0f }; std::uint32_t pointCount = 0; };

    struct PhysicsEvents2D
    {
        std::span<const ContactBegin2D> contactBegin;
        std::span<const ContactEnd2D>   contactEnd;
        std::span<const ContactHit2D>   contactHit;
        std::span<const SensorBegin2D>  sensorBegin;
        std::span<const SensorEnd2D>    sensorEnd;
    };
}
```
If `Arcane::Entity::Invalid()` is not spelled that way, use what `ReferencePlayerTrajectoryTest.cpp:77` uses (`Astra::Entity::Invalid()`) through the `Arcane::Entity` alias.

- [ ] **Step 4: Add the Fixture flags.** After `isSensor` in `Fixture`, add `bool contactEvents = true; bool sensorEvents = true; bool hitEvents = false;`. In the reflect block, add:

```cpp
        ARC_REFLECT_FIELD(Fixture, contactEvents)
            ARC_REFLECT_ATTR(Tooltip, "Report contact begin/end for this fixture. Both fixtures of a pair must allow it.")
        ARC_REFLECT_FIELD(Fixture, sensorEvents)
            ARC_REFLECT_ATTR(Tooltip, "Report sensor enter/exit, as the sensor or as the visitor. Both must allow it.")
        ARC_REFLECT_FIELD(Fixture, hitEvents)
            ARC_REFLECT_ATTR(Tooltip, "Report impacts faster than physics.events.hitThreshold. Either fixture suffices.")
```
In `MakeFixtureDef` (`PhysicsSystem.hpp:232`), add `fd.contactEvents = f.contactEvents; fd.sensorEvents = f.sensorEvents; fd.hitEvents = f.hitEvents;`. In PASS 2's `BodyDef` block, beside `def.isSensor = fx0.isSensor;`, add the same three from `fx0`.

- [ ] **Step 5: Add the settings type.** In `PhysicsQuerySettings.hpp`, after `PhysicsGroundSettings`:

```cpp
    // physics.events.* (spec 2026-10-08 s7.5): Live, read by PhysicsSystem at the
    // top of every pass. Deterministic: it changes which hits are reported.
    struct PhysicsEventSettings
    {
        float hitThreshold = 1.0f;   // m/s a contact point's approach speed must exceed to report a hit
    };

    ARC_REFLECT_TYPE(PhysicsEventSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "physics.events", SettingScope::Project, ApplyMode::Live, Audience::Game)
        ARC_REFLECT_FIELD(PhysicsEventSettings, hitThreshold)
            ARC_REFLECT_ATTR(Range, 0.0, 100.0) ARC_REFLECT_ATTR(Deterministic)
            ARC_REFLECT_ATTR(Tooltip, "Approach speed (m/s) an impact must exceed to report a hit event (Fixture::hitEvents).")
    ARC_END_REFLECT_TYPE()
```
In `PhysicsQuerySettings.cpp`, add `ARC_SETTINGS(Arcane::PhysicsEventSettings);`.

- [ ] **Step 6: Resource members and records.** In `PhysicsSystem.hpp`, include `<Arcane/Scene/PhysicsEvents2D.hpp>`, `<Arcane/Scene/PhysicsQuerySettings.hpp>` and `<Manifold2D/Physics/Events.hpp>`. Above `PhysicsResource`:

```cpp
    // One minted body (spec s7.3): who it is and its fixture handles in
    // Collider2D order. Keyed by the packed Phys handle (index << 32 | generation),
    // so a recycled slot (new generation) never resolves to a retired record.
    struct BodyRecord2D
    {
        Arcane::Entity                     entity = Arcane::Entity::Invalid();
        Guid                               guid{};
        std::vector<Phys::FixtureHandle>   fixtures;
        bool                               retired = false;   // removed; erased after the next capture
    };

    struct PhysicsEventBuffers2D
    {
        std::vector<ContactBegin2D> contactBegin;
        std::vector<ContactEnd2D>   contactEnd;
        std::vector<ContactHit2D>   contactHit;
        std::vector<SensorBegin2D>  sensorBegin;
        std::vector<SensorEnd2D>    sensorEnd;
        void Clear() noexcept { contactBegin.clear(); contactEnd.clear(); contactHit.clear(); sensorBegin.clear(); sensorEnd.clear(); }
        [[nodiscard]] PhysicsEvents2D View() const noexcept { return { contactBegin, contactEnd, contactHit, sensorBegin, sensorEnd }; }
    };

    [[nodiscard]] constexpr std::uint64_t PackBody(Phys::BodyHandle h) noexcept
    {
        return (static_cast<std::uint64_t>(h.index) << 32) | h.generation;
    }
```
Put those inside the existing `ARC_INTERNAL` fence only if the spelling guard flags `Phys::`. The guard's `PublicDeclarations` mode exempts fenced code, so fence the `Phys::`-typed declarations. Add to `PhysicsResource`, after `reconciled`:

```cpp
        // ---- 2D physics events (spec 2026-10-08 s7) ---------------------------
        std::unordered_map<std::uint64_t, BodyRecord2D> bodyRecords;
        PhysicsEventBuffers2D stepEvents;     // replaced at the end of every stepping pass
        PhysicsEventBuffers2D frameEvents;    // appended per step, cleared by BeginFrame (RunLoop hook)

        ARC_CORE_API void RecordBody(Arcane::Entity entity, Guid guid, Phys::BodyHandle handle,
                                     std::vector<Phys::FixtureHandle> fixtures);
        ARC_CORE_API void RetireBody(Phys::BodyHandle handle);
        // Translate the world's arrays for the step just taken, replace stepEvents,
        // append frameEvents, then erase retired records (their End has been read).
        ARC_CORE_API void CaptureStep();
        ARC_CORE_API PhysicsEvents2D StepEvents() const;    // the most recent physics step
        ARC_CORE_API PhysicsEvents2D FrameEvents() const;   // every step since this frame began
        ARC_CORE_API void BeginFrame();                     // clears frameEvents (RunLoop frame hook)
```

- [ ] **Step 7: Implement `PhysicsEvents2D.cpp`.**

```cpp
// PhysicsEvents2D.cpp -- PhysicsResource's event members (spec 2026-10-08 s7).
// Translation: Manifold2D handles -> {entity, GUID, fixture index} through the
// body records PhysicsSystem fills at mint. Order is the world's (already sorted
// upstream); an event naming a body with no record is dropped.
#include <Arcane/Scene/PhysicsSystem.hpp>

#include <algorithm>

namespace Arcane
{
    namespace
    {
        bool Side(const PhysicsResource& res, Phys::BodyHandle body, Phys::FixtureHandle fx, ContactSide2D& out)
        {
            const auto it = res.bodyRecords.find(PackBody(body));
            if (it == res.bodyRecords.end()) return false;
            const auto& fxs = it->second.fixtures;
            const auto f = std::find(fxs.begin(), fxs.end(), fx);
            if (f == fxs.end()) return false;
            out.entity  = it->second.entity;
            out.guid    = it->second.guid;
            out.fixture = static_cast<std::uint32_t>(f - fxs.begin());
            return true;
        }
    }

    void PhysicsResource::RecordBody(Arcane::Entity entity, Guid guid, Phys::BodyHandle handle,
                                     std::vector<Phys::FixtureHandle> fixtures)
    {
        bodyRecords[PackBody(handle)] = BodyRecord2D{ entity, guid, std::move(fixtures), false };
    }

    void PhysicsResource::RetireBody(Phys::BodyHandle handle)
    {
        if (const auto it = bodyRecords.find(PackBody(handle)); it != bodyRecords.end())
            it->second.retired = true;
    }

    void PhysicsResource::CaptureStep()
    {
        stepEvents.Clear();
        if (world)
        {
            const Phys::ContactEvents c = world->GetContactEvents();
            for (const auto& e : c.begin)
            {
                ContactBegin2D o;
                if (Side(*this, e.bodyA, e.a, o.a) && Side(*this, e.bodyB, e.b, o.b)) stepEvents.contactBegin.push_back(o);
            }
            for (const auto& e : c.end)
            {
                ContactEnd2D o;
                if (Side(*this, e.bodyA, e.a, o.a) && Side(*this, e.bodyB, e.b, o.b)) stepEvents.contactEnd.push_back(o);
            }
            for (const auto& e : c.hit)
            {
                ContactHit2D o;
                if (!Side(*this, e.bodyA, e.a, o.a) || !Side(*this, e.bodyB, e.b, o.b)) continue;
                o.point  = glm::vec2(static_cast<float>(e.point.x), static_cast<float>(e.point.y));
                o.normal = glm::vec2(static_cast<float>(e.normal.x), static_cast<float>(e.normal.y));
                o.approachSpeed = static_cast<float>(e.approachSpeed);
                stepEvents.contactHit.push_back(o);
            }
            const Phys::SensorEvents s = world->GetSensorEvents();
            for (const auto& e : s.begin)
            {
                SensorBegin2D o;
                if (Side(*this, e.sensorBody, e.sensor, o.sensor) && Side(*this, e.visitorBody, e.visitor, o.visitor)) stepEvents.sensorBegin.push_back(o);
            }
            for (const auto& e : s.end)
            {
                SensorEnd2D o;
                if (Side(*this, e.sensorBody, e.sensor, o.sensor) && Side(*this, e.visitorBody, e.visitor, o.visitor)) stepEvents.sensorEnd.push_back(o);
            }
        }
        const auto append = [](auto& dst, const auto& src) { dst.insert(dst.end(), src.begin(), src.end()); };
        append(frameEvents.contactBegin, stepEvents.contactBegin);
        append(frameEvents.contactEnd,   stepEvents.contactEnd);
        append(frameEvents.contactHit,   stepEvents.contactHit);
        append(frameEvents.sensorBegin,  stepEvents.sensorBegin);
        append(frameEvents.sensorEnd,    stepEvents.sensorEnd);
        std::erase_if(bodyRecords, [](const auto& kv) { return kv.second.retired; });
    }

    PhysicsEvents2D PhysicsResource::StepEvents() const  { return stepEvents.View(); }
    PhysicsEvents2D PhysicsResource::FrameEvents() const { return frameEvents.View(); }
    void PhysicsResource::BeginFrame() { frameEvents.Clear(); }
}
```
New `.cpp` files under `ArcaneCore/src` are globbed; re-run `GenerateProjects.bat`.

- [ ] **Step 8: Wire PhysicsSystem.**
  - **Top of the pass**, right after the early-out (`:315-319`):

```cpp
            // Event policy (spec s7.4, s7.5): PhysicsSystem is the single owner. Both
            // fixtures must opt into contact events; the hit threshold is Live.
            world.SetContactEventsRequireBoth(true);
            world.SetHitEventThreshold(static_cast<Phys::Real>(Settings<PhysicsEventSettings>().hitThreshold));
```
  - **PASS 1:** before `world.RemoveBody(it->second);`, add `res->RetireBody(it->second);`.
  - **PASS 2:** after `Phys::BodyHandle handle = world.AddBody(def);`, collect `std::vector<Phys::FixtureHandle> fxs{ world.GetBodyFixture(handle, 0) };`. In the extra-fixture loop, change `world.AddFixture(handle, fd);` to `fxs.push_back(world.AddFixture(handle, fd));`. After `entityToBody[entity] = handle;`, add:

```cpp
                    const Identity* identity = std::as_const(reg).GetComponent<Identity>(entity);
                    res->RecordBody(entity, identity ? identity->id : Guid{}, handle, std::move(fxs));
```
  - **PASS 3:** replace with:

```cpp
            if (m_stepWorld)
            {
                world.Step(m_fixedDt);
                res->CaptureStep();   // spec s7.2: replace StepEvents, append FrameEvents
            }
```
  `res` is the `PhysicsResource*` the operator already holds (check its name at `:315`).

- [ ] **Step 9: ABI 55.** Change `kGamePluginABIVersion` to `55` (`PluginABI.hpp:1131`), and write the ledger paragraph above it in the v54 style:

```cpp
    // v55 (2026-10-08, 2D physics events, spec docs/specs/2026-10-08-physics-2d-
    //     events-design.md): Fixture gained contactEvents/sensorEvents/hitEvents
    //     (an exported Core struct a module embeds through Collider2D -- its layout
    //     moved). PhysicsResource gained bodyRecords/stepEvents/frameEvents and the
    //     exported RecordBody/RetireBody/CaptureStep/StepEvents/FrameEvents/
    //     BeginFrame/ContactsOf; new SDK header Scene/PhysicsEvents2D.hpp; new
    //     settings block PhysicsEventSettings (physics.events); RunLoop gained the
    //     frame-begin hook member (Runtime.hpp includes RunLoop.hpp). The vendored
    //     Manifold2D moved to <M2D_EVENTS> (PhysicsResource holds its world).
    //     A v54 module was compiled against the old layouts; reject the pairing.
    //     ReferenceProject.arcproj and Aphelyon.arcproj restamped.
```
Restamp `"abi": 55` in `ReferenceProject/ReferenceProject.arcproj:6` and `D:\dev\starworks\Aphelyon\Aphelyon.arcproj:6`. The Aphelyon tree has the user's uncommitted edits (`Content/logo_showcase.arcmat`, `Content/scenes/test.arcscene`, `Source/Game/TestComponent.cpp`): stage **only** `Aphelyon.arcproj` there, and never commit the user's files.

- [ ] **Step 10: Add the guard entry.** Append `"ArcaneCore/src/Arcane/Scene/PhysicsEvents2D.hpp",` to `kGameFacingHeaders` and update the test name's count (15 → 16).
- [ ] **Step 11: Build and test.**
  1. Generate.
  2. Build ReferenceProject (Debug, so the staged DLL is ABI 55), then `Arcane.slnx` Debug.
  3. From the exe dir, run `ArcaneTests "[physics][events]"`, `"[guard]"`, `"[physics]"`, `"[trajectory]"` (bit-identical), `"[json]"` and `"[settings]"` (the new block registers).

  Expected: green. Then Release the same way.
- [ ] **Step 12: Commit.** In Arcane: `feat(physics): 2D physics events reach game code -- Fixture event flags, body records keyed by handle+generation, StepEvents translated after every step, the both-fixtures and hit-threshold policy owned by PhysicsSystem, physics.events settings; game ABI 55`. In Aphelyon: `chore(abi): restamp Aphelyon.arcproj to game ABI 55 (Arcane 2D physics events)`.

### Task A4: FrameEvents through the RunLoop frame hook

**Files:**
- Modify: `ArcaneCore/src/Arcane/Sim/RunLoop.hpp` (`Advance` `:104-130`, members `:254-270`)
- Modify: `ArcaneCore/src/Arcane/Base/Runtime.cpp` (constructor, after `InstallEngineSystems()` `:364`)
- Create: `ArcaneTests/src/PhysicsFrameEventsTest.cpp`

**Interfaces:**
- Consumes: A3's `BeginFrame`, `FrameEvents`, `StepEvents`.
- Produces: `void RunLoop::SetFrameBeginHook(std::function<void(Astra::Registry&)> hook)`.

- [ ] **Step 1: Write the failing tests.** The world here is **+Y up** with the engine default gravity of −9.81, because `Runtime::EnsurePhysics` mints it.

```cpp
// PhysicsFrameEventsTest.cpp -- [physics][events]: the per-frame window across
// 0, 1 and N fixed steps, and its clearing (spec 2026-10-08 s7.2). Real Runtime +
// RunLoop; EnsurePhysics mints a +Y-UP world (default gravity -9.81).
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>
#include "Helpers/TestProcess.hpp"   // Arcane::Test::Process(); confirm the header name as RuntimeTest.cpp includes it

namespace
{
    constexpr double kFixed = 1.0 / 60.0;

    void AddBody(Astra::Registry& reg, Manifold2D::Physics::BodyType type, glm::vec2 pos, float hw, float hh)
    {
        Astra::Entity e = reg.CreateEntity();
        Arcane::Identity id; id.id = Arcane::Guid::Generate(); reg.AddComponent<Arcane::Identity>(e, id);
        Arcane::Transform t; t.position = glm::vec3(pos, 0.0f); reg.AddComponent<Arcane::Transform>(e, t);
        reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});
        Arcane::RigidBody2D rb; rb.type = type; reg.AddComponent<Arcane::RigidBody2D>(e, rb);
        Arcane::Fixture f; f.kind = Manifold2D::Physics::ShapeKind::Aabb; f.halfW = hw; f.halfH = hh;
        Arcane::Collider2D c; c.fixtures.push_back(f); reg.AddComponent<Arcane::Collider2D>(e, c);
        reg.AddComponent<Arcane::PhysicsBodyRef>(e, Arcane::PhysicsBodyRef{});
    }

    // Ground top at y = 0; a crate whose bottom face starts 0.01 m above it, so it
    // touches within the first step or two.
    void Scene(Arcane::Runtime& rt)
    {
        AddBody(rt.Registry(), Manifold2D::Physics::BodyType::Static,  { 0.0f, -0.5f }, 10.0f, 0.5f);
        AddBody(rt.Registry(), Manifold2D::Physics::BodyType::Dynamic, { 0.0f, 0.51f }, 0.5f, 0.5f);
        rt.EnsurePhysics();
    }

    const Arcane::PhysicsResource& Res(Arcane::Runtime& rt) { return *rt.Registry().GetResource<Arcane::PhysicsResource>(); }
}

TEST_CASE("FrameEvents gathers every step of a frame and empties on a zero-step frame", "[physics][events]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    Scene(rt);
    std::size_t total = 0;
    for (int f = 0; f < 30 && total == 0; ++f)
    {
        rt.Loop().Advance(2.5 * kFixed);              // two fixed steps per frame
        total = Res(rt).FrameEvents().contactBegin.size();
        CHECK(total <= 1);
    }
    REQUIRE(total == 1);                              // the landing, seen in its frame
    rt.Loop().Advance(0.0);                           // zero fixed steps
    CHECK(Res(rt).FrameEvents().contactBegin.empty());
    CHECK(Res(rt).StepEvents().contactBegin.size() <= 1);   // unchanged by the zero-step frame
}

TEST_CASE("The frame window keeps both steps' events in step order", "[physics][events]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    Scene(rt);
    for (int f = 0; f < 10; ++f) rt.Loop().Advance(kFixed);       // landed and touching
    REQUIRE(Res(rt).StepEvents().contactBegin.empty());           // the Begin is in the past
    // Teleport the crate away: the next multi-step frame holds its End.
    auto& reg = rt.Registry();
    reg.CreateView<Arcane::RigidBody2D, Arcane::Transform>().ForEach([&](Astra::Entity e, Arcane::RigidBody2D& rb, Arcane::Transform&)
    {
        if (rb.type == Manifold2D::Physics::BodyType::Dynamic)
            Res(rt).world->SetPosition(Res(rt).entityToBody.at(e), Manifold2D::Physics::Vec2(0.0f, 50.0f));
    });
    rt.Loop().Advance(2.5 * kFixed);
    CHECK(Res(rt).FrameEvents().contactEnd.size() == 1);
}

TEST_CASE("Re-minting and restoring clear both windows", "[physics][events]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    Scene(rt);
    for (int f = 0; f < 30; ++f) rt.Loop().Advance(kFixed);
    const std::vector<std::uint8_t> blob = rt.SaveRegistry();     // confirm the snapshot API RuntimeTest.cpp:201-214 uses
    REQUIRE(rt.RestoreRegistry(blob));
    CHECK(rt.Registry().GetResource<Arcane::PhysicsResource>() == nullptr);   // windows gone with the world
    rt.EnsurePhysics();
    CHECK(Res(rt).StepEvents().contactBegin.empty());
    CHECK(Res(rt).FrameEvents().contactBegin.empty());
}

TEST_CASE("the frame hook tolerates a registry without PhysicsResource", "[physics][events]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    rt.Loop().Advance(kFixed);                        // no EnsurePhysics: the hook finds nothing
    rt.ResetPhysics();
    rt.Loop().Advance(kFixed);
    SUCCEED();
}
```
Use the snapshot API that `RuntimeTest.cpp:201-214` uses for its `RestoreRegistry` case (read it, then use the same calls). The `EnsurePhysics` gravity-change re-mint is already covered by construction: a new `PhysicsResource` means new empty buffers. Add one more line to the re-mint test: change gravity through `PhysicsSettings` on a scene root as `RuntimeTest.cpp:232-243` does, call `EnsurePhysics`, and assert the windows are empty.

- [ ] **Step 2: Run them to see them fail** (the frame window is never cleared, so the zero-step frame is not empty).
- [ ] **Step 3: Add the hook.** In `RunLoop.hpp`, add a member `std::function<void(Astra::Registry&)> m_frameBegin;` and:

```cpp
        // Called first in every Advance, before any fixed step (spec 2026-10-08
        // s7.2, amendment A7). Physics-agnostic: Runtime installs the physics
        // frame-window clear, so this header never includes PhysicsSystem.hpp.
        void SetFrameBeginHook(std::function<void(Astra::Registry&)> hook) { m_frameBegin = std::move(hook); }
```
Make the first statement of **both** `Advance` overloads `if (m_frameBegin) m_frameBegin(*m_registry);`. `Rebind` keeps the hook (it calls through the current `m_registry`).

In `Runtime::Runtime`, after `InstallEngineSystems();`, add:

```cpp
        // The 2D physics per-frame event window clears at frame begin (spec
        // 2026-10-08 s7.2): every reader (Update, OnUpdate, render) ran before.
        m_impl->loop->SetFrameBeginHook([](Astra::Registry& reg)
        {
            if (PhysicsResource* res = reg.GetResource<PhysicsResource>()) res->BeginFrame();
        });
```
- [ ] **Step 4: Run the tests.** `"[physics][events]"`, `"[runtime]"`, `"[physics]"`, `"[trajectory]"`. All pass in both configs.
- [ ] **Step 5: Commit.** `feat(physics): FrameEvents -- a physics-agnostic RunLoop frame-begin hook clears the per-frame window; every step of a frame accumulates in step order`

### Task A5: ContactsOf

**Files:**
- Modify: `PhysicsSystem.hpp` (declaration), `PhysicsEvents2D.cpp` (definition), `ArcaneTests/src/PhysicsEvents2DTest.cpp`

**Interfaces:**
- Produces: `ARC_CORE_API void PhysicsResource::ContactsOf(Arcane::Entity entity, std::vector<ContactPoint2D>& out) const`.

- [ ] **Step 1: Write the failing test** (in `PhysicsEvents2DTest.cpp`, same `World` helper):

```cpp
TEST_CASE("ContactsOf lists a resting crate's ground contact by entity", "[physics][events]")
{
    World w;
    const Astra::Entity ground = w.Body("Ground", Manifold2D::Physics::BodyType::Static, { 0.0f, 0.5f }, { Box(10.0f, 0.5f) });
    const Astra::Entity crate  = w.Body("Crate",  Manifold2D::Physics::BodyType::Dynamic, { 0.0f, -0.5f }, { Box(0.5f, 0.5f) });
    Arcane::PhysicsSystem physics(kDt);
    for (int i = 0; i < 300; ++i) physics(w.reg);     // long enough to sleep
    std::vector<Arcane::ContactPoint2D> out;
    w.Res().ContactsOf(crate, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].self.entity == crate);
    CHECK(out[0].other.entity == ground);
    CHECK(out[0].other.guid == w.GuidOf(ground));
    CHECK(out[0].normal.y > 0.99f);                   // from the crate down to the ground (+Y down here)
    w.Res().ContactsOf(Astra::Entity::Invalid(), out);
    CHECK(out.empty());
}
```
- [ ] **Step 2: Run it to see it fail.**
- [ ] **Step 3: Implement.**

```cpp
    void PhysicsResource::ContactsOf(Arcane::Entity entity, std::vector<ContactPoint2D>& out) const
    {
        out.clear();
        const auto it = entityToBody.find(entity);
        if (!world || it == entityToBody.end() || !world->IsValid(it->second)) return;
        std::vector<Phys::BodyContact> raw;
        world->GetBodyContacts(it->second, raw);
        for (const Phys::BodyContact& c : raw)
        {
            ContactPoint2D p;
            if (!Side(*this, c.selfBody, c.self, p.self) || !Side(*this, c.otherBody, c.other, p.other)) continue;
            p.normal = glm::vec2(static_cast<float>(c.normal.x), static_cast<float>(c.normal.y));
            p.pointCount = static_cast<std::uint32_t>(c.pointCount);
            out.push_back(p);
        }
    }
```
- [ ] **Step 4: Run the tests** in both configs.
- [ ] **Step 5: Commit.** `feat(physics): ContactsOf -- what an entity touches right now, sleepers included (GetBodyContacts)`

### Task A6: The witness: a game system reacts to the landing

**Files:**
- Create: `ReferenceProject/Source/Game/TintOnContact.hpp`, `TintOnContact.cpp`, `TintOnContactSystem.hpp`, `TintOnContactSystem.cpp`
- Modify: `ReferenceProject/Content/scenes/physics.arcscene` (the Crate entity, `:138-212`)
- Modify: `ArcaneTests/src/WitnessScenariosTest.cpp` (add W5 after W4 `:250-305`)

**Interfaces:**
- Consumes: `Arcane::Res<Arcane::Physics2D>`, `PhysicsEvents2D`, `ContactBegin2D`.
- Produces: component `ReferenceProject::TintOnContact { glm::vec4 color; }` and system `ReferenceProject::TintOnContactSystem` (Client, `Update` phase).

- [ ] **Step 1: Write the failing witness** (W5):

```cpp
TEST_CASE("W5: a game system sees the Crate land (a dynamic-vs-static contact event) and tints it",
          "[witness][gpu]")
{
    // physics.arcscene: the Crate carries ReferenceProject::TintOnContact (red).
    // TintOnContactSystem (Update phase) reads Physics2D::FrameEvents() and sets the
    // tint on the ContactBegin2D that names it. The Crate rests centred at pixel
    // (640, 405) after 60 frames (W4's resting pick). Authored tint is orange
    // (0.9, 0.6, 0.2); after the event it is red (1, 0, 0). Asserting red-dominant
    // with a crushed green keeps the check robust to the post chain's tone mapping.
    WitnessScratch scratch(StagedRuntimeDir(), "w5-physics-event-tint");
    WitnessRun run = RunWitness(HostInv(scratch,
        { "--scene", "4f6a1c2e-7b3d-4e8a-9c1f-2d5b6e7a8f90", "--probe", "rgba@640,405" }));
    INFO("host stdout: " << run.stdoutPath.string());
    INFO("host stderr: " << run.stderrPath.string());
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    REQUIRE(run.exitCode == 0);
    REQUIRE(run.report["exitReason"].get<std::string>() == "frames-complete");
    REQUIRE(run.report["probes"].size() == 1);
    const nlohmann::json& v = run.report["probes"][0]["value"];
    const int r = v["r"].get<int>(), g = v["g"].get<int>();
    INFO("rgba " << r << "," << g << "," << v["b"].get<int>());
    CHECK(r > 2 * g);          // orange (0.9 vs 0.6) fails this; red passes
    CHECK(g < 100);
}
```
- [ ] **Step 2: Build and run it to see it fail** (W5 reads orange): `ArcaneTests.exe "W5*"` from the exe dir, after building the ReferenceProject and the runtime host. First record the measured rgba in the report. Expected: r ≈ 230, g ≈ 150 (orange).
- [ ] **Step 3: Write the component and system.**

`TintOnContact.hpp`:
```cpp
#pragma once

// TintOnContact: a component -- when a contact begins on this entity, its
// SpriteRenderer takes `color`. The 2D physics events witness (spec 2026-10-08
// s9.3): it proves a game module reads Physics2D::FrameEvents().

#include <Arcane/Reflection.hpp>
#include <glm/vec4.hpp>

namespace ReferenceProject
{
    struct TintOnContact
    {
        glm::vec4 color{ 1.0f, 0.0f, 0.0f, 1.0f };
    };

    ARC_REFLECT_TYPE(TintOnContact)
        ARC_REFLECT_FIELD(TintOnContact, color)
    ARC_END_REFLECT_TYPE()
}
```
`TintOnContact.cpp`:
```cpp
#include "TintOnContact.hpp"

#include <Arcane/Plugin/GameComponents.hpp>

ARC_COMPONENT(ReferenceProject::TintOnContact)
```
`TintOnContactSystem.hpp`:
```cpp
#pragma once

// TintOnContactSystem: reads the frame's 2D physics events (pull model, spec
// 2026-10-08) and tints any TintOnContact entity named by a ContactBegin2D.
// Update phase: FrameEvents() holds every fixed step of this frame.

#include <Arcane/Ecs.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsEvents2D.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>

#include "TintOnContact.hpp"

namespace ReferenceProject
{
    struct TintOnContactSystem
    {
        void operator()(Arcane::View<TintOnContact, Arcane::SpriteRenderer>& view,
                        Arcane::Res<Arcane::Physics2D> physics)
        {
            const Arcane::PhysicsEvents2D events = physics->FrameEvents();
            if (events.contactBegin.empty()) return;
            view.ForEach([&](Arcane::Entity entity, TintOnContact& tint, Arcane::SpriteRenderer& sprite)
            {
                for (const Arcane::ContactBegin2D& e : events.contactBegin)
                {
                    if (e.a.entity == entity || e.b.entity == entity)
                    {
                        sprite.tint = tint.color;
                        return;
                    }
                }
            });
        }
    };
}
```
`TintOnContactSystem.cpp`:
```cpp
#include "TintOnContactSystem.hpp"

#include <Arcane/Plugin/GameSystems.hpp>

ARC_SYSTEM(
    ReferenceProject::TintOnContactSystem,
    Arcane::RoleMask::Client,
    Arcane::SystemPhase::Update)
```
If the system parameter types or the `ARC_SYSTEM` arguments differ from `PlayerController2DSystem`'s, follow that file. `Res<Physics2D>` is read-only, and `Update` never conflicts with `PhysicsSystem` in `fixedUpdate`.

- [ ] **Step 4: Give the Crate the component in `physics.arcscene`.** Inside the Crate entity's `"components"` object (Identity name `"Crate"`, `:163`), add:

```json
"ReferenceProject::TintOnContact": { "color": [1.0, 0.0, 0.0, 1.0] }
```
Keep the file's existing key style (alphabetical component keys, if that's what the file uses). Edit it as text; do not re-save it through the editor, which would rewrite unrelated floats.
- [ ] **Step 5: Run the checks.**
  1. Rebuild ReferenceProject (Debug), then `Arcane.slnx` Debug (stages the DLL and the scene).
  2. Run `ArcaneTests "W5*"` (PASS) and `"W4*"` (still PASS: the Crate still rests at the same pixel).
  3. Run `"[trajectory]"`, which must stay bit-identical. `ReferenceGameUnderTest` loads `physics.arcscene` with the new component; the system only writes `tint`.
  4. Release the same way.
- [ ] **Step 6: Commit.** `test(witness): W5 -- ReferenceProject's TintOnContactSystem reads Physics2D::FrameEvents() and tints the Crate when it lands on the static Ground (the dynamic-vs-static case the old events never reported)`

### Task A7: Close the arc

**Files:**
- Modify: `docs/specs/2026-10-08-physics-2d-events-design.md` (Status line)
- Modify: `CLAUDE.md` / `docs/` wherever physics events or `sync-astra.ps1` are described (grep `sync-astra`, `OnContact`, `contact events`)

- [ ] **Step 1: Full gates, both configs**, in the established order:
  1. ReferenceProject build.
  2. `Arcane.slnx`.
  3. Delete `imgui.ini` in both host exe dirs.
  4. `ArcaneTests "~[gpu]"` (record the seed).
  5. `"[witness]~[shell]"`.
  6. `ArcaneTests "[gpu]"` lanes per the desk's `~[gpu]` baseline convention.
  7. `scripts\golden-gate.ps1 -Configuration Debug`, then `Release`.

  Expected: the goldens 14/14 unchanged since A1. Every count is reported against A1's baseline, with each delta named.
- [ ] **Step 2: Aphelyon builds against ABI 55.** `%ARCANE_SDK%\bin\Debug-windows-x86_64-md\arcbuild\arcbuild.exe build --project D:\dev\starworks\Aphelyon --config Debug`, then `probe` (exit 0). If Aphelyon used any removed Manifold2D API (grep `OnContact|SetBodyEvents|eventsEnabled` in `D:\dev\starworks\Aphelyon\Source`), report it; do not edit Aphelyon gameplay code.
- [ ] **Step 3: Update the docs.**
  - Replace `sync-astra.ps1` mentions with `sync-vendor.ps1 -Library Astra`; the shim still works.
  - Add one paragraph on reading physics events (`Physics2D::StepEvents()` / `FrameEvents()` / `ContactsOf`) to the game-module docs section that lists `Motion` / `SetVelocity` (grep `SetVelocity` in `docs/` and `CLAUDE.md`).
  - Set the spec's Status line to `Implemented (plan docs/plans/2026-10-08-physics-2d-events-plan.md; Manifold2D <M2D_EVENTS>; Arcane <tip>)`.
- [ ] **Step 4: Commit.** `docs(physics): events arc closed -- spec status, game-module docs, sync-vendor references`
- [ ] **Step 5: Hand back to the controller** for the user's call on merging and pushing all four repos (Mosaic, Manifold2D, Astra's `vendor/Mosaic` commit, Arcane + Aphelyon's restamp). Nothing is pushed by this plan.
