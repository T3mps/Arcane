# Sprite/Material Resolution Lift — the standalone runtime cannot draw a textured sprite

> **Start here. This file is the whole brief — it assumes no prior context.**
> Written 2026-07-29 at the end of a long session, deliberately so the next
> session can begin cold. Every claim below was verified in the source; the
> "UNVERIFIED" markers are the ones that were not.

## The bug, in one paragraph

`RenderSubmissionSystem` (`Arcane/Arcane/src/Arcane/Scene/RenderSystems.hpp`)
deliberately never touches the Assets facade. It only reads two pre-resolved
lookup tables that the HOST must publish every frame — `SpriteTable` (an
`.arcsprite` Guid → texture + UV sub-rect + world size in meters + pivot) and
`SpriteMaterialTable`. `SceneResources.hpp:4` states the contract outright:
*"SpriteTable and SpriteMaterialTable are set by the host each frame."*

**The editor publishes them. `ArcaneRuntime` never does.** The editor calls
`m_runtime->SetSpriteTable(&m_sprites->Table())` at
`Arcane/ArcaneEditor/src/EditorAppFrame.cpp:1115`; a repo grep for
`SetSpriteTable` in `Arcane/ArcaneRuntime/src/` returns **nothing**. The
resolution cache itself lives at `Arcane/ArcaneEditor/src/SpriteCache.hpp` —
**editor-side code** — so the standalone runtime structurally cannot render a
textured sprite, whatever the scene contains.

Consequence, which matches the user's report exactly: a sprite renders in the
editor viewport and is invisible in a separate-window play. `SpriteRenderer`'s
own doc comment says an unresolved sprite draws *"a 1x1 m untextured tint
quad"* — so the sprite has been "drawing" all along as a blank ~72 px square
(at 72 px/m) rather than the texture. And because BOTH tables are missing, the
user observed no difference with and without a material applied — neither path
can resolve.

## Why this is worth doing properly

This is the second instance of one pattern in a single evening: **the editor
grew a capability and the standalone host never got it.** The first was the
camera (fixed earlier today — the camera is now a scene component, see
`Scene/SceneCamera.hpp`). Both had the same signature: "looks right in the
editor, renders nothing in the game."

The user's standing **homogenized-rendering directive** ("one canonical render
path, no bespoke per-renderer chains") is exactly this. The fix is not to
duplicate `SpriteCache` into the runtime — it is to lift resolution into the
engine so both hosts drive one implementation.

## Design

**Move sprite + material resolution into `Arcane.dll`, host-agnostic.** Both
hosts then call the same thing each frame and publish the same tables.

What the resolver needs (why it cannot be a pure function like the camera
sweep was): the **Assets facade** (to load `.arcsprite` + its texture) and the
**render device** (textures are GPU objects). Both are already reachable from
`Runtime` — `AssetsFacade()` exists and `SetRenderResources(device, shaders)`
is called by every host at boot.

Suggested shape (adjust to what the code actually wants — the point is the
seam, not these names):

- New engine unit, e.g. `Arcane/Scene/SpriteResolver.{hpp,cpp}` in `Arcane.dll`
  (`ARCANE_API`; NOT header-only — it needs the Assets facade and nvrhi, which
  do not belong in a widely-included header).
- It owns the cache `ArcaneEditor/src/SpriteCache.hpp` currently owns: Guid →
  `SpriteEntry`, refcount/memoised-failure semantics, invalidation on re-save.
- One per-frame call, e.g. `resolver.Refresh(registry, assets)` then
  `runtime.SetSpriteTable(&resolver.Table())`.
- **The editor switches to it too.** Leaving the editor on its own cache would
  keep two implementations, which is the thing being fixed. The editor's
  `invalidateSprite` hook (wired in `EditorApp.cpp`) has to keep working — the
  sprite editor re-saves an asset and the viewport must pick it up.

### Read these before designing

- `Arcane/ArcaneEditor/src/SpriteCache.hpp` — what is being lifted. Read it
  FIRST and in full; it holds the semantics (refcounting, memoised failures)
  that must survive the move.
- `Arcane/Arcane/src/Arcane/Scene/SceneResources.hpp:85-110` — `SpriteEntry`,
  `SpriteTable`, `SpriteMaterialTable`. The tables are non-owning pointers, so
  whoever owns the maps must outlive the frame.
- `Arcane/Arcane/src/Arcane/Scene/RenderSystems.hpp` — the consumer. Do not
  change its contract; it is the one canonical submission path.
- `Arcane/ArcaneEditor/src/EditorAppFrame.cpp:1100-1125` — how the editor
  refreshes and publishes today (the behaviour to preserve).
- `Arcane/Arcane/src/Arcane/Scene/SceneCamera.hpp` — the camera lift done
  earlier today, as a precedent for "engine-side, both hosts, one seam".

### Open questions for the next session

1. Does the material table need the same lift, or is it already engine-side?
   `SpriteMaterialTable` is populated from the material/shader machinery —
   **UNVERIFIED**, check before scoping. The runtime does not set it either.
2. Does `ArcaneRuntime` need the `.arcsprite` assets mounted? It opens the
   project, so the mounts should be there — **UNVERIFIED**.
3. Ordering: resolution must run before `SubmitRender`, and after any scene
   load that introduced new sprite Guids.

## Verification

- Automated: the resolver is engine-side and testable. Pin the cache semantics
  (a Guid resolving once, a failure memoised, invalidation forcing a re-resolve).
  Registry-touching tests must use `Arcane::Test::SharedTypeContext()` — never
  a bare `Arcane::Runtime` (it steals `Arcane.dll`'s TypeContext slot).
- The real acceptance is the user's: **a textured sprite visible in a
  separate-window play, identical to the editor viewport.** Cannot be verified
  from here (GPU-driver hazard on this box; windowed D3D12 runs are desk-only).
- `ArcaneRuntime.log` (next to the exe, truncated per launch) is the
  diagnostic channel — the editor now redirects the child's stdout/stderr there.

## State of the tree (2026-07-29 ~19:50)

- Branch `arcane-runtime-host-fold`, **not merged**. It is ~40 commits ahead of
  `main` but only some are this work — it was cut from the material-panel
  branch, and a **concurrent user session commits to the same working tree**.
  Always `git branch --show-current` before committing, and never `git add -A`.
- Gate baseline: **31082 assertions / 639 test cases** (`~[gpu]`).
- Today's relevant commits: `7b3b7f65` camera-as-component, `02d46841` camera
  test fixture, `1cd1ef7d` Astra name-collision fix, `440b627c` per-process
  plugin images, `8f8ae9a9` runtime spawn log + no console.
- **Desk-verify owed** (GPU hazard): camera rect + PIE/standalone parity, the
  play-mode dropdown items in `19610327`'s message, and the runtime-fold arc's
  list in `.superpowers/sdd/progress.md`.

## Build discipline (hard-won today — ignore at your peril)

- Build with the **VS 18** MSBuild, not PATH's:
  `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /nodeReuse:false /t:"Arcane;ArcaneTests;ArcaneEditor;ArcaneRuntime" /v:minimal`
  from `Arcane/`, via PowerShell (Bash mangles `/p:`). Run it in the background
  and POLL **inside the same turn** — a foreground run hits the 600 s timeout,
  kills the tree and orphans `cl.exe`.
- **A NEW SOURCE FILE REQUIRES `Arcane\GenerateProjects.bat`.** The projects
  glob, but the `.vcxproj` is generated — a new file is silently NOT compiled
  until you regenerate. This bit me today: a new test file made the build look
  green while running zero of its tests. Always run the new tests by tag and
  confirm the count moved.
- Gate FROM the exe dir:
  `Arcane\bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe "~[gpu]"`.
  Capture the `Randomness seeded to:` banner. The gate does NOT compile
  `EditorApp*.cpp` / `EditorPanels.cpp`, so a green gate does not prove the
  editor relinked — check `ArcaneEditor.exe`'s mtime against your sources.
- **Never kill a running `ArcaneTests.exe`** — it is probably the other
  session's gate. Queue behind it. Symptom of collision: postbuild copies fail
  because the running gate has the HotReloadPlugin/PlaygroundGame DLLs mapped.
- `LNK1168` = the user's editor is running; rename the locked exe aside
  (`ArcaneEditor.exe.running-lock-N`) and relink.
- Astra identifies types by **unqualified name** and refuses duplicates —
  an anonymous namespace does not protect a name. See
  memory `project_astra_unqualified_type_names`.
- **After an Astra vendor sync, `Aphelyon.dll` must be rebuilt**
  (`D:\dev\starworks\Aphelyon\Aphelyon.slnx`, same MSBuild) or the editor
  aborts inside the ECS on project open. Never `git commit` in that repo.
