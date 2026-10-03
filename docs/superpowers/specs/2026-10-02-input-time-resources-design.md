# Input and time as resources, one system style, one namespace for game code

**Status:** Approved in conversation 2026-10-02 (brainstorm with the user, sections 1-3). This written spec awaits the user's review.
**Raised by:** the user, 2026-09-29: "why is this in the main file", about `ReferenceProject/Source/Game/ReferenceGame.cpp`.
**Runs:** in parallel with the node-page phase, in the worktree `D:\dev\starworks\Arcane-input` (branch `feat/input-time-resources` off main `b3908116`). It merges AFTER the node-page phase (s9).

## 1. The problem

Game systems cannot read gameplay input or time.

- **Systems get only the registry.** A system's only argument is `Astra::Registry&`.
- **The engine doesn't publish input or time.** It publishes the physics, mesh, sprite and render tables as registry resources (`ClientRuntime.cpp:101-125`), but not gameplay input or time.
- **So the game module copies them in.** `ReferenceGame.cpp:29-43` (`Module::OnFixedUpdate`) reads `Client()->GameInput()` and the fixed `dt` every fixed step. It copies them into four transient fields on every `PlayerController2D`: `value`, `jumpRequested`, `jumpHeld` and `fixedDt`. `PlayerController2DSystem` then reads them back.
- **Time is mostly invisible.** `RunLoop` (`ArcaneCore/src/Arcane/Sim/RunLoop.hpp:159-190`) keeps `fixedDt` as a local, keeps no step counter and no elapsed sim time, and exposes only `alpha` (through `RenderContext2D`).
- **One copied field leaks into scenes.** `PlayerController2D::value` is reflected without `Serializable(false)`, so a live stick value is written into scenes (`ReferenceProject/Content/scenes/physics.arcscene:396`, `"value": 0.0`).

Two more problems surfaced in the brainstorm and are fixed here.

- **Game code mixes three library namespaces.** A game author writes `Astra::Registry`, `Astra::SystemTraits`, `ASTRA_REFLECT_*`, `Arcane::RigidBody2D` and sometimes `Manifold2D::Physics::`. They have to know which library owns which name. Astra, Manifold2D and Mosaic are Starworks libraries, but they are standalone. Their own namespaces stay. Game code should only ever see `Arcane::`.
- **Astra's parameter-style systems cannot express what a real game system needs.**
  - They take exactly four parameter kinds: `View<...>&`, `Res<T>`, `ResMut<T>` and `Commands` (`Astra/System/SystemParam.hpp`). They carry no ordering (`Before`/`After`).
  - The player controller needs `Before<PhysicsSystem>`.
  - It also calls `GetBodyMotion2D(Registry&, Entity)` and `SetBodyVelocity2D(Registry&, Entity, ...)`, which need the whole registry.

## 2. The goal and what success looks like

- **One way to reach engine state.** Time, input, physics and every other engine-provided state are registry RESOURCES. Systems declare them as `Arcane::Res<T>` / `Arcane::ResMut<T>` parameters, so the scheduler sees every read and write. Code outside a system (a module's `OnUpdate`, `OnDrawUI`) reads the same data with `Registry().GetResource<T>()`.
- **No global accessors.** There is no `Arcane::Time()`. One process holds several worlds:
  - the editor's edit registry;
  - its Play registry;
  - the in-process server world;
  - test worlds.

  A global would have to guess which one. A hidden read would also defeat the scheduler's access analysis. Unreal scopes time to the world, and Unity's DOTS moved to world-scoped `SystemAPI.Time` for the same reason.
- **One system style for game code: parameter style.** Templates and docs teach only this style. The registry style (`operator()(Registry&)` plus declared traits) remains for engine-internal systems.
- **`Arcane::` only in game-facing code.** An alias facade re-exports every library name game code touches. The libraries keep their namespaces, and engine internals may still write `Astra::`.
- **The adapter disappears and gameplay is unchanged.**
  - `ReferenceGame.cpp` keeps only its startup check.
  - `PlayerController2D` loses its four copied fields.
  - `PlayerController2DSystem` reads `Time`, `GameInput` and `Physics2D` itself.
  - The player's trajectory under scripted input is bit-identical before and after (s8 T6).

**Out of scope:**
- Networked input (clients sending commands to the server) belongs to the replication spec.
- Local multiplayer (several input users) and possession.
- Input rebinding UI.
- Folding `PhysicsBodyRef` into `PhysicsResource::entityToBody`. That belongs to the physics rework (s10).

## 3. The `Time` resource

`ArcaneCore/src/Arcane/Sim/Time.hpp` is new. It is a plain struct and includes no Core headers, because `RunLoop.hpp` sits on the plugin-facing include chain, which must stay Core-free (`RunLoop.hpp:100-103`).

```cpp
namespace Arcane
{
    struct Time
    {
        double        realDt      = 0.0;   // this frame's wall-clock dt, unscaled
        double        dt          = 0.0;   // realDt * timeScale; 0 while paused
        double        fixedDt     = 0.0;   // 1 / fixedHz
        double        alpha       = 0.0;   // render interpolation factor
        double        elapsed     = 0.0;   // sim time = fixedStep * fixedDt
        std::uint64_t fixedStep   = 0;     // fixed ticks since the registry was bound
        double        timeScale   = 1.0;
        bool          paused      = false;
        bool          inFixedStep = false; // true while the fixedUpdate scheduler runs
    };
}
```

**Publisher: `RunLoop`.** It already owns the registry pointer, the config, `paused` and `timeScale`. It republishes `Time` on every pass, so a registry swap can never lose it:
- **Before each fixed step:** it increments `fixedStep`, recomputes `elapsed`, sets `inFixedStep = true`, then runs the module's `pluginFixed` and the fixedUpdate scheduler.
- **After the fixed phase:** before the Update scheduler, it publishes with `inFixedStep = false` and the new `alpha`.

Rules:
- **Single step:** `SingleStep()` advances `fixedStep` by exactly 1.
- **Paused:** no fixed steps run, `dt = 0`, `paused = true`, and `realDt` is still the frame's wall time.
- **Rebind:** `Rebind(registry)` resets `fixedStep` and `elapsed` to 0, so Play starts at step 0. Pause state is NOT reset (`RunLoop.hpp:138-145`, the host-mode rule).
- **Both `Advance` overloads publish.** The no-callback one (`:93`) and the host-driven one (`:109`) both publish, so every host does: editor, runtime, server and headless test worlds.

`RenderContext2D::alpha` stays: the render path reads it. `Time::alpha` is the same value, published for systems.

## 4. The `GameInput` resource and `ActionRef`

`ArcaneClient/src/Arcane/Input/GameInput.hpp` is new. It is a small read-only handle over the client's `LocalInputUser`.

```cpp
namespace Arcane
{
    class ARCANE_API GameInput
    {
    public:
        GameInput() = default;                         // no user: answers zero/false
        explicit GameInput(const LocalInputUser* user) noexcept;

        std::uint64_t Generation() const noexcept;     // bumps on Configure / Clear
        std::optional<Guid> FindAction(std::string_view map, std::string_view action) const;

        InputActionValue Value(const ActionRef&) const;
        bool Down(const ActionRef&) const;
        bool Pressed(const ActionRef&) const;          // this frame
        bool Released(const ActionRef&) const;
        bool PressedThisFixedStep(const ActionRef&) const;
        bool ReleasedThisFixedStep(const ActionRef&) const;
        // ...plus Guid overloads of the same queries
    };

    struct ActionRef
    {
        ActionRef(std::string_view map, std::string_view action);
        // Resolves lazily against GameInput and re-resolves when Generation() changes.
        // An unknown action answers zero/false and warns once per generation.
    };
}
```

- **Query only.** Maps, schemes, rebinding and profiles stay on `Client()->GameInput()` (the `LocalInputUser&`). `GameInput` is the query half that systems use.
- **`Generation()`.** `LocalInputUser` gains a `std::uint64_t generation_` that increments on `Configure` and `Clear`. This changes the layout of a class modules already see through `Client()->GameInput()`, so it is **the one ABI bump** in this spec (s9).
- **Publisher: `ClientRuntime`.** It publishes `GameInput{&m_pres.gameInput}`:
  - in `UpdateGameInput`, once per frame (`ClientRuntime.cpp:76`, called at `EditorAppFrame.cpp:844` and `RuntimeFrame.cpp:249`);
  - in `BeginGameInputFixedStep`, before each fixed step (`ClientRuntime.cpp:78`, called at `EditorAppFrame.cpp:1388` and `RuntimeFrame.cpp:292`).

  Every client world therefore has a `GameInput`, including a headless client or one with no input asset configured; it answers with empty values.
- **Server worlds have no `GameInput`.** Client-role systems never run there.
- **Fixed-step edge semantics are unchanged.** `BeginFixedStep` resets the window. When one frame runs several fixed steps, a press is visible in the first of them only, exactly as today.

Amendment (2026-10-02, IN-8 ruling): Time and GameInput are transient resources (Astra AstraTransientResource): never serialized, so a snapshot-seeded server world has no GameInput and a restore never revives stale values; Rebind republishes Time at once.

## 5. One system style: parameter systems with ordering

### 5.1 Astra (committed in the Astra repo FIRST, then `sync-astra.ps1`)

- **Ordering traits on parameter-style systems.** A parameter-style functor may derive `SystemTraits<...>` containing ONLY ordering traits: `Before<...>`, `After<...>` and `AmbiguousWith<...>`. Astra's parameter wrapper (`FunctionSystemWrapper`, `SystemScheduler.hpp:1148-1160`) forwards these to the scheduler's ordering graph.
- **Access traits are rejected.** `Reads<>`, `Writes<>`, `ReadsResources<>`, `WritesResources<>` or `Exclusive` in a parameter-style system's traits is a compile error (`static_assert`): the parameters already state access, and two sources would disagree.
- **"Resource absent, system skipped" logs once.** It logs once per system per scheduler until the resource reappears, not every frame. If Astra spams today, this fixes it.
- **Where it lands.** The change goes on Astra `dev`. It does not touch the separate storage-policies branch (`D:\dev\starworks\Astra-storage`).

### 5.2 `ARCANE_SYSTEM` registers both shapes

`Arcane::Game::Detail::AddSystemFactory` (`ArcaneCore/src/Arcane/Plugin/GameSystems.hpp:52-65`) currently calls `scheduler.AddSystem<System>(args...)`, the typed registry-style path. It learns to detect a parameter-style functor (Astra's `ParamFunctor` concept) and to register it through `scheduler.AddSystem(System{})`. The macro's spelling, roles, phases and duplicate rejection are unchanged.

### 5.3 Physics through a resource: `Arcane::Physics2D`

- **Today's free functions** are `GetBodyMotion2D(Registry&, Entity)` and `SetBodyVelocity2D(Registry&, Entity, x, y)` (`ArcaneCore/src/Arcane/Scene/PhysicsCommands.hpp:25-28`). They touch only `RigidBody2D`, `PhysicsBodyRef` and the already-published `PhysicsResource`.
- **The new name:** `using Physics2D = PhysicsResource;`.
  - It is the SAME type, so a system that takes `ResMut<Physics2D>` conflicts correctly with `PhysicsSystem` in the scheduler.
- **Two exported member functions** (`ARCANE_CORE_API`, because `PhysicsWorld` links inside ArcaneCore):
  - `BodyMotion2D Motion(Entity, const RigidBody2D&) const`
  - `void SetVelocity(Entity, RigidBody2D&, float x, float y)`
- **The body handle comes from `entityToBody`** (`PhysicsSystem.hpp:118`), not from `PhysicsBodyRef`. A system's view therefore needs no optional term. This repeats the ABI-27 ruling (`PluginABI.hpp:745-751`): the map is used in preference to `Optional<PhysicsBodyRef>`.
- **Behaviour is identical to today's functions:**
  - before the body exists, values come from `RigidBody2D` and `bodyReady = false`;
  - floor support is checked by contacts, then by a shape cast for sleeping bodies;
  - non-finite velocities are ignored;
  - non-dynamic bodies are ignored.
- **The free functions and `PhysicsCommands.hpp/.cpp` are deleted.** There is no second path. The floor-support helper moves with them.

### 5.4 The controller after the change

```cpp
struct PlayerController2DSystem : Arcane::SystemTraits<Arcane::Before<Arcane::PhysicsSystem>>
{
    Arcane::ActionRef move{"Player", "Move"};
    Arcane::ActionRef jump{"Player", "Jump"};

    void operator()(Arcane::View<PlayerController2D, Arcane::RigidBody2D>& view,
                    Arcane::Res<Arcane::Time> time,
                    Arcane::Res<Arcane::GameInput> input,
                    Arcane::ResMut<Arcane::Physics2D> physics)
    {
        const float dt     = std::clamp(static_cast<float>(time->fixedDt), 0.0f, 0.05f);
        const float axis   = std::clamp(input->Value(move).scalar, -1.0f, 1.0f);
        const bool  jumped = input->PressedThisFixedStep(jump);
        const bool  held   = input->Down(jump);
        view.ForEach([&](Arcane::Entity e, PlayerController2D& c, Arcane::RigidBody2D& rb)
        {
            const Arcane::BodyMotion2D motion = physics->Motion(e, rb);
            // ...today's movement maths, unchanged, reading dt/axis/jumped/held...
            physics->SetVelocity(e, rb, nextX, nextY);
        });
    }
};
```

- **`PlayerController2D` keeps** only its authored tuning and the system's own state: coyote time, the jump buffer, `jumpConsumed` and `jumpCutArmed`.
- **It loses** `value`, `jumpRequested`, `jumpHeld` and `fixedDt`.
- **`ReferenceGame.cpp` keeps `OnInit`'s check:** the module refuses to load when `Player.Move` or `Player.Jump` is missing. `OnFixedUpdate`, `moveId` and `jumpId` are deleted.
- **`GameModule::OnFixedUpdate` stays as a hook.** Its comment (`GameModule.hpp:85`) is updated: systems read the `Time` and `GameInput` resources, and modules must not copy values into components.

### 5.5 Scenes

- **The stale key.** `physics.arcscene` is rewritten without `"value"`.
- **Old scenes must still load.** A scene that still carries a field the type no longer reflects loads, and its other fields stay intact.
  - The plan's first scene task establishes what the JSON scene loader does with an unknown field today.
  - If it rejects or misreads one, tolerating it (warn once per type and field) is fixed in that task.
  - The binary snapshot path is unaffected: transient snapshots never outlive a build.

## 6. The `Arcane::` facade

### 6.1 Two headers, aliases only

These are aliases, so the types are identical. No ABI or serialization changes; scene files keep their type names.

**`ArcaneCore/src/Arcane/Ecs.hpp`:**
- `Registry`, `Entity`, `View<...>`, `Res<T>`, `ResMut<T>`, `Commands`
- `SystemTraits<...>`, `Before<...>`, `After<...>`, `AmbiguousWith<...>`, `Exclusive`, `ReadsResources<...>`, `WritesResources<...>` (the last three are for registry-style engine systems)
- `Not<T>`, `With<T>`, `Changed<T>`, `Added<T>`, `Optional<T>`, `Any<...>`, `OneOf<...>`
- `BinaryWriter`, `BinaryReader`, `ComponentModule`, `ComponentRegistry`, `TypeContext`, `Tick`, `Result<T, E>`, `SerializationError`
- `IWorkScheduler` (from Mosaic)

**`ArcaneCore/src/Arcane/Reflection.hpp`:**
- **Type and enum macros:**
  - `ARCANE_REFLECT_TYPE`, `ARCANE_REFLECT_FIELD`, `ARCANE_END_REFLECT_TYPE`, `ARCANE_REFLECT_TYPE_ATTR`, `ARCANE_REFLECT_TYPE_END`;
  - `ARCANE_REFLECT_ENUM`, `ARCANE_REFLECT_ENUM_VALUE`, `ARCANE_REFLECT_ENUM_VALUE_NAMED`, `ARCANE_REFLECT_ENUM_VALUE_FULL`, `ARCANE_REFLECT_ENUM_FLAGS`, `ARCANE_END_REFLECT_ENUM`, `ARCANE_REFLECT_ENUM_END`.

  Each forwards to its `ASTRA_` macro. `ASTRA_REFLECT_TYPE` token-pastes its argument, so the type stays unqualified, exactly as today.
- **Attributes:** `ARCANE_REFLECT_ATTR(AttrType, ...)` expands to `.Attr<::Arcane::AttrType>(__VA_ARGS__)`. `ASTRA_REFLECT_ATTR` hard-prefixes `::Astra::` (`Astra/Reflection/Macros.hpp:91`), so every attribute gets an `Arcane::` alias:
  - `Serializable`, `Hidden`, `Tooltip`, `Category`, `Range`, `ReadOnly`, `AngleFormat` (with `AngleFormat::Unit`);
  - `DisplayName`, `ColorFormat` (with `::Format`), `Multiline`, `FilePath`, `DragSpeed`, `Deprecated`, `AliasName`, `Precision`.

  Amendment (2026-10-02, IN-11 / plan spec-error #2): the attribute aliases live in `Arcane::Attr`, not `Arcane::`, because `Arcane::Hidden` is the serialized Outliner tag component (Scene/Components.hpp:302). `ARCANE_REFLECT_ATTR(AttrType, ...)` expands to `.Attr<::Arcane::Attr::AttrType>(__VA_ARGS__)`, and an AngleFormat argument is spelled `Arcane::Attr::AngleFormat::Unit::Degrees`.

- **Change tracking:** `ARCANE_CHANGE_TRACKED` expands to Astra's `static constexpr bool AstraChangeTracked = true;` member convention.

**Manifold2D** reaches game code only through component field types (`RigidBody2D::type`, `Fixture::kind`). Those stay spelled through the existing `Arcane::Phys::` namespace alias (`PhysicsComponents.hpp:55`). `PhysicsBodyRef` is engine-internal and not part of the facade.

### 6.2 The sweep (22 files)

- **Game-facing engine headers (16).** Their PUBLIC declarations spell the `Arcane::` names; their internals may keep `Astra::`. The headers:
  - `Plugin/`: `GameModule.hpp`, `GameComponents.hpp`, `GameSystems.hpp`, `SystemFactory.hpp`, `PluginABI.hpp`, `ProcessContext.hpp`;
  - `Base/`: `Runtime.hpp`, `Assert.hpp`, `Log.hpp`;
  - `ClientRuntime.hpp`;
  - `Scene/`: `SceneResources.hpp`, `Components.hpp`, `TransformSystems.hpp`, `PhysicsComponents.hpp`, `PhysicsSystem.hpp`;
  - `PhysicsCommands.hpp`, which is deleted by s5.3.
- **`ArcaneEditor/src/Project/ClassTemplates.cpp`.** Every template emits `Arcane::` names and `ARCANE_REFLECT_*`, and the system templates emit the parameter style with a commented `Res<Time>` / `Res<GameInput>` example.
- **ReferenceProject game sources:** `PlayerController2D.hpp/.cpp`, `PlayerController2DSystem.hpp/.cpp`, `ReferenceGame.cpp`.
- **Docs:**
  - Arcane `README.md`;
  - Aphelyon `docs/examples/arcane-physics-example.cpp`, done at the Aphelyon restamp (s9).

### 6.3 The guard

A test fails when any of these contain `Astra::`, `ASTRA_`, `Manifold2D` or `Mosaic::` outside comments:
- `ReferenceProject/Source/**`;
- the generated output of every `ClassTemplates.cpp` template;
- the public declarations of the 15 surviving game-facing headers (`PhysicsCommands.hpp` is deleted).

The checked-header list lives in the test, beside the rule: **"game-facing code spells `Arcane::` only"**.

## 7. Errors

| Condition | Behaviour |
|---|---|
| `Time` absent | Cannot happen in a host world (`RunLoop` always publishes it). If it does, Astra skips the system and logs once. |
| `GameInput` absent (server world) | Client-role systems never run there. A server-role system asking for it is skipped and logged once. That is the correct failure. |
| `GameInput` with no input asset configured | Present, answers zero/false. |
| `ActionRef` names an unknown action | Answers zero/false, warns once per `Generation()`. |
| Required actions missing at module load | `ReferenceGame` `OnInit` refuses to load, as today. |
| Non-finite velocity to `SetVelocity` | Ignored, as today. |
| Scene carries a field the type no longer reflects | Loads; the field is ignored, with one warning per type and field (s5.5). |

## 8. Testing

T1-T10 are required. Tags follow the existing suites. Run them from the exe directory with seeds, as the hygiene rules require.

- **T1 Astra (in the Astra repo):**
  - a parameter system with `Before`/`After` is ordered correctly;
  - access traits on a parameter system fail a concept check (`static_assert`-backed; a negative test asserts `!CanAddParamSystem<Bad>`);
  - the absent-resource skip logs once.
- **T2 `Time` (`RunLoopTest.cpp`):**
  - `fixedStep` goes up once per fixed step, and single step adds 1;
  - while paused there are no steps, `paused == true` and `dt == 0`;
  - `dt == realDt * timeScale`;
  - `elapsed == fixedStep * fixedDt`;
  - `inFixedStep` is true inside fixedUpdate and false in Update;
  - `alpha == Loop().Alpha()`;
  - `Rebind` resets `fixedStep` and `elapsed` but not `paused`;
  - both `Advance` overloads publish.
- **T3 swaps (`EditorPlayModeTest.cpp` pattern):** `Time` and `GameInput` are present and correct after Play, Stop, scene open and hot reload.
- **T4 `GameInput`:**
  - it is published with no input asset configured and answers empty;
  - `Generation()` goes up on `Configure` and on `Clear`;
  - an `ActionRef` re-resolves after a reconfigure;
  - an unknown action warns once per generation;
  - several fixed steps in one frame show a press in the first step only.
- **T5 `Physics2D`:**
  - `Motion` and `SetVelocity` match the deleted functions' behaviour, case by case;
  - a body not yet created reads from `RigidBody2D` with `bodyReady == false`;
  - non-finite input is ignored;
  - a non-dynamic body is ignored;
  - a sleeping body on the floor reads as supported.
- **T6 trajectory (THE behaviour gate).** A headless test plays a fixed input script against ReferenceProject's player for N fixed steps:
  - hold right;
  - tap jump;
  - release jump early (a short hop);
  - turn while airborne;
  - land and brake.

  It records position and velocity at every step. The recording is captured BEFORE any change (task 1 of the plan) and committed. After the rewrite, the same script must reproduce it bit for bit.
- **T7 scenes:**
  - a scene carrying a stale `"value"` loads, and the other fields keep their values;
  - `physics.arcscene` has no `"value"`.
- **T8 generated code (`ClassTemplatesTest.cpp`):** every system and component template is generated, compiled and registered in a scratch project. This reuses the compile path `ClassTemplatesTest` already has; if it has none, an arcbuild-driven case is added. The generated system is parameter style and reads `Res<Time>`.
- **T9 the guard:** s6.3.
- **T10 images:** the goldens must match unchanged. Any difference is investigated, never re-blessed.

## 9. How it lands

- **Branch and build.** Worktree `D:\dev\starworks\Arcane-input`, branch `feat/input-time-resources` off main `b3908116`. Tests use a private TEMP (`.tmp-tests`, excluded via `info/exclude`). Builds run at a capped CPU share beside the node-page tracks.
- **Order of work:**
  1. T6's trajectory recording, before anything changes;
  2. Astra (s5.1), committed to Astra `dev` and synced;
  3. `Time`;
  4. `GameInput` and `ActionRef`;
  5. `ARCANE_SYSTEM` parameter registration;
  6. `Physics2D`;
  7. the facade headers;
  8. the controller and ReferenceProject rewrite;
  9. the scene fix;
  10. the templates;
  11. the header sweep;
  12. the guard;
  13. docs;
  14. the gate.
- **ABI.** Main is at 46. The node-page branch moves it to 48, and its T5 lane to 49. This branch takes the NEXT FREE number when it merges, after the node-page phase. It rebases onto the merged node-page work and bumps once (`LocalInputUser` layout, s4). ReferenceProject's `.arcproj` and Aphelyon's `D:\dev\starworks\Aphelyon\Aphelyon.arcproj` (the repo-root project; built with `--project D:\dev\starworks\Aphelyon`) are restamped then. The Aphelyon example doc (s6.2) is swept in the same Aphelyon commit.
- **Execution.** Subagent-driven, task by task with review, then a whole-branch review. Estimated 20-25 tasks.
- **Never:** push; `git add -A`; touch the user's untracked files.

## 10. Decisions and owed items

**Decisions (the user, 2026-10-02):**
1. Approach A: engine-published resources. Not a per-step snapshot (B) and not per-entity input components (C).
2. The resource is named `Time`, not `TimeStep`: it is a clock, not one step.
3. Access goes through resources and `Res<T>` parameters. There are no global accessors.
4. The namespace is an alias facade, done as a WHOLE sweep now. The libraries keep their own namespaces.
5. Parameter style everywhere: Astra gains ordering on parameter systems, and physics commands become the `Physics2D` resource.
6. `Physics2D` takes the body handle from `entityToBody`, so views need no `Optional<PhysicsBodyRef>`.

**Owed:**
- **Folding `PhysicsBodyRef` and `PhysicsResource::entityToBody` into one record** of the entity-to-body link. Physics rework / Box3D arc.
- **Replicated input** (clients send action snapshots, servers consume them). Replication spec. Approach B's snapshot shape is reconsidered there.
