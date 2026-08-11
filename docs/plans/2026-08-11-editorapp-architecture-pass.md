# EditorApp Architecture Pass Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Execute `docs/superpowers/specs/2026-08-11-editorapp-architecture-pass-design.md` -- kill the three EditorApp behavior defects by construction, then decompose the spread behavior (consume extraction, input edges, error queue, member groupings, launch-intent fold, recents facade, SwitchProject unification).

**Architecture:** All work is editor-side (`Arcane/ArcaneEditor/src/`) except one enum value + one gate in `SceneSession` (already editor-side) and zero engine-DLL changes. New pure units (DialogSlot, ModalErrorQueue, InputEdges) are header-only and headless-tested; host wiring has no automated coverage (EditorApp*.cpp is not compiled into ArcaneTests -- a green suite proves nothing about it), so every host task's gate is build-both-configs + full suite + the spec's desk battery at the end.

**Tech Stack:** C++23, ImGui, Catch2, premake5/msbuild (VS18).

## Global Constraints

- **Co-edited working tree.** The user live-edits from VS. Before touching any file, `git status` the repo; stash ONLY the dirty paths your task touches (`git stash push -m "<task>-wip" -- <paths>`), do your work, commit, `git stash pop`. If the pop conflicts, STOP and report BLOCKED -- never resolve over user WIP. NEVER `git add -A` or `git add .` -- always add exact paths. Known user WIP right now: `Arcane/ArcaneEditor/src/EditorApp.cpp`, `Arcane/ArcaneClient/src/Arcane/Render/Device.cpp`, and an in-progress `Arcane/Tests/` -> `Arcane/ArcaneTests/` directory rename (uncommitted). Use whichever tests path exists in the tree when your task runs; this plan writes `Arcane/ArcaneTests/`.
- **Build gate (every task):** from `Arcane/`, `msbuild Arcane.slnx /p:Configuration=Debug /m` with the VS **18** MSBuild located via vswhere (NOT PATH's msbuild), 0 errors 0 warnings; tasks that end a phase (1, 5, 8, 12) also build Release. Then run the suite FROM THE EXE DIR: `cd bin\Debug-windows-x86_64-md\ArcaneTests && ArcaneTests.exe ~[gpu]` -- capture the random-seed banner in your report. Never run `ArcaneEditor.exe` (GPU is desk-only on this machine).
- **New files** -> rerun `Arcane\GenerateProjects.bat` (projects glob at generation time). New test files in `ArcaneTests/src/` are globbed; new editor `.cpp` files: verify the ArcaneEditor project picks them up after regen.
- Tests: tag `[editor]`; never a bare `Arcane::Runtime rt;` -- use `Arcane::Runtime runtime{&Arcane::Test::SharedTypeContext(), false};` (see EditorSceneSessionTest.cpp's Harness).
- `#include <Json.hpp>` (never nlohmann/json.hpp). ASCII comments, UTF-8 no BOM. MSVC output is truth; clangd diagnostics in this environment are known noise.
- **Behavior preservation** is the default. The ONLY sanctioned deltas: (a) the three defect fixes, (b) error modals display sequentially from one queue, (c) the standalone-launch confirm becomes the shared "Unsaved Scene" modal with intent-specific buttons, (d) mid-frame Play-STOP transitions may leave edit affordances disabled for the remainder of that one frame (safe direction).
- Do NOT push. Commit per task on `main`.
- Line numbers in this plan cite the tree at plan time and WILL drift as tasks land -- anchor on the quoted code and names.

---

### Task 1: Play/edit authority (spec section 1 -- fixes A1, collapses B1)

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditorApp.hpp`
- Modify: `Arcane/ArcaneEditor/src/EditorAppFrame.cpp`
- Modify: `Arcane/ArcaneEditor/src/EditorAppScene.cpp`, `EditorAppProject.cpp` (sweep only)

**Interfaces:**
- Produces: `bool EditorApp::InPlayMode() const noexcept` and `bool EditorApp::ShortcutsLive(const Arcane::InputSnapshot&, bool requireViewportFocus) const` -- later tasks use both.

- [ ] **Step 1: Add the accessor + helper declaration** in EditorApp.hpp's private section (beside the `m_play` declaration):

```cpp
// THE Play/edit predicate (architecture pass sec 1). Editor code asks this,
// never m_play.IsPlaying() raw, so the predicate has one greppable name.
[[nodiscard]] bool InPlayMode() const noexcept { return m_play.IsPlaying(); }
// The one "may editor shortcuts fire" predicate (three near-duplicates
// collapsed): Edit mode, ImGui not capturing the keyboard, and -- for keys
// that switch a viewport TOOL rather than act on the selection -- viewport
// focus.
[[nodiscard]] bool ShortcutsLive(const Arcane::InputSnapshot& snap,
                                 bool requireViewportFocus) const;
```

- [ ] **Step 2: Frame-top editMode write.** In `EditorAppFrame.cpp`, `FrameInput`, immediately after `m_gizmoCapturedClick = false;`:

```cpp
// Play/edit authority (architecture pass sec 1): re-derived HERE -- before
// any phase that reads it -- and again immediately after the toolbar draw
// in DrawEditorUi, the one site that can START Play mid-frame. RULE: every
// site that can call PlayMode Start/Stop within the frame must be followed
// by this same re-derivation. (A mid-frame STOP elsewhere leaves edit
// affordances off for the rest of that frame -- the safe direction; a
// mid-frame START without re-derivation is the A1 memento-vs-play-registry
// bug this write exists to kill.)
m_editBinding.editMode = !InPlayMode();
```

- [ ] **Step 3: Post-toolbar write.** In `DrawEditorUi`, immediately after the `DrawSimTimeToolbar(...)` call and its `LaunchStandalone();` branch (before `EndDockSpace`):

```cpp
// The toolbar's Play/Stop click is the only mid-frame PlayMode flip point
// (sec 1's rule): re-derive so this frame's consume blocks and panels see
// the true state, not last frame's.
m_editBinding.editMode = !InPlayMode();
```

- [ ] **Step 4: Delete the phase-18 write.** In `DrawSelectionPanels`, delete the line `m_editBinding.editMode = !m_play.IsPlaying();` (currently EditorAppFrame.cpp:1884).

- [ ] **Step 5: Delete Cut's hand patch.** In the clipboard consume block, first CONFIRM `CutSelection` in EditorPanels.cpp gates on `!binding.editMode` before its copy half (it does -- EditorPanels.cpp ~:1051), then change:

```cpp
// BEFORE (delete the comment above it too):
if (menuReq.cutSelection && !m_play.IsPlaying())
    Arcane::Editor::CutSelection(...);
// AFTER (symmetric with Copy/Paste/Duplicate -- the binding is fresh now):
if (menuReq.cutSelection)
    Arcane::Editor::CutSelection(m_runtime->Registry(), m_selection, *m_undo, m_editBinding);
```

- [ ] **Step 6: ShortcutsLive.** Define in EditorAppFrame.cpp (near the top of the phase methods):

```cpp
bool EditorApp::ShortcutsLive(const Arcane::InputSnapshot& snap,
                              bool requireViewportFocus) const
{
    return !InPlayMode() && !snap.wantCaptureKeyboard
        && (!requireViewportFocus || m_viewportActive);
}
```

Replace the three predicates (byte-identical behavior per site):
  - `HandleUndoRedoAndSceneShortcuts` `const bool active = !m_play.IsPlaying() && !snap.wantCaptureKeyboard;` -> `const bool active = ShortcutsLive(snap, false);`
  - `HandleGizmoModeKeys` `const bool keysActive = !m_play.IsPlaying() && !snap.wantCaptureKeyboard && m_viewportActive;` -> `ShortcutsLive(snap, true)`
  - `UpdateEditorCamera` `const bool framingActive = !m_play.IsPlaying() && !snap.wantCaptureKeyboard;` -> `ShortcutsLive(snap, false)`

- [ ] **Step 7: InPlayMode sweep.** Replace every remaining editor-side `m_play.IsPlaying()` READ with `InPlayMode()` (grep `m_play.IsPlaying()` across `ArcaneEditor/src/EditorApp*.cpp`; ~25 sites across EditorAppFrame/Scene/Project.cpp). Do NOT touch `m_play.Stop(...)`/`m_play.Play(...)` calls or sites passing `m_play` itself.

- [ ] **Step 8: Build Debug + Release, run suite `~[gpu]` from the exe dir.** Expect green (no test compiles this code -- the gate is compile + no regression).

- [ ] **Step 9: Commit** `refactor(editor): frame-top + post-toolbar editMode authority -- A1 dies by construction`

---

### Task 2: DialogSlot unit (spec section 2, the pure half)

**Files:**
- Create: `Arcane/ArcaneEditor/src/DialogSlot.hpp`
- Create: `Arcane/ArcaneTests/src/DialogSlotTest.cpp`

**Interfaces:**
- Produces: `template <typename Payload> class Arcane::Editor::DialogSlot` with `Arm() -> uint64_t`, `Stash(epoch, Payload)`, `Take() -> optional<Payload>`, `Clear()`. Task 3 consumes it.

- [ ] **Step 1: Write the failing test** (`DialogSlotTest.cpp`):

```cpp
// DialogSlot: one in-flight async file-dialog result, epoch-guarded so a
// result from a dead project or a superseded dialog is dropped, never applied.
#include <catch2/catch_test_macros.hpp>
#include "DialogSlot.hpp"
#include <string>

using Arcane::Editor::DialogSlot;

TEST_CASE("Arm/Stash/Take round-trips a payload", "[editor][dialog]")
{
    DialogSlot<std::string> slot;
    const auto epoch = slot.Arm();
    slot.Stash(epoch, "C:/scene.arcscene");
    const auto got = slot.Take();
    REQUIRE(got.has_value());
    CHECK(*got == "C:/scene.arcscene");
    CHECK_FALSE(slot.Take().has_value());   // Take empties
}

TEST_CASE("a Stash with a stale epoch after Clear is dropped", "[editor][dialog]")
{
    DialogSlot<std::string> slot;
    const auto epoch = slot.Arm();
    slot.Clear();                       // project switch
    slot.Stash(epoch, "dead-project-result");
    CHECK_FALSE(slot.Take().has_value());
}

TEST_CASE("re-Arm supersedes the first dialog (the A2 double-dialog case)", "[editor][dialog]")
{
    struct InstanceResult { std::string path; int parent; };
    DialogSlot<InstanceResult> slot;
    const auto first  = slot.Arm();
    const auto second = slot.Arm();
    slot.Stash(first,  { "first.arcmat",  1 });   // dropped: superseded
    slot.Stash(second, { "second.arcmat", 2 });   // lands, parent rides WITH path
    const auto got = slot.Take();
    REQUIRE(got.has_value());
    CHECK(got->path == "second.arcmat");
    CHECK(got->parent == 2);
}

TEST_CASE("Arm drops an unconsumed prior result (last-writer-wins)", "[editor][dialog]")
{
    DialogSlot<std::string> slot;
    slot.Stash(slot.Arm(), "unconsumed");
    (void)slot.Arm();                   // user opened the dialog again
    CHECK_FALSE(slot.Take().has_value());
}
```

- [ ] **Step 2: Run `Arcane\GenerateProjects.bat`, build.** Expected: FAIL -- `DialogSlot.hpp` not found.

- [ ] **Step 3: Write the header** (`DialogSlot.hpp`):

```cpp
#pragma once
// DialogSlot: one in-flight async file-dialog result (architecture pass sec 2).
// The OS dialog's completion thunk fires on an SDL BACKGROUND thread; the
// consumer Take()s at the top of the next frame. The epoch is what makes
// clear-on-switch airtight: a thunk completing AFTER the switch Stashes into
// a bumped epoch and is dropped, so a dialog opened in project A can never
// land in project B (audit defect A2's sibling). Contract:
//   Arm()   at the dialog-LAUNCH site; the returned epoch rides in the thunk.
//   Stash() from the thunk (any thread); ignored when the epoch is stale.
//   Take()  once per frame at the consume site; empties the slot.
//   Clear() on project switch (ResetPerProjectState).
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace Arcane::Editor
{
    template <typename Payload>
    class DialogSlot
    {
    public:
        [[nodiscard]] std::uint64_t Arm()
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_value.reset();   // last-writer-wins, matching today's raw-string behavior
            return ++m_epoch;
        }

        void Stash(std::uint64_t epoch, Payload value)
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (epoch != m_epoch)
                return;        // Clear()/re-Arm() since Arm -> a dead dialog's result
            m_value = std::move(value);
        }

        [[nodiscard]] std::optional<Payload> Take()
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            std::optional<Payload> out;
            out.swap(m_value);
            return out;
        }

        void Clear()
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            ++m_epoch;
            m_value.reset();
        }

    private:
        std::mutex             m_mutex;
        std::uint64_t          m_epoch = 0;
        std::optional<Payload> m_value;
    };
}
```

- [ ] **Step 4: Build, run** `ArcaneTests.exe "[dialog]"` from the exe dir. Expected: PASS. Then the full `~[gpu]` suite.

- [ ] **Step 5: Commit** `feat(editor): DialogSlot -- epoch-guarded async dialog result slot + tests`

---

### Task 3: DialogInbox integration (spec section 2 -- fixes A2, collapses B2)

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditorApp.hpp`, `EditorAppFrame.cpp`, `EditorAppScene.cpp`, `EditorAppProject.cpp`

**Interfaces:**
- Consumes: `DialogSlot` (Task 2).
- Produces: `EditorApp::DialogInbox m_dialogs` with slots `sceneOpen`, `sceneSave`, `projectOpen`, `materialNew`, `materialOpen` (`DialogSlot<std::string>`), `instanceNew` (`DialogSlot<InstanceNewResult>`), and `void ClearAll()`. Task 5 calls `ClearAll()`.

- [ ] **Step 1: Declare the inbox** in EditorApp.hpp (replacing the pending-string block near `m_pendingMaterialNewPath`):

```cpp
#include "DialogSlot.hpp"   // with the other editor includes at the top

// ---- Async file-dialog inbox (architecture pass sec 2) ------------------
// One DialogSlot per dialog kind. The old six pending strings + three
// mutexes + the unguarded m_pendingInstanceParent sidecar live here now;
// the instance parent rides INSIDE its payload so it is stashed under the
// lock, taken atomically with the path, and cleared with the slot.
struct InstanceNewResult
{
    std::string  path;
    Arcane::Guid parent;
};
struct DialogInbox
{
    DialogSlot<std::string>       sceneOpen;
    DialogSlot<std::string>       sceneSave;
    DialogSlot<std::string>       projectOpen;
    DialogSlot<std::string>       materialNew;
    DialogSlot<std::string>       materialOpen;
    DialogSlot<InstanceNewResult> instanceNew;
    void ClearAll()
    {
        sceneOpen.Clear(); sceneSave.Clear(); projectOpen.Clear();
        materialNew.Clear(); materialOpen.Clear(); instanceNew.Clear();
    }
};
DialogInbox m_dialogs;
```

DELETE from EditorApp.hpp: `m_pendingMaterialNewPath`, `m_pendingMaterialOpenPath`, `m_pendingInstanceNewPath`, `m_pendingMaterialMutex`, `m_pendingInstanceParent`, `m_pendingProjectPath`, `m_pendingProjectMutex`, `m_pendingSceneOpenPath`, `m_pendingSceneSavePath`, `m_pendingSceneMutex`, and the six thunk declarations (`MaterialNewPickedThunk`, `MaterialOpenPickedThunk`, `InstanceNewPickedThunk`, `ProjectPickedThunk`, `SceneOpenPickedThunk`, `SceneSavePickedThunk`). Their explanatory comments move onto DialogInbox where still true.

- [ ] **Step 2: Two shared trampolines** replace the six thunks. Declare in EditorApp.hpp, define in EditorAppFrame.cpp's anonymous-namespace-adjacent section (or EditorApp.cpp -- implementer's call, one place):

```cpp
// In EditorApp.hpp (private):
struct PathDialogRequest
{
    DialogSlot<std::string>* slot;
    std::uint64_t            epoch;
};
struct InstanceDialogRequest
{
    DialogSlot<InstanceNewResult>* slot;
    std::uint64_t                  epoch;
    Arcane::Guid                   parent;
};
static void PathPickedThunk(const char* path, void* user);
static void InstancePickedThunk(const char* path, void* user);
```

```cpp
// Definitions. SDL's dialog backend fires the callback exactly once per
// ShowXFileDialog (null path on cancel -- see the old thunks' early return),
// so the heap request is single-owner and freed here. If a future backend
// ever skipped the callback the request would leak (bounded, one small
// struct per un-fired dialog) -- acceptable, noted.
void EditorApp::PathPickedThunk(const char* path, void* user)
{
    std::unique_ptr<PathDialogRequest> req(static_cast<PathDialogRequest*>(user));
    if (path)
        req->slot->Stash(req->epoch, path);
}

void EditorApp::InstancePickedThunk(const char* path, void* user)
{
    std::unique_ptr<InstanceDialogRequest> req(static_cast<InstanceDialogRequest*>(user));
    if (path)
        req->slot->Stash(req->epoch, InstanceNewResult{ path, req->parent });
}
```

- [ ] **Step 3: Convert the six launch sites.** Pattern for each (exact dialog titles/extensions/default dirs unchanged):

```cpp
// DrawEditorUi, menuReq.openProject (was &EditorApp::ProjectPickedThunk, this):
if (menuReq.openProject)
    m_gpu->Win().ShowOpenFileDialog(&EditorApp::PathPickedThunk,
        new PathDialogRequest{ &m_dialogs.projectOpen, m_dialogs.projectOpen.Arm() },
        "Arcane Project", "arcproj");
```

Apply the same shape to: `menuReq.openScene` (-> `m_dialogs.sceneOpen`), `ShowSceneSaveDialog` in EditorAppScene.cpp (-> `m_dialogs.sceneSave`), `menuReq.newMaterial` (-> `m_dialogs.materialNew`), `menuReq.openMaterial` (-> `m_dialogs.materialOpen`). The instance site (browserActions.createInstanceOf) captures the parent INTO the request and stops writing `m_pendingInstanceParent`:

```cpp
if (browserActions.createInstanceOf.IsValid())
{
    const Arcane::Project* proj = m_runtime->CurrentProject();
    const std::string contentDir =
        proj ? (proj->Root() / "Content").string() : std::string();
    m_gpu->Win().ShowSaveFileDialog(&EditorApp::InstancePickedThunk,
        new InstanceDialogRequest{ &m_dialogs.instanceNew,
                                   m_dialogs.instanceNew.Arm(),
                                   browserActions.createInstanceOf },
        "Arcane Material", "arcmat",
        contentDir.empty() ? nullptr : contentDir.c_str());
}
```

- [ ] **Step 4: Convert the two recents bypasses** (DrawEditorUi) to Arm+Stash -- same data path as a real dialog, no second entry point:

```cpp
if (!menuReq.openRecentPath.empty())
    m_dialogs.projectOpen.Stash(m_dialogs.projectOpen.Arm(), menuReq.openRecentPath);
if (!menuReq.openRecentScenePath.empty())
    m_dialogs.sceneOpen.Stash(m_dialogs.sceneOpen.Arm(), menuReq.openRecentScenePath);
```

(Keep the existing comments about why recents flow through the same slot.)

- [ ] **Step 5: Convert the three consumers.** Guards and effects UNCHANGED -- only the source changes:

```cpp
// ConsumeSceneDialogResults:
if (const auto sceneOpen = m_dialogs.sceneOpen.Take())
{
    if (m_scene.Request(Arcane::Editor::SceneIntent::OpenScene, *sceneOpen, *m_undo))
        DoOpenScene(*sceneOpen);
}
if (const auto sceneSave = m_dialogs.sceneSave.Take())
{
    std::filesystem::path p = *sceneSave;
    // ... existing extension coercion + DoSaveScene + parked-intent resume,
    //     verbatim (including the m_launchAfterSceneSave block until Task 8
    //     deletes it) ...
}

// ConsumeProjectDialogResult:
if (const auto pending = m_dialogs.projectOpen.Take())
{
    if (m_scene.Request(Arcane::Editor::SceneIntent::OpenProject, *pending, *m_undo))
        SwitchProject(*pending);
}

// ConsumeMaterialDialogResults:
if (const auto materialNew = m_dialogs.materialNew.Take())
    CreateMaterialAt(*materialNew);
if (const auto materialOpen = m_dialogs.materialOpen.Take())
    m_documents.OpenPath(*materialOpen);
if (const auto instance = m_dialogs.instanceNew.Take())
    CreateInstanceAt(instance->path, instance->parent);
```

- [ ] **Step 6: Delete the six old thunk DEFINITIONS** (EditorAppProject.cpp: Material/MaterialOpen/InstanceNew/ProjectPickedThunk; EditorAppScene.cpp: SceneOpen/SceneSavePickedThunk).

- [ ] **Step 7: Build Debug, run full suite from the exe dir.** Green.

- [ ] **Step 8: Commit** `refactor(editor): DialogInbox -- six thunks/strings/mutexes + the A2 parent sidecar become epoch-guarded slots`

---

### Task 4: ModalErrorQueue (spec section 7 -- collapses B6)

**Files:**
- Create: `Arcane/ArcaneEditor/src/ModalErrorQueue.hpp`
- Create: `Arcane/ArcaneTests/src/ModalErrorQueueTest.cpp`
- Modify: `EditorApp.hpp`, `EditorAppFrame.cpp`, `EditorAppScene.cpp`, `EditorAppProject.cpp`, `EditorApp.cpp`, `DocumentHost.cpp` (comment only)

**Interfaces:**
- Produces: `Arcane::Editor::ModalErrorQueue m_modalErrors` on EditorApp -- `Push(title, message)`, `Front() -> const ModalError*`, `Pop()`, `Clear()`. Tasks 5, 8, 12 push into it.

- [ ] **Step 1: Write the failing test** (`ModalErrorQueueTest.cpp`):

```cpp
#include <catch2/catch_test_macros.hpp>
#include "ModalErrorQueue.hpp"

using namespace Arcane::Editor;

TEST_CASE("errors display FIFO, one at a time", "[editor][modal]")
{
    ModalErrorQueue q;
    CHECK(q.Front() == nullptr);
    q.Push("Scene Error", "bad file");
    q.Push("Play in Separate Window Failed", "no exe");
    REQUIRE(q.Front() != nullptr);
    CHECK(q.Front()->title == "Scene Error");
    q.Pop();
    REQUIRE(q.Front() != nullptr);
    CHECK(q.Front()->title == "Play in Separate Window Failed");
    CHECK(q.Front()->message == "no exe");
    q.Pop();
    CHECK(q.Front() == nullptr);
}

TEST_CASE("Clear drops everything (project switch)", "[editor][modal]")
{
    ModalErrorQueue q;
    q.Push("Open Project Failed", "stale");
    q.Clear();
    CHECK(q.Front() == nullptr);
}
```

- [ ] **Step 2: Regen, build.** Expected FAIL (header missing).

- [ ] **Step 3: Write the header:**

```cpp
#pragma once
// ModalErrorQueue (architecture pass sec 7): pure error-modal state. The
// three parallel error strings (project/scene/launch) and their five copied
// ~16-line popup blocks become one queue + ONE drawing block in DrawModals.
// Errors display sequentially (FIFO) instead of racing for the popup stack.
// Pure state -- drawing stays host-side, same split as ConsoleBuffer.
#include <deque>
#include <string>
#include <utility>

namespace Arcane::Editor
{
    struct ModalError
    {
        std::string title;     // popup title -- stays honest about which action failed
        std::string message;
    };

    class ModalErrorQueue
    {
    public:
        void Push(std::string title, std::string message)
        {
            m_queue.push_back({ std::move(title), std::move(message) });
        }
        [[nodiscard]] const ModalError* Front() const
        {
            return m_queue.empty() ? nullptr : &m_queue.front();
        }
        void Pop()   { if (!m_queue.empty()) m_queue.pop_front(); }
        void Clear() { m_queue.clear(); }

    private:
        std::deque<ModalError> m_queue;
    };
}
```

- [ ] **Step 4: Build, run** `ArcaneTests.exe "[modal]"` -- PASS.

- [ ] **Step 5: Integrate.** In EditorApp.hpp: add `Arcane::Editor::ModalErrorQueue m_modalErrors;` and DELETE `m_projectOpenError`, `m_sceneError`, `m_launchError` (move their doc comments' surviving content onto the member). Sweep every assignment (grep `m_projectOpenError|m_sceneError|m_launchError` across `ArcaneEditor/src`) to `m_modalErrors.Push(<title>, <message>)` with the title the old modal used:
  - `m_projectOpenError = X` -> `m_modalErrors.Push("Open Project Failed", X)` (sites: SwitchProject rival-lock / invalid-project / dirty-documents / plugin-banner / switch-failed; EditorApp.cpp StagePluginLoad banner + StageFinalize `--project` derive)
  - `m_sceneError = X` -> `m_modalErrors.Push("Scene Error", X)` (DoOpenScene x2, DoSaveScene play-refusal + any write-fail site, DrawEditorUi setBootScene failure)
  - `m_launchError = X` -> `m_modalErrors.Push("Play in Separate Window Failed", X)` (LaunchStandalone x3)
  Comments that SAY "sets m_sceneError" update to name the queue.

- [ ] **Step 6: One drawing block.** In `DrawModals`, DELETE the three error-modal blocks ("Open Project Failed", "Scene Error", "Play in Separate Window Failed" -- keep the "Unsaved Scene" and "Save and Play?" CONFIRM modals untouched) and add, in their place at the top:

```cpp
// ONE error modal, FIFO off the queue (architecture pass sec 7). The title
// is per-error so it stays honest about which action failed; Pop advances
// to the next queued error on the following frame.
if (const Arcane::Editor::ModalError* err = m_modalErrors.Front())
{
    if (!ImGui::IsPopupOpen(err->title.c_str()))
        ImGui::OpenPopup(err->title.c_str());
    if (ImGui::BeginPopupModal(err->title.c_str(), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(err->message.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(120, 0)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Enter))
        {
            m_modalErrors.Pop();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
```

- [ ] **Step 7:** Add a one-line comment at DocumentHost.cpp's own error-popup block (~:191): `// Deliberately NOT folded into EditorApp's ModalErrorQueue -- DocumentHost is self-contained; see the architecture-pass spec sec 7.`

- [ ] **Step 8: Build Debug, full suite.** Green. **Commit** `refactor(editor): ModalErrorQueue -- three error strings + five popup copies become one FIFO queue`

---

### Task 5: ResetPerProjectState (spec section 3 -- fixes A3)

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditorApp.hpp`, `EditorAppProject.cpp`

**Interfaces:**
- Consumes: `m_dialogs.ClearAll()` (Task 3), `m_modalErrors.Clear()` (Task 4).
- Produces: `void EditorApp::ResetPerProjectState()` -- Task 12's teardown stage keeps calling it.

- [ ] **Step 1: Extract.** In EditorAppProject.cpp, add `ResetPerProjectState()` and have the `switch_teardown` stage body call it. The stage keeps ONLY its two non-reset effects (GPU idle + plugin unload) after the call:

```cpp
// EVERY mutable EditorApp member whose value refers to the current project
// must appear in this function -- or carry a comment at its declaration
// saying why it survives a switch (architecture pass sec 3; audit defect
// A3 was three implicit reset lists, one of them unowned). Called from the
// switch_teardown stage only: boot has no prior project to reset.
void EditorApp::ResetPerProjectState()
{
    m_documents.CloseAll();
    if (m_resolver)
        m_resolver->Clear();       // sprites + materials + post chain, together
    m_diagnostics.ClearAll();      // Problems is per-project STATE, not a log
    ClearSceneReferences();        // also runs per scene switch -- see its body
    if (m_undo) m_scene.Reset(*m_undo);
    m_sceneRecents = {};
    m_assetBrowser = {};
    // -- previously in NO list (the A3 gap): --
    m_dialogs.ClearAll();          // in-flight dialogs die with their project (sec 2)
    m_modalErrors.Clear();         // a dead project's modal must not pop post-switch
    m_materialMtimes.clear();      // outgoing project's path-keyed watch cache --
    m_materialWatchNext = 0.0;     //   grew unbounded across switches before this
    m_launchModalPending   = false;   // a parked "Save and Play?" dies with its scene
    m_launchAfterSceneSave = false;   //   (both flags deleted entirely in the
                                      //    LaunchStandalone-intent task)
}
```

Move the surviving content of switch_teardown's five inline essays into ONE comment block above this function (each member keeps at most a one-line why). The stage becomes:

```cpp
stages.push_back(stage("switch_teardown", {}, Arcane::BootThread::Main,
                       Arcane::BootPolicy::Fatal, 2, [&]
{
    ResetPerProjectState();
    // Idle the GPU before freeing plugin-owned GPU resources, then unload
    // the plugin (dtor: Unload -> ClearSystems + ResetRegistry, DLL mapped).
    m_gpu->Device().Nvrhi()->waitForIdle();
    m_plugin.reset();
    return true;
}));
```

- [ ] **Step 2: Survivor comments.** At `m_activeMaterialGuid` / `m_materialDocCount` declarations (EditorApp.hpp), replace the accident with the contract: `// Survives a project switch ON PURPOSE: re-resolved from the (now empty) DocumentHost next frame -- the ResolveActiveMaterialDoc fallback self-heals it. See ResetPerProjectState's rule.` Give `m_moduleBuildRoot` the same treatment (it guards an async worker; PollModuleBuild's root-mismatch check is the staleness guard).

- [ ] **Step 3: Build Debug + Release, full suite.** Green. **Commit** `refactor(editor): ResetPerProjectState -- one owned per-project reset list; A3's unreset set joins it`

---

### Task 6: DrawEditorUi extraction (spec section 4 -- collapses B3)

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditorApp.hpp`, `EditorAppFrame.cpp`

**Interfaces:**
- Produces: `void ConsumeMenuRequests(Arcane::Editor::MenuRequests& menuReq, const FrameState& fs, LoopState& ls)`, `void ConsumeBrowserActions(const Arcane::Editor::AssetBrowserActions& actions)`, `Arcane::Editor::ShaderEditorDocument* ResolveActiveMaterialDoc()`.

- [ ] **Step 1: Pure code motion, three extractions.** Declare the three methods in EditorApp.hpp's frame-loop section. Move, verbatim (comments included):
  - **ConsumeMenuRequests**: everything from the `m_raiseOpenProjectOnStart` latch through the `saveSceneAs` block (the 19 `menuReq` consume blocks, including the recents Arm+Stash bypasses, the file-menu rising-edge refresh, the clipboard fold-ins reading `fs`, and the Exit request writing `ls.sceneAction`).
  - **ConsumeBrowserActions**: the six `browserActions.*` consume blocks (createInstanceOf / createSpriteFrom / openScene / setBootScene / showInExplorer / copyPath). The `DrawAssetBrowserPanel` CALL stays in DrawEditorUi; its result is passed in. The openScene block writes `ls.sceneAction` -- pass `LoopState& ls` too if needed; adjust the signature accordingly (implementer picks the minimal parameter set the moved code needs -- no new state).
  - **ResolveActiveMaterialDoc**: the `activeMat` resolve (guid lookup + ForEach fallback + `m_activeMaterialGuid` write-back), returning the pointer; DrawEditorUi calls `DrawMaterialPanel(ResolveActiveMaterialDoc());`.

  The remaining DrawEditorUi shell reads: title -> BeginFrame -> menu context -> BeginDockSpace -> toolbar -> editMode re-derivation (Task 1) -> EndDockSpace -> panelVis reset -> `ConsumeMenuRequests(menuReq, fs, ls)` -> browser draw + `ConsumeBrowserActions(...)` -> console cap + console/problems draws -> `DrawMaterialPanel(ResolveActiveMaterialDoc())` -> `m_documents.DrawAll(...)`.

- [ ] **Step 2: Diff discipline.** No statement reordering, no logic edits -- reviewer verifies the diff is move-only.

- [ ] **Step 3: Build Debug, full suite.** Green. **Commit** `refactor(editor): DrawEditorUi becomes a shell -- consume runs extracted (B3)`

---

### Task 7: InputEdges (spec section 6 -- collapses B5)

**Files:**
- Create: `Arcane/ArcaneEditor/src/InputEdges.hpp`
- Create: `Arcane/ArcaneTests/src/InputEdgesTest.cpp`
- Modify: `EditorApp.hpp`, `EditorAppFrame.cpp`

**Interfaces:**
- Produces: `Arcane::Editor::Edge` (`down`/`pressed`/`released`, `Update(bool)`) and `EditorApp`'s `InputEdges m_edges` with named members.

- [ ] **Step 1: Write the failing test** (`InputEdgesTest.cpp`):

```cpp
#include <catch2/catch_test_macros.hpp>
#include "InputEdges.hpp"

using Arcane::Editor::Edge;

TEST_CASE("Edge reports rising and falling once each", "[editor][input]")
{
    Edge e;
    e.Update(true);
    CHECK(e.pressed); CHECK(e.down); CHECK_FALSE(e.released);
    e.Update(true);                     // held: no repeat
    CHECK_FALSE(e.pressed); CHECK(e.down);
    e.Update(false);
    CHECK(e.released); CHECK_FALSE(e.down); CHECK_FALSE(e.pressed);
    e.Update(false);
    CHECK_FALSE(e.released);
}

TEST_CASE("a skipped Update means a dead key, not auto-repeat", "[editor][input]")
{
    // The designed failure mode: forgetting the per-frame Update leaves
    // pressed stale-false after the first frame -- the key goes dead instead
    // of firing every frame (what a forgotten write-back used to cause).
    Edge e;
    e.Update(true);
    CHECK(e.pressed);
    // no Update this frame -- a consumer re-reading sees the OLD edge only once
    e.Update(true);
    CHECK_FALSE(e.pressed);
}
```

- [ ] **Step 2: Regen, build -- FAIL.** Then write the header:

```cpp
#pragma once
// Edge (architecture pass sec 6): per-key rising/falling edge tracking.
// Replaces 17 hand-rolled m_prev* bools + 18 `down && !prev` expressions +
// write-backs scattered across three functions. Update owns the write-back:
// forgetting the Update line makes a key DEAD (visible immediately), not
// auto-repeating (the old silent failure).
namespace Arcane::Editor
{
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
}
```

- [ ] **Step 3: Build, run** `ArcaneTests.exe "[input]"` -- PASS.

- [ ] **Step 4: Integrate.** In EditorApp.hpp, DELETE the 17 bools (`m_prevUndoKeyDown`, `m_prevRedoKeyDown`, `m_prevKeyW/E/R/Q/N/O/S/X/C/V/D/F/Home`, `m_prevLmbDown`, `m_prevRmbDown`) and add ONE grouped member (their comments compress onto it):

```cpp
#include "InputEdges.hpp"
// Editor keybind + mouse edge tracking (architecture pass sec 6). All
// Updated within FrameInput's phases (6a-6d) at the site each chord's
// `down` value is computed; consumers read .pressed/.released.
struct InputEdges
{
    Edge undo, redo;            // Ctrl+Z / Ctrl+(Shift+)Z|Y
    Edge w, e, r, q;            // gizmo tools
    Edge n, o, s;               // Ctrl+N/O/S scene shortcuts
    Edge x, c, v, d;            // Ctrl+X/C/V/D clipboard shortcuts
    Edge f, home;               // camera framing
    Edge lmb, rmb;              // gizmo press/release; camera pan
};
InputEdges m_edges;
```

  Convert each site in EditorAppFrame.cpp -- the pattern, shown on undo (all others identical in shape):

```cpp
// BEFORE:
if (active && noOpenTxn && undoKeyDown && !m_prevUndoKeyDown) m_undo->Undo();
...
m_prevUndoKeyDown = undoKeyDown;   // (write-back line DELETED)
// AFTER:
m_edges.undo.Update(undoKeyDown);
if (active && noOpenTxn && m_edges.undo.pressed) m_undo->Undo();
```

  Site-by-site notes:
  - `HandleUndoRedoAndSceneShortcuts`: undo/redo/n/o/s/x/c/v/d -- Update each right after its `*Down` chord is computed, then read `.pressed`; delete the three write-back runs.
  - `HandleGizmoModeKeys`: w/e/r/q same shape.
  - `UpdateEditorCamera`: `m_edges.rmb.Update(rmbDown);` at the top of the function. `rmbDown && !m_prevRmbDown` -> `m_edges.rmb.pressed`. The held-across-both-frames pan guard `m_camPanning && m_prevRmbDown` -> `m_camPanning && m_edges.rmb.down && !m_edges.rmb.pressed` (identical truth table given panning implies down -- keep the original guard comment and add this equivalence note). f/home same shape as the keys.
  - `UpdateGizmoInteraction`: `m_edges.lmb.Update(<the lmb down expression the function computes>);` at its top; the press edge -> `.pressed`, the release edge -> `.released`; delete the write-back (~:890).

- [ ] **Step 5: Build Debug, full suite.** Green. **Commit** `refactor(editor): InputEdges -- 17 prev-bools and their write-backs become Edge trackers (B5)`

---

### Task 8: SceneIntent::LaunchStandalone (spec section 9 -- collapses B9's parallel resume, dissolves the launch flags)

**Files:**
- Modify: `Arcane/ArcaneEditor/src/SceneSession.hpp`, `SceneSession.cpp`
- Modify: `Arcane/ArcaneTests/src/EditorSceneSessionTest.cpp`
- Modify: `EditorApp.hpp`, `EditorAppFrame.cpp`, `EditorAppScene.cpp`, `EditorAppProject.cpp` (ResetPerProjectState line)

**Interfaces:**
- Produces: `SceneIntent::LaunchStandalone`; `void EditorApp::DoLaunchStandalone()` (the spawn effect). `EditorApp::LaunchStandalone()` is DELETED.

- [ ] **Step 1: Write the failing tests** -- append to EditorSceneSessionTest.cpp (reuse its `Harness`):

```cpp
TEST_CASE("LaunchStandalone parks on a dirty scene", "[editor][scene]")
{
    Harness h;
    SceneSession s;
    s.Adopt("D:/Games/G/Content/scenes/level_one.arcscene",
            Arcane::Guid::Generate(), h.stack);
    h.Edit(1.0f);

    CHECK_FALSE(s.Request(SceneIntent::LaunchStandalone, {}, h.stack));
    CHECK(s.Pending() == SceneIntent::LaunchStandalone);

    const SceneSession::PendingRequest req = s.TakePending();
    CHECK(req.intent == SceneIntent::LaunchStandalone);
    CHECK(s.Pending() == SceneIntent::None);
}

TEST_CASE("LaunchStandalone parks on a never-saved scene even when clean", "[editor][scene]")
{
    // A nil scene guid means the spawned runtime would boot the manifest's
    // bootScene instead of what is on screen -- "never saved" is exactly as
    // unready as "dirty" for this one intent (LaunchStandalone's old guard,
    // now owned by the machine).
    Harness h;
    SceneSession s;                       // Untitled: nil id, clean
    CHECK_FALSE(s.IsDirty(h.stack));
    CHECK_FALSE(s.Request(SceneIntent::LaunchStandalone, {}, h.stack));
    CHECK(s.Pending() == SceneIntent::LaunchStandalone);
    s.ClearPending();
    CHECK(s.Pending() == SceneIntent::None);
}

TEST_CASE("LaunchStandalone acts immediately on a saved clean scene", "[editor][scene]")
{
    Harness h;
    SceneSession s;
    s.Adopt("D:/Games/G/Content/scenes/level_one.arcscene",
            Arcane::Guid::Generate(), h.stack);
    CHECK(s.Request(SceneIntent::LaunchStandalone, {}, h.stack));
    CHECK(s.Pending() == SceneIntent::None);
}

TEST_CASE("a second Request while LaunchStandalone is parked is ignored", "[editor][scene]")
{
    Harness h;
    SceneSession s;
    h.Edit(1.0f);
    CHECK_FALSE(s.Request(SceneIntent::LaunchStandalone, {}, h.stack));
    CHECK_FALSE(s.Request(SceneIntent::OpenScene, "other.arcscene", h.stack));
    CHECK(s.Pending() == SceneIntent::LaunchStandalone);
}
```

- [ ] **Step 2: Run** `ArcaneTests.exe "[scene]"` -- FAIL (enum value missing).

- [ ] **Step 3: SceneSession changes.** Add `LaunchStandalone,` after `Exit,` in the enum (comment: `// payload unused; parks when dirty OR never saved`). In `Request` (SceneSession.cpp), extend the park condition:

```cpp
// LaunchStandalone ALSO parks on a nil id: the standalone runtime loads the
// scene from DISK by guid, so a never-saved scene is exactly as unready as a
// dirty one for this intent (the old LaunchStandalone guard, now here).
const bool unready = IsDirty(stack) ||
    (intent == SceneIntent::LaunchStandalone && !m_id.IsValid());
```

(Adapt to the actual body: today it parks on `IsDirty(stack)` alone; the ignore-while-pending check stays first.) Add the completion-convention rule to `Request`'s declaration comment in SceneSession.hpp:

```cpp
// COMPLETION CONVENTION (architecture pass sec 9): a top-of-frame consumer
// (ConsumeDeferredSceneAction / ConsumeSceneDialogResults /
// ConsumeProjectDialogResult / PumpFrameEvents) may act on `true`
// immediately; any site inside the ImGui pass defers via ls.sceneAction to
// the top of the next frame. Each intent's EFFECT lives in exactly one
// function (RunSceneAction's cases), called by both the immediate and the
// parked-resume path.
```

- [ ] **Step 4: Run the new tests -- PASS.** Commit checkpoint is at the task end (one commit).

- [ ] **Step 5: Split the launch.** In EditorAppScene.cpp, rename `LaunchStandalone()` to `DoLaunchStandalone()` and replace its park branch with a backstop (mirrors DoSaveScene's):

```cpp
// BEFORE:
if (!m_scene.Id().IsValid() || m_scene.IsDirty(*m_undo))
{
    m_launchModalPending = true;
    return;
}
// AFTER:
// Backstop, same rationale as DoSaveScene's Play refusal: the Request
// machine is the gate; if a caller reaches here unready anyway, refuse
// loudly rather than spawn against a stale file.
if (!m_scene.Id().IsValid() || m_scene.IsDirty(*m_undo))
{
    ARC_ERROR("Play Standalone: refused -- scene is unsaved (the intent machine should have parked this)");
    return;
}
```

(Keep everything else -- exe resolve, error pushes, spawn -- verbatim; update the header comment's park description to name the SceneSession machine.) Update the declaration in EditorApp.hpp; delete `m_launchModalPending` / `m_launchAfterSceneSave` and their comment block.

- [ ] **Step 6: Wire the intent.**
  - `RunSceneAction` (EditorAppFrame.cpp): add `case Arcane::Editor::SceneIntent::LaunchStandalone: DoLaunchStandalone(); break;`
  - Toolbar site (DrawEditorUi): `if (DrawSimTimeToolbar(...)) LaunchStandalone();` becomes:

```cpp
if (Arcane::Editor::DrawSimTimeToolbar(m_play, *m_runtime,
                                       m_plugin ? m_plugin->Vtable() : nullptr, m_playMode,
                                       (uint64_t)(intptr_t)m_toolbarLogo.Get()))
{
    // Mid-ImGui-pass site -> the deferral convention (SceneSession::Request's
    // comment): clean+saved acts next frame top; dirty/never-saved parks
    // behind the shared confirm modal.
    if (m_scene.Request(Arcane::Editor::SceneIntent::LaunchStandalone, {}, *m_undo))
        ls.sceneAction = { Arcane::Editor::SceneIntent::LaunchStandalone, {} };
}
```

  - DELETE the whole "Save and Play?" modal block in DrawModals, and the `m_launchAfterSceneSave` resume block in ConsumeSceneDialogResults (the generic parked-intent resume above it now handles the launch: a parked LaunchStandalone whose Save-As lands runs `RunSceneAction(m_scene.TakePending(), ls)` -> `DoLaunchStandalone()`).
  - ResetPerProjectState: delete the two flag resets (members gone); add `// A parked LaunchStandalone cannot survive into a switch: OpenProject's own Request is ignored while any intent is parked, so the modal resolves first.`

- [ ] **Step 7: Parameterize the shared confirm modal.** In DrawModals' "Unsaved Scene" block:

```cpp
const Arcane::Editor::SceneIntent pending = m_scene.Pending();
const bool isLaunch = (pending == Arcane::Editor::SceneIntent::LaunchStandalone);
// message:
ImGui::TextUnformatted((isLaunch
    ? "'" + m_scene.DisplayName() + "' must be saved before playing in a separate window."
    : "'" + m_scene.DisplayName() + "' has unsaved changes.").c_str());
// affirmative button (Save branch body UNCHANGED -- both the synchronous
// DoSaveScene path and the never-saved ShowSceneSaveDialog stay-parked path
// already do the right thing for a launch intent):
if (ImGui::Button(isLaunch ? "Save and Play" : "Save", ImVec2(isLaunch ? 140.f : 90.f, 0)))
// Discard: hidden for a launch -- the standalone runtime reads the scene
// from DISK, so "discard and play" would run a stale file:
if (!isLaunch)
{
    ImGui::SameLine();
    if (ImGui::Button("Discard", ImVec2(90, 0))) { ... unchanged ... }
}
```

(Compute `pending` once at the block top; the existing Pending()==None early-close branch is untouched and now also covers the launch's dialog-in-flight frames.)

- [ ] **Step 8: Build Debug + Release, full suite from the exe dir.** Green. **Commit** `refactor(editor): standalone launch joins the SceneSession intent machine -- parallel resume path deleted (B9)`

---

### Task 9: Remaining member groupings (spec section 8 -- B7)

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditorApp.hpp`, `EditorApp.cpp`, `EditorAppFrame.cpp`, `EditorAppScene.cpp`, `EditorAppProject.cpp`

Mechanical member consolidation -- no behavior change, no new files. **Declaration POSITION is load-bearing** (teardown contract): each struct replaces its FIRST member's current declaration position, and the moved comments come along.

- [ ] **Step 1: GameUiInputHandoff.** Replace `m_lastViewportMouse`/`m_lastMouseButtons`/`m_lastWheel`/`m_lastInViewport`/`m_lastFrameDt` (EditorApp.hpp ~:542-546) with:

```cpp
// Viewport-local input snapshot for the game ImGui pass, captured inside
// FrameInput (whose locals are out of scope at the render site) and read
// where the game UI is composited. Only what the game context needs is
// hoisted -- the input phase's scope stays narrow.
struct GameUiInputHandoff
{
    glm::vec2    viewportMouse{0.0f, 0.0f};  // viewport-local cursor px
    std::uint8_t mouseButtons = 0;           // raw snap.mouseButtons (LMB=bit0)
    float        wheel        = 0.0f;        // raw snap.wheelY
    bool         inViewport   = false;
    double       frameDt      = 0.0;         // per-frame dt (seconds)
};
GameUiInputHandoff m_gameUi;
```

Sweep the writes (FrameInput) and reads (CompositeGameUi, PumpEditorDocuments' `m_lastFrameDt` use, and any other `m_last*` site grep finds) to `m_gameUi.*`.

- [ ] **Step 2: ViewportTargets.** Replace `m_viewport` (at ITS declaration position, ~:533), `m_pick`, `m_outline`, `m_pendingViewportW/H` with:

```cpp
// The viewport render-target triple -- canvas, GPU picker, selection
// outline -- created and RESIZED IN LOCKSTEP (ApplyPendingResize), plus the
// deferred size measured last frame. Declared after m_gpu: all three hold
// NVRHI handles and must destruct before the device. m_resolver (below)
// holds the canvas's batcher and must destruct first -- keep this struct
// declared BEFORE m_resolver.
struct ViewportTargets
{
    std::unique_ptr<Arcane::OffscreenCanvas>  canvas;
    std::unique_ptr<Arcane::PickBuffer>       pick;
    std::unique_ptr<Arcane::SelectionOutline> outline;
    std::uint32_t pendingW = 0, pendingH = 0;   // measured last frame, applied at frame top
    void ApplyPendingResize(GpuContext& gpu);   // body: the current
                                                // EditorApp::ApplyPendingViewportResize
};
ViewportTargets m_viewportTargets;
```

`EditorApp::ApplyPendingViewportResize()` (the MainLoop phase) becomes a thin forward: `m_viewportTargets.ApplyPendingResize(*m_gpu);` -- move the current body onto the struct method (it resizes all three targets; pass whatever else it references as parameters, adding none). Sweep all `m_viewport->`/`m_pick->`/`m_outline->`/`m_pendingViewportW/H` sites (RenderSceneToViewport, RenderSelectionOutline, DrawViewportPanelPhase, HandleViewportPick, WriteAutoScreenshot, FrameCamera, StageRenderBridge, ...). Keep `m_viewportRect`/`m_viewportActive` OUT of the struct (they are the input-side one-frame-lag pair, not render targets).

- [ ] **Step 3: ConsoleDiagnostics.** Replace `m_console`/`m_consoleSink`/`m_consoleUi` and `m_diagnostics`/`m_problemsUi` (contiguous block, position of `m_console`) with:

```cpp
struct ConsoleDiagnostics
{
    ConsoleBuffer                                    console{512};
    std::shared_ptr<spdlog::sinks::callback_sink_mt> sink;      // erased in Uninstall
    Arcane::Editor::ConsoleUiState                   ui;
    Arcane::Editor::DiagnosticStore                  store;
    Arcane::Editor::ProblemsUiState                  problemsUi;
    void Install();     // console sink + store.InstallAsEngineSink (was
                        // InstallConsoleSink + the Create() call beside it)
    void Uninstall();   // the Shutdown-top erase/uninstall pair
};
ConsoleDiagnostics m_consoleDiag;
```

Move `InstallConsoleSink`'s body into `Install()` (plus the adjacent `m_diagnostics.InstallAsEngineSink()` call from Create()); move Shutdown's sink-erase + diagnostics-uninstall pair into `Uninstall()`; Create()/Shutdown() call them. Keep the existing "why installed in Create, why erased at Shutdown top" comments on the methods. Sweep all member uses.

- [ ] **Step 4: CameraPanGesture.** Replace `m_camPanning`/`m_camPanLastMouse` with:

```cpp
// RMB-drag pan gesture (rules: starts only inside the viewport, keeps
// tracking once started -- see UpdateEditorCamera).
struct CameraPanGesture
{
    bool      panning = false;
    glm::vec2 lastMouse{0.0f, 0.0f};   // WINDOW px -- only the delta is used
};
CameraPanGesture m_camPan;
```

(`m_prevRmbDown` already moved to `m_edges.rmb` in Task 7; StandaloneLaunch fully dissolved in Tasks 4+8 -- confirm no launch members remain and note that in the report.)

- [ ] **Step 5: Build Debug, full suite.** Green. **Commit** `refactor(editor): member groupings -- GameUiInputHandoff, ViewportTargets, ConsoleDiagnostics, CameraPanGesture (B7)`

---

### Task 10: EditorRecents facade (spec section 10 -- B8)

**Files:**
- Create: `Arcane/ArcaneEditor/src/EditorRecents.hpp`, `EditorRecents.cpp`
- Modify: `EditorApp.hpp`, `EditorApp.cpp`, `EditorAppFrame.cpp`, `EditorAppScene.cpp`, `EditorAppProject.cpp`
- Modify: `Arcane/premake5.lua` ONLY if the ArcaneEditor project lists files explicitly (verify; it globs today)

**Interfaces:**
- Produces: `Arcane::Editor::EditorRecents` -- `RefreshAll(const Arcane::Project*)`, `NoteProjectOpened(const Arcane::Project*)`, `NoteSceneOpened(const Arcane::Project*, const std::filesystem::path&)`, public `projects` (RecentSelection) + `scenes` (SceneRecents::List) + `fileMenuWasOpen`. Task 11's `OnProjectOpened` calls `NoteProjectOpened`.

- [ ] **Step 1: Create the facade.** Move the BODIES of `EditorApp::RefreshRecents`, `NoteProjectOpened`, `ReloadSceneRecents`, `NoteSceneOpened` onto it (each currently reads `m_runtime->CurrentProject()` -- that becomes the `proj` parameter; everything else moves verbatim):

```cpp
// EditorRecents.hpp
#pragma once
// Both recents lists behind one facade (architecture pass sec 10): the Hub's
// shared machine-wide project list + the per-project scene list were two
// fully parallel ~10-site call ladders, always invoked in adjacent lines.
// A third recents kind is one method here, not ten call-site edits.
#include "RecentProjects.hpp"
#include "SceneRecents.hpp"
#include <filesystem>

namespace Arcane { class Project; }

namespace Arcane::Editor
{
    struct EditorRecents
    {
        RecentSelection    projects;         // Hub chain (RecentProjects.hpp)
        SceneRecents::List scenes;           // <root>/Saved/recent_scenes.json
        bool               fileMenuWasOpen = false;

        // File-menu rising edge + any explicit refresh point.
        void RefreshAll(const Arcane::Project* proj);
        // SUCCESS paths only -- a refused open must never reorder either list.
        // Records the project in the Hub file, refreshes, and reloads the
        // scene list (the two calls every site already made adjacently).
        void NoteProjectOpened(const Arcane::Project* proj);
        void NoteSceneOpened(const Arcane::Project* proj,
                             const std::filesystem::path& file);
    };
}
```

`EditorRecents.cpp` receives the four moved bodies (RefreshAll = old RefreshRecents + old ReloadSceneRecents; NoteProjectOpened = old NoteProjectOpened + reload; NoteSceneOpened = old body). Keep the empties-when-no-project behavior and every comment.

- [ ] **Step 2: Replace on EditorApp.** `m_recents`/`m_fileMenuWasOpen`/`m_sceneRecents` and the four method declarations become `Arcane::Editor::EditorRecents m_recents;`. Call-site sweep:
  - File-menu rising edge (ConsumeMenuRequests): `RefreshRecents(); ReloadSceneRecents();` -> `m_recents.RefreshAll(m_runtime->CurrentProject());`; `m_fileMenuWasOpen` -> `m_recents.fileMenuWasOpen`.
  - `BeginDockSpace(..., &m_recents, &m_sceneRecents)` -> `..., &m_recents.projects, &m_recents.scenes`.
  - Success tails (StageFinalize + switch_plugin_load): the adjacent `NoteProjectOpened(); ReloadSceneRecents();` pairs -> `m_recents.NoteProjectOpened(m_runtime->CurrentProject());`
  - `DoOpenScene`/`DoSaveScene`: `NoteSceneOpened(file)` -> `m_recents.NoteSceneOpened(m_runtime->CurrentProject(), file);`
  - ResetPerProjectState: `m_sceneRecents = {}` -> `m_recents.scenes = {};`

- [ ] **Step 3: Regen (new .cpp), build Debug, full suite** (RecentProjects/SceneRecents unit tests unchanged and must stay green). **Commit** `refactor(editor): EditorRecents facade -- the two parallel recents ladders become one (B8)`

---

### Task 11: PatchHostStages + OnProjectOpened (spec section 5, groundwork)

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditorApp.hpp`, `EditorApp.cpp`, `EditorAppProject.cpp`

**Interfaces:**
- Produces: `bool EditorApp::PatchHostStages(std::vector<Arcane::BootStage>& stages)` (false = table drift, fail loud) and `void EditorApp::OnProjectOpened(bool recordRecents = true)`. Task 12 consumes both.

- [ ] **Step 1: Extract PatchHostStages.** Move Create()'s `kHostStages` table + patch loop (EditorApp.cpp ~:1050-1091, including the membership-contract comment) into:

```cpp
// THE host-owned stage patch (architecture pass sec 5): Create() applies it
// to the boot list; SwitchProject applies it to the same shared
// EditorStages(ctx) list before cherry-picking its subset -- ONE patch
// path, both directions of drift still fail loudly (see the table comment).
bool EditorApp::PatchHostStages(std::vector<Arcane::BootStage>& stages)
{
    static constexpr HostStagePatch kHostStages[] = { /* the table, moved verbatim */ };
    for (const HostStagePatch& patch : kHostStages)
    {
        const auto it = std::ranges::find(stages, patch.id,
            [](const Arcane::BootStage& s) { return std::string_view(s.id); });
        if (it == stages.end())
        {
            ARC_ERROR("EditorApp::PatchHostStages: patch '{}' matched no stage in "
                      "EditorStages() -- renamed/removed in ProjectBoot.cpp without "
                      "updating this table", patch.id);
            return false;
        }
        it->run = [this, fn = patch.fn] { return (this->*fn)(m_bootCtx); };
    }
    return true;
}
```

(`HostStagePatch`/`StageFn` live in EditorApp.cpp's anonymous namespace today -- hoist the two-line definitions above the member function or into the hpp's private section; implementer's call, one place.) `Create()` becomes `if (!PatchHostStages(stages)) return false;`.

- [ ] **Step 2: Extract OnProjectOpened.** The success tail, called from BOTH copies:

```cpp
// The project-open SUCCESS tail (architecture pass sec 5) -- previously
// duplicated verbatim in StageFinalize and switch_plugin_load, with a
// partial third copy in the switch failure fallback. recordRecents=false is
// the fallback's case: it re-establishes the PROJECT-LESS baseline (or the
// old project), and a refused open must never reorder the recents lists.
void EditorApp::OnProjectOpened(bool recordRecents)
{
    if (m_undo)
    {
        if (const Arcane::Project* proj = m_runtime->CurrentProject())
        {
            if (const auto boot = Arcane::HostBoot::BootScene(*m_runtime, *proj))
            {
                m_scene.Adopt(boot->file, boot->id, *m_undo);
                m_frameOnSceneOpen = true;
            }
        }
    }
    EnsureScene();
    UpdateWindowTitle();
    if (recordRecents && m_runtime->CurrentProject())
        m_recents.NoteProjectOpened(m_runtime->CurrentProject());
}
```

  - `StageFinalize`: keep its `--project`-failed derive (error Push), then replace its Adopt/EnsureScene/title/recents run with `OnProjectOpened();` (verify against the current body -- every statement in the tail must be accounted for: moved, or kept in place with a why-comment).
  - `switch_plugin_load` (EditorAppProject.cpp): keep `m_runtime->Loop().SetPaused(true);`, then replace its tail with `OnProjectOpened();`.
  - Switch failure fallback: replace its `EnsureScene(); ... UpdateWindowTitle();` pair with `OnProjectOpened(/*recordRecents=*/false);` keeping `CloseProject`/`m_plugin.reset()`/`ResetRegistry`/lock-clear/`SetPaused` exactly where they are (order preserved: converge on project-less FIRST, then the tail).

- [ ] **Step 3: Build Debug, full suite** (BootStageParityTest must stay green -- ids/deps untouched). **Commit** `refactor(editor): PatchHostStages + OnProjectOpened extracted -- the switch can now share boot's bodies (B4 groundwork)`

---

### Task 12: SwitchProject unification (spec section 5 -- FULL, per user ruling)

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditorAppProject.cpp`

**Interfaces:**
- Consumes: `PatchHostStages`, `OnProjectOpened` (Task 11), `ResetPerProjectState` (Task 5), `HostBoot::EditorStages` (ProjectBoot.cpp:345).

- [ ] **Step 1: Replace the hand-built stage list.** Keep, untouched: the rival-lock guard, the probe/ABI validation, the dirty-documents guard, `outgoingRoot`/`lockedRoot`, the overlay presenter, `seq.Run`, and the whole failure/quit handling. Replace the `stage` lambda factory + four hand-rolled stages with:

```cpp
// ONE shared stage source (architecture pass sec 5). The ctx and pathStr are
// NAMED locals -- ctx.projectPath is a c_str view and BootSequence::Run is
// synchronous inside this scope (Amendment 1's dangling-temporary hazard is
// why these are not inline temporaries).
const std::string pathStr = path.string();
Arcane::HostBoot::BootContext ctx{};
ctx.runtime     = &*m_runtime;
ctx.projectPath = pathStr.c_str();
ctx.pluginPath  = m_config.pluginPath.c_str();
ctx.moduleName  = "ArcaneEditor.exe";

std::vector<Arcane::BootStage> all = Arcane::HostBoot::EditorStages(ctx);
if (!PatchHostStages(all))
{
    // Table drift -- the same fail-loud contract Create() has. Refuse the
    // switch; the session is still untouched (nothing torn down yet).
    m_modalErrors.Push("Open Project Failed",
        "Internal error: the host stage table no longer matches EditorStages() "
        "(see Console). The current session is unchanged.");
    return;
}

// Cherry-pick the reopen subset by id, in switch order. Boot-only stages
// (window/GPU/fonts/shell/finalize/splash) are skipped by omission.
auto take = [&all](std::string_view id) -> Arcane::BootStage
{
    const auto it = std::ranges::find(all, id,
        [](const Arcane::BootStage& s) { return std::string_view(s.id); });
    ARC_ASSERT(it != all.end(), "EditorStages lost a stage the switch needs");
    return std::move(*it);
};

std::vector<Arcane::BootStage> stages;

// switch_teardown stays switch-LOCAL: boot has no equivalent (nothing to
// tear down at boot), so there is no shared body to reuse.
{
    Arcane::BootStage teardown;
    teardown.id = "switch_teardown";
    teardown.thread = Arcane::BootThread::Main;
    teardown.policy = Arcane::BootPolicy::Fatal;
    teardown.weight = 2;
    teardown.run = [&]
    {
        ResetPerProjectState();
        m_gpu->Device().Nvrhi()->waitForIdle();
        m_plugin.reset();
        return true;
    };
    stages.push_back(std::move(teardown));
}

// project_open: the SHARED CoreStages body, as-is (ctx.runtime->OpenProject
// over ctx.projectPath + scan-progress detail -- the switch overlay now
// shows content-scan progress, which the hand-rolled body never did).
// Policy tightened to Fatal: at boot a failed open falls back to
// project-less startup; here the old project is already torn down, so
// there is genuinely nothing to fall back to except the failure fallback
// below (unchanged).
{
    Arcane::BootStage projectOpen = take("project_open");
    projectOpen.dependsOn = { "switch_teardown" };
    projectOpen.policy    = Arcane::BootPolicy::Fatal;
    stages.push_back(std::move(projectOpen));
}

// render_bridge: switch-LOCAL body (the boot body builds the viewport
// canvas/picker/outline, which already exist). The delta IS the switch:
// hand the editor lock over and load the new project's input config.
{
    Arcane::BootStage bridge = take("render_bridge");
    bridge.dependsOn = { "project_open" };
    bridge.run = [&]
    {
        if (!lockedRoot.empty())
            Arcane::EditorLock::Clear(lockedRoot);
        lockedRoot.clear();
        if (const Arcane::Project* proj = m_runtime->CurrentProject())
        {
            Arcane::EditorLock::Write(proj->Root());
            lockedRoot = proj->Root();
        }
        if (!Arcane::HostBoot::LoadInputConfig(m_gpu->Input(), m_runtime->Configuration()))
            ARC_WARN("Open Project: input actions failed to load");
        return true;
    };
    stages.push_back(std::move(bridge));
}

// plugin_load: REUSES the boot body (StagePluginLoad -- module resolve,
// host engage, failure banner, SetPaused are byte-identical needs), then
// runs the shared success tail. Policy stays Optional (a failed module
// load leaves the same safe disengaged state boot produces on purpose --
// see the 2026-07-30 ruling in this stage's old comment). DELIBERATE
// LOG-TEXT DELTA: the boot body's "no --project/--plugin" INFO line now
// also serves the switch (was "no game module / plugins for this
// project") -- ledgered, not hidden.
{
    Arcane::BootStage plugin = take("plugin_load");
    plugin.dependsOn = { "render_bridge" };
    plugin.run = [this]
    {
        const bool ok = StagePluginLoad(m_bootCtx);   // ignores its ctx argument
        OnProjectOpened();
        return ok;
    };
    stages.push_back(std::move(plugin));
}
```

  Notes for the implementer:
  - `StagePluginLoad` reads `m_runtime->CurrentProject()` + `m_config.pluginPath` directly and ignores its `BootContext&` -- verify that before reusing it, and say so in a comment at the reuse site.
  - The old switch stage ids (`switch_project_open` etc.) appear in watchdog/diagnostics phase labels and the failure banner ("failed at stage 'X'"); the shared ids replacing them is an accepted, ledgered delta.
  - VERIFY the shared `project_open` body's failure behavior composes with `policy = Fatal` (BootSequence aborts on a Fatal stage returning false -- the failure fallback then runs exactly as today).

- [ ] **Step 2: Failure fallback.** Unchanged except its tail already calls `OnProjectOpened(false)` from Task 11. Re-read the fallback's reachability comment and update the parts that name `switch_project_open`/`switch_plugin_load` to the new ids.

- [ ] **Step 3: Build Debug + Release, full suite from the exe dir** (BootStageParityTest + HostBootTest green). **Commit** `refactor(editor): SwitchProject cherry-picks shared EditorStages bodies -- the hand-rolled stage fork is gone (B4)`

- [ ] **Step 4: Report the desk-soak requirement.** This task's changes are UNVERIFIABLE without the desk battery (spec Testing item 7: A->B->A switch + failing-module fallback). State that explicitly in the completion report -- do not claim the switch verified.

---

## Verification (after all tasks)

- Full suite `~[gpu]` green from the exe dir, Debug + Release build 0/0, both hosts (`ArcaneEditor`, `ArcaneRuntime`) link.
- The spec's desk battery (7 items) is the USER's checklist -- surface it in the final summary; the A1 kill-shot is click-Play-then-same-frame-Del/Ctrl+V.
- Grep gates for the final review: zero hits for `m_prevKey`, `m_pendingInstanceParent`, `m_launchModalPending`, `m_launchAfterSceneSave`, `m_sceneError`, `m_launchError`, `m_projectOpenError`, `PickedThunk` (except the two new trampolines), and at most the two sanctioned `m_editBinding.editMode =` writes.
