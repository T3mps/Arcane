# Arcane flat gameplay API (supersedes the namespace layout of the facades spec)

**Date:** 2026-10-10 · **Status:** Decided and approved for implementation by the user ("lets do the flattening"; "use it as is without needing a plan"); amended FA1-FA9 after the spec review · **Supersedes:** N1 and N3 of `2026-10-10-arcane-namespace-facades-design.md` (the `Arcane::ECS` and `Arcane::Physics2D` homes). Every other decision there (N2, N4, N5, N6 in spirit, N7, N8) and every amendment SA1-SA11 stays in force with the respellings below. · **Base:** local main `77ae30a9` (facades merged, game ABI 56) · **ABI:** 56 -> 57, numbered at execution from main.

## 1. Why

The facades work got the boundary right (no Astra or Manifold2D in game code, one name per type, enforced by guards) but the surface wrong: game code is mostly system signatures, and `Arcane::ECS::ResMut<Arcane::Physics2D::World>` is too verbose for C++, where a per-file `using` is not free in headers. Unreal (flat, prefixed gameplay types, namespaced internals), Unity (`Rigidbody2D`, `Collider2D`, `Physics2D`), Godot (`RigidBody2D`, `PhysicsServer2D`) and Bevy (prelude-flattened) all present a flat gameplay vocabulary. Arcane does the same; namespaces stay for internals, where depth costs users nothing.

## 2. Naming rule (binding for all future subsystems)

| # | Rule |
|---|---|
| F1 | Everything a game spells is flat in `Arcane::`. There is no `Arcane::ECS` and no `Arcane::Physics2D` namespace. |
| F2 | A dimension-specific type carries a `2D` or `3D` suffix (`RigidBody2D`, later `RigidBody3D` for Box3D). |
| F3 | A generic noun takes its subsystem as a prefix (`PhysicsWorld2D`, `PhysicsSystem2D`, `PhysicsSettings2D`); a specific noun stands alone (`RigidBody2D`, `Collider2D`, `ContactBegin2D`, `BodyMotion2D`). |
| F4 | Engine internals live in nested namespaces under `Arcane::Detail::<Subsystem>` (here `Arcane::Detail::Physics2D`). Depth is fine there; games never spell `Detail`. |
| F5 | Facades are aliases or renamed Arcane types, never wrappers (unchanged from the facades spec N8). One name per type: no alias may give a second public spelling. |
| F6 | Cross-cutting core stays plain (`Time`, `GameInput`, `Guid`, `Transform`, ...). Scene/render components are not renamed by this spec. |

## 2a. Amendments from the spec review (controller, 2026-10-10; binding, win over any older sentence)

Review: `.superpowers/sdd/2026-10-10-flat-api/spec-review.md` (Grok xhigh).

- **FA1 (scene keys vs the v6 skip pin; C1).** The flat names reuse the pre-facade strings `Arcane::RigidBody2D` / `Arcane::Collider2D`, and that is intended: those keys load again. The v6-skip pin in `SceneMigrationTest.cpp:235-266` is RETARGETED to the facade-era keys (`Arcane::Physics2D::RigidBody`, `Arcane::Physics2D::Collider`, `Arcane::Physics2D::SceneSettings`) plus the pre-facade `Arcane::PhysicsSettings`: a v6 document with those keys loads none of them. A v5 document with `Arcane::RigidBody2D` / `Arcane::Collider2D` is Y-flipped by `MigrateV5ToYUp` and then LOADS (add that assertion); `Arcane::PhysicsSettings` in a v5 file is flipped and still not loaded (the live key is `Arcane::PhysicsSettings2D`). The migrator's key strings are unchanged. `kSceneJsonVersion` stays 6. No alias table.
- **FA2 (compile-fail set; I1).** `Control.cpp` (must compile) asserts the flat names: `Arcane::PhysicsWorld2D`, `Arcane::Entity`, `Arcane::View<int>`, `Arcane::RigidBody2D`, `Arcane::Detail::Physics2D::Phys` exist. Must-NOT-compile TUs, each a qualified missing member: `using Probe = Arcane::ECS::Entity;`, `using Probe = Arcane::Physics2D::World;`, `Arcane::Phys::PhysicsWorld`, `Arcane::PhysicsResource`, `Arcane::PhysicsSystem`, `Arcane::RigidBody`, `Arcane::PhysicsWorld`, `Arcane::Body`, `Arcane::BodyHandle`, and the private-member probe `void Probe(Arcane::PhysicsWorld2D& w) { (void)w.world; }`. Delete `View.cpp` and `RigidBody2D.cpp` from the must-fail set (now legal). Update the filename list and the count message in `scripts/namespace-compile-fail.ps1`. Manifold2D names ARE reachable at `Arcane::Detail::Physics2D::*` by design (engine-internal; the `[facade]` guard forbids `Detail::` in game sources).
- **FA3 (forward declarations and the friend; I2).** `Physics2DFwd.hpp` forward-declares the flat names in `namespace Arcane` and never reopens `Physics2D`. `PhysicsDebugDraw.hpp:48` forward-declares `struct PhysicsInterpBuffer2D;` in `namespace Arcane`. The friend in `PhysicsWorld2D` is `friend struct ::Arcane::Detail::Physics2D::Access;` with a matching forward declaration of that exact namespace (`namespace Arcane::Detail::Physics2D { struct Access; }`), so it cannot bind `Arcane::Detail::Access`.
- **FA4 (cvar names frozen; I3).** Settings category strings (`"physics"`, `"physics.ground"`, `"physics.events"`, `"debug.physics"`, `"debug.physics.draw"`, `"debug.physics.color"`, `"debug.physics.trace"`, `"debug.physics.style"`) stay byte-for-byte. The settings type-hash change from the struct rename is expected and is not a reason to rename cvars. Fix the user-visible tooltip in `Physics2DBinding.hpp:41` that still says "SceneSettings".
- **FA5 (rewrite scope; I4).** Rewrite: code, tests, scenes, `fuzz/dict/scene.dict` (its three entries; no BodyRef line is added), class templates, the README sample, and these known pins: `ComponentCatalog.cpp:51` and its tests, `EngineRoster.hpp:34-37` (rename in place), ReferenceProject `PlayerController2DSystem.hpp` and `TintOnContactSystem.hpp`, `SceneUnknownFieldTest.cpp`, `SceneJsonTest.cpp`, `PhysicsEvents2DTest.cpp`, `EditorInspectorVectorTest.cpp`, `ClassTemplatesTest.cpp`, `NamespaceFacadeTest.cpp` (its ECS-absence checks invert to flat-presence checks), `fuzz/scene_fuzz.cpp`, `Control.cpp` and the compile-fail script. Do NOT rewrite: `docs/` history, the `PluginABI.hpp` changelog entries for older versions (add a new v57 entry instead), the migrator's key strings (FA1).
- **FA6 (constants; M1).** `kAuthorPosEps` / `kAuthorRotEps` move to `Arcane::Detail::Physics2D`.
- **FA7 (functions; M2).** Function names are unchanged by F2 (`MakePhysicsDebugDrawOptions`, `DrawPhysicsDebug`, `DrawNarrowphaseWorldOverlay` stay); only types take the suffix. Both `RegisterComponents` overloads become `RegisterPhysicsComponents2D`.
- **FA8 (Detail inventory; M3).** Everything in `Arcane::Physics2D::Detail` moves to `Arcane::Detail::Physics2D` (incl. `kInvalidBody`, `FixtureHandle`, `Adopt`, `ToBroadphaseKind`, `PackBody`, `AngleDelta`); list them in the report.
- **FA9 (reflection and headers; M4, M5).** `ARC_REFLECT_*` blocks sit inside `namespace Arcane` with unqualified type names (token pasting). `Ecs.hpp` and `EcsFwd.hpp` keep their paths; only the namespace changes. `IWorkScheduler` stays `Arcane::IWorkScheduler`.

## 3. ECS vocabulary

The 32 curated aliases in `Arcane::ECS` (`Ecs.hpp`, `EcsFwd.hpp`) move back to `namespace Arcane`, same names, same Astra targets (N2 still holds: curated, no `using namespace ::Astra`). `Arcane::ECS` is deleted.

## 4. 2D physics rename table

| Current (facades) | Flat |
|---|---|
| `Arcane::Physics2D::World` | `Arcane::PhysicsWorld2D` |
| `Arcane::Physics2D::System` | `Arcane::PhysicsSystem2D` |
| `RigidBody`, `Collider`, `Fixture` | `RigidBody2D`, `Collider2D`, `Fixture2D` |
| `SceneSettings` | `PhysicsSettings2D` |
| `BodyRef` | `PhysicsBodyRef2D` |
| `BodyType`, `ShapeKind` (Arcane enums, SA1) | `BodyType2D`, `ShapeKind2D` (enumerator names and serialized strings unchanged) |
| `ContactSide`, `ContactBegin`, `ContactEnd`, `ContactHit`, `ContactPoint`, `SensorBegin`, `SensorEnd` | same + `2D` |
| `Events` | `PhysicsEvents2D` |
| `BodyMotion` | `BodyMotion2D` |
| `WorldSettings`, `EventSettings`, `GroundSettings`, `Broadphase` | `PhysicsWorldSettings2D`, `PhysicsEventSettings2D`, `PhysicsGroundSettings2D`, `PhysicsBroadphase2D` |
| `DebugSettings`, `DebugDrawSettings`, `DebugColorSettings`, `DebugStyleSettings`, `DebugTraceSettings`, `DebugDrawOptions` (ArcaneClient) | `PhysicsDebugSettings2D`, `PhysicsDebugDrawSettings2D`, `PhysicsDebugColorSettings2D`, `PhysicsDebugStyleSettings2D`, `PhysicsDebugTraceSettings2D`, `PhysicsDebugDrawOptions2D` |
| `InterpBuffer`, `InterpSlot`, `InterpPose` (+ `Lerp`, `AngleLerp`) | `PhysicsInterpBuffer2D`, `PhysicsInterpSlot2D`, `PhysicsInterpPose2D`; the helpers move to `Arcane::Detail::Physics2D` |
| `BodyRecord`, `RetiredFixture`, `EventBuffers` (engine-internal) | `Arcane::Detail::Physics2D::BodyRecord`, `RetiredFixture`, `EventBuffers` |
| `RegisterComponents` | `Arcane::RegisterPhysicsComponents2D` |
| `Arcane::Physics2D::Detail::*` (`Phys`, `ToVendor`, `MakeScaledShape`, `MakeFixtureDef`, `RebuildScaledFixtures`, handle aliases, `Access`, `ToWorldDef`) | `Arcane::Detail::Physics2D::*` |

Any 2D-physics type found that is not listed follows F2-F4 and is listed in the report.

## 5. What stays

- Manifold2D and Astra are not exposed: `PhysicsWorld2D`'s solver members stay private with the same friend/Detail access; `RigidBody2D::type` is `BodyType2D`, `Fixture2D::kind` is `ShapeKind2D`.
- Headers: `Arcane/Physics2D.hpp` (game include) and `Arcane/Physics2DFwd.hpp` keep their file names and roles; the Detail header stays out of the game include and out of ArcaneClient's Core boundary (SA3, SA10).
- `EngineComponentRoster` renamed in place, never regrouped (SA8), with the ordered-list check updated.
- v5 migrator and its fixture keep the pre-facade key strings (SA7). `kSceneJsonVersion` stays 6.
- Guards: `[facade]` (game sources + class templates; tokens `Astra::`, `Manifold2D::`, `Phys::`, `Mosaic::`, `Detail::`), the spelling guard over the public headers, the roster check, the v5/v6 pins. Compile-fail TUs are respelled: forbidden now are `Arcane::ECS`, `Arcane::Physics2D` (any use), `Arcane::Phys`, `Arcane::PhysicsResource`, `Arcane::PhysicsSystem` (unsuffixed), `Arcane::RigidBody` (unsuffixed), a Manifold2D name through any Arcane spelling, and a game-side access to `PhysicsWorld2D`'s private solver.
- Positive checks: each flat ECS alias `is_same` its Astra type; `decltype(RigidBody2D::type)` is `BodyType2D`; `decltype(Fixture2D::kind)` is `ShapeKind2D`.

## 6. Scenes and stored strings

Scene keys become the flat qualified names: `"Arcane::RigidBody2D"`, `"Arcane::Collider2D"`, `"Arcane::PhysicsSettings2D"`, `"Arcane::PhysicsBodyRef2D"`. Rewrite `ReferenceProject/Content/scenes/physics.arcscene`, `fuzz/corpus/scene/physics.arcscene`, `fuzz/dict/scene.dict`, the README sample, the Create-C++-Class templates (`ClassTemplates.cpp`) and `TemplateSmoke`, `ComponentCatalog.cpp`'s hidden-type compare and its tests, and anything else a whole-tree grep finds. The scene round-trip test stays byte-identical against the rewritten file. Aphelyon has no physics components; its one-token macro fix and ABI stamp are already in its working tree (uncommitted, the user's repo).

## 7. Samples

ReferenceProject's systems use the flat names, e.g.:

```cpp
struct PlayerController2DSystem : Arcane::SystemTraits<Arcane::Before<Arcane::PhysicsSystem2D>>
{
    void operator()(Arcane::View<PlayerController2D, Arcane::RigidBody2D>& view,
                    Arcane::Res<Arcane::Time> time,
                    Arcane::Res<Arcane::GameInput> input,
                    Arcane::ResMut<Arcane::PhysicsWorld2D> physics);
};
```

## 8. Gates

As the facades spec s9: Debug and Release `~[gpu]`, `[guard]`, `[facade]`, `[namespaces]` (renamed tag allowed), `[physics]`, `[physics2d]`, `[witness]~[shell]`, both golden gates (a component display-label-only diff is an explained re-bless; anything else stops), ReferenceProject via `arcbuild` both configs, Aphelyon `arcbuild build --project D:\dev\starworks\Gacha\Game --config Debug` against the new SDK, end on a Debug game DLL. House build rules apply.

## 9. Out of scope

Renaming scene/render/input components (F6), the 3D side (Box3D follows F1-F5 when it lands), the facade-gap APIs (raycasts, forces, joints), a Mosaic facade.
