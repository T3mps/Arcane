# CVar system — design

**Status:** design approved 2026-09-02. **No implementation.** Build begins after Arc A closes
and F2b lands (user's explicit sequencing call — see §10).

**Goal:** one typed, discoverable, runtime-mutable registry of scalar knobs and commands,
serving three audiences from one substrate — engine/editor development, players via an in-game
console, and later server administration — without foreclosing the multiplayer shape.

---

## 1. Why, and why now only as a spec

Arcane already has two configuration surfaces:

- **`Arcane::Config`** (`ArcaneClient/src/Arcane/Config/Config.{hpp,cpp}`) — layered JSON,
  engine defaults → plugins → project → user, deep-merged per category by file stem. Load-time
  only. No runtime mutation, no discovery, no per-variable metadata.
- **`HostConfig`** (`ArcaneClient/src/Arcane/Host/HostConfig.{hpp,cpp}`) — the typed host CLI.

What neither provides, and what a cvar system is actually for: runtime `set`, enumeration and
autocomplete, per-variable metadata (type, bounds, help, flags), change callbacks, and a policy
model saying *who may change what, from where*.

`data/EngineConfig/diagnostics.json` is literally `{"drawMarkers": false}` — a cvar in everything
but name and mutability. The gap is real.

**Spec-now-build-later** is deliberate. The engine is mid-3D-pivot with F2b next and Arc A at
8/13 tasks. The spec's job is to stop an ad-hoc first implementation foreclosing the flag space
and the precedence model, both of which are expensive to retrofit and cheap to decide now.

---

## 2. Prior art, and the provenance boundary

Four systems were read. The boundary matters and is stated so nobody re-derives it later:

| Source | Access | Standing |
|---|---|---|
| **Source 1** (`source-sdk-2013`) | Valve-published source, fully readable | Primary reference for the **policy/flag model** |
| **Source 2** (CS2/Dota 2) | Public docs + community reverse-engineering of a shipped binary | Reference for the **type/storage model** only, at design level |
| **Unreal** (`IConsoleManager.h`) | Local reference dump | Source of the **`SetBy` provenance** idea |
| **Vulkan Guide** cvar chapter | Public article | Source of the **hot-path layout** idea |

**Standing rule honoured: no leaked Source 2 code, ever.** Source 2 informs this design only
through publicly documented behaviour and community RE of a shipped binary — design decisions,
never code. Adopting a design decision is not transcribing an implementation.

**A conflict that dissolves on inspection:** Source 2's flags carried over from Source 1 largely
intact. Taking flags from the readable Source 1 header and storage shape from Source 2 is not a
compromise between two engines — it *is* Source 2, with the half we can actually read.

Where our knowledge is asymmetric: Source 2's registration, threading and replication mechanics
are not publicly documented. Source 1 remains the only documented model for **how the registry
behaves**; Source 2 is the model for **what a cvar is**.

---

## 3. Type and flag vocabulary

### 3.1 Types

Source 2's typed model, narrowed by YAGNI. Implemented in v1:

`Bool`, `Int32`, `Int64`, `Float32`, `Float64`, `String`

Reserved in the enum, accessors deferred until a real caller exists: `Color`, `Vec2`, `Vec3`,
`Vec4`. The enum is a persistence and tooling contract, so leaving numbering holes is worse than
declaring the values; implementing accessors before a caller is speculative.

**Dropped deliberately:**

- `Int16` / `UInt16` — no plausible caller.
- **`Qangle`** — Source's Euler-angle type. The 3D pivot committed to one `Transform` with
  quaternion rotation. A Euler cvar type would be a standing invitation to precisely the class of
  bug that decision exists to prevent.

Values are stored **typed**, not string-primary. Default, min and max are typed values on the
variable, not parsed from strings on demand. Source 1 stored strings with parsed numeric caches;
Valve rewrote that into a typed union for Source 2, which is about as strong a signal as the
industry offers.

### 3.2 Flags

| Family | Flags | Notes |
|---|---|---|
| Visibility | `Dev`, `Hidden` | `Dev` is **compiled out** in Dist; `Hidden` cannot be compiled out but never appears in find/autocomplete |
| Access | `UserSettable` | **The mark that satisfies default-deny (§7.2).** Without it a cvar is unreachable from a player-facing console, whatever else is set |
| Persistence | `Archive` | Persists to the user Config layer, subject to §5.3. **Implies `UserSettable`** — a value the user is invited to persist is one they are invited to change |
| Cheat | `Cheat` | Gated on an `sv_cheats` equivalent; bulk-reverted when it clears (§7.2) |
| Authority *(reserved)* | `Replicated`, `ServerOnly`, `NotConnected`, `Protected`, `ServerCanExecute`, `ClientCmdCanExecute` | Declared now, enforced when networking exists (§3.3) |
| Reaction | `ReloadShaders`, `ReloadMaterials` | Fire at the publish barrier (§6.3) |
| Automation | `Deterministic` | **Ours, not Source's** (§3.4) |

**`Protected` is taken exactly as Source has it:** a secret-bearing variable transmits `1` if
non-blank and `0` otherwise, never its value. Presence without disclosure. This is a good
primitive that would otherwise be invented badly under deadline.

### 3.3 The authority flags and the multiplayer shape

The authority flags are declared **in the engine** regardless of whether multiplayer ends up
first-class or a package. The engine defines the vocabulary; whoever owns the network supplies
the enforcement. This matches the standing directional rule — generic capability lives in Arcane,
consumers contribute vocabulary and domain types on top — and means choosing "package" later
costs nothing, because the package interprets engine-declared bits.

**Rejected:** a claimable high bit-range for package-defined flags. Extensible flag spaces make
declarations non-constant and autocomplete non-enumerable, for a benefit that does not exist
while the engine owns the vocabulary.

### 3.4 `Deterministic` — ours, and why

Arc A's research established that **a declared knob is not an applied knob**: UE declares
thirteen capture-time CVar swappers and applies twelve, with one constructed, never `Set`, and
its `Restore()` commented out — while the declared boolean still reads as implemented, and
nothing in their type system notices.

`Deterministic` marks a cvar as one that capture correctness depends on. Because the registry is
enumerable (§4.5), the set is walkable: `-SelfTest` or the golden gate can assert every
`Deterministic` cvar sits at its expected value before a capture, rather than trusting a comment.
This is also the natural home for the capture-time CVar equivalents that the automation spec
explicitly deferred to F4.

---

## 4. Registry, declaration, and module lifetime

### 4.1 Declaration

```
ARC_CVAR(render_drawMarkers, Bool, false, CVarFlags::Dev, "Draw debug markers.");
```

Name, type, default, flags, help at the declaration site, so metadata cannot drift from the
variable. Two-phase registration as Source does it: the constructor only links into an intrusive
list; real registration happens when the owning module connects. That shape exists to survive
static-initialisation order and is worth keeping for the same reason.

### 4.2 Storage lives in the registry, not the declaration object

**First deliberate divergence from Source 1.** In Source, `ConVar::GetFloat()` reads
`m_pParent->m_fValue` — a pointer chase into whichever module owns the root. With hot-reloaded
game DLLs and plugins that is the dangling-pointer-into-an-unloaded-module class this engine has
already hit once. If the registry owns value storage and the declaration object holds only a
handle, unloading a module removes declarations without invalidating anyone's read path.

### 4.3 Access by handle

`CVarHandle` = array index + generation. A stale handle is **detectable**; Source 1's `ConVarRef`
caches a raw state pointer, which is fine when nothing unloads and unsafe when things do.

### 4.4 Callbacks by ID, and module teardown

Source 2 stores a numeric `callbackId` indexed into a central table rather than a function
pointer. For an engine that unloads game modules at runtime this is not an optimisation, it is
the safety property: a stale ID is checkable, a stale function pointer is a crash.

Every declaration is tagged with its owning module. Source 1's `CVarDLLIdentifier_t` +
`UnregisterConCommands(id)` is exactly the needed primitive: one call removes a module's entries
and clears its callback IDs. The console must therefore tolerate a cvar disappearing mid-session,
which falls out naturally from handle-based access.

**Per-source history (see §5.2)** covers the other half: cvars a module *set* rather than
*declared*. Unload pops that module's contribution and the previous value re-emerges. Without it,
a plugin's fingerprints outlive the plugin.

### 4.5 Enumeration is first-class

The registry can be walked and dumped as JSON. This is what makes `Deterministic` enforceable
rather than advisory, and it yields `cvarlist` and autocomplete from the same walk.

### 4.6 Duplicate names are a hard error

Registration of an already-registered name fails loudly, naming both modules. Source aliases
duplicates via `m_pParent` because its client and server DLLs genuinely both declare `sv_gravity`;
we have no such need yet, and silent last-wins aliasing produces two knobs that look like one. If
client/server aliasing is ever needed it becomes an **explicit** declaration, never an implicit
merge.

### 4.7 Not taken from Source

`ICvar` also owns console *output* (`IConsoleDisplayFunc`, `ConsoleColorPrintf`). That bundling is
a Source-ism from before it had a separate logger. Arcane has `Arcane::Log`; the variable registry
and the log sink stay separate.

---

## 5. The Config seam and precedence

### 5.1 The cvar's name is its Config path

First name segment is the Config category; the remainder is the key path within that document.
`diagnostics.drawMarkers` reads `diagnostics.json`'s `drawMarkers`.

Consequences: the existing `data/EngineConfig/*.json` files become cvar default sources with
**zero migration**; the whole layered-JSON machinery is reused rather than reimplemented; there is
no second file format and no third config surface.

### 5.2 `SetBy` provenance replaces flat precedence

Adopted from UE, which packs *who set the value* into the flag word and refuses a weaker source's
attempt to stomp a stronger one's value. Ladder, weak → strong:

| Level | Source |
|---|---|
| `Default` | the C++ declaration |
| `EngineConfig` | `data/EngineConfig/*.json` |
| `Plugin` | an enabled plugin's `Config/` — **history-retaining** |
| `Project` | project `Config/` |
| `User` | local gitignored overrides |
| `CommandLine` | `HostConfig --set name=value` |
| `Code` | programmatic `Set()` at runtime |
| `Console` | interactive console / editor UI |

Eight levels rather than UE's fifteen. Dropped: `Scalability` / `ScalabilityGroup` (no
quality-preset system), `DeviceProfile` (single platform today — the eventual WASM target is where
it returns), `Hotfix` (no live-ops), `Preview` (F4's business), and UE's `GameSetting` /
`GameOverride` split (no settings UI yet). **Number with gaps** so inserting a level later does not
renumber.

`SetBy` is **not serialized** — it is rebuilt from the layers on every boot.

Flat precedence loses provenance the moment it finishes. Provenance is what makes §5.3 statable,
makes §7.2 non-destructive, and makes `cvar_explain` possible.

### 5.3 Archive persistence

Persist an `Archive` cvar **only when its `SetBy` is `User` or stronger.** A value that arrived
from `Project` must not be written into the user layer — that silently pins a project default as a
personal override, and it is undiscoverable afterwards. Writes go to `<user>/<category>.json` at
the key path, where the layering already expects them.

This rule cannot be stated correctly without provenance. That is the argument for §5.2 in one line.

### 5.4 Unknown keys warn

A Config category is declared either **cvar-shaped** (`render`, `diagnostics`) or
**document-shaped** (`input`, holding `actionMaps`). A key in a cvar category with no registered
cvar produces a warning and lands in an enumerable list.

A misspelled setting that silently does nothing is a failure class this engine keeps meeting. This
makes it loud for free.

### 5.5 `cvar_explain <name>`

Prints the current value, its `SetBy`, and the full history stack — the query answering *why is
this value what it is*. Impossible under flat precedence; near-free once provenance is stored.
Directly useful to automation: a capture that drifts can be traced to the layer that moved it.

### 5.6 `HostConfig` is not replaced

The existing typed host flags (`--fixed-time`, `--settle`, `--frames`, …) do **not** become cvars.
They are host-lifecycle arguments validated before anything boots, and Arc A invested real effort
in that validation. `--set` is an additional generic door into the registry, not a replacement for
the CLI surface.

---

## 6. Threading and access

### 6.1 Layout

Type-segregated value arrays with metadata in a parallel structure (Vulkan Guide's shape), rather
than a variant record per variable (Source 2's). A `Bool` read touches one byte, not a union beside
a name pointer and help string. Hot data (values) and cold data (name, help, flags, history) never
share a cache line.

`CVarHandle` is the array index, so a read is a bounds-checked indexed load — no hash, and no
pointer chase into a possibly-unloaded module.

### 6.2 The concurrency contract — deliberate divergence from both Source and UE

Source 1 reads unsynchronized and writes from the main thread, then bolts on
`QueueMaterialThreadSetValue` for render-thread-read cvars — retrofit evidence that the hazard was
found late. UE's answer is `ECVF_RenderThreadSafe`, a *declaration* that a variable is safe rather
than a mechanism that makes it so.

Neither fits an engine with multithreaded broadphase, narrowphase and solver, a single-recorder
render thread, and determinism as a first-class goal. Under Source's model a cvar changed mid-frame
means two systems in one frame observe different values: not merely a race, an **irreproducible**
one, which is the worst kind for a capture-comparison gate.

**Contract: one writable pending store, one immutable published snapshot, swapped at a defined
frame boundary.** Reads always hit the published snapshot. `Set` writes pending. Any job on any
worker reads without synchronization because the snapshot is immutable for the frame's duration —
the safety is structural, not declared.

### 6.3 What falls out

- **Change callbacks fire once, at the publish point, in a defined order** — not re-entrantly from
  whatever thread called `Set`. `ReloadShaders` / `ReloadMaterials` hook here, respecting the
  existing debounce clock.
- **`Deterministic` becomes enforceable.** A `Deterministic` cvar whose pending value differs at
  publish means the capture's inputs moved mid-run — detectable, reportable, attributable to a
  frame number.
- The publish point is a natural telemetry seam for recording which cvars moved during a run.

### 6.4 The cost, stated

A `Set` takes effect next frame, and **read-your-own-writes is deliberately not provided.** Code
that sets a cvar and branches on it immediately is doing control flow on a cvar mid-frame, which
is the pattern the snapshot exists to prevent; it should use a local. Outside the frame loop
(boot, tools, tests) publish is immediate and idempotent, so the snapshot is always valid.

### 6.5 Dependency

There is a standing owed item: a threading policy plus wrapper, with three hand-rolled copies
in-tree. A cvar system with an explicit publish barrier is either a consumer of that policy or a
forcing function for it. It must not become a fourth hand-rolled copy.

---

## 7. Console surfaces and the safety boundary

### 7.1 Commands share the registry

Source unifies commands and variables under `ConCommandBase`; so do we. One namespace, one
autocomplete, one discovery walk. A command is name + callback + flags + help, no value.
`cvar_explain`, `cvarlist` and the `Deterministic` assertion are all commands.

### 7.2 Default-deny — the one place we invert Source

Source is **default-allow**: an unflagged cvar is settable by anyone with a console. That is an
artifact of an era when the console was a dev tool that happened to ship. For an engine serving
players and servers it means every newly added cvar is a potential exploit until somebody
remembers to flag it, and the failure is silent.

**A cvar is not player-settable unless it carries `UserSettable`** (which `Archive` implies).

Enforcement needs a subject as well as an object: every console surface carries a **permission
context** — `Editor`, `Player`, or `Server` — and the registry checks the requested set against
both the cvar's flags and the requesting context. A permission context is not a UI property; the
`--set` CLI door and the server RPC door each carry one too, so the boundary cannot be walked
around by choosing a different entry point.

The evidence is in Valve's own code: `ConVar::IsCompetitivelyRestricted()` computes

```
bHasCompSettings || !(bClientCanAdjust || bInternalUseOnly)
```

— "restricted *unless* explicitly adjustable". That is Valve retrofitting default-deny for
competitive play because the original default was wrong. We start where they ended up, before
there are hundreds of declarations to audit.

**Cheat revert uses provenance.** Source's `RevertFlaggedConVars(FCVAR_CHEAT)` snaps to the
constructor default, discarding legitimate config-layer values. With history, revert pops the
`Console` and `Code` contributions and lets the config layers re-emerge — same protection, no
collateral damage.

### 7.3 The three surfaces

- **Editor** — full access; `Dev` and `Hidden` visible.
- **Runtime / in-game** — default-deny; only explicitly user-settable and `Archive` cvars, `Cheat`
  gated. `Dev` cvars are compiled out in Dist, so they are unreachable rather than merely hidden.
- **Server** — rides the services' **existing** authenticated internal RPC (HMAC-SHA256 signed,
  shared secret from `APHELYON_INTERNAL_SECRET`, bound to `127.0.0.1`) rather than opening a second
  listener with its own auth story. Every remote set lands in the existing audit log. `Protected`
  does real work here.

### 7.4 Not built

**Source's `exec`.** Config-file execution is largely redundant against the layered Config, and a
second scripting path for setting values is a second thing to secure. `--set` plus the config
layers cover it; Arc A's repro-command line covers reproduce-this-run.

---

## 8. Console access, per host

**One console model, two presentations.** The model — history buffer, input line, registry access,
autocomplete — is engine-owned and lives in ArcaneClient. Only presentation differs.
`works-in-editor-broken-in-runtime` is a standing bug class here, and it bites hardest when two
hosts grow their own copy of a mechanism. Share the model and behaviour cannot diverge.

**The rule, which has no branches:**

> The console presentation is a property of the host, and the host is determined by **where the
> game's window is**. Game in the editor viewport → the editor's **Console tab**. Game in its own
> window → the **in-game overlay**.

- **Editor viewport play** → the existing **Console tab** gains the cvar input line and
  autocomplete. (Problems is a separate tab and stays as it is; it has no business growing an
  input line.) The toggle focuses that panel rather than raising an overlay — panels are the
  editor's idiom.
- **Standalone launch from the editor** → no special case at all. `DoLaunchStandalone`
  (`EditorAppScene.cpp`) resolves `ArcaneRuntime.exe` beside `ArcaneEditor.exe` and **spawns a
  separate process**, so it *is* the runtime host and gets the overlay for the same reason a
  shipped game does.
- **Standalone runtime** → ImGui overlay, drawn last so it sits above any debug ImGui the game
  DLL draws.

**The toggle is an input action, not a hardcoded key.** `console_toggle` joins the existing action
map beside `toggle_stats`, defaulting to `~` by convention but rebindable through existing
machinery. Hardcoding `~` would be the one piece of input in the engine ignoring the input system.

**While open, the console captures keyboard exclusively**, with ImGui's `WantCaptureKeyboard` as
the authority — it is already the arbiter in the editor, so deferring to it keeps one rule. The
console pushes an input layer suppressing game action maps for its lifetime; otherwise typing
`noclip` walks the player forward while you are still typing it.

**Dist:** the console exists, because player-facing access is one of the three drivers. It is safe
there precisely because of §7.2.

**Headless:** no console, and none required. The console is strictly a presentation over a registry
that works without it — which is what keeps the automation and server paths independent of any UI
ever being built.

### 8.1 Two consequences worth stating

- **Editor cvar changes do not propagate into a standalone-launched game.** Separate processes,
  separate registries. What carries over is anything `Archive`-flagged that persisted to the user
  layer, since the spawned runtime re-reads the same layers at boot. `cvar_explain` in the launched
  game will show `User` rather than `Console`, making it self-explaining.
- **`DoLaunchStandalone` is the natural place to forward overrides later** via `--set`, if
  "launch standalone with my current debug toggles" ever earns its keep. Not v1; it needs a real
  use case.

---

## 9. Testing

The registry is ordinary testable C++ and `ArcaneTests` covers it directly:

- Type round-trip per implemented type; bounds clamping; default retention and revert.
- **`SetBy` precedence**: a weaker source cannot stomp a stronger one; history pops correctly on
  module unload; `cvar_explain` reports the layer that supplied the value.
- **Archive persistence rule**: a `Project`-sourced value is *not* written to the user layer; a
  `Console`-sourced one is.
- **Handle invalidation**: a handle to a cvar whose module unloaded is detected, not read.
- Duplicate registration fails loudly and names both modules.
- Unknown-key warning fires for a cvar-shaped category and not for a document-shaped one.
- **Publish barrier**: a `Set` is not observable until publish; callbacks fire once, at publish.
- Default-deny: an unflagged cvar is refused from a runtime-console context and permitted from an
  editor context.

The host-level behaviours (overlay vs panel, input capture) belong to the **Arc B witness harness**
shape — `ArcaneTests` links neither host, so they are not unit-testable here. That is a known
limitation, not an oversight.

---

## 10. Scope, sequencing, and revisit triggers

**Sequencing (user's call, 2026-09-02):** spec in full now; build nothing until Arc A closes and
F2b lands.

**In scope for the eventual v1:** registry, typed cvars, flags, `SetBy` seam with Config,
publish-barrier threading, commands, editor Console-tab integration, runtime overlay, default-deny
boundary, `--set`.

**Out of scope for v1:** the vector/colour accessors (reserved only), `exec`, scalability groups,
device profiles, remote server console UI (the RPC path is specced; a UI is not), forwarding
editor cvars into a standalone launch.

**Revisit trigger — GUIDs.** Cvars are **name-addressed, not GUID-addressed**, against this
engine's general GUID direction. The rule that decides it: *GUIDs identify what content
references; names identify what code references.* Assets are content-referenced and renamed by
humans in a pipeline; a cvar is a code declaration whose every reference is recompiled or typed.
GUID-keyed config would also destroy §5.1's readable, hand-editable JSON. Rename stability is
served instead by an explicit `ARC_CVAR_ALIAS(old, new)` — greppable and auditable, where a GUID
makes renames invisible.

**Trigger to revisit:** if a cvar ever becomes referenceable **from an asset** (e.g. a
settings-menu asset binding a slider to a cvar, serialized into a `.arc*` file), cvars have entered
the content-addressing model and should gain GUIDs at that point.

**Revisit trigger — multiplayer shape.** The authority flags are reserved and engine-declared so
that "multiplayer as a package" and "multiplayer as first-class" both remain open. Deciding that
question does not reopen this design.

---

## 11. Decisions log

| # | Decision | Rationale |
|---|---|---|
| 1 | Serve dev + player + server from one substrate | User's call; retrofitting a safety boundary onto a shipped console is the expensive version |
| 2 | Registry owns scalars; Config keeps documents | `input.json`'s `actionMaps` is not cvar-shaped and never will be |
| 3 | Source 2's type model, Source 1's flag model | They are the same flags; only Source 1's are readable |
| 4 | Drop `Qangle`; reserve vectors/colour unimplemented | Pivot committed to quaternion rotation; enum is a contract, accessors are not |
| 5 | `Deterministic` flag added | Makes "a declared knob is not an applied knob" structurally checkable |
| 6 | Registry-owned storage, handle access | Dangling pointers into unloaded modules are a known in-tree bug class |
| 7 | Callbacks by ID, not function pointer | A stale ID is checkable; a stale pointer is a crash |
| 8 | Duplicate names are a hard error | Silent aliasing yields two knobs that look like one |
| 9 | Name is the Config path | Zero migration; reuses the existing layering wholesale |
| 10 | `SetBy` provenance over flat precedence | Makes the archive rule statable and `cvar_explain` possible |
| 11 | Archive only when `SetBy >= User` | Otherwise a project default silently becomes a personal override |
| 12 | Publish-barrier snapshot for reads | Determinism is first-class; a mid-frame change is an irreproducible race |
| 13 | No read-your-own-writes | The pattern it would enable is the one the barrier prevents |
| 14 | Default-deny, inverting Source | Valve's own `IsCompetitivelyRestricted` is them retrofitting it |
| 15 | Server access rides the existing signed RPC | Reuses real auth instead of adding an attack surface |
| 16 | Console presentation follows the game's window | Standalone-from-editor is a separate runtime process, so it needs no special case |
| 17 | Names, not GUIDs, with a stated revisit trigger | GUIDs identify what content references; names identify what code references |
