# ArcaneCore becomes the shared engine DLL — N worlds per process and a real dedicated server

**Date:** 2026-09-15
**Status:** Approved (user ruling 2026-09-13 "Core = the shared engine DLL"; the
shape below approved in-session 2026-09-15). Implementation plans to follow via
`superpowers:writing-plans` — see §11 for the two-plan split.
**Research:** `docs/research/2026-09-15-core-dll-split-context-brief.md` (current
state: build shape, the six edges, per-module statics, `Runtime`'s member split,
logging, ABI 29, Gacha's static-CRT source build, the single `ARCANE_API`, LOC);
`docs/research/2026-09-15-core-dll-split-ue-inspiration.md` (the UE shape and the
verdict on the module fork); `docs/research/2026-09-15-core-dll-split-engine-survey-a.md`
and `-b.md` (nine open engines + Unity/Unigine: the option space, the singleton
trap, one-world-dual-role listen servers, Unity DOTS `[WorldSystemFilter]`,
separate-process PIE). Every design statement below traces to one of those four.
**Prior law this builds on:** `docs/specs/2026-09-13-game-module-boilerplate-design.md`
(`ARCANE_GAME_MODULE`, engine-owned systems, ABI 29),
`docs/specs/2026-09-13-arcbuild-driver-design.md` (the game-module build driver,
`--engine` explicitly unbuilt).

---

## 1. Two DLLs, one seam

**Goal.** One engine for every Starworks game, single- and multiplayer, with all
behaviour in C++ modules; the server integrated into the editor in-process; the
Gacha backend services and the Aphelyon sim server both linking the same engine
DLL. Today `ArcaneClient.dll` is the whole engine and `ArcaneCore` is a static
lib compiled into whoever wants it (context brief §1) — a headless server would
have to link the renderer to get a scene. This arc draws the seam that makes a
headless host possible and makes the game module the only place behaviour lives.

### 1.1 The two modules

| | `ArcaneCore.dll` | `ArcaneClient.dll` |
|---|---|---|
| Export macro | `ARCANE_CORE_API` (new, `ArcaneCore/src/Arcane/Core/Api.hpp`) | `ARCANE_API` (today's `ArcaneClient/src/Arcane/Base/Api.hpp`) |
| Links | — | `ArcaneCore.dll` |
| Contents | today's ArcaneCore (`Net`, `Crypto`, `Cli`, `Guid`, `Jobs`, `Build`, `Util`) + `Base` (`Log`, `Diagnostics`), `Project`, `Plugin` (`PluginHost`, `PluginABI`, ImGui as `void*`), `Scene` data + physics, `Serialization`, `Sim`, `Jobs`, `Assets`, the asset formats (`Material`, `Sprite`, `Scene`, `Mesh`), and the headless `Runtime` | `Render`, `ImGui`, `Input`, `Audio`, `Platform`, `Host` boot, `RenderSystems`/`MeshSubmissionSystem`, `Edit`, `ClientRuntime` |

`Assets/` sits in Core. The brief's LOC census (§8) groups it with presentation
because of where the folder lives today, but that census is a size tally, not an
edge audit: a direct grep of `ArcaneClient/src/Arcane/Assets/` for includes of
`Render`/`Nri`/`Platform`/`ImGui`/`Input`/`Audio`/`Host` returns **zero hits**.
It is already Core-clean, and it has to be in Core anyway because the headless
`Runtime` keeps `AssetsFacade()` (§2). GPU-side residency (`NriTextureCache`,
`NriMeshBufferCache`) is `Render/` and stays Client, consuming Core's CPU-side
identity — the dependency direction the brief already blessed (§2, F2c note).

### 1.2 Hosts and linkage

| Target | Links |
|---|---|
| `ArcaneServer` (new, §6) | Core |
| `ArcaneRuntime`, `ArcaneEditor` | Core + Client |
| `ArcaneTests` | Core + Client |
| game modules (`Aphelyon.dll`, `ReferenceGame.dll`, the hot-reload plugins) | both import libs |
| `arccook` | Core |
| `arcbuild` | Core + Client today; the split makes Core-only correct (§10) |

The existing rule "ArcaneCore links into exactly ONE module per process"
(`premake5.lua:563-570`) becomes "exactly one *DLL* per process, and it is
`ArcaneCore.dll`" — every current direct-static-link consumer re-verified against
that (brief, constraint 2).

### 1.3 Prep, before any of the above

Four tasks that are correct independent of the split's shape, so they land first
and keep the tree green on their own:

1. **`MeshData`/`MeshBounds` re-homed** out of `Render/MeshBuilder.hpp` into Core
   (`Mesh/` or `Scene/` — the plan picks, both are Core). This is CPU geometry
   identity that never touched the GPU; it is only in `Render/` by accident of
   where the builder was written.
2. **The six edges cut** (brief §2, unchanged since 09-13 and re-derived against
   current source): `Scene/MeshSubmissionSystem.hpp` and `Scene/RenderSystems.hpp`
   **move to Client** (they include `Render/Nri/nodes/MeshNode.hpp` and
   `Render/Batcher2D.hpp` — they are presentation wearing a `Scene/` path);
   `Scene/SceneResources.hpp` stops including `Render/MeshBuilder.hpp` (task 1
   closes it); `Base/Runtime.hpp`/`.cpp` lose the `Input/InputSnapshot.hpp` and
   `Audio/AudioDevice.hpp` includes (§2 lifts those members into `ClientRuntime`).
   `Edit/Gizmo.cpp` → `Render/Batcher2D.hpp` is the sixth and needs no work:
   `Edit/` stays Client, so it is presentation depending on presentation.
3. **The `Runtime` member lift** (§2) — mechanical, before the physical file move.
4. **The 92 `ARCANE_API` sites re-audited** (brief §8): each declaration becomes
   `ARCANE_CORE_API` or stays `ARCANE_API` according to which DLL defines the
   symbol. This is the single largest mechanical cost of the arc (§12).

C4251 stays disabled workspace-wide and everything stays `/MD` — the two rules
that make a second DLL boundary cheap here (`premake5.lua:4-7,31-39`), and the
reason `ArcaneCore` (already `staticruntime "off"`) needs no CRT change.

## 2. `Runtime` splits into two classes

`Runtime` is today one pimpl'd class mixing headless and presentation members in
one object (brief §4). It splits into two types, one per DLL — option (a) of the
brief's fork, because the workspace has no precedent for one class defined across
a DLL seam and Flax/UE both keep the engine facade whole inside its own module.

**`Arcane::Runtime` (Core, headless).** `Registry()`, `Schedulers()`, `Loop()`,
`TypeContext()`, `WorkScheduler()`, `TaskExecutor()`, `Jobs()`, `Components()`,
`AssetsFacade()`, `Configuration()`; the project surface
(`OpenProject`/`CurrentProject`/`CloseProject`/`RegisterCreatedAsset`/
`SetProjectBootScene`/`RestampProjectEngineAbi`); the hot-reload surface
(`SnapshotRegistry`/`RestoreRegistry`/`ResetRegistry`/`ClearSystems`); and the
Manifold2D-free physics surface (`InstallEngineSystems`/`EnsurePhysics`/
`PhysicsEditPass`/`ResetPhysics`/`ResolvedGravity`).

**`Arcane::ClientRuntime` (Client).** **Owns** a `Runtime` (composition, not
inheritance — the seam is a member, so no vtable crosses it) and adds exactly the
presentation members the brief enumerates: `AudioSystem()`, `SetInputSnapshot`/
`Input()`, the ImGui handoff (`SetImGui`/`ImGuiContext`/`ImGuiAlloc`/`ImGuiFree`/
`ImGuiUserData`), and the render bridge (`SetRenderContext`/`SetSpriteMaterials`/
`SetSpriteTable`/`SetMeshTable`/`SetMeshMaterials`/`SetCamera`/`CameraOffset`/
`CameraZoom`).

**Invariant: no class straddles the seam.** Every type is defined, exported, and
destroyed inside one DLL.

**`EngineContext`** — the plugin-facing struct — points at the headless `Runtime`
plus an **optional client-extension pointer**, null on the server. A module that
wants audio or the render bridge checks the pointer, exactly as it already checks
`ctx->imguiContext` for null in a headless host (game-module spec §3.2). Across
this arc `EngineContext` gains exactly three things and no others: that
client-extension pointer, the `ProcessContext` (§3), and the `Runtime`'s
`NetMode` (§4) — the same three §8 and R7 cite as the ABI 30 justification.

## 3. `ProcessContext` — process-wide state, owned once, by Core

A new Core object, created **exactly once per process** by the host
(`ArcaneServer`, `RuntimeApp`, `EditorApp`, `ArcaneTests`). It owns:

- **The Astra `TypeContext`.** Moves from host-allocated (`Host/ProjectBoot.cpp:151-154`)
  to Core-owned. Every `Runtime` and every loaded module imports it, so component
  IDs are shared by construction rather than by convention.
  `VerifySharedTypeContext` keeps verifying — the gate does not go away because
  the owner changed.
- **The Diagnostics sink.** `Diagnostics::SetSink`/`Publish`/`Clear` move from
  `ArcaneClient/src/Arcane/Base/Diagnostics.hpp` into Core. Reason: it is already
  documented as "process-wide, last writer wins", and a headless `arccook` or
  `ArcaneServer` must be able to publish diagnostics without linking Client. One
  owning DLL exports the storage; Client imports it — UE's `GConfig` pattern
  (`CoreGlobals.h:96`, storage in Core, read and written from above).
- **The engine logger.** `Log::Engine()` and the `ARC_*` macros move to Core;
  Client re-exports the macros so no Client call site changes. `Arcane::Logger`
  (`ArcaneCore/src/Arcane/Util/Logger.hpp`) stays a subclassable class in Core —
  Gacha's `Aphelyon::Logger` derives from it and must keep doing so. **Merge
  rule, stated once:** one engine logger, in Core; per-module spdlog registries
  and per-module Mosaic sink copies remain per-module **by design** (both are
  documented per-module-copy contracts today, `Base/Log.hpp:1-9,24-31`) — Core
  gaining its own copy is existing practice, not a new problem class.

`JobSystem` stays **per-Runtime**: it is already instance-owned by `Runtime::Impl`
("a fixed part of this Runtime's substrate"), not a hidden global, and N worlds
want N job substrates, not one shared pool.

**Invariant, tested (§9): N `Runtime`s per process share one `ProcessContext`;
constructing a second `ProcessContext` is a refusal, not a warning.**

This rule is the survey's singleton trap written down. O3DE's `AZ::Interface<T>`
holds **one slot per type per Environment**, which is structurally why O3DE's
default multiplayer PIE has to spawn a separate `ServerLauncher` process — the
registry has no room for two `IMultiplayer`s. CryEngine's `gEnv` is a flat struct
of ~45 raw pointers that every DLL compiles against byte-identically, and its
Sandbox Game Mode therefore flips **one** world in place; a second world is never
allocated. Both engines lost N-worlds-per-process to their process-wide registry
layer. Arcane pays that price exactly once, deliberately, for the state that is
genuinely process-wide (type IDs, the diagnostics sink, the logger) and for
nothing else — everything per-world lives on `Runtime`.

## 4. Net mode and role — two fields, never one

Two independent fields, never collapsed into one:

1. **`ProcessContext::IsDedicatedServerProcess`** — a process launch flag. How
   was this executable booted.
2. **`NetMode { Standalone, DedicatedServer, ListenServer, Client }`** — per
   `Runtime`, carried on `Runtime` and surfaced on `EngineContext`.

`Runtime::HasAuthority()` is `mode == DedicatedServer || ListenServer || Standalone`
— the same predicate as UE's "NetMode < NM_Client is always some variety of
server" (`EngineBaseTypes.h:944`).

**Systems branch on the per-Runtime field, never on the launch flag.** UE names
this exact conflation a bug class: `IsRunningDedicatedServer()`'s own doc says
"should not be used for gameplay or networking purposes, check NetMode instead"
(`CoreMisc.h:149-150`), and `UWorld::IsNetMode()` re-derives from `GetNetMode()`
in editor builds "because of PIE, which can run a dedicated server without
`-server`" (`World.h:4583`). An editor process running an embedded server world
(§7) is precisely that case: `IsDedicatedServerProcess` is false while a `Runtime`
in it is genuinely an authoritative server.

**Role-masked system factories.** The game module registers, once per process at
DLL load, alongside the Astra component registrar it already drains: a table of
**system factories, each carrying `RoleMask { Server, Client, Both }`**. Each
`Runtime` walks that table at construction and instantiates only the factories
whose mask matches its own `NetMode`; `ListenServer` matches `Server|Client`.

This is the third shape the UE research names, and neither fork the brainstorm
opened: not a module object per world (UE never re-instantiates a module —
`FModuleManager::Modules` is keyed by module name), and not a second loaded copy
of the module DLL (UE has no mechanism to load a module twice; fork B has zero
precedent anywhere in the survey). UE registers classes once into a process-wide
class table and stamps instances per world through
`FSubsystemCollectionBase::Initialize`, gated by the CDO's `ShouldCreateSubsystem`.
Unity DOTS is the same shape with the role mask made explicit and public:
systems are declared once with `[WorldSystemFilter(WorldSystemFilterFlags…)]`,
each `World` carries `WorldFlags` (`GameServer`/`GameClient`/`GameThinClient`),
and `DefaultWorldInitialization.GetAllSystems(flags)` intersects the one global
system list against a world's role to populate it. Survey B calls DOTS "the
closest public analogue to Arcane's target shape"; the branching is **structural**
(which world a system exists in) rather than a runtime `if`, which is what makes
the compile-out follow-on in §10 possible later.

Arcane already has the registration half: `ARCANE_GAME_MODULE`'s prologue opens
the module's `Astra::ComponentModule` and drains the `ARCANE_COMPONENT` registrar
once per DLL load. The system-factory table is the missing half, and it preserves
the 09-13 ruling verbatim — "systems stay explicit, their order is a design act":
a factory is registered by an explicit line in the module's `OnInit`, with an
explicit mask, and Astra's `Before`/`After` traits still place it. Nothing
self-registers a system through static initialisation.

**`ListenServer` is the one-world dual-role mode.** One `Runtime`, authoritative,
with the host's local client presentation reading that same authoritative world
directly — no second world, no loopback duplication of state. That is id Tech 4
BFG's listen server (one shared `gameLocal`, role queried live per call site),
Quake III's (`SV_Frame` + `CL_Frame` from one `Com_Frame`, `"localhost"` trapped
to an in-process loopback pair), and lightyear's host-server mode, which
**deliberately collapses** what could be two Bevy Worlds into one for a single
authoritative state — the one surveyed engine with a real N-worlds primitive has
its flagship networking user not using it for client+server-together.

**Replication and transport are not this arc.** This arc lands the vocabulary
(`NetMode`, `RoleMask`, `HasAuthority()`) and the branch points. The replication
arc will add: a net driver per `Runtime`, connection↔entity bindings, ghost/
replicated-component vocabulary, and a per-entity authority notion (UE's
`ENetRole` on `AActor`, O3DE's `NetEntityRole` on `NetBindComponent`, DOTS'
`GhostOwner`) — every surveyed engine that ships replication has a per-entity
role *in addition to* the per-world one, so the entity-level field is expected,
just not now.

## 5. Hot reload with N Runtimes

The module-swap transaction becomes: **snapshot ALL Runtimes → reload the DLL
once → re-run registration → restore ALL Runtimes**. Never per-Runtime and never
sequentially: the instant the DLL unmaps, every Runtime's descriptors dangle, so
a per-world reload would leave world two holding pointers into an unmapped image.
The per-Runtime halves already exist (`SnapshotRegistry`/`RestoreRegistry`), and
`ClearSystems()` already reinstalls the engine systems afterwards; what is new is
the orchestration across the process's Runtime set. Re-running registration means
both halves: the component registrar drain (exists) and the system-factory table
(§4), which every restored Runtime re-instantiates from.

**Refused while any Runtime has an active net driver** — a `PluginHost`
precondition returning a clear diagnostic naming the offending Runtime, not a
silent no-op. UE refuses the same case for the same reason: `FHotReloadModule::Tick`
returns without acting if any world context is `EWorldType::PIE` with a non-null
`NetDriver` (`HotReload.cpp:1278-1288`), commented as re-wiring possessed
pawns/controllers across networked PIE being too complicated. Replication state
and connection↔entity bindings are cross-Runtime references a per-Runtime
snapshot cannot repair. (UE's Live Coding carries no such guard and accepts the
documented crash risk; that is a different, later regime.)

Until the replication arc lands there is no net driver, so in practice this
precondition is dormant vocabulary that the replication arc turns on — which is
exactly why it belongs here, beside the `NetMode` it keys off.

## 6. `ArcaneServer.exe`, real

A Core-only host. It creates the `ProcessContext` and **one** `Runtime` in
`DedicatedServer` mode, opens the project, loads the game module, and ticks
scene/sim/physics headless at a fixed step. It never constructs a swapchain,
audio device, input poller, or ImGui context — **an init-time construction gate,
not scattered `if (isServer)` checks**. UE's decisive server-boot gate is exactly
this: `FSlateApplication::Create()` simply never runs on a dedicated server
(`LaunchEngineLoop.cpp:2910-2921`), so the renderer/viewport/input stack does not
exist for the process's life, while `GEngine->Tick(...)` is still called
unconditionally — simulation continues. Downstream code asks "is this subsystem
present", mirroring `FSlateApplication::IsInitialized()`. Arcane's structural
advantage over UE here is that the gate is a **link line**: `ArcaneServer` does
not link `ArcaneClient.dll`, so there is no renderer to accidentally construct.

CLI mirrors the other hosts: `--project`, `--frames`, `--report` (a census, the
`[witness]` shape the automation arc established), `--fixed-dt` for the step, and
`--headless` implied by the target rather than passed. Tick rate is a **runtime**
value, never a compile-time constant — UE's `t.MaxFPS`, DOTS' `ClientServerTickRate`
(which even picks `Sleep` for dedicated-server builds and `BusyWait` for
client+server ones); Godot and Flax both reach headless through a runtime flag on
the same binary rather than a second build product.

This is the natural **first Linux target** — it is the only host with no
swapchain, no SDL window, and no D3D12 — but **Linux stays a later milestone**:
this repo's Jenkinsfile declares `windows && gpu` only, and there is no Linux lane
here to be green in (brief, constraint 11).

**What `ArcaneServer` is not:** Aphelyon's backend services. Auth, Account and
Combat are **game services** — they link Core for `Net`/`Crypto`/`Log` and know
nothing about scenes, worlds, or game modules. `ArcaneServer` is the **sim
server**: the host that loads the game module and ticks the authoritative world.
Two different things that both link `ArcaneCore.dll`, and the distinction is why
§8's Gacha task is a build-system change to the services and not a rewrite.

## 7. PIE in the editor — in-process, and out-of-process

Once §3's `ProcessContext` and §4's role masks exist, the editor can create a
**second `Runtime` on the same `ProcessContext` and the same loaded module**, in
`DedicatedServer` mode, with its own world switching to `Client` — or a single
`ListenServer` Runtime. That is Unity DOTS' PlayMode Tools verbatim: client and
server worlds coexist in one process inside the editor's Play Mode, created by
`ClientServerBootstrap` and driven by a `PlayType` picker
(`Client`/`Server`/`ClientAndServer`), enabling "immediate Editor iteration
testing of your multiplayer game".

**Scope, this arc:** the multi-Runtime foundation plus a **minimal picker** —
"Play as: Standalone / Listen server / Client + embedded server" — that stands
the Runtimes up and ticks them. No replication traffic (there is none yet), no
new panels beyond the picker, no per-world inspector. The picker's third option
stands up two Runtimes that tick independently and share nothing but the
`ProcessContext` and the module; proving that is the point of the arc, and it is
what §9's host-level scenario reports.

**Out-of-process PIE is a supported mode of the same picker**, not a rival
design: spawn `ArcaneServer.exe` as the host and have the editor connect to it.
It is cheap once §6 exists (the exe is the whole implementation), it is the
*honest* network test because the traffic is real, and it is what Godot
(`OS::create_instance`), Unity MPPM ("each additional Editor is a separate child
process"), O3DE (`ProcessWatcher::LaunchProcess` + loopback TCP) and UE
(`bLaunchSeparateServer`, and forced for `PIE_Client`) all do by default. The
in-process mode buys iteration speed; the out-of-process mode buys fidelity; the
picker offers both because the survey shows every mature engine ends up wanting
both. It lights up fully when the replication arc lands — until then it proves
process bring-up and project load, which is still worth having.

**`GWorld` is the cautionary template.** UE's mutable "current world" pointer is
swapped by `SetPlayInEditorWorld` around operations that assume "the" world; any
transitional "current Runtime" pointer in Arcane must follow the same explicit
save/restore discipline and must never become ambient state. Preferred: don't
introduce one. The editor knows which Runtime it is acting on.

## 8. Boundary mechanics, ABI, and Gacha

**The export macro.** `ARCANE_CORE_API` in a new Core `Api.hpp`, exporting inside
`ArcaneCore.dll`'s own TUs (keyed off an `ARCANE_CORE_BUILD_DLL` define in that
project) and importing everywhere else. This is UE's per-module macro pattern
reduced to what premake can express: UBT computes `<MODULE>_API` per
including-vs-defining-module *relationship* (`UEBuildModule.cs:674-700`, zero
header definitions anywhere in the tree), and premake's per-project `defines{}`
is the coarser equivalent — fine, because Arcane has no monolithic/modular axis
to disambiguate.

**ABI.** `kGamePluginABIVersion` 29 → **30**, bumped **once, at the end of the
arc**, when the last contract change lands. Three things justify it and they are
better as one bump than three: headers move between DLLs (a module compiled
against 29's layout is a cross-build); `EngineContext` gains the net mode, the
optional client-extension pointer, and the `ProcessContext`; and the module
contract gains system-factory registration with role masks. The standing policy
is that bumps are cheap during engine dev — the value of the bump is that the
host's gate makes a cross-build **visible** rather than letting it pass by
accident.

**Restamps and rebuilds, in the same commit as the bump:**
`ReferenceProject.arcproj` and Gacha's `Game/Aphelyon.arcproj` to 30;
`ReferenceGame` and the `HotReload` test plugins (V1/V2/Bad) recompiled — the Bad
plugin's `kGamePluginABIVersion + HOTRELOAD_ABI_OFFSET` keeps working by
construction.

**Gacha services → `/MD`.** The three services move from the from-source,
static-CRT `ArcaneCore` build to linking `ArcaneCore.dll`, with libpq rebuilt on
the already-present-but-unused `x64-windows-static-md` overlay triplet
(`VCPKG_CRT_LINKAGE dynamic`, `VCPKG_LIBRARY_LINKAGE static`, toolset v143).
`Server/premake5.lua`'s `ArcaneCore` project (its own source-list compile of
`$ARCANE_SDK/ArcaneCore/src`, `staticruntime "on"`) is deleted in favour of a
link line; `Common` and the three services flip to `staticruntime "off"`. This is
**the arc's last task, as a Gacha-repo PR** — until it merges, the services keep
the from-source static build, which stays valid because the Arcane-side change
does not remove the sources it compiles.

## 9. Testing

**Device-less (`[runtime]`, `[plugin]`), the load-bearing suite:**

- `ProcessContext` single-ownership: one per process; a second construction is a
  refusal (§3's invariant, tested as a refusal and not merely documented).
- **Two Runtimes, one loaded module, role-masked instantiation:** register a
  `Server`-masked and a `Client`-masked factory; assert the Server-masked system
  is **absent** from the Client Runtime and **present** in the Server one, and
  the converse. This is the §4 mechanism's whole claim, so it is pinned directly.
- `ListenServer` instantiates **both** masks in its one Runtime.
- Snapshot-all / reload / restore-all across two live Runtimes: both worlds'
  state survives one DLL swap, and the re-registered factories repopulate both.
- Net-mode/launch-flag **independence**: a Runtime in `DedicatedServer` mode
  inside a process whose `IsDedicatedServerProcess` is false behaves as a server
  (`HasAuthority()` true) — the §4 bug class, pinned so it cannot regress.
- `HasAuthority()` across all four modes.
- Hot reload refused while a Runtime reports an active net driver — using a test
  double for the driver until the replication arc supplies a real one.

**Host-level (`[gpu][witness]`):**

- `ArcaneServer --report`: a census proving the project opened, the module loaded,
  the world ticked N frames, and no presentation subsystem exists.
- A headless editor scenario that stands up the embedded-server Runtime via the
  §7 picker and reports **both** worlds.

**Golden.** Both lanes and the thumbnail set **must not move**. This arc changes
no pixel path: it relocates declarations, splits a facade, and adds a second
module boundary. `golden-gate.ps1` 4/4 in both configs, run and not assumed, is
the regression net for exactly the two risky mechanical changes — the `MeshData`
re-home (§1.3) and the `Runtime` split (§2). A `diffCount` other than 0 means
something moved that should not have; there is no re-bless in this arc.

Suites run from the exe dir; count deltas attributed per task, as always.

## 10. Non-goals

| Out of scope | Why / trigger to revisit |
|---|---|
| Replication and transport | §4 lands vocabulary and branch points only. Its own arc, which will add net drivers, connection↔entity bindings, and per-entity authority. |
| Linux CI | No Linux lane exists in this repo's Jenkinsfile. §6 makes the server the natural first target; the milestone schedules it. |
| An editor-lib split (`Edit/` into its own module) | `Edit/` stays in Client. Its only presentation edge is presentation→presentation, so it costs nothing where it is; a third DLL buys nothing this arc needs. |
| `arcbuild --engine` | Explicitly unbuilt today (arcbuild spec §6). The split adds a second engine DLL for that eventual command to reason about; noted, not built. `arcbuild` dropping its Client link (§1.2) is the small follow-on that this arc makes correct. |
| Server-only **compile-out** of client-masked systems | Source's compiler-enforced role split (`GAME_DLL`/`CLIENT_DLL`, `src/game/shared/` compiled twice) and Unity's `UNITY_SERVER` strip are the mature end state, and the reason is real: a shipping server should not carry client code at all. §4's role masks are the runtime half and the prerequisite; the compile-time half is a follow-on, triggered by the first shipping server build. |
| Per-domain engine DLLs | Rejected 2026-09-13 and re-confirmed by the survey: CryEngine's ~20 per-domain DLLs need one byte-identical `gEnv` struct every module compiles against with no versioned ABI between them, and its own build treats dynamic per-domain linking as a desktop dev-loop convenience (`Configure.cmake:119-127`); O3DE's per-type single-slot registry is what forces its separate-process PIE. Two DLLs with one seam is the whole shape. |
| Live Coding / networked live reload | §5's refusal is v1 policy; UE's Live Coding is the later regime, its own spec. |
| More than one game module per project | Unchanged from the game-module spec's non-goals. |

## 11. Sequencing — Plan 1 (Arcane) and Plan 2 (Gacha)

This arc needs **two implementation plans**, because it spans two repos with
independent build systems, test suites and CI pipelines, and the second cannot
start until the first has shipped a Core DLL to link against.

**Plan 1 — Arcane repo.** Each step ends green (suites + `golden-gate.ps1` 4/4,
both configs):

1. **Prep** (§1.3): `MeshData`/`MeshBounds` re-home; the six edges cut
   (`MeshSubmissionSystem`/`RenderSystems` to Client, `SceneResources` cleaned,
   `Base/Runtime` losing Audio/Input); the `Runtime` member lift. No DLL yet.
2. **The export macro + `ArcaneCore.dll` builds + every host links it** (§1, §8's
   macro half). The physical folder move happens here; the 92-site audit lands
   here.
3. **`ProcessContext` + Diagnostics/Log move to Core** (§3), with its
   single-ownership refusal test.
4. **`Runtime`/`ClientRuntime` split** (§2) — `RuntimeApp`, `EditorApp` and
   `ProjectBoot` rewired.
5. **Module contract: system factories + role masks, `NetMode`, `HasAuthority()`,
   `EngineContext`'s new fields → ABI 30** (§4, §8), with the restamps and the
   three module rebuilds.
6. **`ArcaneServer.exe` real** (§6), with its `--report` witness.
7. **Editor PIE picker** (§7), in-process modes plus the out-of-process spawn.

**Plan 2 — Gacha repo.** The `/MD` migration (§8): `Server/premake5.lua` drops
its from-source `ArcaneCore` project and links `ArcaneCore.dll`; libpq rebuilt on
`x64-windows-static-md`; `Common` and Auth/Account/Combat flip to dynamic CRT;
AccountTests and the Jenkins lanes green. Starts after Plan 1 step 2 has landed
and pushed; merges last.

## 12. Costs, stated plainly

- **The export macro across 92 files.** Every `ARCANE_API` site re-audited and
  assigned to one of the two macros. Mechanical, unavoidable, and the single
  biggest line-count in the arc (brief §8).
- **The `Runtime` split touches the hosts.** `RuntimeApp`, `EditorApp` and
  `ProjectBoot` all construct and thread a `Runtime` today; each learns the
  `ClientRuntime`-owns-`Runtime` shape (brief §4).
- **Every module recompiles.** ABI 30 plus moved headers means ReferenceGame, the
  three hot-reload plugins, and Gacha's Aphelyon module all rebuild — one
  `arcbuild build` each, by design (arcbuild spec: one compile + a link).
- **The Gacha vcpkg rebuild (~15 min).** libpq on the `-md` triplet, once.
- **One new module boundary to wire.** Link lines, a second export define, and
  the `ProcessContext` ownership handoff at each host's boot.

Against that: ~17.7k LOC moves to Core while ~49.4k stays in Client (`Render/`
alone is 30.6k). This is boundary-drawing work, not a rebalancing of engine mass
— the brief's own framing, and the reason the cost list above is short.

## 13. Rulings record

| # | Question | Ruling | Evidence |
|---|---|---|---|
| R1 | The module fork: (A) module object per world, (B) a second loaded copy of the module DLL, or (C) a third shape | **C** — the module singleton registers component types (exists) and system factories with role masks (new) once per DLL load; each `Runtime` instantiates its own systems from that registry | UE: `IMPLEMENT_MODULE` + `FModuleManager::Modules` keyed by name make one module object structurally inescapable (no precedent for B), yet UE routinely runs N worlds stamped from a process-wide class table via `FSubsystemCollectionBase::Initialize` (so not A). Unity DOTS is the same shape with the mask made explicit: `[WorldSystemFilter]` + `WorldFlags` + `GetAllSystems(flags)` |
| R2 | One net field or two | **Two, never collapsed**: `ProcessContext::IsDedicatedServerProcess` (launch) and per-`Runtime` `NetMode` (behaviour). Systems branch on the second | UE's `IsRunningDedicatedServer()` doc: "should not be used for gameplay or networking purposes, check NetMode instead"; `UWorld::IsNetMode()` re-derives "because of PIE, which can run a dedicated server without `-server`" |
| R3 | Is `ListenServer` two worlds or one | **One world, dual role** — the local client presentation reads the authoritative world directly | id Tech 4 BFG (one shared `gameLocal`, role queried per call), Quake III (`SV_Frame`+`CL_Frame`, `"localhost"`→loopback), lightyear host-server (deliberately collapses two Bevy Worlds into one) |
| R4 | Where does Diagnostics live | **Core**, exported from it; Client imports | Already "process-wide, last writer wins"; headless `arccook`/`ArcaneServer` must publish without linking Client; UE's `GConfig` owns storage in Core and is written from above |
| R5 | Does `Edit/` ship in Core, Client, or its own module | **Client** | Its only presentation edge (`Gizmo.cpp` → `Batcher2D.hpp`) is presentation→presentation; a third module buys nothing this arc needs |
| R6 | One export macro conditioned two ways, or a second macro | **A second macro, `ARCANE_CORE_API`** | UE computes a distinct `<MODULE>_API` per module relationship (zero header definitions in-tree); premake's per-project `defines{}` is the workable equivalent |
| R7 | Does the split bump the plugin ABI | **Yes — 29 → 30, once, at the end** | Headers move DLLs; `EngineContext` gains net mode + client-extension pointer + `ProcessContext`; the module contract gains factory registration. Standing policy: bumps are cheap, and the value is making a cross-build visible |
| R8 | Does Gacha move in the same change | **No — Gacha last, its own PR (Plan 2)** | Two repos, two build systems, two pipelines; the services keep the from-source static build until then, and the Arcane-side change does not remove the sources they compile |
| R9 | Is out-of-process PIE a rival design or a mode | **A mode of the same picker**, kept | Godot `OS::create_instance`, Unity MPPM child processes, O3DE `ServerLauncher` + loopback, UE `bLaunchSeparateServer` — every mature engine ships it; cheap once §6 exists, and the honest network test |
| R10 | Are per-domain engine DLLs reconsidered | **No — rejected, re-confirmed** | CryEngine's `gEnv` (one byte-identical struct, ~45 pointers, no versioned inter-DLL ABI; its own build calls dynamic per-domain linking a desktop convenience); O3DE's one-slot-per-type `AZ::Interface<T>` is what forces its separate-process PIE |

---

Where this spec is silent, the game-module and arcbuild specs stand; where they
disagree about who owns the engine facade or where a header lives, this spec wins.
