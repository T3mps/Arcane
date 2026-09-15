# Context Brief: ArcaneCore Becomes the Shared Engine DLL

Grounds the 2026-09-13 user ruling (ArcaneCore absorbs the headless engine
layer and becomes `ArcaneCore.dll`; `ArcaneClient.dll` links Core and holds
presentation). Repo: `D:\dev\starworks\Arcane` @ `c3eabb94`. Read-only audit;
re-derives the 2026-09-13 include-graph memory against current source.

## 1. Today's build shape

`premake5.lua` (root, single file — no per-project `premake5.lua` under
`ArcaneCore/` or `ArcaneClient/`):

- `ArcaneCore` — `kind "StaticLib"`, `staticruntime "off"` (dynamic CRT,
  `/MD`), C++23, `floatingpoint "Strict"` (premake5.lua:116-172). No export
  macro defined for it anywhere — it's consumed by direct source-list
  compilation or static-lib link, never a DLL boundary today.
- `ArcaneClient` — `kind "SharedLib"`, `staticruntime "off"`, defines
  `ARCANE_BUILD_DLL` (premake5.lua:402-469) which flips
  `ArcaneClient/src/Arcane/Base/Api.hpp:9-15` to `__declspec(dllexport)`
  (import for consumers, `visibility("default")` on non-MSVC). This is the
  **only** export macro (`ARCANE_API`) in the workspace; ArcaneCore has none.
  `ArcaneClient` links `ArcaneCore` (premake5.lua:446) plus NRI/msdfgen/
  freetype/imgui/enkiTS/Manifold2D and, on Windows, d3d12/dxgi/SDL3-static/
  the Win32 chain (premake5.lua:471-486).
- Linkers of `ArcaneCore`: `ArcaneClient` (446), `arccook` (283), `arcbuild`
  (357, alongside `ArcaneClient`), `ArcaneRuntime` (571, alongside
  `ArcaneClient` — comment at 563-570 explains ArcaneCore links into
  **exactly one module per process**, and Runtime/Client are separate
  processes/modules so both linking it is fine), `ArcaneEditor` (704,
  same pattern), `ArcaneTests` (partial file view; links `ArcaneCore`
  directly per its own comment, premake5.lua:784-786).
- Workspace-wide CRT rule: **everything in Arcane.slnx is `/MD`**
  (premake5.lua:4-7 "memory crosses the ArcaneClient.dll / Game.dll
  boundary, so all modules must share one heap"). C4251 is disabled
  workspace-wide for exactly this reason (premake5.lua:31-39).

Gacha `Server/premake5.lua`:
- Compiles `$ARCANE_SDK/ArcaneCore/src` **from source** as project
  `"ArcaneCore"`, `kind "StaticLib"`, `staticruntime "on"` (**static** CRT —
  opposite of the Arcane workspace's `/MD`), C++23
  (Gacha `Server/premake5.lua:81-122`). Comment at 76-80: "same sources the
  Arcane workspace builds /MD; the two flavors never meet in one process."
  Pulls Net/Crypto/Cli/Guid/Jobs/Build/Util wholesale via
  `IncludeDir["ArcaneCore"] .. "/**.hpp"/"**.cpp"` (93-95) — i.e. it takes
  **all** of today's ArcaneCore, not a curated subset.
  `Common` (130-174, static CRT) and the three services (Auth/Account/Combat)
  each `links { "Common", "ArcaneCore" }` (206, 279, 360).
  libpq vcpkg triplet: `VCPKG_TRIPLET = "x64-windows-static"` (Server/
  premake5.lua:18), **not** `-md`.
- `D:\dev\starworks\Gacha\vcpkg-triplets\` holds both
  `x64-windows-static.cmake` (server's current triplet, `VCPKG_CRT_LINKAGE
  static`) and `x64-windows-static-md.cmake` (`VCPKG_CRT_LINKAGE dynamic`,
  `VCPKG_LIBRARY_LINKAGE static`, `VCPKG_PLATFORM_TOOLSET v143`) — the
  overlay already exists, unused by any project today (grep of Server/
  premake5.lua shows only the static triplet referenced).

**Consequence:** a Core-DLL split requires Gacha's server build to flip from
static-CRT source-compiled ArcaneCore to linking `ArcaneCore.dll` /MD (the
ruling's "services move to /MD (static-md triplet)") — this is a second,
independent build-system change beyond anything inside the Arcane repo.

## 2. ArcaneClient/src/Arcane folder census (file count)

```
Assets 9   Audio 3   Base 15   Config 2   Edit 11   Host 24  ImGui 6
Input 5    Jobs 3    Material 12  Mesh 2  Platform 2  Plugin 9
Project 10 Render 83 Scene 9  Serialization 8  Sim 3  Sprite 2
```

Include-graph edges from would-be-Core folders (`Scene, Project,
Serialization, Sim, Sprite, Material, Config, Jobs, Plugin, Base`, plus
`Edit` as requested) into presentation (`Render/Nri/Platform/ImGui/Input/
Audio/Host`) — full grep, none omitted:

| From | Line | Includes |
|---|---|---|
| `Scene/MeshSubmissionSystem.hpp:31` | `<Arcane/Render/Nri/nodes/MeshNode.hpp>` (MeshInstance) |
| `Scene/RenderSystems.hpp:23` | `<Arcane/Render/Batcher2D.hpp>` |
| `Scene/SceneResources.hpp:9` | `<Arcane/Render/MeshBuilder.hpp>` (MeshData/MeshBounds) |
| `Base/Runtime.cpp:5` | `<Arcane/Audio/AudioDevice.hpp>` |
| `Base/Runtime.hpp:10` | `<Arcane/Input/InputSnapshot.hpp>` |
| `Edit/Gizmo.cpp:2` | `<Arcane/Render/Batcher2D.hpp>` |

`Project, Serialization, Sim, Sprite, Material, Config, Jobs, Plugin` — **zero**
matches; clean today.

**Memory's "three edges" confirmed unchanged**: (1) `SceneResources.hpp` →
`MeshBuilder.hpp` still stands verbatim; (2) `RenderSystems.hpp` +
`MeshSubmissionSystem.hpp` are still presentation (confirmed by their own
Render/Nri includes above — they earn the label by using the same
mechanism the memory names); (3) `Base/Runtime.hpp`/`.cpp` still pulls
Audio+Input (Scene/Project/Sim referenced in `Runtime.hpp` are headless
already, see §4).

**No new edges since 09-13.** The F2c-Plan-2 names the task called out
(`MeshResidencyBudget.hpp`, `NriMeshBufferCache`, `Render/MeshCache`,
`Host/SettleBound.hpp`, `Host/ReferenceImages.hpp`) all live under
`Render/`/`Host/` (presentation) and only **read** the CPU-only geometry
identity `Scene/SceneResources.hpp` already exposes (`MeshEntry`,
`MeshBounds`) — grep confirms every would-be-Core-side mention of
`MeshCache`/`NriMeshBufferCache` in `Scene/*.hpp` and `Project/
AssetRegistry.cpp:311` is prose commentary, not an `#include` or type use
(`Scene/SceneResources.hpp:166-248`, `Scene/MeshSubmissionSystem.hpp:73-117`,
`Scene/SceneCamera.hpp:252`). Dependency direction is correct
(presentation → core data), so nothing here is a new boundary violation —
these are GPU-residency caches consuming CPU identity that belongs in Core.

`Edit/` is itself editor-side (EntityOps, Gizmo) — its one Render include is
presentation-depends-on-presentation, not a Core violation; whether `Edit`
ships in Core or Client is a design call, not a fact this brief settles.

## 3. Per-module-static state

| State | Lives today | Existing DLL-boundary handling | What Core|Client adds |
|---|---|---|---|
| spdlog registry / `Log::Engine()` | `ArcaneClient/src/Arcane/Base/Log.hpp:1-9` — "spdlog is header-only in this workspace: each module has its OWN spdlog registry. Consumers must reach this logger via Engine()... spdlog::get("Arcane") in another module returns null." | Every module (Arcane.dll, ArcaneRuntime.exe, the plugin, tests) is already expected to hold its own registry and go through `Engine()`/`ARC_*` macros. | A new module (ArcaneCore.dll) would need its **own** `Log::Init`/`Engine()` pair if Core code wants to log independently of Client, or Core stays log-free and only Client logs — an open design point. |
| Mosaic sink install | `Base/Log.hpp:24-31` — `InstallMosaicSink()` is `inline`, deliberately, because "Mosaic's g_logSink is a per-module inline atomic, so each module ... installs into its own copy." | Same per-module-copy contract already in force. | Core.dll would need its own `InstallMosaicSink`-equivalent call if it wants Mosaic diagnostics routed anywhere. |
| Diagnostics sink | `Base/Diagnostics.hpp:390-413` — `Diagnostics::SetSink`/`Publish`/`Clear` are `ARCANE_API` (exported from ArcaneClient.dll today), explicitly "process-wide," "last writer wins" (396, 401). | Single process-wide slot inside ArcaneClient.dll; anyone linking the DLL shares it. | Whichever DLL (Core or Client) owns this slot becomes the one true Diagnostics sink; the other module must import it, not redeclare it — a real ownership decision, not just a copy. |
| Astra `TypeContext` | Allocated by the **host** (`ArcaneClient/src/Arcane/Base/Runtime.hpp:50-69`: ctor takes `Astra::TypeContext* externalContext`, "NO DEFAULT... deliberately," "the FIRST Runtime in a process permanently pins Arcane.dll's component-ID numbering"). Verified via `VerifySharedTypeContext` (`Host/ProjectBoot.hpp:81-84`, called from `Host/ProjectBoot.cpp:169-170`). `Astra::SetTypeContext(...)` happens "inside runtime_create's host-supplied body" (`ProjectBoot.cpp:151-154`) — i.e. the **host exe**, not Runtime itself, installs the context. | The `GameModule.hpp`/`PluginHost` precedent already treats this as cross-module shared state passed explicitly through `EngineContext`, never re-derived per module. | A Core|Client split adds a **third** module boundary (Core.dll, Client.dll, host exe, game module DLL) that must all agree on one `TypeContext*` — the existing "host allocates, everyone else imports" pattern generalizes, but now Core.dll is a candidate owner instead of the host exe. |
| `JobSystem` | `ArcaneClient/src/Arcane/Jobs/JobSystem.{hpp,cpp}` (287 LOC total) — grep found no static/global; it is owned per-`Runtime::Impl` (`Runtime.hpp:89`: "Reference, not pointer: the JobSystem is a fixed part of this Runtime's substrate"). Not a hidden global — already instance-owned. | N/A — already correctly scoped to Runtime, no cross-module static to migrate. | If `Jobs/` moves into Core.dll, Runtime (Client-side facade per §4) must still own/reference the instance, so Runtime itself must either move to Core or keep a Core-owned member — a facade-split question. |
| `GImGui` | ThirdParty `imgui` static lib, exported from ArcaneClient.dll via `/WHOLEARCHIVE:imgui` + `IMGUI_API=__declspec(dllexport)` (premake5.lua:448-469) — "each module keeps its own null GImGui... without this". | Stays Client per the ruling; ImGui context crosses the plugin boundary as `void*` (`Runtime.hpp:220-230`: `SetImGui`/`ImGuiContext()` etc., "Stored as void* to keep this header imgui-include-free"). | No new work — GImGui was already Client-only and already crosses module boundaries as an opaque pointer; the Core|Client split doesn't touch this pattern. |

## 4. `Base/Runtime.hpp` today

Full header at `ArcaneClient/src/Arcane/Base/Runtime.hpp` (298 lines).
Comment at top (3-6): "the engine facade handed to plugins via
EngineContext. Owns the substrate that MUST outlive plugin reloads."

Headless members (Scene/Project/Sim/Jobs/Plugin-shaped):
`Registry()`, `Schedulers()`, `Loop()`, `TypeContext()`, `WorkScheduler()`,
`TaskExecutor()`, `Jobs()`, `Components()`, `AssetsFacade()`,
`Configuration()` (76-92); `OpenProject`/`CurrentProject`/`CloseProject`/
`RegisterCreatedAsset`/`SetProjectBootScene`/`RestampProjectEngineAbi`
(117-166); hot-reload `SnapshotRegistry`/`RestoreRegistry`/`ResetRegistry`/
`ClearSystems` (236-249); engine-owned physics `InstallEngineSystems`/
`EnsurePhysics`/`PhysicsEditPass`/`ResetPhysics`/`ResolvedGravity`
(271-288, "Manifold2D-free surface: hosts and modules never see
PhysicsSystem or PhysicsWorld").

Presentation members: `AudioSystem()` (93, `Audio::AudioDevice&`),
`SetInputSnapshot`/`Input()` (217-218), the ImGui handoff `SetImGui`/
`ImGuiContext`/`ImGuiAlloc`/`ImGuiFree`/`ImGuiUserData` (220-230, "ABI v2"),
and the render bridge `SetRenderContext(Batcher2D*)`/`SetSpriteMaterials`/
`SetSpriteTable`/`SetMeshTable`/`SetMeshMaterials`/`SetCamera`/
`CameraOffset`/`CameraZoom` (168-212) — explicitly typed against
presentation pointers (`Batcher2D*`) or forward-declared presentation
types (`Audio::AudioDevice`).

**Concrete split point**: `Runtime` is one `pimpl`'d class
(`struct Impl; std::unique_ptr<Impl> m_impl;`, 291-292) mixing both
categories in one object with one vtable-free ABI surface. A literal
Core|Client split means either (a) splitting `Runtime` into a headless
`Runtime` (Core.dll) plus a `ClientRuntime`/extension (Client.dll) that
wraps it and adds Audio/Input/ImGui/render-bridge methods, or (b) keeping
one `Runtime` type but moving its *implementation* so Core.dll and
Client.dll jointly define one class across a DLL seam (harder, not the
pattern the workspace uses elsewhere). `RuntimeApp`/`EditorApp` (the two
hosts) and a headless `ArcaneServer` would consume option (a)'s split
directly: server links Core only → gets headless `Runtime`; RuntimeApp/
EditorApp link Core+Client → get the extended one.

## 5. Logging

- `ArcaneCore/src/Arcane/Util/Logger.hpp:1-8` — "Generic engine logging
  built on spdlog: lazily-created string-keyed named loggers... Game/service
  vocabulary... lives with the consumer (e.g. the server-side facade in
  Server/Common)." Namespace `Arcane`, class-based named-logger registry —
  engine-agnostic, library-shaped.
- `ArcaneClient/src/Arcane/Base/Log.hpp:1-9` — "Engine logger (Base module):
  console-sink spdlog logger named 'Arcane'. Deliberately separate from
  Core's Logger (Util/Logger.hpp)... this is the engine runtime's own
  console logger." Free functions `Log::Init`/`Shutdown`/`Engine()` plus
  `ARC_*` macros.
- Gacha CLAUDE.md: "`Aphelyon::Logger` derives from `Arcane::Logger`" — i.e.
  the Gacha server consumes **Util/Logger.hpp's** class, not Base/Log.hpp's
  free-function logger. These are two independently-shaped logging systems
  today, already living in the two folders the ruling assigns to Core vs
  Client respectively — no rename needed, but a "merge" (per the ruling's
  parenthetical "Util/Logger merges") must preserve: (a) Gacha's derivation
  contract (`Arcane::Logger` stays a subclassable type in Core), and (b) the
  per-module-registry contract Base/Log.hpp documents (§3) so `ARC_*` macros
  keep working unchanged in Client.

## 6. ABI

- `ArcaneClient/src/Arcane/Plugin/PluginABI.hpp:832`:
  `inline constexpr uint32_t kGamePluginABIVersion = 29;` — a **header
  constant**, baked into every module at compile time (833-837 commentary).
  `ARCANE_API uint32_t PluginABIVersion();` (844) publishes the version
  compiled into the *loaded* Arcane(Client).dll for cross-checking.
  `PluginVTable` (848+) carries `uint32_t abiVersion` and an `ABIVersion()`
  function pointer (868, 884); ImGui context is `void*` in this struct (857).
- Gacha `Game/Aphelyon.arcproj:5-7`: `"engine": { "abi": 29 }` — matches.
- Arcane's own `ReferenceProject/ReferenceProject.arcproj:5-7`: `"engine":
  { "abi": 29 }` — also matches; both project files are current.
- A Core|Client DLL split does not by itself change 29 (no ABI-surface
  change described in the ruling beyond the module boundary itself) — but
  moving `Plugin/` code into Core.dll while `PluginVTable`'s ImGui-adjacent
  fields stay Client-shaped is exactly the kind of change that has bumped
  this constant before (workspace convention: "ABI bumps are cheap during
  engine dev").

## 7. Conventions

- **Spec location**: this repo (Arcane) uses `docs/specs/` exclusively —
  there is no `docs/superpowers/specs/` here (`docs/superpowers/` is a
  Gacha-repo-only convention; `ls docs/` shows `audits/ plans/ research/
  specs/` plus a handful of top-level dated `.md`s). Newest 5 in
  `docs/specs/`: `2026-09-13-arcbuild-driver-design.md`,
  `2026-09-13-game-module-boilerplate-design.md`,
  `2026-09-11-physics-2d-wiring-design.md`,
  `2026-09-11-astra-adoption-design.md`,
  `2026-09-10-f2c-mesh-import-design.md`. F2c's own plan/closeout artifacts
  live under `.superpowers/sdd/2026-09-10-f2c-mesh-import-plan2-runtime-editor/`
  (referenced from `premake5.lua:597`), i.e. plan execution detail sits
  outside `docs/` in a session-scoped `.superpowers/sdd/` tree, separate
  from the `docs/specs/` design doc itself.
- **Linux/CI**: `Jenkinsfile` (Arcane repo) declares only
  `agent { label 'windows && gpu' }` (Jenkinsfile:19) — **no `linux-1` lane
  exists in this repo's own pipeline** (that label lives in the Gacha repo's
  Jenkinsfile per its CLAUDE.md). `docs/` mentions of "Linux" are all
  incidental (audits, plans, servitor closeout notes) — no dedicated
  Linux-port doc found under `docs/specs/` or `docs/plans/`.
- **arcbuild `--engine`**: confirmed **not built**. `docs/specs/
  2026-09-13-arcbuild-driver-design.md:54`: "`--engine <root>` is a second
  target kind, not [yet] built" (§6 forward-reference). Source comments
  echo this across `arcbuild/src/{Compose,Driver,Exit,Request,Slot}.hpp`
  and `main.cpp` — every file that will eventually grow `--engine` support
  currently only implements `--project`. This matches the memory note that
  the engine workspace stays off arcbuild until the Linux/CI milestone.

## 8. Sizes (LOC, `wc -l` over `**.hpp`+`**.cpp`)

Would-be-Core (today's ArcaneCore, by ArcaneCore/src/Arcane/* folder):
`Build 192, Cli 310, Crypto 520, Jobs 48, Net 1092, Util 701` — **Core total
≈ 2863 LOC**.

Would-be-Core candidates still inside ArcaneClient:
`Scene 2786, Project 1990, Serialization 1973, Sim 243, Sprite 210,
Material 4313, Config 146, Jobs 287, Plugin 2670, Base 3094` — **≈ 17,712
LOC** that the ruling's scope (Scene data + physics, Serialization, Sim,
asset formats, Project, Plugin, Base) would pull into Core.dll.

Presentation (stays in Client): `Render 30,568, Platform 423, ImGui 1912,
Input 1605, Audio 1113, Host 6293, Edit 2181, Assets 4663, Mesh 624` —
**≈ 49,382 LOC**. `Render/` alone is ~62% of ArcaneClient's presentation
surface and dwarfs everything moving to Core.

**Export-macro scope**: `grep -rl "ARCANE_API" ArcaneClient/src` → **92
files** reference the macro today (declarations across all folders, Core-
and Client-destined alike) — a rough proxy for how many headers need a
second macro (e.g. `ARCANE_CORE_API`) or a re-audit of which files keep
`ARCANE_API` (Client) vs gain the new one (Core) once the physical folder
move happens.

## Facts that constrain the design

1. The workspace is `/MD` end-to-end by explicit architecture decision
   (premake5.lua:4-7); a Core.dll must also be `/MD` — consistent with
   ArcaneCore already building `staticruntime "off"` (121) today.
2. ArcaneCore already has an established "links into exactly ONE module per
   process" rule (premake5.lua:563-570) — a Core.dll changes this to "one
   *DLL* per process," and every current direct-static-link consumer
   (ArcaneRuntime, ArcaneEditor, ArcaneTests, arcbuild, arccook) needs its
   link line and any headers assuming static-lib semantics re-verified.
3. Gacha's server ArcaneCore consumption is a **from-source, static-CRT**
   build today (Gacha `Server/premake5.lua:81-122`) — switching it to link
   `ArcaneCore.dll` is a cross-repo build change requiring the already-
   present but unused `x64-windows-static-md` triplet, not a drop-in.
4. Only one export macro exists workspace-wide (`ARCANE_API`,
   `Base/Api.hpp`) — a second DLL needs its own macro or a shared one keyed
   off two build defines; nothing in the codebase anticipates this split.
5. The 09-13 "three edges" are still exactly right and have **not grown**
   — F2c Plan 2's new mesh-residency machinery all reads Scene's CPU data,
   the correct dependency direction; the boundary work is unchanged in
   scope since the ruling.
6. `Runtime` is a single pimpl'd class mixing headless and presentation
   responsibility (`Base/Runtime.hpp`) — the split needs either two
   classes across the DLL boundary or one class split at the DLL seam;
   the workspace has no existing precedent for the latter.
7. Diagnostics is an exported, explicitly "process-wide, last-writer-wins"
   singleton (`Base/Diagnostics.hpp:396`) already living in ArcaneClient —
   its new home (Core or Client) is a real ownership decision, not copying.
8. Astra's `TypeContext` is host-allocated, not Runtime-allocated
   (`Host/ProjectBoot.cpp:151-154`) — a third module (Core.dll) joining the
   boundary needs this "host allocates, everyone imports" contract
   generalized, not re-derived.
9. spdlog and Mosaic's log sink are both **per-module-copy by design**
   already (`Base/Log.hpp` comments) — Core.dll gaining its own copy is
   consistent with existing practice, not a new problem class.
10. arcbuild's `--engine` path (which would build/rebuild the engine DLLs
    themselves via the same driver used for game modules) is explicitly
    unbuilt (`docs/specs/2026-09-13-arcbuild-driver-design.md:54`) — a
    Core-DLL split adds a second engine-side DLL for that eventual command
    to reason about.
11. This repo's own CI has no Linux lane (`Jenkinsfile:19`, `windows &&
    gpu` only) — the "headless server is the natural first Linux target"
    framing is aspirational relative to current CI, not scheduled.
12. LOC moving to Core (~17.7k) is small next to what stays in Client
    (~49.4k, `Render/` alone 30.6k) — the split is boundary-drawing work,
    not a rebalancing of engine mass.

## Open questions only the user can answer

1. Does `Runtime` split into two classes (headless Core `Runtime` +
   Client-side extension) or stay one class defined across the DLL seam —
   §4's concrete fork, with no existing workspace precedent for the latter?
2. Where does `Diagnostics::SetSink`/`Publish` live — Core (so headless
   tools like `arccook`/a future `ArcaneServer` can publish diagnostics
   without linking Client) or Client (keep it where it is)?
3. Does `Edit/` (11 files, editor-only ops/gizmo) ship in Core, Client, or
   a third editor-only module — it's presentation-shaped today but not
   named in the ruling's Client list either?
4. Is a second export macro (`ARCANE_CORE_API`) introduced, or does
   `ARCANE_API` become build-define-conditional across two DLLs?
5. What is the ABI bump plan — does the Core|Client split alone justify
   bumping `kGamePluginABIVersion` past 29, independent of any surface
   change to `PluginVTable`?
6. Does Gacha's server move to `ArcaneCore.dll` in the same change that
   creates it, or does the Arcane-side split land first with Gacha's
   `/MD` migration as a separate, later PR (given it needs the unused
   `x64-windows-static-md` triplet wired into `Server/premake5.lua`)?
