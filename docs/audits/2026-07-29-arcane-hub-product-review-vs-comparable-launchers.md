# Arcane Hub -- Product Review vs Comparable Launchers

Date: 2026-07-29
Subject: `Arcane/Hub/` (Tauri 2 + Rust + SvelteKit), binary `arcane_hub.exe`
Bar: "a sleek, elegant but effective hub for managing engine versions and projects"
Scope of the product (recorded decision): engine SELECTOR + project creator/launcher.
Engine download/distribution is explicitly out of scope.

## Method

This is the product-experience pass the 2026-07-29 grading lesson calls for:
benchmark the behavior SET against the comparable products, not just the build
quality of the behaviors we have. Three independent research passes gathered
VERIFIED feature inventories (official docs, engine source, support forums --
claims the researchers could not confirm are excluded or marked):

- **Unity Hub 3.x** -- official docs (docs.unity.com/hub) + support/forums.
- **Epic Games Launcher / Unreal Engine** -- dev.epicgames.com docs (current to
  UE 5.8) + forums.
- **JetBrains Toolbox 3.x** -- jetbrains.com docs/blog + YouTrack.
- **Godot 4.x Project Manager** -- docs + verbatim engine source
  (`editor/project_manager/*.cpp`, master).

The Arcane Hub side comes from its source (the `lib.rs` command surface,
`state/settings/store/spawn/resolve`, the Svelte views), plus the interactive
screenshot passes earlier in this arc. This is a documentary benchmark: the
comparables were not installed and driven side-by-side on this machine.

## Flow-by-flow comparison

| Flow | Unity Hub | Epic (UE) | Toolbox | Godot PM | **Arcane Hub** |
|---|---|---|---|---|---|
| Project search | name+path | **none** (years-old complaint) | yes | yes, + `tag:` syntax | yes, name+path |
| Sort / favorites | column sort + star | none | favorites pinned | sort dropdown + favorites pinned | **recency only -- no sort, no favorites** |
| Missing-project state | warning icon; hide/remove-missing | **tile silently vanishes** | n/a | dimmed + broken icon; Remove only | dimmed + coral badge + menu narrows to Locate/Remove |
| Relocate a moved project | **none** (users ask) | **none** | n/a | **none** (open proposal #9829) | **yes -- Locate... keeps pin/args, refreshes name/ABI** |
| Rename | display name only (Hub list) | none | n/a | project name field only | **full on-disk rename** (folder + manifest, rollback on failure) |
| Per-project engine/version pin | yes + change-version dialog | yes (`EngineAssociation`) | yes (limited to associated IDEs) | n/a (engine = the binary) | yes: pin or default, dangling-pin state, chip text |
| Version mismatch on open | pick/install dialog | pick + data-loss warning | n/a | graduated convert dialogs; hard refusal on newer | pre-launch re-probe + compat note; editor's own ABI gate refuses with reason surfaced (exit-2 toast) |
| Same project opened twice | editor-side lockfile; **stale-lock false "already open" is a common complaint** | n/a | n/a | n/a | **live pid map -> focuses the running editor; stale entries self-clean** |
| Launch lifecycle | minimize-to-tray (settings) | auto-minimize (+tray setting) | tray-resident widget | n/a (PM *is* the editor) | hide -> **restore when the LAST editor exits** (+toggle); editors survive Hub exit |
| Running indicator | "already open" row-warning only | none verified | none (only "Update Pending") | none | none in the UI (backend tracks pids) |
| Taskbar identity | -- | -- | tray-only (complained about) | -- | **AUMID family grouping**: Hub + editors under one `dev.starworks.arcane` button |
| File association / deep links | `unityhub://` links only | **`.uproject` + UnrealVersionSelector**: double-click routes to the right engine, right-click verbs | `jetbrains://` + generated CLI launchers | none | **none -- `.arcproj` is not shell-registered** |
| Create + templates | full gallery, downloadable templates | none in the launcher (engine-side Project Browser) | n/a | create + renderer pick | name/location/engine -> blank project |
| Bulk import / scan | add from disk / from repo | none (list driven by hand-editable ini) | auto-detects installs | **recursive Scan of a folder tree** | one at a time via file dialog |
| Duplicate / clone project | no | Clone | no | Duplicate | no |
| Thumbnails | icons only | auto-screenshot PNG per project | icons | project icon | generated monogram cover |
| Engine install / download | full pipeline + modules | full + per-install components + **Verify/repair** | full + channels + **instant rollback** | none (third-party hubs fill it) | locate-based registry (per scope) |
| State durability | project-list loss is a known complaint class; no file versioning | plain ini; entries drop silently | fine | lives in project.godot | **versioned envelope + atomic writes + corrupt-quarantine + too-new guard** |
| Self-update | forced (heavily complained) | forced, blocking at startup (complained) | self-updates + restart | n/a | none (manual NSIS reinstall) |
| Sign-in required | **YES** (mandatory since 3.6; top complaint) | **YES** | optional | no | **no** |
| Store / news / marketing surface | some | heavy (the core "bloat" complaint) | no | no | **none** |

## Where the Hub beats the verified field

1. **Moved-project recovery.** None of the four comparables can re-point a
   moved project: Unity says remove-and-re-add, Epic tiles silently vanish,
   Godot's Locate is an open unimplemented proposal. Our Locate... (preserving
   the engine pin, args, and history while refreshing name/ABI) is a flow the
   whole field is missing and users of two of them actively request.
2. **Double-open enforcement quality.** Unity's is an editor-side lockfile
   whose stale locks after crashes produce false "already open" errors (a
   recurring complaint). Ours is a live pid map: relaunch focuses the running
   editor, and a dead pid is detected and cleaned on the spot.
3. **Launch lifecycle.** The field's model is minimize-to-tray, and its users
   complain about launchers lingering in the background and popping back up.
   Ours hands the screen over and RETURNS when the last editor exits -- no
   tray residue, and relaunching the Hub un-hides it early via the
   single-instance callback.
4. **State durability.** Versioned `.archub` envelopes, atomic temp-rename
   writes, corrupt-file quarantine, and a too-new-version guard -- against a
   field where Unity list-loss has its own community fix-it folklore and
   Epic's project list is a hand-editable ini.
5. **On-disk rename.** Unity renames only its display label; Godot only the
   name field. Ours renames folder + manifest with rollback on every failure
   path.
6. **Taskbar identity.** No comparable groups launcher + editors into one
   taskbar family. `dev.starworks.arcane` AUMID grouping is beyond the field.
7. **Structural absence of the field's top complaints.** Login gates, forced
   updates, store/news surface, tray residency, Electron/Slate weight -- every
   top complaint category across all four products is absent by design here.

## Where the field beats the Hub (ranked)

**Tier 1 -- addressable now, core-flow:**
- **`.arcproj` shell integration** -- THE gap. UE's model (double-click a
  .uproject -> UnrealVersionSelector -> the right engine opens it) is the gold
  standard, and we have the plumbing: single-instance already forwards a second
  invocation's argv, and engine resolution + the probe already exist. Register
  the association (Tauri `fileAssociations` + NSIS), route a double-clicked
  .arcproj through the Hub's resolve -> launch path.
- **Favorites + sort.** Table stakes in Unity (stars + column sort) and Godot
  (sort dropdown, favorites always pinned above). We order by recency only.
- **Running badge.** No comparable shows which project is open -- and we
  already track the pids. A card badge (+ a running-changed event) would
  exceed the entire field for near-zero cost, and would explain WHY launch
  focuses an existing window.
- **Duplicate project.** Epic and Godot both have it; cheap and genuinely
  useful for "riff on this project".

**Tier 2 -- scale-triggered (roughly the ~20-project mark):**
- Recursive **Scan-folder bulk import** (Godot's is the model).
- **Tags** (Godot: stored in the project, `tag:` search syntax).
- **Template gallery** in New Project (Unity's is the model; Epic proves
  launcher-side creation can stay minimal instead).
- **Real thumbnails** -- Epic's convention is the precedent to steal: the
  editor auto-writes a screenshot (`Saved/AutoScreenshot.png`) and the
  launcher falls back to it. Engine-side work; already named as the Hub's
  biggest visual leap.

**Tier 3 -- distribution-triggered (when engines ship as builds to others):**
- Engine download/install, per-install **Verify/repair** (Epic), version
  channels + rollback (Toolbox).
- Hub **self-update** (do it opt-in; the field proves forced updates are
  hated), code signing.

**Deliberate non-goals, validated by the field's complaint logs:** sign-in,
store/news/marketing, telemetry, forced updates, tray residency. Every one is
a top-cited pain in at least one comparable. Keeping them out IS the "sleek"
in the bar.

## Seam grades (the ones invisible to code review)

- **Launch & lifecycle: A.** Tracked children, hide/restore-on-last-exit,
  per-project focus-existing, boot-refusal surfaced with reason, editors
  survive Hub death. Exceeds all four comparables' verified behavior.
  Conditional on the owed interactive desk pass.
- **OS integration & identity: B+.** AUMID taskbar grouping is ahead of the
  field, but the absence of `.arcproj` shell registration is the one place a
  core flow (open a project from Explorer) dead-ends outside the Hub.
- **Engine management, as scoped: A.** Locate-based registry, probe-as-truth,
  stale-ABI self-heal on load and pre-launch, per-project pin with dangling
  handling. Epic's verify/repair only becomes relevant when engine builds are
  distributed.
- **Project management: A-.** Search, missing-state, Locate..., true rename,
  per-project args all strong; loses the half-grade to no favorites/sort and
  no duplicate.
- **Creation: B+ by design.** Matches Epic's minimal launcher-side model;
  templates are Tier 2.
- **State/robustness: A+.** Field-verified now, not just internally graded:
  the comparables are demonstrably worse here.

## Verdict

**A as a product, measured against the field at its declared scope** -- with
three of the four comparables beaten outright on the flows they get the most
complaints about (moved projects, double-open, lifecycle, launcher bloat).
The distance to a defensible S/A+ is short and concrete: `.arcproj` shell
integration (the one core flow where UE is still ahead), favorites + sort,
and the running badge. Nothing structural stands in the way of any of them.

## What this review does NOT cover

- The interactive desk pass (user-owed): actually watching hide/restore,
  focus-existing, taskbar grouping, and the refusal toasts on this desktop.
  This session cannot safely launch editors (windowed D3D12 under the
  Parsec/virtual-display environment SIGSEGVs).
- NSIS installer hand-verify (no-admin install) -- oldest standing debt.
- Accessibility, keyboard-only navigation, DPI/multi-monitor behavior.
- Long-run soak (wait-thread/pid-map behavior over many launch cycles).
- macOS/Linux (the Hub is Windows-only today).
- Hands-on side-by-side with the comparables on this machine -- their
  behavior is verified from docs/source/forums, not personally driven.
