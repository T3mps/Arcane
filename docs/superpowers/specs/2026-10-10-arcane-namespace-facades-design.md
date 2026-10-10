# Arcane namespace facades: `Arcane::ECS` and `Arcane::Physics2D`

**Date:** 2026-10-10 · **Status:** Approved for implementation by the user (2026-10-10, "kick off"); amended SA1-SA11 after the spec review · **Follows:** input-seam spec 2026-10-02 s5.3/s6.1 (the flat ECS prelude and the `Arcane::Physics2D` type alias), the 2D physics events arc (merged on local main `7e433a74`), the namespace job on `feat/arcane-namespaces` (superseded in part, see s7) · **ABI:** one game-module bump, numbered at execution from main.

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

## 2a. Amendments from the spec review (controller, 2026-10-10; binding, they refine N4/N8 and win over any older sentence below)

Review: `.superpowers/sdd/2026-10-10-namespace-facades/spec-review.md` (Grok xhigh, NOT READY: N4 was not met as written).

- **SA1 (authored fields get Arcane enums; C1).** `Physics2D::BodyType` and `Physics2D::ShapeKind` are NEW Arcane enumerations with the same enumerator names as Manifold2D's (`Static`, `Kinematic`, `Dynamic`; the shape kinds as they are), reflected with `ARC_REFLECT_ENUM` inside `namespace Arcane::Physics2D`. `RigidBody::type` and `Fixture::kind` use them. Conversion to the vendor enum is a `constexpr` switch in `Detail`, called where the system syncs a body (the `Physics2DBroadphase` / `ToBroadphaseKind` pattern). Separate enum + conversion, never a wrapper object. Serialized enumerator strings are unchanged, so scenes keep `"type": "Dynamic"`. The vendor-enum reflection blocks are removed if nothing else uses them.
- **SA2 (World hides the solver without pimpl; C2).** Every Manifold2D-typed member of `World` (`world`, `entityToBody`, `bodyRecords`, `RecordBody`, `RetireBody`, and any other) is private. Engine code reaches them through `friend` (the `System`, and `Detail` helpers) or a `Detail` accessor. The game-facing public set is exactly `StepEvents`, `FrameEvents`, `ContactsOf`, `Motion`, `SetVelocity`, `BeginFrame` (M2); `lastReconcile` / `reconciled` are private or engine-only. Keep the existing `unique_ptr<PhysicsWorld>` (existing storage, not a facade). Put `World`'s destructor out of line so `Arcane/Physics2D.hpp` needs only a forward declaration of `PhysicsWorld` where possible.
- **SA3 (header split; C2).** `Arcane/Physics2D.hpp` (the game include) defines `World` (private solver members), `System`, components, events, query results, the Arcane enums. The `Detail` helpers (`MakeScaledShape`, `MakeFixtureDef`, `RebuildScaledFixtures`, the enum conversions, `ToWorldDef`, `Phys` alias) live in an engine-only header that `Arcane/Physics2D.hpp` does NOT include. `Physics2D::BodyRef` stays a component (runtime, `Hidden`, not serialized); its `handle` field is typed `Detail::BodyHandle` (an alias in `Detail`). `Detail::` is engine-internal by rule and is forbidden in game sources (s6).
- **SA4 (negative checks are compile-fail TUs; C3).** "Does not name anything" checks use the repo's compile-fail pattern (`ArcaneTests/compile-fail/` + `scripts/settings-compile-fail.ps1`): one TU per forbidden spelling, expected not to compile: `Arcane::Physics2D::PhysicsWorld`, `::Body`, `::BodyHandle`, `::Physics::PhysicsWorld`, `::Phys`, `Arcane::Phys`, `Arcane::View`, `Arcane::RigidBody2D`, `Arcane::PhysicsResource`, a game-side access to `World::world` (private). Positive checks stay `STATIC_REQUIRE(std::is_same_v<...>)`, plus `std::is_same_v<decltype(RigidBody::type), Physics2D::BodyType>` and the same for `Fixture::kind`. `Arcane::Physics2D` stays a namespace: do not test it as "absent".
- **SA5 (interp; I1).** `InterpSlot`, `InterpPose`, `Lerp`, `AngleLerp` move with `InterpBuffer` into `Physics2D`. Fix the forward declaration in `PhysicsDebugDraw.hpp:48`.
- **SA6 (stored strings; I2, M4).** Also rewrite: `fuzz/dict/scene.dict` (new keys), `README.md` gameplay sample, the Create-C++-Class raw-string templates in `ArcaneEditor/src/Project/ClassTemplates.cpp` and `ArcaneTests/plugins/TemplateSmoke/SmokeSystem.hpp`, the exact-name compare in `ArcaneEditor/src/Scene/ComponentCatalog.cpp:51` (BodyRef stays hidden) and the tests that pin it. The `[facade]` scan covers string literals too, and also scans `ClassTemplates.cpp` (its literals are game code); tokens: `Astra::`, `Manifold2D::`, `Phys::`, `Mosaic::`, `Detail::`.
- **SA7 (historical migrator; I3).** `MigrateV5ToYUp` (`SceneSerializer.hpp:294-309`) and its `SceneMigrationTest` fixture KEEP the old key strings: they match v5 files. Do not rewrite them. `kSceneJsonVersion` stays 6. Pin with tests: a v5 document with old keys is still migrated; a v6 document with old keys skips those components (N7).
- **SA8 (roster; I4).** `EngineComponentRoster` order is the component id order: rename in place, never regroup. Add a `[namespaces]` check that the roster list equals the expected ordered list. `ARC_REFLECT_TYPE` blocks sit inside `namespace Arcane::Physics2D` with unqualified names.
- **SA9 (names the table missed; I6).** `RegisterPhysicsComponents` becomes `Physics2D::RegisterComponents`. Callers of the moved `Detail` helpers qualify them (ADL no longer finds them). `PhysicsOverlayPlan` stays in `Arcane::Editor`.
- **SA10 (DLL split; M1).** `Arcane::Physics2D` spans ArcaneCore and ArcaneClient. The Core header `Arcane/Physics2D.hpp` never includes ArcaneClient headers; the debug-draw settings live in ArcaneClient under the same namespace.
- **SA11 (s7 correction; I5).** The first job's work is `stash@{0}` in `Arcane-ns`, not a branch tip; it is superseded entirely and is not applied. Its `[namespaces]` cases (flat prelude kept, `Physics2D::Physics::PhysicsWorld` visible) are replaced by SA4's checks, not kept.

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
