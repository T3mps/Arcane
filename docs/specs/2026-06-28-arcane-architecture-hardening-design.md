# Arcane Architecture Hardening - Design

**Date:** 2026-06-28
**Status:** PROPOSED - design contract for reducing Arcane architectural risk before
Game.dll/Grimoire growth turns today's working seams into expensive boundaries.
**Upstream:**
- `2026-06-11-engine-architecture-design.md` - original Arcane engine workspace,
  Runtime, host/plugin, renderer, and Core extraction decisions.
- `2026-06-12-arcane-2d-renderer-architecture.md` - renderer north star:
  linear-HDR Canvas, sort-keyed Batcher2D, TonemapPass, shader/data conventions.
- `2026-06-14-arcane-m6-physics-design.md` and later physics specs/plans - the
  presentation-free Core physics module, current `PhysicsWorld` facade, and
  shared simulation ambition.
- Architectural review in Codex thread, 2026-06-28 - identified the main risks:
  Core duplicate linkage/state ownership, header-only cross-module systems,
  `Runtime` service-locator growth, `PhysicsWorld` monolith pressure, and
  transitional asset/render ownership.

## Problem

Arcane has reached a useful M5/M6 shape: one engine DLL, a thin `Loom` host,
hot-reloadable gameplay plugins, NVRHI rendering, a presentation-free Core, and
an interactive Sandbox. The immediate risk is not that the architecture is wrong;
it is that the boundaries are still informal while the system is beginning to
grow multiple owners.

The risky seams are:

1. **Core duplicate linkage and state ownership.** `Arcane/Core` is compiled into
   multiple modules by design. That is safe for stateless algorithms and
   per-module data, but risky for mutable process-global services, ownership
   crossing DLL boundaries, and future shared simulation state.
2. **Header-only cross-module systems.** Some engine-facing systems can be
   instantiated in any module that includes them. That is convenient during
   bring-up, but it can blur which DLL owns components, worlds, handles, and
   destruction.
3. **Runtime surface growth.** `Runtime` is the plugin's root capability handle.
   Without grouping, it can quietly become a broad service locator whose API is
   hard to version, test, or explain.
4. **PhysicsWorld pressure.** `PhysicsWorld` is currently the right public facade,
   but internally it is carrying broadphase, contacts, islands, sleeping,
   solving, queries, and debug data. That will make later correctness work
   expensive unless internal seams are introduced.
5. **Asset/render transitional ownership.** Texture and render-resource lifetime
   are functional enough for the current Sandbox, but Grimoire/content-editing
   will need stable asset identities and clearer ownership than raw pointer/table
   conventions.

This hardening effort turns those risks into explicit contracts and small,
reviewable moves. It is not a big-bang refactor.

## Goals

1. Make Arcane's module and DLL ownership rules explicit, written down, and
   review-enforceable.
2. Stabilize the plugin-facing `Runtime` surface by introducing grouped capability
   facades without breaking existing plugins.
3. Define the physics ownership model across Core, Arcane.dll, Loom, and plugins.
4. Split `PhysicsWorld` internally along natural seams while preserving the public
   facade and existing behavior.
5. Add lightweight verification gates that catch Core presentation-boundary drift.
6. Record the future asset/render ownership requirement without prematurely
   building the full asset pipeline.

## Non-goals

- No Game.dll, Grimoire, editor, or gameplay feature work.
- No physics solver rewrite, behavior change, or determinism policy change.
- No renderer rewrite, render graph, bindless material system, or full asset
  pipeline implementation.
- No server workspace merge or change to the Core strangler strategy.
- No forced public plugin ABI break. Existing plugins should continue to build
  during the transition.
- No attempt to make every dependency rule perfect on day one. Explicitly
  documented exceptions are acceptable when they are temporary and owned.

## Decisions

### D1. Boundary Contract Comes First

The first deliverable is a living Arcane module-boundary document. It should be
short enough to be read during review and concrete enough to settle arguments.

Default ownership rules:

- `Arcane/Core` owns presentation-free algorithms, data structures, simulation
  code, networking primitives, serialization helpers, and shared utility code.
  It must not include SDL, NVRHI, ImGui, audio, platform windows, GPU resources,
  or engine-host lifecycle code.
- `Arcane.dll` owns platform integration, render resources, engine runtime
  services, exported plugin-facing APIs, first-party ImGui integration, and
  engine-owned scene/runtime state.
- `Loom.exe` stays a thin host: argument parsing, host boot, plugin selection,
  window/run-loop startup, and diagnostics. It should not become a second engine.
- Gameplay plugins own gameplay systems and plugin-local state. If they directly
  instantiate Core types, they also own those instances unless an exported
  Arcane.dll facade explicitly says otherwise.
- Tests may duplicate Core and fixture code intentionally, but test layout is not
  precedent for production module ownership.

### D2. Core Duplicate-Linkage Policy

Core may be linked into multiple production modules only for:

- Stateless or value-semantic code.
- Per-module state that never crosses ownership/destruction boundaries.
- Explicitly documented stateful services that are safe when each module has its
  own copy.

If an object crosses a DLL boundary by pointer/reference and has non-trivial
ownership, one module must be the clear owner and must also destroy it. Other
modules consume facades, handles, IDs, snapshots, or const debug views.

Any new process-global mutable state in Core requires one of:

1. A written note that it is intentionally per-module state.
2. An exported Arcane.dll facade that owns the singleton/service.
3. A redesign into explicit caller-owned state.

### D3. Physics Ownership Is Hybrid for Now

The public `PhysicsWorld` facade remains intact. The current Sandbox may keep
plugin-owned physics worlds while Arcane is still proving the gameplay feel.

The boundary rule is:

- A plugin-created `PhysicsWorld` is plugin-owned. Arcane.dll may inspect it only
  through const views, snapshots, or draw-command data supplied by the owner.
- Arcane.dll-created scene physics is engine-owned and exposed through engine
  facades/components, not by transferring ownership to plugins.
- Future shared Game/Combat simulation should use Core deterministic APIs with
  explicit caller ownership and no hidden engine presentation dependency.

This avoids pretending there is one universal owner before Game.dll exists, while
still preventing accidental cross-DLL lifetime coupling.

### D4. Runtime Becomes a Root Handle With Grouped Facades

`Runtime` remains the plugin's root handle, but it should stop accumulating
unrelated methods directly. New capabilities should be grouped under small
facades such as:

- `RuntimeScene`
- `RuntimeRender`
- `RuntimeInput`
- `RuntimeJobs`
- `RuntimeAssets`
- `RuntimePluginServices` or `RuntimePlatform`

The transition should be non-breaking: add grouped accessors first, then make old
methods forward to the new facades. Removal can wait until a deliberate plugin ABI
version bump.

### D5. Header-Only Systems Need Ownership Notes

Header-only engine systems are allowed when the local ECS/type context makes them
useful, but any header-only system that can be instantiated by plugins must state
which module owns the objects it touches.

Preferred direction:

- Engine-stable behavior moves behind Arcane.dll facades over time.
- Plugin-local convenience systems remain allowed when their state is fully
  plugin-owned.
- Cross-boundary ownership is passed as IDs, handles, snapshots, or facades, not
  implicit raw ownership.

### D6. PhysicsWorld Splits Internally, Not Publicly

`PhysicsWorld` remains the public user-facing API while its implementation gains
internal seams. Target seams:

- `BodyStore` - body records, handles, transforms, velocities, awake flags.
- `FixtureStore` - shape/material/filter data and fixture lifetime.
- `ContactPipeline` - broadphase pairs, narrowphase contact generation,
  persistence, sensors, and event staging.
- `IslandSleepManager` - graph extraction, awake/sleep transitions, island tags.
- `SolverDriver` - solver selection, sub-step orchestration, constraint staging.
- `PhysicsQueries` or `QueryDebug` - raycasts, overlaps, shape casts, and debug
  snapshots.

Each extraction must be behavior-preserving and gated by the existing physics
suite. This is a maintainability refactor, not a solver change.

### D7. Verification Starts Simple

Add lightweight checks before heavyweight tooling:

- A Core forbidden-include/dependency check that fails if `Arcane/Core` includes
  SDL, NVRHI, ImGui, platform windows, render code, or audio.
- A module-boundary documentation check or test fixture that records the intended
  dependency direction.
- Existing functional gates remain mandatory for touched areas: `[wire]`,
  `[hotreload]`, `[physics]`, and the scripted Loom GPU verify when render/plugin
  host code changes.

The first version may be a small PowerShell or C++ test helper. It can become a
CI stage once the false-positive rate is understood.

### D8. Asset/Render Ownership Is Recorded, Not Solved Here

The hardening milestone should not build the final asset pipeline. It should
record the requirement that Grimoire-era content needs stable asset identities:

- Render code should move toward asset handles/IDs rather than ad hoc texture
  pointer ownership.
- Asset cache lifetime, hot reload, and failure memoization should be expressed
  through a facade rather than scattered ownership conventions.
- The future design must explain how authored content, runtime cache, render
  resources, and plugin reload interact.

## Proposed Implementation Slices

These slices are intentionally small and ordered from lowest-risk to higher-risk.
The implementation plan should choose exact files and tests after this spec is
approved.

### Slice 0 - Current-Branch Hygiene

Before architecture work starts, remove throwaway instrumentation and either get
the current physics suite green or isolate the physics behavior failures into a
separate active task. Architecture hardening should not bury known behavioral
failures under refactor noise.

Expected output:

- No temporary profiling/env knobs left in production code.
- A clear status for the existing `[physics]` failures: fixed, tracked, or
  explicitly excluded from this architecture milestone.

### Slice 1 - Boundary Contract and Guard

Create the short module-boundary document and add the first Core dependency guard.

Expected output:

- `docs/architecture/arcane-module-boundaries.md` or equivalent.
- A check that flags forbidden includes/dependencies from `Arcane/Core`.
- Review notes for any existing exceptions.

### Slice 2 - Runtime Facades, Non-Breaking

Introduce grouped Runtime facades and make existing direct methods forward where
practical.

Expected output:

- New facade types/accessors.
- Existing plugins continue to build.
- Hot reload tests remain green.

### Slice 3 - Physics Ownership and Debug View

Make the physics ownership rule concrete in code and docs. Prefer const views or
snapshot/debug-command data when Arcane.dll visualizes plugin-owned worlds.

Expected output:

- Ownership comments/API notes at the plugin/engine seam.
- Debug rendering path avoids implying Arcane.dll owns plugin-created worlds.
- Sandbox behavior unchanged.

### Slice 4 - PhysicsWorld Internal Seams

Extract one internal seam at a time behind `PhysicsWorld`, starting with the
lowest-risk data grouping. Each step should be small enough to review independently.

Expected output:

- Internal helper classes/modules with no public API break.
- Existing physics tests continue to pass after each seam.
- No solver behavior changes hidden inside the extraction.

### Slice 5 - Asset/Render Ownership Note

Record the future asset identity/lifetime design constraint in the renderer or
asset architecture notes so Grimoire planning starts from the right premise.

Expected output:

- A short note describing the required move from raw render-resource conventions
  toward asset handles/facades.
- No broad renderer implementation in this milestone unless separately approved.

## Acceptance Criteria

The hardening effort is complete when:

1. Arcane has an approved module-boundary contract.
2. Core presentation-boundary drift is checked by an automated guard.
3. Runtime has a non-breaking grouped-facade path for new plugin capabilities.
4. Physics ownership across plugins, Arcane.dll, and Core is documented and
   reflected at the debug/render seam.
5. At least one `PhysicsWorld` internal seam is extracted without changing public
   behavior, or the implementation plan explicitly defers extraction after the
   boundary/facade work with rationale.
6. Existing relevant tests are run and their results are recorded.
7. Known temporary exceptions are listed with an owner and a follow-up.

## Risks and Mitigations

- **Risk: Architecture work hides behavior bugs.** Mitigation: Slice 0 requires a
  current test status and keeps physics behavior failures visible.
- **Risk: Refactor churn breaks hot reload.** Mitigation: keep the first Runtime
  facade pass non-breaking and gate with hot-reload tests.
- **Risk: Boundary rules become ceremonial.** Mitigation: add an automated Core
  dependency guard immediately after the doc.
- **Risk: Overcorrecting slows experimentation.** Mitigation: allow explicit,
  owned exceptions and keep Sandbox plugin-local ownership valid for now.
- **Risk: Physics extraction destabilizes the solver.** Mitigation: extract
  structure only, one seam at a time, with no solver algorithm changes in the same
  commit.

## Open Decisions for Review

1. Should production gameplay plugins continue linking Core directly after
   Sandbox, or should direct Core linkage become a Sandbox/test exception once
   Game.dll begins?
2. Should the boundary contract live under `docs/architecture/` as durable
   project architecture, or remain only in `docs/superpowers/specs/` until the
   first implementation slice lands?
3. Should the first Core dependency guard be CI-enforced immediately, or run as a
   local test until existing exceptions are cataloged?

Default recommendation: keep direct Core linkage allowed with explicit ownership
rules, promote the durable boundary doc into `docs/architecture/`, and start the
guard as a local/Arcane test before making CI enforcement mandatory.
