# Arcane namespace facades: `Arcane::ECS` and `Arcane::Physics2D`

**Date:** 2026-10-10 · **Status:** Draft for the user's review · **Follows:** input-seam spec 2026-10-02 s5.3/s6.1 (the flat ECS prelude and the `Arcane::Physics2D` type alias), the 2D physics events arc (merged on local main `7e433a74`), the namespace job on `feat/arcane-namespaces` (superseded in part, see s7) · **ABI:** one game-module bump, numbered at execution from main.

## 1. Goal

Game code spells only `Arcane::` names. Arcane's libraries (Astra, Manifold2D) stay underneath and are not part of the game-facing API, the way Unreal hides Chaos and Unity hides Box2D. Every facade name is an **alias of the library type or a renamed Arcane type, never a wrapper**: no indirection, no copies, no performance cost. The facades exist to reduce confusion and keep one namespace, not to abstract.

## 2. Decisions (user, 2026-10-10)

| # | Decision |
|---|---|
| N1 | `Arcane::ECS` is the single home of the ECS vocabulary. The flat prelude names (`Arcane::View`, `Arcane::Res`, `Arcane::Entity`, `Arcane::Registry`...) move into it and are removed from `Arcane::`. One name per thing. |
| N2 | `Arcane::ECS` is a curated set of aliases of Astra types. No `using namespace ::Astra`: a new Astra feature reaches game code only by adding an alias on purpose. |
| N3 | `Arcane::Physics2D` holds Arcane's 2D physics API: the resource, the system, components, events, query results and settings structs. The `2D`/`Physics` prefixes are dropped inside it. |
| N4 | Manifold2D is not exposed. `Arcane::Physics2D` has no using-directive and no alias to Manifold2D. Engine code names `::Manifold2D` directly. |
| N5 | No duplicate names. `PhysicsResource` is renamed to `Physics2D::World`; the `Arcane::Physics2D` type alias is deleted. |
| N6 | The step system is `Physics2D::System` (there is one physics system today; a future sync/step/write-back split would be ordered as a set or phase, not by its pieces). |
| N7 | Scene migration rewrites the in-repo scene files only. No loader alias table. |
| N8 | Facades are aliases or renamed Arcane types, never wrappers or pimpl. Library headers may still be included transitively (a complete `World` needs them); "not exposed" means not nameable through the Arcane API, not "not compiled". |

## 3. `Arcane::ECS`

Every alias now in `ArcaneCore/src/Arcane/Ecs.hpp` and `EcsFwd.hpp` moves from `namespace Arcane` into `namespace Arcane::ECS`, unchanged otherwise: `Entity`, `View`, `Not`, `With`, `Changed`, `Added`, `Optional`, `Any`, `OneOf`, `IncludeDisabled`, `Res`, `ResMut`, `Commands`, `SystemTraits`, `Before`, `After`, `AmbiguousWith`, `Reads`, `Writes`, `ReadsResources`, `WritesResources`, `Exclusive`, `Tick`, `Result`, `SerializationError`, `Registry`, `ComponentRegistry`, `TypeContext`, `BinaryWriter`, `BinaryReader`, `ComponentModule`, `SystemScheduler`.

- The light/full split stays: `EcsFwd.hpp` (forward declarations + the non-template aliases; for engine headers such as `Runtime.hpp` and `PluginABI.hpp`) and `Ecs.hpp` (full Astra includes + the template aliases; what game code includes). File names are unchanged.
- `IWorkScheduler` aliases Mosaic, not Astra. It stays `Arcane::IWorkScheduler` in this spec. A Mosaic facade is a later decision.
- The `using namespace ::Astra;` block added on `feat/arcane-namespaces` is removed.
- Call sites: every use of a flat prelude name becomes `Arcane::ECS::<name>` (qualified) or `ECS::<name>` inside `namespace Arcane`. That includes engine code inside `namespace Arcane` that relies on the flat alias unqualified (e.g. `Registry`). Engine code that already spells `Astra::` is left alone (no mass rename of internals).
- Game code may write `using namespace Arcane::ECS;` in a `.cpp`; the samples show the qualified form in headers.

## 4. `Arcane::Physics2D`

### 4.1 Rename table

Every type below moves into `namespace Arcane::Physics2D` under its new name. Anything else in the 2D physics vocabulary that the implementer finds follows the same rule (move in, drop the `2D`/`Physics` prefix or suffix) and is listed in the report.

| Today | New | Kind |
|---|---|---|
| `PhysicsResource` (+ the `Arcane::Physics2D` type alias) | `World` | registry resource |
| `PhysicsSystem` | `System` | fixed-step system |
| `RigidBody2D` | `RigidBody` | serialized component |
| `Collider2D` | `Collider` | serialized component |
| `Fixture` | `Fixture` | serialized (Collider's fixture array element) |
| `PhysicsSettings` (scene gravity) | `SceneSettings` | serialized component |
| `PhysicsBodyRef` | `BodyRef` | component (runtime) |
| `ContactSide2D`, `ContactBegin2D`, `ContactEnd2D`, `ContactHit2D`, `ContactPoint2D` | `ContactSide`, `ContactBegin`, `ContactEnd`, `ContactHit`, `ContactPoint` | events / query results |
| `SensorBegin2D`, `SensorEnd2D` | `SensorBegin`, `SensorEnd` | events |
| `PhysicsEvents2D` | `Events` | event window view |
| `BodyMotion2D` | `BodyMotion` | query result |
| `Physics2DWorldSettings`, `PhysicsEventSettings`, `PhysicsGroundSettings`, `Physics2DBroadphase` | `WorldSettings`, `EventSettings`, `GroundSettings`, `Broadphase` | settings structs / enums |
| `DebugPhysicsSettings`, `DebugPhysicsDrawSettings`, `DebugPhysicsColorSettings`, `DebugPhysicsStyleSettings`, `DebugPhysicsTraceSettings`, `PhysicsDebugDrawOptions` | `DebugSettings`, `DebugDrawSettings`, `DebugColorSettings`, `DebugStyleSettings`, `DebugTraceSettings`, `DebugDrawOptions` | client debug-draw settings |
| `BodyRecord2D`, `RetiredFixture2D`, `PhysicsEventBuffers2D`, `PhysicsInterpBuffer` (+ `InterpPose`) | `BodyRecord`, `RetiredFixture`, `EventBuffers`, `InterpBuffer` (+ `InterpPose`) | engine-internal |
| free helpers (`MakeScaledShape`, `MakeFixtureDef`, `RebuildScaledFixtures`, `AngleDelta`, ...) | same names | engine-internal, see 4.2 |

Cvar names (the `physics.*` and debug-draw cvar strings, as they are today) do **not** change: they are strings, and settings persist by cvar name. Verify that no config file or saved preference keys by a settings struct's C++ name; if one does, stop and report.

### 4.2 Hiding Manifold2D

- `namespace Phys = Manifold2D::Physics;` currently sits in `namespace Arcane` (`PhysicsComponents.hpp:56`, `PhysicsSystem.hpp:102`), which makes `Arcane::Phys::` a public spelling of the solver. It moves into `namespace Arcane::Physics2D::Detail` (engine-internal by convention, the same rule as other `Detail` namespaces), or the code spells `::Manifold2D::Physics` directly.
- Engine-internal helpers whose signatures name Manifold2D types (`MakeScaledShape`, `MakeFixtureDef`, `RebuildScaledFixtures`, `RecordBody`, `RetireBody`) live in `Physics2D::Detail` or stay member functions not intended for games. `World`'s game-facing members keep Arcane types only: `StepEvents`, `FrameEvents`, `ContactsOf`, `Motion`, `SetVelocity`.
- Test: a compile-time detection check that `Arcane::Physics2D::PhysicsWorld`, `Arcane::Physics2D::Body`, `Arcane::Physics2D::BodyHandle` and `Arcane::Phys` do not name anything.

### 4.3 Headers

| Header | Contents | Includes |
|---|---|---|
| `Arcane/Physics2DFwd.hpp` | forward declarations of the Physics2D types engine headers need (`World`, `System`, components) | no Manifold2D, no Astra |
| `Arcane/Physics2D.hpp` | the one game include for 2D physics: `World` (complete), `System`, components, events, query results | whatever those definitions need, transitively |

The existing `Scene/PhysicsSystem.hpp`, `Scene/PhysicsComponents.hpp`, `Scene/PhysicsEvents2D.hpp`, `Scene/PhysicsQuerySettings.hpp` keep their locations (or are renamed to match; implementer's choice, stated in the report). `Arcane/Physics2D.hpp` includes them. No header includes `Arcane/Physics2D.hpp` from below (the `feat/arcane-namespaces` inversion, `PhysicsSystem.hpp` including `Physics2D.hpp` at its tail, is removed).

## 5. Scenes, serialization and the editor

- Components are keyed in `.arcscene` by their qualified reflected name (`"Arcane::Collider2D": {...}`). After the rename the keys are `"Arcane::Physics2D::Collider"`, `"Arcane::Physics2D::RigidBody"`, `"Arcane::Physics2D::SceneSettings"`. Verify how the name is derived (TypeID / `ARC_REFLECT_TYPE`) before editing anything.
- Rewrite every in-repo file that stores an old name: `ReferenceProject/Content/scenes/physics.arcscene`, `fuzz/corpus/scene/physics.arcscene`, and anything else a grep of the whole Arcane tree and Aphelyon (`D:\dev\starworks\Gacha`) finds (scenes, prefabs, `.arcproj`, test fixtures, goldens metadata, witness scripts). Aphelyon has no physics components today; confirm it.
- Binary archives (play-mode snapshots) key by type hash. They are transient; confirm no persisted binary registry file exists in either repo.
- Editor: component display names, the add-component menu and any inspector label derived from the type name change. If an editor golden changes **only** in that label text, it is an explained re-bless under the staged-slot procedure; any other pixel change stops the work.

## 6. Guard

A test (tag `[facade]`) scans the game-facing sources in this repo (`ReferenceProject/Source/**`) for the tokens `Astra::`, `Manifold2D::`, `Phys::` and `Mosaic::` (comments stripped, whole-token match) and fails on any hit. This is the "game code spells only Arcane names" rule, enforced. Aphelyon's `Game/` is checked by the same grep once during this arc (it is a separate repo; a standing guard there is its own change).

## 7. Relation to the running work

- `feat/arcane-namespaces` (the first namespace job) added `Arcane::ECS { using namespace ::Astra; }`, `Arcane/Physics2D.hpp` with `using namespace ::Manifold2D;` and `using World = PhysicsResource;`, and migrated `Arcane::Physics2D` uses to `Physics2D::World`. This spec supersedes those three pieces; its gates (the first on main after the physics-events merge) still count. The implementation branches from that branch's tip and replaces them.
- The physics-events showcase (`.superpowers/sdd/2026-10-10-physics-events-showcase/dispatch.md`) waits for this rename and is rewritten to the new names before it starts.

## 8. Out of scope (owed)

- **Facade gaps** that hiding Manifold2D makes urgent: raycasts, shape casts and overlap queries; forces, impulses and runtime gravity scale; joints (spec 3 of the events + joints arc). Each lands in `Physics2D::World` / components as its own change. Until then a game cannot reach the solver; that is intended.
- A Mosaic facade (`IWorkScheduler`).
- Engine-internal `Astra::` spellings.
- The 3D side: `Arcane::Physics3D` (Box3D) follows this same shape when D5 lands.

## 9. Testing and gates

- `[facade]` guard (s6); `[namespaces]`: static checks that each `Arcane::ECS` alias is the Astra type, that each renamed physics type exists under `Arcane::Physics2D`, that the old flat names (`Arcane::View`, `Arcane::RigidBody2D`, `Arcane::PhysicsResource`, `Arcane::Physics2D` as a type) no longer name anything, and the Manifold2D non-exposure check (s4.2).
- A scene round trip: load the rewritten `physics.arcscene`, save, compare (byte-identical to the rewritten file).
- Existing suites: Debug `~[gpu]` (full), `[physics]`, `[physics2d]`, `[witness]~[shell]`, golden gate Debug; Release `~[gpu]` and golden gate Release; ReferenceProject via `arcbuild` for both configs; end on a Debug game DLL. House build rules apply (CL=/FS, `/m:8 /nodeReuse:false`, exe-dir test runs, restage the game DLL into the hosts' staged `ReferenceProject\Binaries`, delete exe-dir `imgui.ini` before goldens).
- Aphelyon `Game/` builds against the new SDK (`arcbuild build --project Game --config Debug`), and its manifest is restamped with the new ABI.
