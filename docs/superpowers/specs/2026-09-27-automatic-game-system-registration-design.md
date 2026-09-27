# Automatic Game-System Registration Design

**Date:** 2026-09-27
**Status:** Approved in conversation; awaiting written-spec review

## Purpose

Creating a game system in the editor currently produces a header with a
paste-ready `RegisterSystem` example. The developer must then find the game
module, include the system, and add the registration to `OnInit`. Components do
not require this extra step because `ARCANE_COMPONENT` lets the module discover
them when it initializes.

System creation should have the same one-rebuild workflow without hiding the
decisions that affect scheduling. The Create C++ Class dialog will collect the
system's execution phase and network role, generate an explicit registration
beside the system, and let the game-module prologue discover it.

Success means that a newly created system is instantiated in every matching
runtime after **Rebuild Game Module**, with no hand edit to the primary module.
The generated code must still make phase, role, and ordering visible.

## Goals

- Register editor-created systems without rewriting a game's module source.
- Let the creator select `FixedUpdate`, `Update`, or `Render`.
- Let the creator select `Both`, `Server`, or `Client`.
- Preserve the existing per-DLL factory ownership, hot-reload cleanup,
  multi-runtime instantiation, and net-mode filtering contracts.
- Keep semantic system ordering explicit through Astra `Before<>` and `After<>`
  traits.
- Preserve manual `GameModule::RegisterSystem` for conditional registration and
  systems that require constructor arguments.
- Explain the lifecycle and ordering rules in the public headers and generated
  files.

## Non-goals

- Automatically infer phase, network role, or ordering from a system body.
- Establish scheduling order from translation-unit or static-initialization
  order.
- Migrate arbitrary existing external projects automatically.
- Add constructor-argument syntax to `ARCANE_SYSTEM`.
- Change the plugin export ABI or the serialized scene format.

## User Experience

When **Assets > Create > C++ Class** has `System` selected, the dialog shows two
additional combo boxes:

| Field | Choices | Default |
|---|---|---|
| Execution Phase | Fixed Update, Update, Render | Fixed Update |
| Network Role | Both, Server, Client | Both |

These fields are hidden for components and plain classes. Resetting or
reopening the dialog restores their defaults.

Creating a system produces both `<System>.hpp` and `<System>.cpp`. The source
contains the selected registration, for example:

```cpp
#include "PlayerController2DSystem.hpp"

#include <Arcane/Plugin/GameSystems.hpp>

ARCANE_SYSTEM(
    ReferenceProject::PlayerController2DSystem,
    Arcane::RoleMask::Both,
    Arcane::SystemPhase::FixedUpdate)
```

The surrounding generated comments explain that the declaration is discovered
by `ARCANE_GAME_MODULE`, creates one factory per DLL load, and produces a system
instance in each matching runtime.

## Public SDK Contract

### `ARCANE_SYSTEM`

A new public header, `Arcane/Plugin/GameSystems.hpp`, defines:

```cpp
ARCANE_SYSTEM(Type, RoleMask, SystemPhase)
```

The macro is valid at namespace scope in exactly one `.cpp` for a default-
constructible system type. It adds a trivially destructible registrar node to a
module-local linked list. The node holds only a function pointer, type name,
role mask, phase, and next pointer; it owns no live engine state and performs no
cleanup from `DLL_PROCESS_DETACH`.

The registration macro remains explicit in source. “Automatic” means the game
module discovers that declaration; it does not mean Arcane guesses the phase,
role, or ordering.

### Shared factory helper

`GameSystems.hpp` provides one internal template helper that builds and adds a
`SystemFactoryEntry`. Both `ARCANE_SYSTEM` and
`GameModule::RegisterSystem<System>(...)` use this helper so their factory
semantics cannot drift. The registrar form supplies no constructor arguments;
the manual member remains the supported form when arguments or conditional
logic are required.

### Duplicate registrations

`SystemFactoryTable::Add` rejects registrations of the same system type more
than once in the same phase under the same module owner. It asserts in debug
builds and logs then drops the later entry in non-asserting builds. The same
system type may be registered in different phases. Separate `Server` and
`Client` registrations are still duplicates: both match `Standalone` and
`ListenServer` runtimes, which play both roles.

This catches accidental combinations of `ARCANE_SYSTEM` and a leftover manual
`RegisterSystem` line before a runtime receives duplicate work.

## Registration Lifecycle

1. Loading a game DLL performs its ordinary C++ static initialization. Each
   `ARCANE_SYSTEM` links its trivial node into that DLL's private registrar
   list.
2. `PluginHost` opens the existing system-factory owner bracket for the image
   and calls `GamePlugin_Init`.
3. The `ARCANE_GAME_MODULE` prologue registers components, constructs and binds
   the module, then traverses the automatic system list immediately before
   calling `GameModule::OnInit`.
4. The traversal adds factories to the process-owned `SystemFactoryTable`.
   `OnInit` may then add manual or conditional factories through the existing
   member function.
5. The host instantiates matching factories into each runtime's phase
   scheduler according to its `NetMode`.
6. On reload, reinitialization, or unload, the existing image-owner cleanup
   removes every automatic and manual factory before module code can be
   unmapped.

The registrar list is traversable more than once because an already loaded
image may be initialized again. `SystemFactoryTable::BeginOwner` clears that
owner's previous factories before the next traversal, so reinitialization
replaces rather than duplicates them. If `OnInit` fails, the existing plugin
failure teardown clears factories registered earlier in the failed attempt.

No plugin ABI bump is required: exports and `EngineContext` do not change, old
modules continue using manual registration, and new behavior is compiled into
new modules through the header-only game-module prologue.

## Scheduling and Ordering

Registrar link order is unspecified across translation units and is never a
scheduling contract. Systems with a semantic dependency must express it using
`Astra::Before<>` or `Astra::After<>`; independent systems may be ordered or
parallelized by the scheduler.

Generated traits depend on the selected phase:

- `FixedUpdate` includes `TransformSystems.hpp` and starts with
  `Before<Arcane::TransformPropagationSystem>`. The generated comment explains
  that gameplay commonly writes local transforms before propagation.
- `Update` and `Render` omit the fixed-step transform anchor. Their generated
  comment explains that an anchor absent from their scheduler would add no edge
  and instructs the developer to add the appropriate `Before<>` or `After<>`
  relation when ordering matters.

The comments in `GameComponents.hpp` are updated as well: component registrar
order remains irrelevant, while automatic system registration is safe because
meaningful order comes from declared scheduler dependencies rather than static
initialization order.

## Editor and Template Data Flow

`ClassTemplates` gains a typed `SystemOptions` value containing Arcane's
`SystemPhase` and `RoleMask`, defaulted to `FixedUpdate` and `Both`. `Render`
accepts this value while non-system templates ignore it. Central mapping
functions turn valid enum values into UI labels and generated C++ spellings.
Unknown enum values normalize to the documented defaults, preventing malformed
generated code if stale UI state reaches the renderer.

The Create dialog owns integer combo indices but converts them to typed options
before calling `Render`. Existing file creation already writes an optional
source when `Rendered::sourceName` is non-empty, so emitting a source for
systems requires no second filesystem workflow.

The current helper text that asks the user to paste an `AddSystem` or
`RegisterSystem` line is replaced with text describing automatic registration
and the editable generated declaration.

## Reference Project Migration

`PlayerController2DSystem` becomes the in-repository example:

- Add `PlayerController2DSystem.cpp` with `ARCANE_SYSTEM`, using `Both` and
  `FixedUpdate`.
- Remove its include and manual registration from `ReferenceGame.cpp`, returning
  the primary module to the minimal empty-module form.
- Keep the system's existing component query and fixed-update ordering traits.

The other uncommitted reference-project component work is outside this change
and must not be rewritten.

## Error Handling and Diagnostics

- Registering outside an open image-owner bracket retains the existing assert,
  error log, and dropped-entry behavior.
- Duplicate registration in one owner and phase follows the new
  assert/log/drop behavior described above.
- An empty automatic registrar list is normal and produces no warning.
- Module initialization logs the number of automatically registered systems,
  parallel to the component-registration count.
- Invalid dialog indices are clamped or normalized before template rendering.
- Compile errors remain the feedback for a non-default-constructible system
  using `ARCANE_SYSTEM`; the generated comments direct such systems to manual
  `RegisterSystem` with constructor arguments.

## Verification Strategy

Implementation follows test-driven development.

1. **Template tests** verify both files are emitted, every phase/role spelling,
   defaults and invalid-value normalization, fixed-update placement, the lack
   of a misleading fixed-step anchor in the other phases, and explanatory
   comments without leftover template tokens.
2. **Registrar/factory tests** verify automatic discovery, phase placement,
   role filtering, and duplicate rejection, including separate `Server` and
   `Client` registrations of one type.
3. **Plugin lifecycle tests** exercise a real DLL registrar through initial
   load, secondary-runtime attachment, net-mode changes, hot reload, failed
   initialization, and unload cleanup. They verify no callable factory survives
   its image.
4. **Pure dialog-state/template tests** verify labels, selection conversion,
   and reset defaults without testing ImGui drawing calls themselves. The
   system-only visibility branch receives a narrow desk verification because
   the existing dialog has no rendered-UI harness.
5. **Integration verification** builds and runs the full `ArcaneTests` suite,
   then builds `ReferenceProject.slnx` and confirms `ReferenceGame.dll` links
   without a manual system registration in its module.

## Documentation Requirements

Comments are part of the developer-facing contract for this feature. They must
explain reasons, not merely restate code:

- Why the registrar node must be trivially destructible.
- Why the list must be module-local.
- Why system scheduling must not depend on registrar order.
- Why only fixed-update systems receive the transform-propagation anchor.
- Why factories must be registered inside the image-owner bracket.
- When manual `RegisterSystem` is still the correct API.
