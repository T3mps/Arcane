# Arcane flat gameplay API (supersedes the namespace layout of the facades spec)

**Date:** 2026-10-10 · **Status:** Decided by the user ("lets do the flattening"); draft for review · **Supersedes:** N1 and N3 of `2026-10-10-arcane-namespace-facades-design.md` (the `Arcane::ECS` and `Arcane::Physics2D` homes). Every other decision there (N2, N4, N5, N6 in spirit, N7, N8) and every amendment SA1-SA11 stays in force with the respellings below. · **Base:** local main `77ae30a9` (facades merged, game ABI 56) · **ABI:** 56 -> 57, numbered at execution from main.

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
