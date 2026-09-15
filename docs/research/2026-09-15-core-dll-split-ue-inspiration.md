# UE Inspiration: Core|Client DLL Split, Multiplayer-in-Editor

Read-only research over the local UE dump `.example/UnrealEngine-release/`
(license-bound: identifiers/signatures/paths only, no code bodies). Grounds
the ArcaneCore-becomes-a-DLL ruling (2026-09-13) and the fork between
(A) instance-per-world game module vs (B) a second loaded copy of the module
DLL. Context: `2026-09-15-core-dll-split-context-brief.md` §3-4 (Arcane's
`Runtime`, `EngineContext`, `PluginHost`, Astra `TypeContext`).

## 1. N worlds in one process for PIE

`FWorldContext` (`Engine/Classes/Engine/Engine.h:333-486`) is UE's per-world
track: `WorldType` (339), `PIEInstance` (400), `PIEPrefix` (403),
`OwningGameInstance` (393), `ActiveNetDrivers` (397); callers are told never
to hold `FWorldContext*` directly (323). `UEngine::WorldList`
(`Engine.h:3559`, `TIndirectArray<FWorldContext>`) is the one process-wide
registry; `CreateNewWorldContext` (`Engine.h:3489`/`UnrealEngine.cpp:16481`)
appends an entry. `EWorldType::PIE` is one tag among Game/Editor/Inactive
(`Classes/Engine/EngineTypes.h:1222-1249`).

`UGameInstance::InitializeForPlayInEditor` (`Private/GameInstance.cpp:
278-350`) stands up one `FWorldContext` per PIE instance, then builds the
world via `CreatePIEWorldByDuplication`/`GetDuplicatedWorldForPIE`
(`PlayLevel.cpp:2320-2349`, `World.cpp:4291-4310`) — a full duplicate
object graph namespaced by `PIEInstance`, so N graphs coexist without name
collisions. `EPlayNetMode` (`LevelEditorPlaySettings.h:92-100`:
`PIE_Standalone/PIE_ListenServer/PIE_Client`) plus `bLaunchSeparateServer`/
`RunUnderOneProcess` drive `StartPlayInEditorSession`
(`PlayLevel.cpp:2826-2932`): a separate OS process is forced only for
`PIE_Client` or an explicit separate-server request; otherwise a loop
(2884-2932) calls `CreateNewPlayInEditorInstance` once per client plus once
for an implicit listen server — each call is one more `FWorldContext` +
`UGameInstance` + duplicated `UWorld` in the SAME process.
Per-process state outside `FWorldContext`: `GEngine`, `GLog`, `GConfig`, and
the mutable "current" pointer `GWorld` — a legacy convenience swapped by
`SetPlayInEditorWorld` (`Editor/Private/Editor.cpp:1036-1057`) around
operations assuming "the" world, while N `UWorld`s (each with its own
`NetDriver`/`GameInstance`/subsystems) stay live simultaneously.

**Maps to Arcane as:** `FWorldContext` is what Arcane's `Runtime` needs an
analogue of — today one `Runtime` is built once against an
externally-supplied `TypeContext*` (UE's pre-multi-context case). Embedded-
server PIE needs a process-level registry of `Runtime`s (`WorldList`), each
carrying its own Registry/Schedulers/PIEInstance-id, while `PluginHost`/
logging/diagnostics stay genuine per-process singletons. `GWorld`'s
save/restore is a cautionary template: any transitional "current Runtime"
pointer must follow the same explicit discipline, never become ambient
state.

## 2. Module singleton vs. world instance

`IMPLEMENT_MODULE` (`ModuleManager.h:916-921` monolithic, `925-939` DLL)
always produces a *factory*; the one instantiation happens inside
`FModuleManager::LoadModuleWithFailureReason` (`ModuleManager.cpp:
769,872,1013`), stored in `FModuleManager::Modules` (`ModuleManager.h:721`,
a name-keyed map — no room for a second instance of the same module).
Game classes register once at module-load (`UObjectBase::Register` →
`ProcessNewlyLoadedUObjects`, `UObjectBase.cpp:537,1015,1004`, hooked to
`FModuleManager::OnProcessLoadedObjectsCallback`) into one process-wide
class table with one CDO per class (`Class.h:3103,3509-3518`).

Per-`UGameInstance` (`GameInstance.h:151`, doc 143-148: "one... per PIE
instance"): owns `SubsystemCollection<UGameInstanceSubsystem>` (669).
Per-`UWorld`: `AuthorityGameMode` (`World.h:1405`) and
`SubsystemCollection<UWorldSubsystem>` (4297); `AGameModeBase`
(`GameModeBase.h:32-38,47`) is instanced per-World, server-side only.

The bridge: `FSubsystemCollectionBase::Initialize`
(`SubsystemCollection.cpp:143`) walks the process-wide class table
(`GetDerivedClasses`, 185) and calls `AddAndInitializeSubsystem` (189/278)
per owner, gated by the class's CDO answering `ShouldCreateSubsystem(Outer)`
(299) — register a class once, instantiate it per World/GameInstance.
`CreateNewPlayInEditorInstance` (`PlayLevel.cpp:1790,1832-1833`) confirms
this at scale: ONE module singleton, ONE class table, N world/instance
pairs each with its own GameMode and subsystems.

**Maps to Arcane as:** argues for a **third shape**, not (A) or (B): module
singleton registers types once, each Runtime instantiates systems.
`ARCANE_GAME_MODULE`/`PluginHost` already match `IMPLEMENT_MODULE`/
`Modules` (one DLL load, one module object, `OnInit` once per process);
Astra's registrar draining into `TypeContext` at DLL load already matches
`UObjectBase::Register`. Missing piece: a per-process *system-factory
registry* (populated by the module alongside the component registrar,
mirroring `FSubsystemCollectionBase::Initialize`) that every `Runtime`
walks at construction to instantiate its own systems, gated by a role
predicate off `EngineContext`. Fork (B) has no UE analogue: UE never loads
a module twice, it multiplies *worlds*, never modules.

## 3. Net mode and role

`ENetMode` (`EngineBaseTypes.h:931-949`: `NM_Standalone/DedicatedServer/
ListenServer/Client`, doc at 944: "NetMode < NM_Client is always some
variety of server") is per-`UWorld` via `GetNetMode()` (`World.h:
4569-4578`) → `InternalGetNetMode()` (`World.cpp:9272-9297`, falls back to
a cached `PlayInEditorNetMode` field when no `NetDriver` exists yet).
`GetNetModeFromPlayNetMode` (`GameInstance.cpp:248,350`) translates the
editor's `EPlayNetMode` into this runtime mode right after PIE duplication.

`IsRunningDedicatedServer()`/`IsRunningClientOnly()` (`CoreMisc.h:152,202`)
are **process-launch** checks whose own doc comments say "should not be
used for gameplay or networking purposes, check NetMode instead" (149-150,
175, 200) — structurally false during single-process PIE even when a PIE
world is genuinely a server; `UWorld::IsNetMode()` (`World.h:4580-4596`)
re-derives from `GetNetMode()` in editor builds "because of PIE, which can
run a dedicated server without `-server`" (4583).

`ENetRole` (`EngineTypes.h:3294-3305`) is per-actor; `HasAuthority()`
(`Actor.h:1928,4911-4914`) is `GetLocalRole()==ROLE_Authority`. Concrete
branch site: `AGameModeBase::ProcessServerTravel`
(`GameModeBase.cpp:509-519,527`, `#if WITH_SERVER_CODE`) reads `GetNetMode()`
and skips its countdown only on standalone/non-networked worlds.

**Maps to Arcane as:** `EngineContext`/`Runtime` needs two independent
fields: a process-launch flag (how was `ArcaneServer.exe` booted) and a
per-Runtime net mode/role carried on the §1 world-context analogue.
Systems must branch on the per-Runtime field, as `ProcessServerTravel`
branches on `GetNetMode()`, never on a process-wide "am I the server exe"
flag — PIE-with-embedded-server is exactly the case UE calls out as
breaking that assumption. A finer per-entity `HasAuthority()` mirrors
`ENetRole` on `AActor`.

## 4. The Core/CoreUObject/Engine DLL split

The export macro is not generated header text — it's a per-relationship
compiler define UnrealBuildTool computes at compile time
(`UEBuildModule.cs:221` names it, `:674-700` decides DLLIMPORT/DLLEXPORT/
empty per including-vs-defining-module relationship — the same header
compiles differently depending on who includes it). `DLLEXPORT`/`DLLIMPORT`
resolve to `__declspec` at `WindowsPlatform.h:216-217`. A whole-tree grep
for `#define CORE_API` (or any `<MODULE>_API`) found **zero header hits**
— confirms these exist purely as UBT-injected `/D` flags; `Misc/Build.h:
66-67,74-76` shows `WITH_EDITOR`/`WITH_ENGINE` follow the same pattern.
Module boundary: Core (`Runtime/Core/Public/`) = platform/primitives
(`HAL/Platform.h`, `Containers/`, `Delegates/Delegate.h`,
`Modules/ModuleManager.h`, `Misc/ConfigCacheIni.h`, `CoreGlobals.h`) — zero
UObject/gameplay knowledge; Engine (`Classes/Engine/World.h`,
`Classes/GameFramework/Actor.h`, `Classes/Engine/NetDriver.h`) depends
downward only. Cross-DLL globals, by ownership: `GLog` is a macro (`CoreGlobals.h:95`)
resolving to a function-call singleton (`GetGlobalLogSingleton()`,
`CoreGlobals.h:68`) — sidesteps static-init-order issues across DLLs.
`GConfig` is a true extern (`CoreGlobals.h:96`, storage in
`ConfigCacheIni.cpp:5670`), Core.dll owns it. `GIsServer`/`GIsEditor` are
defined in Core (`Misc/CoreGlobals.cpp:197,211`) but WRITTEN from a higher
layer (`LaunchEngineLoop.cpp:2056` etc.) — cross-DLL assignment works
because a `dllimport`ed extern is still a valid target through its import
thunk. `GEngine` mirrors this one layer up: defined in Engine.dll
(`UnrealEngine.cpp:434`, `ENGINE_API`), imported everywhere downstream.

**Maps to Arcane as:** Arcane's single `ARCANE_API`
(`Base/Api.hpp:9-15`) is UE's monolithic case. The split needs UE's
per-module version: a second macro (e.g. `ARCANE_CORE_API`) toggled by its
own build define, exporting inside ArcaneCore.dll's own TUs and importing
elsewhere; premake's per-project `defines{}` is the coarser equivalent of
UBT's per-relationship injection — fine, since Arcane has no monolithic/
modular axis to disambiguate. For `Diagnostics`, the Mosaic sink, and
Astra's `TypeContext*`: pick one owning DLL (Core, the lower layer) and
export storage from exactly it, the way `GConfig`/`GIsServer` live in Core
and are read/written through imports elsewhere; `GLog`'s function-call-
singleton is the safer template if init-order matters across the boundary.

## 5. The dedicated server target

`UE_SERVER` is UBT-injected like `WITH_EDITOR`/`WITH_ENGINE`
(`Misc/Build.h:41-42`). `WITH_EDITORONLY_DATA` is forced off in UBT's C#
(`UEBuildTarget.cs:5839`) — all three are target-configuration outputs, not
header-derivable. `FApp::IsGame()` (`Misc/App.h:175-182`) hard-returns true
on any non-editor build. Null RHI (`Runtime/NullDrv/`, `IMPLEMENT_MODULE(FNullDynamicRHIModule,
NullDrv)` at `NullDrv.cpp:13`) still special-cases the server
(`NullRHI.cpp:56-60`: skips `InitPreRHIResources()` under
`IsRunningDedicatedServer()`). That helper gates rendering/audio
pervasively (`GameEngine.cpp:1690,1763,1878,1989`; `SoundWave.cpp:
419,490,1584,1865,1923,2355,4016,4578`). Decisive server-boot gate: `LaunchEngineLoop.cpp:2910` — Slate is only
created for non-server, regular-client-or-editor-token processes; on a
dedicated server `FSlateApplication::Create()` (2921) never runs, so the
renderer/viewport/input stack simply doesn't exist for the process's life.
`FEngineLoop::Tick()` (5337) still calls `GEngine->Tick(...)` (5619)
unconditionally — simulation continues — while downstream Slate
touchpoints (5599,5670,5745) check `IsInitialized()` rather than
re-deriving server-ness (5674 hard-asserts that path unreachable on a
server build). Tick-rate is a runtime CVar, `t.MaxFPS`
(`UnrealEngine.cpp:11745-11845`), plus a fixed-frame-rate mode (~2724-2834)
for deterministic ticking — not a compile-time constant.

**Maps to Arcane as:** `ArcaneServer.exe` should mirror this as an
init-time subsystem-construction gate, not scattered `if (isServer)`
checks: resolve server-ness once at boot and never construct the renderer/
ImGui host/audio device/input-device poller — downstream code checks "is
this subsystem present," mirroring `FSlateApplication::IsInitialized()`.
Arcane's headless `Runtime` is the `GEngine->Tick(...)` equivalent —
full-rate on the server, network commands substituting for the
device-input poll Client would do. Tick-rate cap should be a runtime
config value (mirroring `t.MaxFPS`), not compile-time.

## 6. Hot reload / Live Coding across PIE worlds

Classic hot reload: `FHotReloadModule` (`Developer/HotReload/Private/
HotReload.cpp`), `IHotReloadInterface` (`CoreUObject/Public/Misc/
HotReloadInterface.h:95,106,70`). Live Coding:
`Public/ILiveCodingModule.h:61-62,67` (`Developer/Windows/LiveCoding/`).
Re-instancing: `FReload::Reinstance()` (`ReloadUtilities.cpp:1020`) →
`FReloadClassHelper::ReinstanceClasses` (651) →
`FReloadClassReinstancer : FBlueprintCompileReinstancer` (49) →
`ReplaceInstancesOfClass_Inner` (`KismetReinstanceUtilities.cpp:2560`) —
native hot reload and Blueprint recompile share one reinstancer. Delegates:
`UObjectGlobals.h:3292,3296,3300`. `GIsPlayInEditorWorld`
(`CoreGlobals.h:519`) is a process-global flag; per-instance identity lives
in `FWorldContext::PIEInstance` instead. Re-instancing scope is **process-wide, not per-world**: victim collection
uses a bare `TObjectIterator<UObject>` sweep (`KismetReinstanceUtilities.cpp:
2723,2812,3823,3154`) — no world/outer parameter anywhere — so one sweep
touches the editor world, every PIE client, and the PIE server world
together. Live Coding's sync path (`LiveCodingModule.cpp:807-899`) does the
equivalent independently. Class-layout changes are flagged as dangerous
even with re-instancing on (`:933,938,940,973`).

What UE refuses while PIE is live (`FHotReloadModule::Tick`,
`HotReload.cpp:1243`): if Live Coding is active, classic hot reload no-ops
(1261-1266); if a play session is running, reload defers until it ends
(1272-1276); and — decisively — if ANY world context is `EWorldType::PIE`
with a non-null `NetDriver`, hot reload returns without acting
(1278-1288), commented as: re-wiring possessed pawns/controllers across
networked PIE instances is too complicated. Live Coding carries **no** such
guard — it patches in place and accepts the documented crash risk instead.

**Maps to Arcane as:** two regimes. (1) Layout-safe reload is what
`SnapshotRegistry`/`RestoreRegistry` already do per-Runtime — but since
`TypeContext` is per-process while Registries are per-Runtime, a DLL swap
with two live Runtimes must be one orchestrated transaction: snapshot EVERY
Runtime, unload, reload, restore EVERY Runtime — never sequentially, since
old descriptors dangle the instant the DLL unloads. (2) The networked case
is where UE explicitly refuses classic hot reload, and the reason
transfers directly: replication state and connection↔entity bindings are
cross-Runtime references a per-Runtime snapshot cannot repair. Arcane's
v1 policy: when the embedded server Runtime has an active net driver/
connection, "Rebuild Game Module" defers until that session stops
(mirroring `IsPlaySessionInProgress`); full networked-live reload (Live
Coding's answer) is a separate, later spec.

## 7. What NOT to copy

- **UObject reflection/GC.** `UPROPERTY`/`UFUNCTION`/`UCLASS`
  (`ObjectMacros.h:723,724,751,754`) and `GENERATED_BODY` (742-743) expand
  into UHT-filled glue; `CollectGarbage`/`TryCollectGarbage`
  (`UObjectGlobals.h:912,920`) are UE's mark-sweep GC. Arcane's stand-in:
  Astra's plain, explicitly-owned component registration — no reflection
  macros, no GC.
- **Blueprint.** `Runtime/Engine/Classes/Kismet/` is UE's visual-scripting
  layer. Arcane has no scripting layer by design.
- **UHT + UnrealBuildTool.** `Programs/UnrealHeaderTool` (stripped in this
  dump, but the macro expansions above prove the code-gen dependency) and
  `Programs/UnrealBuildTool/` (a C# per-module build-graph compiler
  computing export macros/target flags per relationship). Arcane's
  stand-ins: premake's `defines{}` (§4, hand-written, no code-gen step) and
  premake5.lua itself — already in use; nothing here calls for reproducing
  UBT's relationship graph, only a second export-define pair.

## Verdict on the fork

**A third shape** — module singleton registers component AND system types
once at DLL load; each `Runtime` instantiates its own systems from that
registry, gated by a role read off `EngineContext` — not (A) [module
object-per-Runtime] and not (B) [second loaded DLL copy].

Decisive UE evidence: `IMPLEMENT_MODULE` (`ModuleManager.h:916-939`) and
`FModuleManager::Modules` (`:721`, keyed by module name) make a module
singleton structurally inescapable — UE has no mechanism to load the same
module twice in one process, so (B) has zero precedent. Yet UE routinely
runs N concurrent worlds (`CreateNewPlayInEditorInstance`,
`PlayLevel.cpp:1790-1833`), each with its own GameMode/subsystem instances
stamped from a *process-wide class table* via
`FSubsystemCollectionBase::Initialize` (`SubsystemCollection.cpp:143-299`)
— neither (A) [module re-instantiated per Runtime, which UE never does]
nor today's Arcane shape (`OnInit` runs once, never distinguishing
per-Runtime state), but a third pattern: one registration pass, N
instantiation passes.

## Shape decisions to carry into the spec

- Keep the game module's `OnInit` once-per-DLL; split its body into
  "register component types" (Astra, exists) + "register system factories
  with role masks" (new, mirrors `FSubsystemCollectionBase::Initialize`).
- Add a per-process `Runtime` registry (UE's `WorldList`) so embedded-
  server PIE can enumerate/tick N live Runtimes in one process.
- Give `EngineContext`/`Runtime` two independent fields — process-launch
  flag and per-Runtime net-mode/role — never collapsed; UE calls
  conflating them a bug class exactly in the PIE-server case.
- `TypeContext` stays the one process-wide registry Astra already treats
  it as; only systems become per-Runtime instances (UClass vs. instance).
- A DLL reload with 2+ live Runtimes must snapshot ALL, reload once,
  restore ALL — never per-Runtime; v1 defers the reload entirely while the
  server Runtime has an active net driver, mirroring `HotReload.cpp:1272-1288`.
- Introduce a second export macro (`ARCANE_CORE_API`); pick one owning DLL
  (Core) for `Diagnostics`/Mosaic sink/`TypeContext*` storage and export it
  — `GLog`'s function-call-singleton is the safer template if init-order
  matters across the seam.
- `ArcaneServer.exe` should never construct render/ImGui/audio/input-device
  objects (an init-time gate, like UE never calling `Create()` on
  `FSlateApplication` for a dedicated server); tick-rate cap = runtime
  config value (UE's `t.MaxFPS`), not compile-time.
- No UObject reflection/GC/Blueprint/UHT/UBT to copy — Astra + premake
  already occupy those roles; the only gap is the system-factory registry.
