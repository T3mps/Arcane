# Closed-engine survey B: Unity (classic + DOTS/Netcode for Entities) and Unigine

Scope: public docs / public package mirrors only. Read-only research for the
"core becomes a shared DLL" spec (headless Runtime world facade, dedicated
server host, editor running an embedded server world in-process, one game
module). Unreal and open-source engines are covered elsewhere; this note
covers the two CLOSED engines whose public docs are detailed enough to
answer the six axes. "Not found" marks facts public docs don't state.

---

## 1. Unity classic (MonoBehaviour) + Netcode for GameObjects

**1.1 Engine split.** Native player runtime (`UnityPlayer.dll`, "Unity
playback engine") + managed facades `UnityEngine.dll`/`UnityEditor.dll`,
themselves a facade over per-feature module DLLs (physics, audio, UI...);
.NET Standard 2.1 as of 2022+ ([Managed plug-ins](https://docs.unity3d.com/6000.3/Documentation/Manual/plug-ins-managed.html)).
Scripting crosses the boundary via Mono JIT ("Unity's fork of ... Mono ...
powers Unity's Editor and Players") or IL2CPP ("converts IL to C++ ...
compiled into platform-specific native code")
([IL2CPP](https://docs.unity3d.com/6000.3/Documentation/Manual/il2cpp-introduction.html),
[Mono backend](https://docs.unity3d.com/6000.3/Documentation/Manual/scripting-backends-mono.html)).
How process-wide native globals cross module-DLL boundaries — **not found**.

**1.2 Game code unit.** One managed assembly, not role-split. Role is a
runtime property read off a `NetworkManager`/`NetworkBehaviour` instance
(`IsServer`/`IsClient`/`IsHost`/`IsOwner`), not a separate compiled unit
([NetworkObject ownership](https://docs.unity3d.com/Packages/com.unity.netcode.gameobjects@2.11/manual/components/core/networkobject-ownership.html)).
One `NetworkManager` singleton per process; no per-world instantiation —
scenes, not worlds, are the unit, one active scene set per process.

**1.3 N worlds in one process.** Not applicable in production, apart from
host mode: `StartHost()` runs "a special 'local' client that communicates
to the in-process server using a message queue instead of the real
network" — one world plus a loopback client shim, not two isolated worlds
([StartHost API](https://docs.unity3d.com/2017.3/Documentation/ScriptReference/Networking.NetworkManager.StartHost.html)).
For editor multi-instance testing, **Multiplayer Play Mode**
(`com.unity.multiplayer.playmode`) uses **separate OS processes**: "Each
additional Editor is a separate child process, distinct from the Editor,
that acts as additional game clients," launched by the main Editor,
sharing assets but not memory; up to 4 total Players (1 main + 3 "Virtual
Players")
([Editor instances](https://docs.unity3d.com/Packages/com.unity.multiplayer.playmode@2.0/manual/instance-types/main-and-additional-editor-instances.html),
[About MPPM](https://docs.unity3d.com/Packages/com.unity.multiplayer.playmode@2.0/manual/index.html)).

**1.4 Net mode / role / authority.** `NetworkManager.IsServer/IsClient/
IsHost` decided by which `Start*()` was called
([NetworkManager](https://docs.unity.cn/Packages/com.unity.netcode.gameobjects@1.0/api/Unity.Netcode.NetworkManager.html)).
Per-object: `NetworkBehaviour.IsOwner`/`HasAuthority` ("`HasAuthority` is
recommended [for] object-specific operations, while `IsServer`... for
global actions"; distributed-authority sessions have no single
authoritative server)
([Ownership and authority](https://docs.unity3d.com/Packages/com.unity.netcode.gameobjects@2.4/manual/basics/ownership.html)).
`NetworkVariableWritePermission` (`Server`/`Owner`) gates writes; in
distributed authority "all NetworkVariables are automatically configured
with owner write and everyone read permissions"
([enum](https://docs.unity3d.com/Packages/com.unity.netcode.gameobjects@1.6/api/Unity.Netcode.NetworkVariableWritePermission.html)).
Branching is inline `if (IsServer)`/`if (IsOwner)` — no compiled split.

**1.5 Dedicated server build.** `Dedicated Server` build target defines
`UNITY_SERVER`; code excluded via `#if !UNITY_SERVER` or per-Assembly
Definition exclusion
([Optimizing server builds](https://docs.unity3d.com/Packages/com.unity.multiplayer.tools@2.2/manual/porting-to-dgs/optimizing-server-builds.html)).
Strips "unnecessary assets and compiled code" — rendering/asset pipeline
work is called out as unneeded on servers, audio/textures/meshes/shaders
are candidates ([Dedicated Server intro](https://docs.unity3d.com/6000.0/Documentation/Manual/dedicated-server-introduction.html)).
Similar to, but more optimized than, Desktop headless mode
(`BuildOptions.EnableHeadlessMode`, `-batchmode -nographics`)
([Headless mode](https://docs.unity3d.com/6000.2/Documentation/Manual/desktop-headless-mode.html)).
Tick control = the ordinary frame loop (`Application.targetFrameRate`,
`Time.fixedDeltaTime`); no NGO-specific server tick governor documented —
**not found** (contrast §2.5).

**1.6 Hot reload with live worlds.** Governed by **domain reload**: by
default, entering Play Mode or any recompile tears down/reloads the .NET
AppDomain. **Enter Play Mode Options** can disable domain/scene reload "to
prioritize faster development iteration times over accuracy of the Play
mode simulation," but then "it is up to you to ensure your scripting state
resets" — static fields do not auto-reset
([Configuring Play mode entry](https://docs.unity3d.com/6000.0/Documentation/Manual/configurable-enter-play-mode.html),
[Domain Reloading](https://docs.unity3d.com/2021.3/Documentation/Manual/DomainReloading.html)).
No supported "hot-patch code under a running server/host world" path in
the box.

---

## 2. Unity DOTS / Netcode for Entities (`com.unity.netcode`, `needle-mirror/com.unity.netcode`)

**2.1 Engine split.** Same native/managed substrate as §1.1; Entities/
NetCode are packages on top — **not found** beyond §1.1.

**2.2 Game code unit — the key finding.** The unit is **one set of ECS
systems**, each optionally tagged `[WorldSystemFilter(WorldSystemFilterFlags...)]`,
registered exactly once; **`World`s are the per-role instantiation**. "By
default, systems are created ... for both client and server worlds ... if
you want [a system] created and run only on the client world, [put it in]
a system group present only on the desired world; systems in a system
group inherit system group world filtering"
([Client/server worlds](https://docs.unity3d.com/Packages/com.unity.netcode@1.5/manual/client-server-worlds.html)).
Flag values: `ServerSimulation`, `ClientSimulation`, `ThinClientSimulation`,
`LocalSimulation` ("a world that doesn't run any Netcode systems")
([WorldSystemFilterFlags enum](https://docs.unity3d.com/Packages/com.unity.entities@1.0/api/Unity.Entities.WorldSystemFilterFlags.html)).
The filter-to-instance mechanism is
`DefaultWorldInitialization.GetAllSystems(WorldSystemFilterFlags, bool)` —
"calculates a list of all systems filtered with WorldSystemFilterFlags,
[DisableAutoCreation] etc." — paired with
`AddSystemsToRootLevelSystemGroups` to inject that filtered list into a
given `World`'s root system groups
([GetAllSystems](https://docs.unity3d.com/Packages/com.unity.entities@0.17/api/Unity.Entities.DefaultWorldInitialization.GetAllSystems.html)).
`ClientServerBootstrap.CreateDefaultClientServerWorlds()` is the default
entry point calling this filtering machinery at startup, overridable for
custom flows ([mirror](https://github.com/needle-mirror/com.unity.netcode/blob/master/Documentation~/client-server-worlds.md)).
Each `World` carries `WorldFlags` (`GameServer`/`GameClient`/
`GameThinClient`); a `[WorldSystemFilter(ClientSimulation)]` system exists
only in worlds with `WorldFlags.GameClient` set. **Systems are declared
once at compile time; `World`s are instantiated per role at runtime; each
world's system set = the one global registry filtered by that world's role
flags** — the closest public shape to "module registers, world
instantiates with a role mask."

**2.3 N worlds in one process — confirmed in-process, editor-native.**
Client, server and thin-client `World`s coexist in **one process**,
including inside the **Editor's Play Mode**. `ClientServerBootstrap.
CreateClientWorld`/`CreateServerWorld`/`CreateThinClientWorld` are the
entry points, the last "used for soak testing ... multiple thin client
worlds for stress testing"
([ClientServerBootstrap class](https://docs.unity3d.com/Packages/com.unity.netcode@1.7/api/Unity.NetCode.ClientServerBootstrap.html)).
The manual states both worlds operate in a single process during Editor
Play Mode, enabling "immediate Editor iteration testing of your
multiplayer game" ([client-server-worlds](https://docs.unity3d.com/Packages/com.unity.netcode@1.5/manual/client-server-worlds.html)).
Driven by the **PlayMode Tools** window (`Window > Multiplayer > PlayMode
Tools`): "set the PlayMode Type to make this clone act as a Client, a
Server, or both a Client & Server" — controls whether `ClientServerBootstrap`
creates a client world, server world, or both, configures thin-client
counts, and lets you "view, control, and debug ... client and server
worlds ... (only available after entering Play mode)"
([PlayMode Tool](https://docs.unity3d.com/Packages/com.unity.netcode@1.13/manual/testing/playmode-tool.html)).
Thin clients are in-process `World`s too, not separate processes.

**2.4 Net mode / role / authority.** Role lives on the `World` as
`WorldFlags` (`GameServer`/`GameClient`/`GameThinClient`), set at
world-creation by `ClientServerBootstrap`, mirrored in-editor by `PlayType`
(`Client`/`Server`/`ClientAndServer` via `RequestedPlayType`)
([PlayMode Tool](https://docs.unity3d.com/Packages/com.unity.netcode@1.13/manual/testing/playmode-tool.html)).
Entity-level authority is `GhostOwner` ("optional component ... to create a
bond ... between an entity and a specific client"; owner-predicted mode
predicts for the owner, interpolates for everyone else, compared against
`GhostOwner.NetworkId`) ([GhostOwner](https://docs.unity3d.com/Packages/com.unity.netcode@1.0/api/Unity.NetCode.GhostOwner.html)).
`GhostAuthoringComponent` configures replicated ghost types (Name,
Importance, SupportedGhostModes, DefaultGhostMode); `PredictedGhost` is
auto-added when baking targets Client/ClientAndServer
([GhostAuthoringComponent](https://docs.unity.cn/Packages/com.unity.netcode@1.9/api/Unity.NetCode.GhostAuthoringComponent.html)).
Branching is **structural** via `[WorldSystemFilter]` (which world a
system even exists in), not a runtime `if` the way NGO does it.

**2.5 Dedicated server build.** "NetCode uses the Server Build property in
the Build Settings window ... If ... enabled, NetCode sets the
`UNITY_SERVER` define and you get a server-only build" — only `ServerWorld`
is created, client-filtered systems never instantiated in it
([Client server Worlds](https://docs.unity3d.com/Packages/com.unity.netcode@1.1/manual/client-server-worlds.html)).
Tick control is explicit via `ClientServerTickRate`: server can `BusyWait`
(skip catch-up) or `Sleep` via `Application.TargetFrameRate`; `Auto` picks
`Sleep` for dedicated-server builds and `BusyWait` for client+server builds
and the editor; a separate cap bounds catch-up ticks per frame
([ClientServerTickRate](https://docs.unity3d.com/Packages/com.unity.netcode@1.0/api/Unity.NetCode.ClientServerTickRate.html)).

**2.6 Hot reload with live worlds.** Not supported out of the box:
"recompile and continue is not supported out of the box for Entities. When
code is reloaded ... everything that was 'Baked' and the World can
disappear" ([Unity Discussions thread](https://discussions.unity.com/t/hot-reloading-recompile-and-continue-with-entities/831215)).
`World.DisposeAllWorlds()` exists and is used in reload/reset code, but is
a full teardown-and-recreate of every `World`, not an in-place patch
([API](https://docs.unity3d.com/Packages/com.unity.entities@0.17/api/Unity.Entities.World.DisposeAllWorlds.html)).
Domain-reload semantics from §1.6 still apply underneath.

---

## 3. Unigine (docs.unigine.com)

Public doc coverage is much thinner here than Unity's; several manual
pages render via JS and returned no static body text on fetch — noted
per-claim below.

**3.1 Engine split.** Native engine core (`Unigine_x64.dll` on Windows) +
a C++ Plugin system: "You can load a custom library module (a `*.dll`,
`*.so` or `*.dylib` file) and access its services ... via the `Plugin`
class interface," placed at `bin/plugins/<vendor>/<plugin>`, loadable
"without recompiling a Unigine executable"
([Creating C++ Plugin](https://developer.unigine.com/en/docs/latest/code/cpp/plugin?rlang=cpp)).
Plugins "are designed as singletons, meaning there is only one instance of
the plugin in the Engine"; manager/system classes broadly expose a static
`get()` accessor for that one process-wide instance (e.g.
`ComponentSystem::get()`) — from search-indexed text of the same page and
[Unigine::Plugin class](https://developer.unigine.com/en/docs/latest/api/library/common/class.plugin).
How globals cross the engine-core/plugin-DLL boundary mechanically — **not
found**.

**3.2 Game code unit.** Plugins register callbacks rather than being
instantiated per world: "C++ plugins implement `init()` and `shutdown()`
... using `Engine::addSystemLogic()`, `Engine::addWorldLogic()`, and
`Engine::addEditorLogic()`" ([same page](https://developer.unigine.com/en/docs/latest/code/cpp/plugin?rlang=cpp)) —
register-once, engine-calls-back-into-it, for the one active world. No
public documentation of per-world instantiation — **not found**.

**3.3 N worlds in one process.** No documented mechanism for running two
independent `World`s (e.g. client + server) concurrently in one process;
the `World Management` and `Editor2 > Worlds` manual pages render via JS
and returned no fetchable body text, and search snippets surfaced nothing
explicit either way — **not found**, an open question rather than a "no."
Unigine does confirm it ships **no built-in multiplayer networking**: "As
for the game multiplayer solution, we don't provide one. Some 3rd-party
library can be integrated via our C++/C# [API]"
([Unigine, X/Twitter](https://twitter.com/Unigine/status/1210539110504574979)) —
only a low-level `Unigine::Socket` (raw sockets + SSL) is offered
([Networking / Socket](https://developer.unigine.com/en/docs/latest/api/library/networking/class.socket)).
With no first-party client/server world concept, there is no first-party
answer for "editor runs client+server in-process" — **not found**.

**3.4 Net mode / role / authority.** No engine-level net-mode enum or
authority flag documented, consistent with §3.3 — any split would be
user/third-party code atop `Unigine::Socket` — **not found**.

**3.5 Dedicated server build.** Headless launch via `-video_app null`
(forum-corroborated only, not a fetched manual page:
[headless mode thread](https://developer.unigine.com/forum/topic/1132-starting-without-graphics-headless-mode/?logged_in=0)).
No canonical manual page confirming a `NullRenderer` class or a documented
stripped-code list — **not found**.

**3.6 Hot reload with live worlds.** Not found for the native C++ plugin
path (no statement of support or refusal). A separate C# "Component
System" is referenced only by an API page title with no retrievable body
— **not found**.

---

## Comparison table

| Axis | Unity classic + NGO | Unity DOTS / Netcode for Entities | Unigine |
|---|---|---|---|
| 1. Engine split | Native `UnityPlayer.dll` + managed `UnityEngine.dll`/`UnityEditor.dll` facades over module DLLs; Mono JIT or IL2CPP-to-native | Same substrate as classic (packages on top) | Native engine core DLL + optional C++ plugin DLLs via `Plugin`; plugins are process-wide singletons (`X::get()`) |
| 2. Game code unit | One assembly; role = runtime bool (`IsServer`/`IsClient`/`IsOwner`), no compiled split | Systems registered **once** via `[WorldSystemFilter]`; **`World`s instantiated per role**, populated by `DefaultWorldInitialization.GetAllSystems`+`WorldSystemFilterFlags` | Plugin registers callbacks once (`addSystemLogic`/`addWorldLogic`/`addEditorLogic`); no documented per-world instantiation |
| 3. N worlds/process | Not applicable except host-mode loopback; editor multi-instance = **separate OS processes** (MPPM: "each additional Editor is a separate child process") | **Yes, in-process**: client+server(+thin-client) `World`s coexist in one process incl. Editor Play Mode, via `ClientServerBootstrap` + **PlayMode Tools** (`PlayType`) | Not found — no multi-`World`/process mechanism documented; no built-in networking at all |
| 4. Net mode/role/authority | `NetworkManager.IsServer/IsClient/IsHost`; `NetworkBehaviour.IsOwner`/`HasAuthority`; `NetworkVariableWritePermission`; runtime `if` branches | `WorldFlags.GameServer/GameClient/GameThinClient`; `RequestedPlayType`/editor `PlayType`; per-entity `GhostOwner`/`PredictedGhost`; **structural** branching via `[WorldSystemFilter]` | Not found — no engine-level concept |
| 5. Dedicated server build | `UNITY_SERVER` on Dedicated Server target; strips rendering/asset-heavy code+assets; tick = ordinary frame loop | `UNITY_SERVER` via "Server Build" checkbox ⇒ only `ServerWorld` created; explicit `ClientServerTickRate` (`BusyWait`/`Sleep`, max catch-up ticks) | `-video_app null` headless launch (forum-only); no documented stripped-code page |
| 6. Hot reload w/ live worlds | **Domain reload** tears down/reloads AppDomain by default; disable via Enter Play Mode Options at the cost of manual static-state reset | **Not supported out of the box**; "the World can disappear" on reload; `World.DisposeAllWorlds()` = full teardown, not a live patch | Not found for native plugin path |

---

## The option space, as found

**(a) Engine/game split shapes**

1. **Monolithic native core + thin managed facade, no separately-loaded
   game DLL** (Unity classic/DOTS): one native runtime hosts a managed
   layer; game code compiles *into* it (or IL2CPPs into the native binary)
   rather than loading as an independently-versioned game module.
   Trade-off: no engine/game ABI-versioning problem (one build product per
   platform), at the cost of no clean hot-reload once domain reload is
   disabled, and no plugin-DLL boundary to reason about for globals.
2. **Native core + optional plugin DLLs, singleton-per-process** (Unigine):
   engine core is one DLL; game/tooling code loads as `Plugin` DLLs
   registering callbacks into engine-owned logic slots. Trade-off: closest
   of the three to Arcane's "engine DLL + game module DLL," but public
   docs show no solution for multi-world-per-process — the plugin model
   was built for one world, one process, one plugin instance.

**(b) Client+server-in-one-process shapes**

1. **Separate OS processes, shared source/assets** (Unity MPPM): each
   additional "player" is a full separate Editor process; no in-process
   world isolation attempted. Trivially safe against cross-contaminated
   statics/singletons, but heavyweight, and explicitly scoped as
   "small-scale, local testing," not a production listen-server pattern.
2. **Host-mode message loopback, one authoritative code path** (NGO
   `StartHost`): one world plus a fake-network client shim talking over an
   in-memory queue, not two isolated worlds.
3. **Multiple ECS `World`s in one process, role-filtered from one system
   registry** (Unity DOTS/NetCode for Entities) — **the closest public
   analogue to Arcane's target shape**. Systems declared once with
   `[WorldSystemFilter(...)]`; `ClientServerBootstrap` (driven in-editor by
   PlayMode Tools' `PlayType`) instantiates a `World` per role and
   populates each by intersecting the global system list against that
   world's `WorldFlags`. Runs client and server worlds — and any number of
   thin-client worlds — in the same process, including inside the editor's
   Play Mode: exactly "embedded server world alongside the editor's own
   world, same game module, one process." Documented trade-off: buys
   "immediate Editor iteration testing," but worlds are not independently
   hot-reloadable (`World.DisposeAllWorlds` is all-or-nothing), and
   dedicated-server builds special-case `UNITY_SERVER` to skip creating the
   client world entirely rather than create-then-discard it.
4. **No first-party answer** (Unigine): no built-in networking, so no
   documented in-process client+server story exists to compare against.

---
*Companion to the (separately covered) Unreal `FWorldContext`/`ENetMode`
survey. Sources cited inline per claim; package versions in URLs are the
versions the cited page/snippet was drawn from, not necessarily latest.*
