# Aphelyon repo relocation — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `D:\dev\starworks\Aphelyon` becomes the Aphelyon repo — a fresh-history git repo whose ROOT is the Arcane project (`Aphelyon.arcproj` at the root, like a `.uproject`), with the game module at `Source/Game/` and the backend services at `Source/Services/` — built, tested and booted from the new location; `D:\dev\starworks\Gacha` stays untouched as the archaeology copy with its remote removed.

**Architecture:** Decision record `docs/research/2026-09-16-multiplayer-shape-and-project-layout.md` §5 (L1–L3) and the user's 2026-09-16 rulings (fresh history; the new dir inherits `StarworksDev/Aphelyon`; `Source/Game` + `Source/Services`, not Client/Server). The move is a `git archive` of Gacha `HEAD` (tracked files only — the 7.9 GB of ignored build output never moves) into the new directory, one rearrangement, and one commit. The SDK prerequisite landed in Arcane (`0cf74883..707d3b75`): the manifest's `sourceDir` field drives the build glob and the editor. Nothing changes in the engine. Only four Gacha files hard-code `Server/` or `Game/` paths (`scripts/setup.ps1`, the wizard's `Result.svelte`, `ci/docker-compose.ci.yml`, `Jenkinsfile`); the services' premake and scripts escape their directory with `..` in eight places, each rewritten once. `ThirdParty/` sheds the engine-era leftovers the services never use.

**Tech Stack:** git, premake5 (bundled), MSBuild (VS 18), the Arcane SDK via `ARCANE_SDK` (must be at or after Arcane `5b652285`, built Debug), arcbuild, Docker (unchanged), PowerShell.

**Spec:** the decision record above; `docs/specs/2026-07-22-arcane-project-format-design.md` §5 (the skeleton); Gacha `CLAUDE.md` (living doc, rewritten by this plan).

## Global Constraints

- **Two directories, one new repo.** Source = `D:\dev\starworks\Gacha` at `080cbb93` (`main`, 4 ahead of origin, never pushed — those commits ride the new repo). Target = `D:\dev\starworks\Aphelyon` (must not exist at start). The Gacha working tree is READ-ONLY for this plan except Task 4's `git remote remove origin`; never `git commit`, `checkout`, `clean` or delete anything there. The two in-flight Gacha files (`Game/Source/TestComponent.{cpp,hpp}` untracked, `Game/Content/scenes/test.arcscene` modified) are the user's: they are NOT carried (archive = tracked `HEAD` only) and are NOT touched; the closeout names them so the user can port them by hand.
- **Fresh history; inherit the remote; DO NOT PUSH.** The new repo gets one initial commit (plus this plan's follow-up commits) and `origin = https://github.com/StarworksDev/Aphelyon.git`, but the first push REPLACES the remote's history and is the user's action (Task 4's closeout gives the exact commands, including tagging the old `main` first). Trailers on every commit in the new repo:
  ```
  Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01Djskfodj1WnyR2g5v9V1Qc
  ```
- **Build rituals (bash).** Services, from `D:\dev\starworks\Aphelyon\Source\Services`: `../../ThirdParty/premake5/premake5.exe vs2026` (never `GenerateProjects.bat` under the Bash tool), then `MSYS_NO_PATHCONV=1 "/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" AphelyonServices.slnx -p:Configuration=<Debug|Release> -m -nologo -v:m -t:Rebuild > <log> 2>&1` (FOREGROUND, `timeout` 600000), `grep -E " error |LNK4098|LNK2019|LNK1181|Error\(s\)|Warning\(s\)" <log>`. Game module, from the repo root: `"$ARCANE_SDK/bin/Debug-windows-x86_64-md/arcbuild/arcbuild.exe" build --project . --config Debug` → `Binaries/Aphelyon.dll`. Suites FROM each exe dir with the `| grep -E "^Randomness|^test cases|^assertions|All tests passed|FAILED"` filter; never AccountTests in this plan (no docker at all — the dev database is off limits; the ephemeral recipe is unchanged and verified by CI on the first push).
- **`ARCANE_SDK`** = `D:\dev\starworks\Arcane` at `707d3b75` or later (the `sourceDir` reader is at `5b652285`), built Debug (arcbuild + ArcaneCore/ArcaneClient present under `bin/Debug-windows-x86_64-md/`).
- **Ledger:** Arcane `.superpowers/sdd/2026-09-16-aphelyon-repo-relocation-plan/progress.md`.

---

## Plan-time rulings

| # | Ruling | Why |
|---|---|---|
| M1 | **Archive, don't clone.** `git -C Gacha archive HEAD` unpacked into the new dir, then `git init`. | Fresh history was the user's choice; `git archive` carries exactly the tracked tree and nothing else (no `.git`, no ignored 17 GB). |
| M2 | **`ThirdParty/` carries only what the services build against**: `Catch2`, `rapidcheck`, `nlohmann`, `picosha2`, `spdlog`, `Xoshiro`, `sqlpp23`, `aphelyon-sql-types`, `premake5`, `README.md`. Dropped: `Astra`, `Manifold2D`, `enkiTS`, `freetype`, `imgui`, `imgui-node-editor`, `msdfgen`, `nvrhi`, `tracy`, `love2d`. | `Source/Services/premake5.lua` names exactly the kept set; the rest are pre-extraction engine copies the game consumes from `$ARCANE_SDK/ThirdParty` (arcane.lua). A fresh repo should not carry 1,500 dead vendored files. `ThirdParty/README.md` is rewritten to the kept list. |
| M3 | **The services workspace is renamed `AphelyonServices`** (→ `AphelyonServices.slnx`). | The root now generates `Aphelyon.slnx` (the game workspace, `arcbuild`); two `Aphelyon.slnx` in one repo is a trap for every "open the solution" instruction. Every reference (Jenkinsfile, `setup.ps1`, the wizard, docs) is rewritten in the same commit. |
| M4 | **Root `.gitignore` = Gacha's root file with `Game/.gitignore`'s derived-tree entries merged in** (`Binaries/`, `Intermediate/`, `Saved/`, `*.filters`, `*.user`), `Server/data/abilities/` rewritten to `Source/Services/data/abilities/`, and the "leftovers of the 2026-08-11 extraction" block dropped (nothing is left over in a fresh tree). | One ignore file at the project root, the way the format spec's template has it. |
| M5 | **`Server/docs/operations/backup-drill.md` moves to `docs/operations/`**; `Server/BUILD.md` moves with the services as `Source/Services/BUILD.md`. | Docs live under `docs/`; BUILD.md is the services' own README and `docs/components/server.md` includes it by path. |
| M6 | **Historical docs are not rewritten** (`docs/superpowers/**`, `docs/jira/**`, `docs/audits/**` keep `Server/`/`Game/` wording; 116 files). Living docs are: `README.md`, `CLAUDE.md`, `Source/Services/BUILD.md`, `docs/components/server.md`, `ci/README.md`, `mkdocs.yml`'s comment, the four scripts, the Jenkinsfile. | Plan 2/3's Q3/Q8. |
| M7 | **`Setup.exe` is carried as-is and will be stale** until CI rebuilds it from `Tools/setup-wizard` (its `lib.rs` path to the services solution changes here). | The exe is CI-maintained by `.github/workflows/build-setup-wizard.yml` on push; committing a locally-built one is not the convention. The closeout says so. |
| M8 | **The Gacha dir keeps everything; its remote is removed by the USER, after they tag the old `main` on GitHub** (sequence in Task 4). No branch rename, no deletion, no agent-side mutation of the archive. | "Keep this for archaeology." The tag push needs the remote, so the removal must follow it; both are the user's outward actions. |
| M9 | **The push is the user's**, with the old remote `main` tagged first. | A fresh-history push to `StarworksDev/Aphelyon` is `--force` by nature and rewrites what Jenkins tracks; the user chose the remote and pushes; the plan writes the exact sequence (`git -C Gacha push origin main:refs/tags/archive/gacha-2026-09-16` from the archive BEFORE its remote is removed, then `git push --force origin main` from the new repo). Not run by any agent. |

---

## File structure (new repo, tracked)

```
Aphelyon/
├─ Aphelyon.arcproj            (+ "sourceDir": "Source/Game")      ├─ ci/           (from Gacha ci/)
├─ premake5.lua                (Game/premake5.lua, header reworded) ├─ docs/         (+ docs/operations/backup-drill.md)
├─ Config/  Content/  Plugins/ (from Game/)                         ├─ marketing/  mkdocs.yml  wrangler.jsonc
├─ Source/Game/                (Game/Source/*)                      ├─ scripts/      (root scripts + setup.ps1 rewritten)
├─ Source/Services/            (Server/* minus docs/)               ├─ Tools/setup-wizard/  (lib.rs + Result.svelte rewritten)
│    Account/ Auth/ Combat/ Common/ data/ scripts/ premake5.lua     ├─ ThirdParty/   (M2's kept set)
│    GenerateProjects.bat BUILD.md Dockerfile.postgres              ├─ vcpkg-triplets/
│    docker-compose.yml .env.example cpp_coding_style.txt           ├─ Jenkinsfile  README.md  CLAUDE.md  AGENTS.md
│    Aphelyon.slnLaunch.user                                        ├─ .gitignore (M4)  .gitattributes  .github/  Setup.exe
```

---

### Task 1: Materialise the tree — archive, rearrange, manifest, ignore file (no build yet)

**Files:** everything above is created by moves; edited in this task: `Aphelyon.arcproj`, `.gitignore`, `ThirdParty/README.md`.

- [ ] **Step 1: RED — the target does not exist; the source is at the expected commit.**
```bash
test ! -e /d/dev/starworks/Aphelyon && echo "target absent"
git -C /d/dev/starworks/Gacha rev-parse --short HEAD          # 080cbb93
git -C /d/dev/starworks/Gacha status --short | grep -v '^??'   # only: M Game/Content/scenes/test.arcscene
```

- [ ] **Step 2: Archive the tracked tree.**
```bash
mkdir -p /d/dev/starworks/Aphelyon && git -C /d/dev/starworks/Gacha archive --format=tar HEAD | tar -x -C /d/dev/starworks/Aphelyon
cd /d/dev/starworks/Aphelyon && ls && test -d Game && test -d Server && test ! -e .git && echo "archived"
```

- [ ] **Step 3: Rearrange** (plain `mv` — no git yet):
```bash
cd /d/dev/starworks/Aphelyon
mkdir -p Source/Game Source/Services docs/operations
mv Game/Source/* Source/Game/ 2>/dev/null; rmdir Game/Source
mv Game/Aphelyon.arcproj Game/premake5.lua Game/Config Game/Content Game/Plugins .
rm Game/.gitignore          # its entries are merged into the root .gitignore in Step 5 (M4)
rmdir Game
mv Server/docs/operations/backup-drill.md docs/operations/ && rmdir Server/docs/operations Server/docs
mv Server/* Server/.env.example Source/Services/ && rmdir Server
for d in Astra Manifold2D enkiTS freetype imgui imgui-node-editor msdfgen nvrhi tracy love2d; do rm -rf "ThirdParty/$d"; done
ls ThirdParty        # Catch2 README.md Xoshiro aphelyon-sql-types nlohmann picosha2 premake5 rapidcheck spdlog sqlpp23
ls Source/Game Source/Services
```
  (`Game/Source/` may contain only tracked files — `ReferenceProject`-style `.gitkeep` or the real module sources; whatever `git ls-files Game/Source` listed in Gacha.)

- [ ] **Step 4: The manifest declares the module directory.** In `Aphelyon.arcproj`, after the `"gameModule": "Aphelyon.dll",` line insert `  "sourceDir": "Source/Game",`. In `premake5.lua`'s header comment, replace `--project .` lines' wording that says "Then host it:  ArcaneRuntime.exe --project <this dir>" (keep), and add after the `arcane_game_module("Aphelyon")` line a comment: `-- The module's sources live at Source/Game/ (Aphelyon.arcproj "sourceDir"); Source/Services/ is the backend, its own premake workspace, never compiled into the module.`

- [ ] **Step 5: `.gitignore` (M4).** Edit the root `.gitignore`: (a) in the header comment replace `(Game/ builds against the Arcane engine SDK and carries its own .gitignore.)` with `(one file at the project root: the services' build output AND the game module's derived tree)`; (b) after the `bin-int/` line add
```
# Arcane project derived tree (format spec 2026-07-22 S3/S10): committed = Aphelyon.arcproj Source/ Content/ Config/ Plugins/ premake5.lua
Binaries/
Intermediate/
Saved/
*.filters
Plugins/**/Binaries/
```
  (c) replace `Server/data/abilities/` with `Source/Services/data/abilities/`; (d) delete the whole trailing block from `# ── On-disk leftovers of the 2026-08-11 repo extraction` through `ThirdParty/box2d-3.1.1/`. `ThirdParty/README.md`: rewrite its inventory to the kept set (one line each: what it is, license, how consumed) and one sentence: "Engine-side deps (Astra, Manifold2D, enkiTS, imgui, …) are consumed from `$ARCANE_SDK/ThirdParty` through `build/arcane.lua` and are not vendored here."

- [ ] **Step 6: GREEN — shape check.**
```bash
cd /d/dev/starworks/Aphelyon && test -f Aphelyon.arcproj && test -f premake5.lua && test -d Source/Game && test -d Source/Services/Account && test -f Source/Services/premake5.lua && test -f docs/operations/backup-drill.md && test ! -e Game && test ! -e Server && grep -c '"sourceDir": "Source/Game"' Aphelyon.arcproj && grep -c "Source/Services/data/abilities" .gitignore && echo SHAPE OK
```
  No commit yet (Task 2 rewrites paths first; the initial commit is Task 3's).

---

### Task 2: Rewrite every path that named `Server/` or `Game/`, and the services workspace name

**Files:** `Source/Services/premake5.lua`, `Source/Services/scripts/{generate,setup-vcpkg-deps,clean}.bat`, `Source/Services/GenerateProjects.bat` (unchanged — relative), `Jenkinsfile`, `scripts/setup.ps1`, `Tools/setup-wizard/src-tauri/src/lib.rs`, `Tools/setup-wizard/src/lib/screens/Result.svelte`, `ci/docker-compose.ci.yml` (comment), `ci/README.md`, `mkdocs.yml`, `docs/components/server.md`, `README.md`, `CLAUDE.md`, `Source/Services/BUILD.md`.

- [ ] **Step 1: RED — count the old paths.** `cd /d/dev/starworks/Aphelyon && grep -rn "Server\\\\\|Server/\|\bGame/\|Game\\\\\|\.\./ThirdParty\|\.\.\\\\ThirdParty\|\.\.\\\\vcpkg-triplets\|Aphelyon\.slnx" Jenkinsfile scripts ci Tools/setup-wizard/src Tools/setup-wizard/src-tauri/src mkdocs.yml docs/components/server.md README.md CLAUDE.md Source/Services/premake5.lua Source/Services/scripts Source/Services/BUILD.md | wc -l` → a non-zero count (record it).

- [ ] **Step 2: `Source/Services/premake5.lua`.** `workspace "Aphelyon"` → `workspace "AphelyonServices"`; every `%{wks.location}/../ThirdParty/…` → `%{wks.location}/../../ThirdParty/…` (nine `IncludeDir` lines); the three `include "../ThirdParty/…"` → `include "../../ThirdParty/…"`; the header comment's first lines become `-- Aphelyon Services -- premake5 for Visual Studio 2026 (generates AphelyonServices.slnx)` / `-- 3 services (Auth, Account, Combat) + 1 shared library (Common); lives at Source/Services/ of the Aphelyon project repo, whose ROOT is the Arcane project (Aphelyon.arcproj; the game module is Source/Game/, built by arcbuild, never by this workspace).`; the ARCANE_SDK comment block's `Server/premake5.lua` mentions → `Source/Services/premake5.lua`.

- [ ] **Step 3: Services scripts.** `scripts/generate.bat`: `set "OVERLAY=%PROJECT_ROOT%\..\vcpkg-triplets"` → `\..\..\vcpkg-triplets`; `set "PREMAKE5=%PROJECT_ROOT%\..\ThirdParty\premake5\premake5.exe"` → `\..\..\ThirdParty\…`; its `Aphelyon.slnx` mentions → `AphelyonServices.slnx`. `scripts/setup-vcpkg-deps.bat`: `set "OVERLAY=%SCRIPT_DIR%..\..\vcpkg-triplets"` → `%SCRIPT_DIR%..\..\..\vcpkg-triplets`; its "Next steps" echo names `AphelyonServices.slnx`. `scripts/clean.bat`: the four `%PROJECT_ROOT%\..\ThirdParty\…` → `\..\..\ThirdParty\…` (and the echo text). `scripts/start-all.bat`'s `%~dp0..\bin` is unchanged (bin is beside the scripts' parent). `db-*.bat`/`.sh`: grep them for `Server` — none expected.

- [ ] **Step 4: Jenkinsfile.** Every `Server\\` / `Server/` → `Source\\Services\\` / `Source/Services/` (Generate `cd Source\\Services && call GenerateProjects.bat`; both msbuild lines → `Source\\Services\\AphelyonServices.slnx`; the three fast-test `cd` lines; the AccountTests `cd`; `CI_COMPOSE` and the `post { always }` line → `-f Source/Services/docker-compose.yml`); the Game stage: `--project Game` → `--project .` (three lines) and its comment `Game/ builds as an external project` → `The repo root IS the Arcane project (Aphelyon.arcproj); the game module builds`; the header comment likewise. `ci/docker-compose.ci.yml`'s two comment lines → `Source/Services/docker-compose.yml`. `ci/README.md`: any `Server/` → `Source/Services/`.

- [ ] **Step 5: setup.ps1 + the wizard.** `scripts/setup.ps1`: `$RepoRoot\Server\scripts\setup-vcpkg-deps.bat` / `\Server\GenerateProjects.bat` / `\Server\scripts\db-setup.bat` → `\Source\Services\…`; `--project "$RepoRoot\Game"` (two places) → `--project "$RepoRoot"`; `"$RepoRoot\Server\Aphelyon.slnx"` → `"$RepoRoot\Source\Services\AphelyonServices.slnx"`; the `Info "Setup complete. Next: open Server\Aphelyon.slnx and run Server\scripts\start-all.bat."` → `Source\Services\AphelyonServices.slnx` / `Source\Services\scripts\start-all.bat`; the `Game/` comment block (lines ~126-133) reworded to "the repo root is the Arcane project". `Tools/setup-wizard/src-tauri/src/lib.rs:213`: `.join("Server").join("Aphelyon.slnx")` → `.join("Source").join("Services").join("AphelyonServices.slnx")`. `Result.svelte:26`: `Server/Aphelyon.slnx` → `Source/Services/AphelyonServices.slnx`, `Server/scripts/start-all.bat` → `Source/Services/scripts/start-all.bat`.

- [ ] **Step 6: Docs.** `mkdocs.yml` comment `(Server/BUILD.md, Client/README.md, etc.)` → `(Source/Services/BUILD.md, etc.)`; `docs/components/server.md`: `include-markdown "../../Server/BUILD.md"` → `"../../Source/Services/BUILD.md"` and its other `Server/` mention. `Source/Services/BUILD.md`: every `Server/`, `Server\`, `Aphelyon.slnx` → the new paths/name; its vcpkg paragraph already says `-md`. `README.md`: the intro paragraph → "built on the Arcane engine — this repo IS the Arcane project (`Aphelyon.arcproj` at the root; the game module at `Source/Game/`, the three services at `Source/Services/`)"; `Server/…`/`Game/` mentions rewritten. `CLAUDE.md`: rewrite the **Repository Layout** tree to the File structure above; every `Server/` → `Source/Services/`, `Game/` → the root (`--project .`), `Aphelyon.slnx` → `AphelyonServices.slnx` where it means the services; the arcbuild lines → `--project .`; the Common-bullet and Build-section sentences keep their Plan 3 content; add one bullet under Common Pitfalls: `- **The two solutions:** `Aphelyon.slnx` at the root is the GAME module workspace (arcbuild generates and builds it); `Source/Services/AphelyonServices.slnx` is the services. They never build each other.`

- [ ] **Step 7: GREEN — no old path survives in the living set.** Re-run Step 1's grep → `0` (historical `docs/superpowers`, `docs/jira`, `docs/audits` are excluded by construction). Then `grep -rn "Aphelyon\.slnx" Source/Services scripts Jenkinsfile Tools/setup-wizard/src Tools/setup-wizard/src-tauri/src README.md CLAUDE.md` → `0`.

---

### Task 3: Build, test, boot — then the initial commit and the remote

- [ ] **Step 1: Services, both configs.** From `Source/Services`: regenerate (`../../ThirdParty/premake5/premake5.exe vs2026` → `AphelyonServices.slnx` appears); Rebuild Debug and Release per the ritual; expected exit 0, no LNK4098/LNK2019/LNK1181, `ArcaneCore.dll` staged beside all seven exes (`ls bin/Debug-windows-x86_64/*/ArcaneCore.dll | wc -l` → 7). Fast suites from the Debug bin dirs: CommonTests 76, AuthTests 12, CombatTests 1, all passed; record seeds. `(cd bin/Debug-windows-x86_64/Auth && ./Auth.exe --help >/dev/null; echo $?)` → 0.

- [ ] **Step 2: The game module from the root.** `"$ARCANE_SDK/bin/Debug-windows-x86_64-md/arcbuild/arcbuild.exe" build --project . --config Debug 2>&1 | tail -3` → exit 0, `Binaries/Aphelyon.dll` present; `grep -c "Source\\\\Game\\\\" Aphelyon.vcxproj` ≥ 1 (the glob followed `sourceDir`); `probe --project . --config Debug` → 0. Boot witness, headless: `(cd "$ARCANE_SDK/bin/Debug-windows-x86_64-md/ArcaneRuntime" && ./ArcaneRuntime.exe --project /d/dev/starworks/Aphelyon --headless --frames 30 --report /d/dev/starworks/Aphelyon/Saved/relocation-boot.json; echo "exit=$?")` → exit 0 and the report's `"module"`/census shows the module loaded (`grep -o '"exitReason":"[a-z-]*"' Saved/relocation-boot.json` → `frames-done` or the host's normal-completion reason — read `ArcaneClient/src/Arcane/Host/VerifyReport.hpp` for the vocabulary if unsure). Delete `Saved/relocation-boot.json` afterwards (`Saved/` is ignored anyway).

- [ ] **Step 3: `setup.ps1 -DryRun`.** `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/setup.ps1 -NonInteractive -DryRun -SkipDoctor -SkipDb -Workspaces server,game 2>&1 | tail -15` → every step names a path that exists (`Source\Services\…`, `--project <root>`); exit 0. Then `scripts\doctor.bat` via `cmd //c` with `_APH_NOPAUSE=1` → its overlay-triplet and ARCANE_SDK checks PASS.

- [ ] **Step 4: Init + the initial commit + the remote (no push).**
```bash
cd /d/dev/starworks/Aphelyon && git init -b main && git add -A && git status --short | wc -l   # a few hundred, no bin/ or Binaries/ (the ignore file works)
git commit -q -F - <<'EOF'
Aphelyon: the project repo -- root = the Arcane project (Aphelyon.arcproj), Source/Game (the game module) + Source/Services (Auth/Account/Combat), fresh history (predecessor: StarworksDev/Aphelyon 'Gacha' tree at 080cbb93, kept locally as D:\dev\starworks\Gacha for archaeology)

Layout per D:\dev\starworks\Arcane docs/research/2026-09-16-multiplayer-shape-and-project-layout.md s5 (L1-L3).
ThirdParty/ carries only the services' deps; engine deps come from $ARCANE_SDK. The services workspace is AphelyonServices.slnx.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Djskfodj1WnyR2g5v9V1Qc
EOF
git remote add origin https://github.com/StarworksDev/Aphelyon.git && git remote -v && git log --oneline
```
  Verify `git ls-files | wc -l` vs Gacha's tracked count minus the dropped ThirdParty dirs (`git -C /d/dev/starworks/Gacha ls-files | grep -v "^ThirdParty/\(Astra\|Manifold2D\|enkiTS\|freetype\|imgui\|imgui-node-editor\|msdfgen\|nvrhi\|tracy\|love2d\)/" | wc -l`) — equal, ±the generated/removed files you can name.

---

### Task 4: The archive dir, the record, the hand-off

- [ ] **Step 1: The hand-off sequence (M8, M9) — written, not run.** Nothing in the Gacha dir changes in this plan. The closeout carries the user's three commands, in this order: (1) from the archive, tag the old remote `main` so its history stays reachable on GitHub: `git -C D:\dev\starworks\Gacha push origin main:refs/tags/archive/gacha-2026-09-16`; (2) then sever the archive: `git -C D:\dev\starworks\Gacha remote remove origin`; (3) then publish the new history: `git -C D:\dev\starworks\Aphelyon push --force -u origin main` (Jenkins re-scans the multibranch job; the agent's `ARCANE_SDK` must be built for the Server stage).

- [ ] **Step 2: Arcane-side record.** Append the Closeout to this plan (commit sha of the new repo's initial commit; the build/test/boot/dry-run results with seeds; the dropped ThirdParty list; the four path-coupled files + the eight `..` rewrites; the in-flight Gacha files the user must port by hand: `Game/Source/TestComponent.{cpp,hpp}` → `Source/Game/`, `Game/Content/scenes/test.arcscene` diff → `Content/scenes/`; Setup.exe stale until CI (M7); the hand-off sequence: tag → remove remote → `git push --force -u origin main` → Jenkins re-scan → build windows-1's SDK if not current). Update `docs/research/2026-09-16-multiplayer-shape-and-project-layout.md` §5's last bullet from "Still open" to the decisions taken (fresh; Arcane pushed first; the new repo awaits the user's push). Commit in Arcane: `docs(plans): Aphelyon repo relocation -- executed; closeout` (+ trailers), do not push.

- [ ] **Step 3: Memory (orchestrator, not the implementer).** Copy the Claude project memory dir `…\.claude\projects\D--dev-starworks-Gacha\` to `…\D--dev-starworks-Aphelyon\` so the new working directory inherits it; update `reference_aphelyon_sibling_project` and `project_aphelyon_repo_relocation` to the executed state.

## Self-review

- Coverage: L1 (new dir, root = project, Gacha kept) → T1/T4; L2 (Game/Services) → T1; L3 (SDK field) → T1 Step 4 + T3 Step 2's vcxproj check; the four path-coupled files → T2 Steps 4-5; the eight `..` escapes → T2 Steps 2-3; M2-M7 each have a step; the user's push → T4 Step 1's sequence.
- Placeholders: none; the boot witness names the report file to read for the vocabulary rather than guessing an exit reason.
- Consistency: `AphelyonServices.slnx` is spelled identically in T2 Steps 2-6, T3 Step 1 and the CLAUDE.md bullet; `--project .` in the Jenkinsfile, setup.ps1 and T3.

<!-- CLOSEOUT -->
