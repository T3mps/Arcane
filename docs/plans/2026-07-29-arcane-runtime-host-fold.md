# Arcane Runtime Host Fold Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Retire Loom — host-boot layer folds into the Arcane DLL as `Arcane/Host/`, the exe becomes `ArcaneRuntime`, the runtime gains `--scene`, and the editor's play button grows an In-viewport / Separate-window dropdown.

**Architecture:** Three behavior-preserving-then-additive slices per the spec (`docs/superpowers/specs/2026-07-29-arcane-runtime-host-fold-design.md`). Slice 1 inverts ownership (engine owns the host layer; editor/tests/runtime consume it). Slice 2 adds the scene override. Slice 3 adds the editor launch UX.

**Tech Stack:** C++23, premake5 (VS2026 slnx), ImGui (editor UI + ImGuiSettingsHandler persistence), Win32 CreateProcessW (spawn), Catch2 (ArcaneTests).

## Global Constraints

- Branch: check `git branch --show-current` before committing (shared tree; commits land wherever the tree is).
- Build with THE VS18 MSBuild: `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /nodeReuse:false /t:"Arcane;ArcaneTests;ArcaneEditor;ArcaneRuntime"` (target is `Loom` until Task 4 renames it). Regenerate via `Arcane/GenerateProjects.bat` after ANY premake edit.
- Gate per task: build exit 0, then from the exe dir `ArcaneTests.exe "~[gpu]"` — capture the `Randomness seeded to:` banner in the commit message. NEVER kill a running ArcaneTests.exe (it is probably another agent's gate — wait-loop behind it). Do not run `[gpu]` tests or window-creating exes in this environment (GPU-driver crash hazard); windowed smoke runs are desk items.
- /MD everywhere in the Arcane workspace; UTF-8 without BOM; ASCII comments.
- NO legacy aliases: when a name changes, every referencer changes in the same commit. Never leave a `LoomConfig` typedef or a `Loom/` include path behind.
- Historical docs (`docs/superpowers/specs|plans|audits/*`) are NOT swept for the name Loom — they describe the past. Only living descriptions change (CLAUDE.md, CI, scripts, memory).

---

### Task 1: Move the host layer into the Arcane DLL (`Arcane/Host/`)

**Files:**
- Move (git mv): `Arcane/Loom/src/LoomConfig.hpp` → `Arcane/Arcane/src/Arcane/Host/HostConfig.hpp`
- Move: `Arcane/Loom/src/LoomConfig.cpp` → `Arcane/Arcane/src/Arcane/Host/HostConfig.cpp`
- Move: `Arcane/Loom/src/GpuContext.hpp/.cpp` → `Arcane/Arcane/src/Arcane/Host/GpuContext.hpp/.cpp`
- Move: `Arcane/Loom/src/FramePerf.hpp` → `Arcane/Arcane/src/Arcane/Host/FramePerf.hpp`
- Move: `Arcane/Loom/src/ProjectBoot.hpp` → `Arcane/Arcane/src/Arcane/Host/ProjectBoot.hpp`
- Modify: `Arcane/premake5.lua` (editor block ~449-454, tests block ~541-545 + ~624, Loom block ~385-398)
- Modify: `Arcane/Loom/src/Loom.hpp/.cpp`, `Arcane/Loom/src/main.cpp` (includes + type names)
- Modify: `Arcane/ArcaneEditor/src/main.cpp`, `EditorApp.hpp/.cpp` and any other `GpuContext`/`LoomConfig`/`FramePerf`/`ProjectBoot` includer (find with grep, step 2)
- Modify: the `[loom]` test file (find via `grep -rn "\[loom\]" Arcane/Tests/src`)

**Interfaces:**
- Produces: `Arcane::HostConfig` (was `struct LoomConfig`, global ns) with unchanged members + `HostConfig::Parse(int, char**)` returning `HostConfig::ParseOutcome`; `Arcane::GpuContext`; `Arcane::FramePerf`; `HostBoot` unchanged (already `namespace Arcane`). All classes with out-of-line methods gain `ARCANE_API`.
- Consumes: `Arcane::Cli` (Core) — now DLL-internal, exactly why consumers stop needing their own compile of the config TU.

- [ ] **Step 1: git mv the five files** into `Arcane/Arcane/src/Arcane/Host/` with the renames above.
- [ ] **Step 2: inventory every includer** — `grep -rn "LoomConfig\|GpuContext\|FramePerf\|ProjectBoot" Arcane --include=*.cpp --include=*.hpp -l` (excluding bin/, .example/). Expect: Loom/src (Loom.hpp/.cpp, main.cpp), ArcaneEditor/src (main.cpp, EditorApp.hpp/.cpp, possibly EditorAppFrame/Project), Tests (the `[loom]` file).
- [ ] **Step 3: rename + namespace the moved files.** In `HostConfig.hpp`: `struct LoomConfig` → `namespace Arcane { struct ARCANE_API HostConfig { ... }; }` (members unchanged; `ParseOutcome` follows the same complete-type pattern it has today). `HostConfig.cpp` follows. `GpuContext.hpp/.cpp`: wrap in `namespace Arcane`, class gains `ARCANE_API`. `FramePerf.hpp`: wrap in `namespace Arcane` (header-only — no export macro needed unless it has statics; check). `ProjectBoot.hpp` already `namespace Arcane` — only its `#pragma once` header comment and include guards touched if needed. Include paths inside the moved files become `<Arcane/Host/...>` where they reference each other. Keep header comments; update the file-purpose lines (LoomConfig → HostConfig, "Loom's command line" → "a runtime host's command line").
- [ ] **Step 4: fix every consumer.** Includes become `<Arcane/Host/HostConfig.hpp>` etc.; bare `LoomConfig` → `Arcane::HostConfig`; bare `GpuContext`/`FramePerf` → `Arcane::` qualified (or a file-local `using Arcane::GpuContext;` where the file already sits in `namespace Arcane::Editor`, which resolves unqualified). Editor `main.cpp`: `LoomConfig::ParseOutcome parsed = LoomConfig::Parse(...)` → `Arcane::HostConfig::...`.
- [ ] **Step 5: premake.** Engine project already globs `Arcane/src/**` — confirm the glob picks up `Arcane/Host/` (it does; same pattern as other modules). ArcaneEditor block: DELETE the two `files` entries `"%{wks.location}/Loom/src/GpuContext.cpp"`, `"%{wks.location}/Loom/src/LoomConfig.cpp"` and the includedir `"%{wks.location}/Loom/src"`. ArcaneTests block: DELETE `"%{wks.location}/Loom/src/LoomConfig.cpp"` from files and `"%{wks.location}/Loom/src"` from includedirs. Loom project block: `files` still globs its own src (now only Loom.hpp/.cpp + main.cpp); it keeps `links { "Core", "Arcane" }` (Core stays: cheap, established two-static-copies pattern, and other exe TUs may use un-exported Core APIs).
- [ ] **Step 6: retag the config tests.** In the `[loom]` test file: tag → `[host]`, `LoomConfig` → `Arcane::HostConfig`, include → `<Arcane/Host/HostConfig.hpp>`. Test BODIES unchanged — they pin the flag vocabulary, which does not change in this task.
- [ ] **Step 7: regenerate + build + gate.** `GenerateProjects.bat`; build `/t:"Arcane;ArcaneTests;ArcaneEditor;Loom"`; run `ArcaneTests.exe "~[gpu]"`; capture seed. Expect identical pass counts (retagged, not removed).
- [ ] **Step 8: commit** — `refactor(arcane): host layer moves into the engine DLL as Arcane/Host` with the ownership-inversion story and the gate line.

### Task 2: `--scene` override in the runtime host

(Slice 2 lands BEFORE the project rename so the rename sweep in Tasks 3-4 is the last time these files churn.)

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Host/HostConfig.hpp/.cpp`
- Modify: `Arcane/Loom/src/Loom.cpp` (scene-load path in Init)
- Test: the `[host]` config test file

**Interfaces:**
- Produces: `HostConfig::sceneOverride` (`std::string`, empty = follow the manifest's bootScene); CLI flag `--scene <guid>`.

- [ ] **Step 1: failing test.** In the `[host]` file, alongside the existing flag tests:
```cpp
TEST_CASE("HostConfig parses --scene as a guid override", "[host]")
{
    const char* argv[] = { "host", "--scene", "a5e0c1de-1111-4222-8333-444455556666" };
    auto out = Arcane::HostConfig::Parse(3, const_cast<char**>(argv));
    REQUIRE(out.config.has_value());
    CHECK(out.config->sceneOverride == "a5e0c1de-1111-4222-8333-444455556666");
    // Absent flag = empty = manifest bootScene wins (today's behavior).
    const char* bare[] = { "host" };
    auto def = Arcane::HostConfig::Parse(1, const_cast<char**>(bare));
    REQUIRE(def.config.has_value());
    CHECK(def.config->sceneOverride.empty());
}
```
(Mirror the existing tests' argv idiom exactly — read one before writing this.)
- [ ] **Step 2: run, watch it fail to compile** (no member).
- [ ] **Step 3: implement.** `HostConfig` gains `std::string sceneOverride = "";` with the comment: `// Boot this scene (asset Guid text) instead of the manifest's bootScene. Empty = follow the manifest. Editor separate-window play passes the ACTIVE scene here.` `HostConfig.cpp`: add the `"scene"` option to the Cli spec + mapping, following the `"project"` option's exact shape.
- [ ] **Step 4: wire the override in `Loom.cpp` Init.** Find where the opened project's `bootScene` is resolved into the scene load (follow `Manifest().bootScene` / the HostBoot scene path). Insert: if `m_config.sceneOverride` non-empty → `Arcane::Guid::FromString(it)`; invalid text = log `ARC_ERROR` naming the value and fail Init (the existing bad-boot path); valid = use it in place of the manifest guid. A guid that parses but resolves to no asset follows the existing missing-bootScene failure path unchanged.
- [ ] **Step 5: build + gate + commit** — `feat(arcane): runtime host boots --scene over the manifest's bootScene`.

### Task 3: living-reference sweep (CI, scripts, CLAUDE.md)

Done BEFORE the rename so Task 4's diff is purely mechanical.

**Files:**
- Modify: `Jenkinsfile` (repo root — grep `Loom`), `Arcane/scripts/launch.ps1`, `Arcane/scripts/launch.bat` (grep first; only if they reference Loom), `CLAUDE.md` (the Arcane build-system section's Loom description + run examples)

- [ ] **Step 1: inventory** — `grep -rn "Loom" Jenkinsfile CLAUDE.md AGENTS.md Arcane/scripts ci/ 2>/dev/null`.
- [ ] **Step 2: rewrite CLAUDE.md's living text.** The engine-workspace paragraph: Loom's description becomes ArcaneRuntime ("the standalone runtime host: opens an .arcproj and runs its game module; hosts bare plugins for the Sandbox showcase and hot-reload fixtures; `--frames N` is the scripted GPU-verify"), and the two `bin\...\Loom\Loom.exe` example lines become `bin\...\ArcaneRuntime\ArcaneRuntime.exe`. Note the fold + retirement of the name with the date. Hot-reload dev-loop paragraph: "while Loom.exe is running" → ArcaneRuntime.
- [ ] **Step 3: CI + scripts.** Every `Loom` occurrence in Jenkinsfile/scripts becomes `ArcaneRuntime` (paths AND target names) — these land in the SAME commit as Task 4's rename so CI never sees a half-renamed tree. Stage them now; commit with Task 4.

### Task 4: rename the project — Loom → ArcaneRuntime

**Files:**
- Move: `Arcane/Loom/` → `Arcane/ArcaneRuntime/`; `src/Loom.hpp/.cpp` → `src/RuntimeApp.hpp/.cpp`
- Modify: `Arcane/premake5.lua` (project block ~372-420; Sandbox postbuild ~354-359; PlaygroundGame postbuild; any `dependson`/comments naming Loom)
- Modify: staged Task-3 files (commit together)

**Interfaces:**
- Produces: project `ArcaneRuntime` → `bin/<cfg>/ArcaneRuntime/ArcaneRuntime.exe`; `class RuntimeApp` (global ns, as `class Loom` was; exe-local).

- [ ] **Step 1: git mv** the directory and the two source files; `class Loom` → `class RuntimeApp` (header comment: "RuntimeApp: the standalone host application object" — keep the teardown-contract comment verbatim); `main.cpp` constructs `RuntimeApp app(*parsed.config);`.
- [ ] **Step 2: premake.** Project block: `project "ArcaneRuntime"`, `location "ArcaneRuntime"`; the block comment rewritten (thin standalone host; Sandbox default; fixtures). Sandbox project's postbuild: `bin/.../Loom` → `bin/.../ArcaneRuntime` (both MKDIR and COPYFILE lines); same for the PlaygroundGame copy inside the runtime's own postbuild block. Grep premake for remaining `Loom` — zero expected.
- [ ] **Step 3: regenerate + full build** `/t:"Arcane;ArcaneTests;ArcaneEditor;ArcaneRuntime"` + gate.
- [ ] **Step 4: delete stale bin output** `Arcane/bin/<cfg>/Loom/` (untracked build output; the new dir replaced it).
- [ ] **Step 5: commit** (with Task 3's staged files) — `refactor(arcane): Loom retires -- the exe is ArcaneRuntime`. Desk-verify note in message: `ArcaneRuntime.exe` interactive Sandbox + `--frames 30` are desk/CI items (GPU hazard locally).

### Task 5: editor-side launch helper (pure + tested)

**Files:**
- Create: `Arcane/ArcaneEditor/src/RuntimeLaunch.hpp`, `RuntimeLaunch.cpp`
- Test: `Arcane/Tests/src/RuntimeLaunchTest.cpp` (new; CPU-only; ArcaneTests already compiles editor-adjacent pure files ONLY if listed — check how existing editor-logic tests are wired; if EditorApp sources are not in the test exe, put the pure functions in the header or list RuntimeLaunch.cpp in the tests project the way LoomConfig.cpp used to be)

**Interfaces:**
- Produces (namespace `Arcane::Editor::RuntimeLaunch`):
  - `std::vector<std::filesystem::path> ExeCandidates(const std::filesystem::path& editorExeDir)` — `{ dir / "ArcaneRuntime.exe", dir / ".." / "ArcaneRuntime" / "ArcaneRuntime.exe" }` (packaged layout first, dev bin layout second; comment cites the Hub's suggest_engine as the pattern).
  - `std::vector<std::wstring> BuildArgs(const std::filesystem::path& projectRoot, const Arcane::Guid& scene, Arcane::GraphicsBackend backend)` — `--project <root>`, `--scene <guid>`, `--backend <dx12|vulkan>`; pure, no quoting decisions left to callers.
  - `bool SpawnDetached(const std::filesystem::path& exe, const std::vector<std::wstring>& args)` — CreateProcessW, working directory = exe's parent (shader-resolution rule, same as the Hub's launch), handles closed immediately, no tracking. Returns false with `ARC_ERROR` naming the exe on failure.

- [ ] **Step 1: failing tests** — candidates order + contents; BuildArgs round-trip (`--scene` present iff guid valid, backend token matches the enum). No spawn test (process creation is desk territory).
- [ ] **Step 2-4: implement, build, gate.**
- [ ] **Step 5: commit** — `feat(editor): RuntimeLaunch -- resolve and spawn the standalone host`.

### Task 6: play-mode dropdown + separate-window flow

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditorApp.hpp` (mode enum + persisted state + `LaunchStandalone()` decl), `EditorAppFrame.cpp` or `EditorApp.cpp` (settings-handler registration beside the existing ones; LaunchStandalone impl), `EditorPanels.cpp` (toolbar, transport block at ~274-316)
- Consult (read before editing): `ShaderEditorDocument.cpp:748-800` + `:1690` (the ImGuiSettingsHandler pattern to mirror), `EditorAppScene.cpp` (authoritative active-scene file/guid + dirty members + `DoSaveScene` at `EditorApp.hpp:473`)

**Interfaces:**
- Produces: `enum class PlayLaunchMode { Viewport, SeparateWindow };` persisted via an ImGuiSettingsHandler section (`[EditorPlayMode][State]`, one line `Mode=N`, mirroring the shader-layout handler's read/write shape); `EditorApp::LaunchStandalone()`.

- [ ] **Step 1: state + persistence.** Add the enum + `PlayLaunchMode m_playMode = PlayLaunchMode::Viewport;` to EditorApp.hpp. Register a settings handler exactly like the shader editor's layout handler (same registration site pattern) reading/writing the one value. Malformed/absent ini line = Viewport (the safe default).
- [ ] **Step 2: toolbar.** In the transport block, after the Step button: `ImGui::SameLine();` + a small chevron button (`ICON_LC_CHEVRON_DOWN`, `iconBtn` idiom, tooltip "Play mode") opening `ImGui::OpenPopup("##play_mode")`; the popup lists `In viewport` / `Separate window` as `ImGui::MenuItem(label, nullptr, m_playMode == X)` rows setting `m_playMode`. The rows are the future server-set seam — comment says so. The PLAY button's click branches: `Viewport` → today's `play.Play(runtime, plugin)` unchanged; `SeparateWindow` → `LaunchStandalone()`. Play-button tint/stop semantics remain PIE-only (standalone is fire-and-forget; the toggle never lights for it).
- [ ] **Step 3: LaunchStandalone.**
```cpp
// Boots the ACTIVE scene, as saved (spec: option b). Dirty -> modal
// save-or-cancel; never a snapshot of unsaved state. Fire-and-forget:
// the runtime takes no lock and the editor does not track the child.
void EditorApp::LaunchStandalone()
{
    // 1. No project open -> notice + return (nothing to --project).
    // 2. Scene dirty (consult EditorAppScene.cpp's dirty flag) ->
    //    OpenPopup("Save and Play?"); the modal's [Save and Play] runs
    //    DoSaveScene(activeFile) and falls through; [Cancel] returns.
    // 3. Resolve exe: first existing candidate from
    //    RuntimeLaunch::ExeCandidates(<dir of current exe>); none found ->
    //    ARC_ERROR + editor notice naming both candidate paths, return.
    // 4. RuntimeLaunch::SpawnDetached(exe, BuildArgs(projRoot, activeSceneGuid,
    //    m_config.backend)); failure -> notice. Editor state untouched.
}
```
The modal follows the editor's existing ImGui modal idiom (find one popup-modal in the editor and mirror it; if none exists yet, `ImGui::BeginPopupModal` with the two buttons is the whole pattern).
- [ ] **Step 4: build + gate.** The gate does not compile EditorPanels/EditorApp into ArcaneTests — check `ArcaneEditor.exe`'s timestamp to confirm the editor actually relinked (the standing lesson).
- [ ] **Step 5: commit** — `feat(editor): play-mode dropdown -- separate-window play via ArcaneRuntime`. Desk list in the message: dropdown persists across restarts; dirty scene prompts; game window opens on the active scene while the viewport stays editable; spawn-failure notice when the runtime exe is renamed away.

### Task 7: close-out

- [ ] **Step 1: repo-wide grep** `grep -rn "Loom" --include=*.{cpp,hpp,lua,md,ps1,bat,yml} . | grep -v ThirdParty | grep -v .example | grep -v docs/superpowers | grep -v bin/` — expect zero living references.
- [ ] **Step 2: full gate** — build all four targets, `~[gpu]` both, seed captured; editor + runtime exe timestamps current.
- [ ] **Step 3: memory** — update `project_loom_folds_into_arcane.md` (directive DONE, date, commits) and the MEMORY.md line; note desk items.
- [ ] **Step 4: commit any stragglers** — `docs(arcane): runtime-fold close-out`.

## Self-review notes

- Spec coverage: Part A = Tasks 1+3+4; Part B = Task 2; Part C = Tasks 5+6; Part D = the comment seam in Task 6 Step 2 (by design, no code); testing section = per-task gates + Task 7.
- Type consistency: `Arcane::HostConfig` / `HostConfig::ParseOutcome` (T1) is what T2 extends and T4's `main.cpp` parses; `RuntimeLaunch::ExeCandidates/BuildArgs/SpawnDetached` (T5) are what T6 calls; `PlayLaunchMode` appears only in T6.
- Deliberate consult-points (not placeholders): the `[loom]` test filename, the bootScene load site in Loom.cpp, the active-scene/dirty members, and the modal idiom are named files the implementer reads first — their exact identifiers are the repo's to state, not this plan's to guess.
