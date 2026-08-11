# EditorApp Architecture Pass -- Design

**Date:** 2026-08-11
**Input:** `docs/superpowers/audits/2026-08-10-editorapp-architecture-audit.md`
(the audit is the evidence base; this spec is the remedy). Audit line numbers
have drifted -- anchor on names.
**Scope (user-ratified):** ONE spec covering both the hazard pass and the
decomposition pass; ALL EIGHT member groupings from audit B7; FULL
SwitchProject unification onto the shared ProjectBoot stage bodies.
**Out of scope:** the cross-host fold (audit section C -- EditorApp/RuntimeApp
duplication into `Arcane/Host`) is a separate future arc. RuntimeApp is not
touched by this pass.

## Goals

1. Kill the audit's three behavior defects (A1 stale `editMode`, A2 unguarded
   instance-dialog sidecar, A3 unowned per-project reset) **by construction**
   -- each dies because the state acquires an owner, not because a guard got
   patched in.
2. Decompose the spread behavior (B1-B9): one Play/edit authority, one dialog
   inbox, one per-project reset list, extracted consume functions, one edge
   tracker, one error-modal queue, the eight member groupings, one recents
   facade, and the standalone-launch flow folded into the SceneSession intent
   machine.
3. Zero intended behavior change EXCEPT the three defect fixes and the two
   deltas called out inline (launch confirm modal becomes the shared confirm
   modal with intent-specific buttons; error modals queue instead of racing).

## Non-goals

- No host-fold work (RuntimeApp's missing patch-miss detection, the shared
  main() blocks, BootScene stage divergence -- all audit C, all deferred).
- No new features. No panel/UX changes beyond what falls out of the fixes.
- No renames of existing public engine API. Everything here is editor-side
  (`Arcane/ArcaneEditor/src/`) except the one `SceneIntent` enum addition.

---

## Part I -- Hazard pass

### 1. Play/edit authority (fixes A1, collapses B1)

**Problem.** `m_editBinding.editMode` is written once per frame in phase 18
(`DrawSelectionPanels`) but consumed in phase 14 (`DrawEditorUi`) by Delete /
Cut / Paste / Duplicate. On the frame Play starts (toolbar click, itself phase
14), those consumers see LAST frame's `true` and push a whole-registry memento
against the play-time registry. Cut alone is immune via a hand-patched
`!IsPlaying()`. Separately, the same predicate exists in four shapes
(`!m_play.IsPlaying()` x25, `m_editBinding.editMode`, the panels' `playing`
parameter, the keybinds' `active`), and three near-duplicate "shortcuts live"
predicates differ only in `m_viewportActive`.

**Design.**

- `m_editBinding.editMode = !m_play.IsPlaying()` is written at **two** points,
  both in `EditorAppFrame.cpp`:
  1. **Frame top** -- before any phase that reads it (a new line in the frame
     preamble, alongside the existing per-frame input snapshot).
  2. **Immediately after the toolbar draw** -- the toolbar's Play/Stop buttons
     are the only site that flips PlayMode state mid-frame today. Re-deriving
     right after the flip point makes every later phase (including the rest of
     phase 14's consume blocks and phase 18's panels) see the true state.
  The phase-18 write is deleted. Rule, documented at the frame-top write:
  *"every site that can call PlayMode Start/Stop within the frame must be
  followed by this same re-derivation; today that is exactly the toolbar."*
- New accessor on EditorApp: `bool InPlayMode() const { return
  m_play.IsPlaying(); }`. Editor-side code stops writing the raw
  `!m_play.IsPlaying()` expression; call sites become `!InPlayMode()` (or read
  `m_editBinding.editMode` where the binding is already in hand). This is a
  mechanical sweep of the ~25 sites -- no logic change, one greppable name.
- Cut's hand-patched `!m_play.IsPlaying()` guard (the 2026-08-10 final-review
  fix) is **deleted** -- `CutSelection` already gates on `!binding.editMode`
  before the copy half, and the binding is now fresh at consume time. This is
  the proof the fix is structural: the patch becomes redundant.
- The three "shortcuts live" predicates collapse into one helper in
  `EditorAppFrame.cpp`:
  `bool ShortcutsLive(bool requireViewportFocus) const` -- body is the current
  common predicate, with the one divergent term (`m_viewportActive`) applied
  only when `requireViewportFocus`. The three sites call it with the argument
  matching their current behavior (byte-identical predicate per site).

### 2. Dialog inbox (fixes A2, collapses B2)

**Problem.** Six byte-identical `*PickedThunk` statics, six pending strings +
three mutexes, plus `m_pendingInstanceParent` -- a sidecar written OUTSIDE any
lock at the "New Instance..." launch site, read outside any lock at consume,
never cleared, surviving project switches. The three consumers apply three
different guard strengths (scene: dirty-guard + extension coercion; project:
dirty-guard; material: none). A dialog opened in project A can land in
project B.

**Design.** One reusable slot type, new header
`Arcane/ArcaneEditor/src/DialogSlot.hpp` (header-only, pure, headless-testable
-- same PURE-state/host-performs-effects split as SceneSession):

```cpp
// One in-flight async dialog result. Payload is per-slot (std::string path
// for plain pickers; a struct where context must ride WITH the path).
template <typename Payload>
class DialogSlot
{
public:
    // Called at the dialog-LAUNCH site. Returns the epoch the completion
    // thunk must present. Arming while a result is pending overwrites it
    // (matches today's last-writer-wins).
    [[nodiscard]] std::uint64_t Arm();

    // Called from the dialog's completion thunk (any thread). Ignored if
    // `epoch` is stale (a Clear() or re-Arm() happened since Arm).
    void Stash(std::uint64_t epoch, Payload value);

    // Called once per frame at the consume site. Returns the payload and
    // empties the slot.
    [[nodiscard]] std::optional<Payload> Take();

    // Bumps the epoch and drops any pending value. Called on project switch
    // (via ResetPerProjectState) -- late-arriving thunks from the dead
    // project stash into a bumped epoch and are dropped.
    void Clear();

private:
    std::mutex             m_mutex;
    std::uint64_t          m_epoch = 0;   // bumped by Arm() and Clear()
    std::optional<Payload> m_value;       // guarded by m_mutex, like m_epoch
};
```

- EditorApp grows one `struct DialogInbox` member holding the six slots:
  `sceneOpen`, `sceneSaveAs` (both `DialogSlot<std::string>`), `projectOpen`
  (`DialogSlot<std::string>`), `materialOpen`, `materialSaveAs`
  (`DialogSlot<std::string>`), and `instanceNew`
  (`DialogSlot<InstanceNewResult>` where
  `struct InstanceNewResult { std::string path; Arcane::Guid parent; }`).
  `DialogInbox::ClearAll()` clears all six. The six pending strings, the
  three mutexes, and `m_pendingInstanceParent` are **deleted**.
- The six `*PickedThunk` statics collapse to one lambda shape at each launch
  site: `[this, epoch](std::string path) {
  m_dialogs.<slot>.Stash(epoch, ...); }` -- the thunk is now three lines and
  carries its epoch, so a copy-paste divergence has nowhere to live.
- The A2 fix specifically: the "New Instance..." launch site calls
  `const auto epoch = m_dialogs.instanceNew.Arm();` and the thunk stashes
  `{path, parentGuidCapturedAtLaunch}` -- the parent Guid rides WITH its path,
  under the lock, cleared with the slot. Two overlapping dialogs: the second
  `Arm()` bumps the epoch, the first thunk's stash is dropped.
- Consumers keep their per-kind guards exactly as they are (dirty-guard,
  extension coercion, parked-resume). The change is WHERE the result comes
  from (`Take()` at the frame-top consume site), not what happens to it.
- The two recents slot-injection bypasses (Open Recent writing the pending
  path directly) become `Arm()` + immediate `Stash()` -- same data path as a
  real dialog, no second entry point.

### 3. Per-project reset registry (fixes A3)

**Problem.** Reset-on-switch is three implicit lists: `switch_teardown` resets
8 things, `ClearSceneReferences` 6 more, and NOTHING resets the material
watcher caches (`m_materialMtimes`/`m_materialWatchNext` -- unbounded growth
across switches), the dialog slots, the launch flags, or the error strings.

**Design.** One function, one list, one rule:

- New `void EditorApp::ResetPerProjectState()` in `EditorAppProject.cpp`,
  called from the `switch_teardown` stage (and ONLY from there -- boot has no
  prior project to reset). Body: the current `switch_teardown` reset list, plus
  `m_dialogs.ClearAll()`, `m_materialMtimes.clear()` +
  `m_materialWatchNext = {}`, `m_modalErrors.Clear()` (section 7), and the
  standalone-launch reset (section 9 folds those flags into SceneSession;
  until then the two bools). `ClearSceneReferences` stays separate -- scene
  lifetime is shorter than project lifetime (it also runs on New/Open Scene)
  -- and `ResetPerProjectState` calls it.
- The five inline essays in `switch_teardown` re-arguing each member's
  membership move to ONE comment block above `ResetPerProjectState`, and the
  rule is stated there: *"every mutable EditorApp member whose value refers to
  the current project must appear in this function or carry a comment at its
  declaration saying why it survives a switch."* The self-healing pair
  (`m_activeMaterialGuid`/`m_materialDocCount`) gets that comment instead of
  relying on accident.
- `PollModuleBuild`'s root-mismatch staleness check stays -- it guards an
  async worker, not resettable state; the comment there gains a pointer to
  this section's rule.

---

## Part II -- Decomposition pass

### 4. DrawEditorUi extraction (B3)

333 lines, 57% flat consume-run, becomes a shell. Three extractions, pure code
motion, no behavior change:

- `void ConsumeMenuRequests(MenuRequests& req, LateState& ls)` -- the 19
  menu-request consume blocks.
- `void ConsumeBrowserActions(AssetBrowserActions& actions, LateState& ls)` --
  the 6 browser-action consume blocks.
- `void ResolveActiveMaterialDoc()` -- the inline material-doc resolve.

(The exact `LateState`/local-state plumbing follows what the code already
threads; the extraction passes references to the same locals. Names final at
implementation; the shell that remains is: menu context -> dockspace ->
toolbar -> editMode re-derivation (section 1) -> consume calls -> panel
draws.)

### 5. SwitchProject unification (B4 -- FULL, per user ruling)

**Problem.** `SwitchProject` is 398 lines, re-declares its own `stage(...)`
factory, and re-implements `project_open` / `render_bridge` / `plugin_load`
bodies that exist shared in `ProjectBoot.cpp`'s
`HostBoot::EditorStages(ctx)`. The project-open success tail is duplicated
verbatim in three places (StageFinalize, switch success, and a partial copy in
the switch failure-fallback).

**Design.**

- Extract `void PatchHostStages(std::vector<Arcane::BootStage>& stages)` from
  `Create()`'s existing `kHostStages` patch loop (EditorApp.cpp) -- the table,
  the fail-loud miss detection in BOTH directions (patch id gone from
  EditorStages; stage id unpatched that the table claims), unchanged. Create()
  calls it; SwitchProject now calls it too.
- SwitchProject builds its stage list by **cherry-picking ids** from
  `Arcane::HostBoot::EditorStages(m_bootCtx)` after `PatchHostStages`:
  it keeps the subset of stage ids a switch needs (the boot-only stages --
  window/GPU/fonts/shell -- are skipped by id; the teardown stage is
  switch-specific and is prepended as a local stage). The hand-rolled
  `stage(...)` factory and the re-implemented bodies are **deleted**.
- Where the switch genuinely needs a different body than boot (if any survive
  contact with the code), the delta is expressed as a **switch-local patch
  table** applied after `PatchHostStages` -- same mechanism, so every
  boot-vs-switch divergence is enumerable in one place and each entry carries
  a comment saying why the switch differs. No silently forked bodies.
- Extract `void OnProjectOpened()` -- the success tail (boot-scene Adopt +
  `m_frameOnSceneOpen` + `EnsureScene` + `UpdateWindowTitle` +
  `NoteProjectOpened` + `ReloadSceneRecents`; the last two become one facade
  call per section 10). Called from StageFinalize (boot), from the switch
  success path, and the failure-fallback's partial third copy is rewritten to
  call it too (it re-opens the OLD project, so the same tail applies; any
  step it must skip is skipped explicitly with a comment, not by omission).
- This is the highest-risk section and lands LAST (see Sequencing). The
  switch path's desk-verify is the heaviest item in Testing.

### 6. Input edge tracker (B5)

New header `Arcane/ArcaneEditor/src/InputEdges.hpp` (header-only, pure,
headless-testable):

```cpp
struct Edge
{
    bool down     = false;
    bool pressed  = false;   // this frame's rising edge
    bool released = false;   // this frame's falling edge

    void Update(bool nowDown)
    {
        pressed  = nowDown && !down;
        released = !nowDown && down;
        down     = nowDown;
    }
};
```

EditorApp's 17 `m_prevKey*` / `m_prevLmb` / `m_prevRmb` bools become one
`struct InputEdges { Edge undo, redo, save, ... , lmb, rmb; }` member with
named fields, all `Update`d together at frame top (section 1's preamble). The
18 hand-rolled `down && !prev` expressions become `.pressed` reads; the
scattered write-backs across three functions **vanish** (Update owns them).
Adding a keybind = one field + one Update line + one `.pressed` read;
forgetting the Update line makes the key dead, not auto-repeating.

### 7. Modal error queue (B6)

New header `Arcane/ArcaneEditor/src/ModalErrorQueue.hpp` (pure state,
headless-testable; drawing stays host-side per the ConsoleBuffer split):

```cpp
struct ModalError { std::string title, message; };

class ModalErrorQueue
{
public:
    void Push(std::string title, std::string message);
    [[nodiscard]] const ModalError* Front() const;   // nullptr = none
    void Pop();
    void Clear();
private:
    std::deque<ModalError> m_queue;
};
```

- `m_projectOpenError` / `m_sceneError` / `m_launchError` are **deleted**;
  every site that set one now calls `m_modalErrors.Push(title, msg)`.
- ONE drawing block in `DrawModals` renders `Front()` as the standard error
  popup and `Pop()`s on OK -- replacing the five ~16-line copies and their
  re-arm halves. Errors now display **sequentially** instead of three parallel
  strings racing for popups (this is one of the two sanctioned behavior
  deltas; today two simultaneous errors are a coin toss).
- `DocumentHost.cpp`'s sixth copy is NOT folded -- DocumentHost is a
  self-contained unit with its own draw path. Its copy gets a one-line comment
  pointing here; folding it means threading the queue through DocumentHost's
  interface and is not worth the coupling. Ledgered as future polish.

### 8. Remaining member groupings (B7 -- all eight, per user ruling)

Sections 2, 6, 7 already delivered three groupings (DialogInbox, InputEdges,
ModalErrorQueue). The remaining five are member-sprawl consolidation in
`EditorApp.hpp` -- plain nested structs, no new files, no behavior change:

| Group | Members | Notes |
|---|---|---|
| `GameUiInputHandoff` | 5 | the header's apology comment for the hoist moves onto the struct |
| `ViewportTargets` | 3 | `m_viewport`/`m_pick`/`m_outline`; `ApplyPendingViewportResize` becomes a method on it (the three resize in lockstep today) |
| `ConsoleDiagnostics` | 5 | install/uninstall/draw state travels together; `InstallConsoleSink` moves onto it |
| `CameraPanGesture` | 3 | pan-anchor state used only by `UpdateEditorCamera` |
| `StandaloneLaunch` | 3 -> ~1 | mostly DISSOLVED by section 9 (the two park flags move into SceneSession); whatever remains (process handle/path) stays grouped |

### 9. Standalone launch joins the SceneSession machine (B9 + the StandaloneLaunch dissolve)

**Problem.** The never-saved -> async-Save-As -> resume protocol is
implemented twice in parallel: once through `SceneSession`'s pending machine,
once through `m_launchModalPending`/`m_launchAfterSceneSave`, resumed 18 lines
apart. Six `Request` sites follow two different completion conventions with
nothing enforcing either.

**Design.**

- `SceneIntent` (SceneSession.hpp) gains `LaunchStandalone`. The launch flow
  becomes `m_scene.Request(SceneIntent::LaunchStandalone, {}, stack)` -- clean
  scene: launch immediately; dirty or never-saved: the intent parks in the
  SAME machine as NewScene/OpenScene/OpenProject/Exit.
  `m_launchModalPending` and `m_launchAfterSceneSave` are **deleted**, along
  with the "Save and Play?" popup and its parallel resume path.
- The shared confirm modal parameterizes by intent: for `LaunchStandalone`
  the affirmative button reads **"Save and Play"** and there is **no Discard
  button** (the standalone process reads the scene from disk -- "discard and
  play" would run a stale file; today's popup already has no Discard, so this
  preserves behavior). All other intents keep Save/Discard/Cancel unchanged.
- Completion convention (the B9 rule): a comment block at
  `SceneSession::Request`'s declaration documents the ONE sanctioned pattern
  -- *"on `true`, perform the action via the same code path the parked-resume
  uses; on `false`, do nothing (the modal + `TakePending` own it)"* -- and the
  performing code for each intent lives in exactly one function that both the
  immediate and the resumed path call. The audit's "three act-immediately,
  three defer via `ls.sceneAction`" split is normalized to the deferral
  pattern where the sites differ (deferral is the safer of the two: it
  performs after the frame's UI is done). `DoSaveScene`'s internal Play
  backstop stays -- it is defense in depth, and section 1 makes its trigger
  condition honest.

### 10. Recents facade (B8)

New thin struct in the editor (no new file needed -- lives beside the recents
code it wraps):

```cpp
struct EditorRecents
{
    void RefreshAll();                                  // reload both lists
    void NoteProjectOpened(const std::filesystem::path& arcproj);
    void NoteSceneOpened(const std::filesystem::path& arcscene);
};
```

The ~10 paired call sites (`NoteProjectOpened` + `ReloadSceneRecents` always
adjacent, etc.) become single facade calls. A third recents kind is one new
method, not ten edits. The underlying Hub-recents chain and `SceneRecents`
are unchanged -- this is call-site consolidation only.

---

## Testing

**New headless suites** (Catch2, in `Arcane/Tests/src/`, added to ArcaneTests'
EXPLICIT premake file list + `GenerateProjects.bat` rerun; no `[gpu]` tag; no
bare `Arcane::Runtime` -- these units touch neither):

- `InputEdgesTest.cpp` -- Edge rising/falling/held/repeat semantics; a missed
  Update means dead-not-repeating (the designed failure mode).
- `DialogSlotTest.cpp` -- Arm/Stash/Take round trip; stale-epoch Stash dropped
  after Clear and after re-Arm; Take empties; payload struct rides atomically
  (the A2 regression case: two Arms, first thunk's stash dropped).
- `ModalErrorQueueTest.cpp` -- FIFO order, Front/Pop, Clear.
- `SceneSessionTest` additions -- `LaunchStandalone` parks when dirty, second
  Request ignored while parked, TakePending performs, ClearPending abandons;
  clean scene returns true immediately.

**Gates (every landing):** both hosts (ArcaneEditor + ArcaneRuntime) build
Debug AND Release, 0 errors / 0 warnings; full ArcaneTests `~[gpu]` suite
green, run FROM THE EXE DIR, seed banner captured.

**Desk battery (user, at the desk -- the pass is not done until these run):**

1. **Play-start-frame regression (the A1 kill-shot):** click Play, and on the
   SAME frame hit Del / Ctrl+X / Ctrl+V / Ctrl+D -- no structural edit, no
   memento, no crash. Then Stop and confirm the same keys work again.
2. **Dialog-over-switch staleness (A2/A3):** open a file dialog, switch
   projects while it is up, then complete the dialog -- nothing happens.
   Open "New Instance..." twice in a row -- only the second dialog's parent
   is honored.
3. **Per-project reset:** switch projects; confirm no stale error modal pops,
   recents reload, material hot-reload still works in the new project.
4. **Launch flow:** Play Standalone with a dirty scene -> "Save and Play" (no
   Discard button) -> saves then launches; Cancel -> nothing; never-saved
   scene -> Save-As chain -> launches after save.
5. **Error queue:** trigger two errors back-to-back (e.g. bad scene open +
   failed launch) -- they display one after the other.
6. **Shortcut parity:** undo/redo/save/camera keys behave identically in and
   out of viewport focus (section 1's ShortcutsLive collapse + section 6's
   edges changed the plumbing under every keybind).
7. **SwitchProject soak (section 5, heaviest):** open project A -> switch to
   B -> back to A; then switch to a project whose module fails to load
   (failure-fallback path) -- editor recovers to the old project with the
   full success tail (title, recents, scene) intact.

## Sequencing

1. **Hazard pass first, in order: sections 1 -> 2 -> 3.** Each lands
   independently green; 3 depends on 2 (it clears the inbox) and on 7's queue
   existing OR carries a temporary reset of the raw strings -- to keep the
   dependency one-directional, section 7 (small) may land between 2 and 3.
2. **Decomposition next, any order:** 4, 6, 7 (if not already landed), 8, 9,
   10. All small, mostly independent; 9 touches SceneSession + the modal and
   should land before 5 so the switch unification sees the final intent set.
3. **SwitchProject (5) LAST.** Biggest blast radius, benefits from
   OnProjectOpened/PatchHostStages being the only remaining duplication, and
   its desk soak (battery item 7) closes the pass.

## Constraints (house rules that bind every task)

- Working tree is co-edited: the user live-edits from VS. Stash ONLY the
  dirty editor paths around each task (`git stash push -m "<task>-wip" --
  <paths>`), pop after commit; NEVER `git add -A`.
- New files -> premake file lists + `GenerateProjects.bat` (ArcaneTests' list
  is explicit; check ArcaneEditor's).
- `#include <Json.hpp>` (never nlohmann/json.hpp); ASCII comments; UTF-8 no
  BOM; MSVC is truth (clangd noise is not).
- No pushes; main stays local until the user says otherwise.
