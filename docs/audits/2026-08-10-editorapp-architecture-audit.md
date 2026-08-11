# EditorApp Architecture Audit

**Date:** 2026-08-10
**Scope:** `Arcane/ArcaneEditor/src/EditorApp.{hpp,cpp}`, `EditorAppFrame.cpp`,
`EditorAppScene.cpp`, `EditorAppProject.cpp` — WORKING TREE state (includes the
user's Create/Init/Main/Shutdown/Destroy boot refactor, uncommitted at audit
time). Cross-host comparison against `ArcaneRuntime/src/RuntimeApp.*`.
**Line numbers** cite the audited tree state and will drift; anchor on names.

## Verdict

5,332 lines, 104 data members, five files. Zero TODO/FIXME markers — the debt
lives in prose comments, and the comments are the diagnostic: `switch_teardown`
carries five inline essays each re-arguing why one member belongs in the reset
list (the shape of a list nobody can validate), and EditorApp.hpp:139-144
admits MainLoop's phase order "is LOAD-BEARING and has NO automated coverage."

Good bones, kept: MainLoop's 20 named phases; the SceneSession confirm
machine; the Create/Init/Main/Shutdown/Destroy + kHostStages boot shape; the
"panel reports, app performs" split. The rot is concentrated in cross-cutting
state that never got an owner: the Play/edit-mode predicate, async dialog
results, per-project lifetime, keyboard edges, and error modals.

## A. Behavior defects (found by the audit, present today)

### A1. `m_editBinding.editMode` one-frame staleness — memento against the play registry

Written ONCE per frame in phase 18 (`DrawSelectionPanels`,
EditorAppFrame.cpp:1884). Read in phase 14 (`DrawEditorUi` consume block) by
Delete (:1322), Cut (:1342), Paste (:1344), Duplicate (:1346). On the frame
Play starts (toolbar click, same phase 14), `editMode` still holds last
frame's `true`: a Del / Ctrl+V / Ctrl+D that frame reaches `ApplyStructural`
with `editMode == true` and pushes a whole-registry memento against the
play-time registry — exactly what EditorPanels.cpp:970-971 documents as
must-not-happen. Cut alone is immune via the hand-patched `!IsPlaying()` at
:1341 (2026-08-10 final-review fix wave). Every future consumer added to a
phase <= 14 must independently rediscover the lag.

### A2. `m_pendingInstanceParent` — unguarded, uncleared dialog sidecar

EditorApp.hpp:608. Written at the "New Instance..." launch site
(EditorAppFrame.cpp:1440) OUTSIDE `m_pendingMaterialMutex`, read at consume
(:412) outside the lock, never cleared, survives project switches. Two
overlapping instance dialogs, or a dialog outliving a switch, apply a stale /
wrong-project parent Guid. Sibling defect: `ConsumeMaterialDialogResults`
(:391-414) is the only dialog consumer with NO guard of any kind — a dialog
opened in project A lands in project B.

### A3. Per-project reset is three implicit lists, one of them unowned

- `switch_teardown` (EditorAppProject.cpp:529-573) resets 8 things.
- `ClearSceneReferences` (EditorAppScene.cpp:164-181) resets 6 more.
- NOTHING resets: `m_materialMtimes`/`m_materialWatchNext` (outgoing project's
  path-keyed cache, grows unbounded across switches), all six pending dialog
  slots, `m_pendingInstanceParent` (A2), `m_launchModalPending`/
  `m_launchAfterSceneSave` (a parked "Save and Play?" survives the switch),
  `m_sceneError`/`m_launchError` (a dead project's modal can pop after the
  switch). `m_activeMaterialGuid`/`m_materialDocCount` self-heal next frame by
  accident (EditorAppFrame.cpp:1824-1834).
- The one consumer-side staleness check done right: `PollModuleBuild`'s
  `proj->Root() != m_moduleBuildRoot` (EditorAppProject.cpp:892). Nothing else
  copies it.

## B. Spread behavior (encapsulation candidates, ranked)

### B1. Four representations of ONE Play/edit predicate

`!m_play.IsPlaying()` x21 in EditorAppFrame.cpp (472, 494, 506, 543, 611, 647,
697, 713, 750, 981, 1002, 1060, 1089, 1123, 1126, 1162, 1225, 1341, 1605,
1765, 1884) + EditorAppScene.cpp:171, 251 + EditorAppProject.cpp:796, 952;
plus `m_editBinding.editMode`; plus the panels' `playing` parameter; plus the
keybinds' `active` (:543). Three near-duplicate "shortcuts live" predicates
differing only in `m_viewportActive` (:543, :611, :697). A1 is the direct
consequence. **Proposal:** one per-frame context snapshot computed at frame
top, passed down; `editMode` derived from it at the same point.

### B2. Async dialog plumbing — one behavior in ~19 sites

Six byte-identical `*PickedThunk` statics (EditorAppProject.cpp:208-231,
374-383; EditorAppScene.cpp:126-141), six pending strings + three mutexes
(EditorApp.hpp:602-605, 676-677, 710-712) + the A2 sidecar, three consumers
with three DIFFERENT guard strengths (scene: dirty-guard + extension coercion
+ two parked-resume protocols; project: dirty-guard; material: none), six
dialog-launch sites + one wrapper + two recents slot-injection bypasses
(:1259-1263, :1268-1272). **Proposal:** one `DialogSlot` (stash-under-lock /
consume-at-frame-top / clear-on-switch contract, payload struct per slot so
the instance-parent rides WITH its path), consumers keep their per-kind
guards.

### B3. `DrawEditorUi` — 333 lines, 57% flat consume-run

EditorAppFrame.cpp:1212-1544; 33 sequential sub-blocks: 19 `menuReq` consume
blocks (:1242-1431 = 190 lines), 6 `browserActions` consume blocks
(:1438-1494), 2 keybind fold-ins, 5 panel draws, 1 inline material-doc
resolve. **Proposal:** extract `ConsumeMenuRequests(...)` and
`ConsumeBrowserActions(...)`; the remainder is a legible shell (menu context
-> dockspace -> toolbar -> panels).

### B4. `SwitchProject` — 398 lines, the largest function in the class

EditorAppProject.cpp:385-782. Re-declares its own `stage(...)` factory (:503)
instead of ProjectBoot's `Make`; its switch stages re-implement
`project_open`/`render_bridge`/`plugin_load` bodies that exist shared in
ProjectBoot.cpp. The project-open success tail (boot-scene Adopt +
`m_frameOnSceneOpen` + `EnsureScene` + `UpdateWindowTitle` +
`NoteProjectOpened` + `ReloadSceneRecents`) is duplicated verbatim at
EditorApp.cpp:710-730 (StageFinalize) and EditorAppProject.cpp:665-682, with a
partial third copy in the failure fallback (:773-779).

### B5. Keyboard edge detection — 18 hand-rolled copies of one idiom

17 `m_prevKey*`/`m_prevLmb/Rmb` bools (EditorApp.hpp:438-490, 508-512), 18
`down && !prev` expressions, write-backs scattered across THREE functions
(HandleUndoRedoAndSceneShortcuts :555-632, UpdateEditorCamera :686/:702-703,
UpdateGizmoInteraction :890). Adding a keybind = 3 edits; forgetting the
write-back = auto-repeat, not a compile error. The header admits the
duplication ("same pattern as..." :472-484). **Proposal:** a tiny `KeyEdge`
tracker (Update(down) -> pressed).

### B6. Modal error idiom — 5 copies, 3 parallel error strings

:1554-1570 (Open Project Failed), :1647-1662 (Scene Error), :1738-1754
(Standalone Failed) are the same ~16 lines with title/string swapped; re-arm
half repeated at :1575-1577, :1671-1673; a sixth copy in DocumentHost.cpp:191-
198. `m_projectOpenError`/`m_sceneError`/`m_launchError` exist only so each
copy has its own variable. **Proposal:** one error-modal helper + one queue.

### B7. Member sprawl — 8 cohesion candidates covering ~46 of 104 members

| Candidate | Members | Note |
|---|---:|---|
| KeyEdge tracker | 17 | B5 |
| Dialog inbox | 9 | B2 (6 strings + 3 mutexes; +1 sidecar) |
| Game-UI input handoff | 5 | header apologizes for the hoist (:538-541) |
| Console/diagnostics bundle | 5 | install/uninstall/draw travel together |
| Viewport targets | 3 | `m_viewport`/`m_pick`/`m_outline` resize in lockstep (:915-921) |
| Standalone launch | 3 | 3-field state machine, 2 documented clear sites |
| Camera pan gesture | 3 | |
| Modal errors | 3 | B6 |

### B8. Recents duo — two fully parallel ~10-site ladders

Every hook site already calls both in adjacent lines (EditorApp.cpp:729/730,
EditorAppProject.cpp:681/682, EditorAppFrame.cpp:1279/1280). A third recents
kind costs ~10 edits. **Proposal:** a small facade (`RefreshAllRecents()` +
one note-success entry per kind).

### B9. Scene-intent completion protocol — two conventions, nothing enforcing

Six `SceneSession::Request` sites: three act immediately on `true`
(:236, :311, :382), three defer via `ls.sceneAction` (:1364, :1413, :1474);
`TakePending`/`ClearPending` pairs appear in three places (:338/:342,
:1617/:1625, :1632/:1639). The never-saved -> async-Save-As -> resume idea is
implemented twice in parallel (`m_scene.Pending()` route and
`m_launchAfterSceneSave` route, resumed 18 lines apart). `DoSaveScene` has 5
call sites with 5 different pre/post protocols; its internal Play backstop
(EditorAppScene.cpp:251) exists because the call sites cannot be trusted to
agree.

## C. Cross-host duplication (EditorApp vs RuntimeApp) — a separate fold arc

RuntimeApp: 696+113 lines, one 275-line inline MainLoop. Near-identical
blocks duplicated across hosts: both `main()`s (4 byte-for-byte blocks:
log+mosaic init, `--print-engine-info`, Diagnostics::Install, splash+teardown
tail), `StageRuntimeCreate`, `StageGpuCore`, render_bridge first half,
sprite_tables resolver wiring, the ~40-line window reveal, the Shutdown tail
(teardown-contract comment pasted verbatim, each citing the other), frame
pump, sim advance, end-of-frame poll/budget.

Sharper asymmetries:
- Runtime's stage-patch loop has NO miss detection (RuntimeApp.cpp:651-673);
  the editor's kHostStages table + fail-loud lookup does. Fold the table
  pattern into Arcane/Host so both hosts get it.
- Runtime hand-rolls the post-chain->tonemap composite (:518-544) that the
  editor already delegates to `OffscreenCanvas::SetPostChain`.
- `ActiveSceneCamera` census (warn-once no-camera/multi-camera) is
  runtime-only (:473-499); the editor silently falls through.
- `HostBoot::BootScene` is called from DIFFERENT stages per host (editor:
  finalize; runtime: plugin_load) with divergent `--scene` support.
- `--perf` is DEAD in the editor: `m_perf` constructed, never driven.
- The editor's project-SWITCH stages re-implement shared ProjectBoot bodies
  (see B4) — the switch path is itself a fold candidate.

## Recommended sequencing

1. **Hazard pass** (small): frame-top Play/edit context (fixes A1, collapses
   B1), dialog inbox (fixes A2, collapses B2), one per-project reset registry
   (fixes A3). All three bugs die by construction, not by patching.
2. **Decomposition pass**: B3 consume extraction, B4 success-tail dedup +
   ProjectBoot::Make reuse, B5 KeyEdge, B6 modal helper, the B7 groupings
   worth their churn, B8 recents facade. B9's protocol unification rides
   along or gets explicitly documented as two sanctioned conventions.
3. **Host-fold arc** (separate): C — EditorApp/RuntimeApp duplication into
   `Arcane/Host`, including the switch-stage unification.

Audit inputs: three parallel source surveys over the working tree (class
surface map, cross-file flow trace, host comparison), 2026-08-10.
