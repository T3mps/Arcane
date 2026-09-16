# Core-DLL split, Plan 3 (Gacha `/MD`) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The three Aphelyon services (Auth/Account/Combat), `Common` and the four test exes link `ArcaneCore.dll` on the dynamic CRT — `/MD`, libpq rebuilt on `x64-windows-static-md` — through a Core-only consumer helper in the SDK's `build/arcane.lua`; the first compiled-Core consumer, `Arcane::Cli` replacing the three hand-rolled `argv` loops through one `Aphelyon::ServiceCli`, is the falsifiable proof the boundary works; Jenkins provisions the `-md` libpq and refuses to build the Server against an unbuilt SDK.

**Architecture:** This is the last step of the Core-DLL split's stated end shape (spec §1.2's linkage table: "`Auth`, `Account` and `Combat` are game services — they link Core for `Net`/`Crypto`/`Log`", §6) and of the user's 2026-09-13 ruling that ArcaneCore is *the code shared by server, client and editor*, with the servers as Arcane-supported consumers (Gacha `CLAUDE.md`, "Directional rule": generic capability is built IN Arcane; never fork parallel infrastructure server-side). Plans 1 and 2 left the services as header-only includers because nothing yet called a compiled Core symbol (spec §8 amendment, R11). Plan 3 pulls that trigger deliberately: the services' three copy-pasted `for (int i = 1; i < argc; …)` loops ARE parallel infrastructure (unknown flags silently ignored, `std::stoi` on the port), and `Arcane::Cli` — the typed parser every engine host already uses (`HostConfig.cpp`, `ArcaneServer/src/ServerConfig.cpp`) — is the engine tool that replaces them. The `/MD` flip exists for exactly one reason: a `Cli::Result` (three `std::unordered_map`s) is allocated inside `ArcaneCore.dll` and freed in the service's `main`; that needs one CRT heap, so every project in the Server workspace, its vendored Catch2/rapidcheck and its vcpkg libpq move to the dynamic CRT together. Nothing renders and nothing changes on the wire: both configs build with zero unresolved externals, the four suites stay green, and a new `[core-dll]` test proves the exe and the DLL carry the same CRT flavor by scanning their own import tables.

**Tech Stack:** premake5 (Gacha: bundled at `ThirdParty/premake5/`; Arcane: same binary), MSBuild (VS 18), vcpkg with the repo's `vcpkg-triplets/x64-windows-static-md.cmake` overlay (v143, dynamic CRT, static libs), Catch2 v3 + rapidcheck (Gacha-vendored), Postgres 16 in Docker (the Jenkins ephemeral recipe), the Arcane SDK checkout via `ARCANE_SDK`.

**Spec:** `docs/specs/2026-09-15-core-dll-split-design.md` (Arcane repo) — §1.2 hosts and linkage, §6 "What `ArcaneServer` is not" (the services link Core), §8 "Gacha services → `/MD`" and its 2026-09-16 amendment (Plan 3's trigger and first task), §11 "Plan 3 — Gacha repo, deferred", §13 R8/R11. Plan 2 (`docs/plans/2026-09-16-core-dll-split-plan2-gacha.md`) is the state inherited: the from-source Core project is gone, `IncludeDir["ArcaneCore"]` is kept, the CRT is `/MT`, libpq is on `x64-windows-static`.

## Global Constraints

- **Two repos, five commits.** Task 1 is an Arcane commit (`D:\dev\starworks\Arcane`, branch `main`, HEAD `f03d272d` at plan time); Tasks 2, 3 and 4 are Gacha commits (`D:\dev\starworks\Gacha`, branch `main`, HEAD `47e44626`); Task 5 is an Arcane docs commit. **Do not push either repo.** Trailers on every commit:
  ```
  Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01Djskfodj1WnyR2g5v9V1Qc
  ```
  Never stage the Arcane strays (`out.txt`, `ArcaneEditor/ArcaneEditor/`, `ArcaneAssetPipeline/ArcaneAs.*/`, `arcbuild/arcbuild/`) nor the Gacha in-flight files (`Game/Source/TestComponent.*` untracked, `Game/Content/scenes/test.arcscene` modified — leave both). Stage by explicit path. Generated `Server/**/*.vcxproj*`, `Server/Aphelyon.slnx`, `ThirdParty/*/*.vcxproj*` and `bin/` are gitignored; never force-add them.
- **`ARCANE_SDK`** must point at the Arcane checkout (`D:\dev\starworks\Arcane`) at or after Task 1's commit — the Gacha premake reads `build/arcane.lua` from it and the link reads `bin/<cfg>-windows-x86_64-md/ArcaneCore/ArcaneCore.lib`, which exists for Debug and Release today (engine HEAD `f03d272d`; verify with `ls "$ARCANE_SDK/bin/Debug-windows-x86_64-md/ArcaneCore/ArcaneCore.lib" "$ARCANE_SDK/bin/Release-windows-x86_64-md/ArcaneCore/ArcaneCore.lib"` before Task 3). Do NOT rebuild the engine in this plan; Task 1 touches no engine C++.
- **Gacha build ritual (bash, from `D:\dev\starworks\Gacha\Server`):** regenerate with `../ThirdParty/premake5/premake5.exe vs2026` (never `GenerateProjects.bat` under the Bash tool — it can hang); msbuild = `MSYS_NO_PATHCONV=1 "/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" Aphelyon.slnx -p:Configuration=<Debug|Release> -m -nologo -v:m -t:Rebuild > <log> 2>&1` (Task 3 flips the CRT of every project, so `-t:Rebuild` on both configs, once; later tasks may build incrementally), then `grep -E " error |warning LNK4098|Error\(s\)|Warning\(s\)" <log>`. Output lands in `Server/bin/<cfg>-windows-x86_64/<Project>/` — the Server's outputdir keeps its no-suffix name (ruling Q4).
- **The two falsifiers.** (1) `LNK2019`/`LNK2001` naming an `Arcane::` symbol after Task 3 = a Core header the services reach declares an `ARCANE_CORE_API` symbol the DLL does not export — STOP, report BLOCKED with the symbol; do not add a `.cpp` from the SDK to the Server's file list (that is the from-source build Plan 2 retired). (2) `warning LNK4098: defaultlib 'LIBCMT'`/`'LIBCMTD'` in either config = a static-CRT object survived the flip (a vcpkg lib from the old triplet, or a ThirdParty wrapper that did not read `THIRDPARTY_STATICRUNTIME`) — STOP and report which project; do not `/NODEFAULTLIB` around it.
- **Suites run FROM each exe's bin dir**, foreground, Debug: `CommonTests`, `AuthTests`, `CombatTests` (the Jenkins "Fast tests" set); `AccountTests` ONLY in Task 5 and ONLY against the EPHEMERAL CI database — never the dev database. Ephemeral recipe (from the Gacha repo root; the `export` line comes FIRST — the Plan 2 closeout's lesson):
  ```bash
  C="docker compose -p aphelyon_ci -f Server/docker-compose.yml -f ci/docker-compose.ci.yml"
  export POSTGRES_PORT=5433
  $C up -d --wait --build
  $C exec -T postgres psql -U aphelyon -d aphelyon -v ON_ERROR_STOP=1 -f /sql/schema.sql
  $C exec -T postgres psql -U aphelyon -d aphelyon -v ON_ERROR_STOP=1 -f /sql/seed.sql
  (cd Server/bin/Debug-windows-x86_64/AccountTests && POSTGRES_PORT=5433 APHELYON_TEST_DB_URL=postgresql://aphelyon:aphelyon@localhost:5433/aphelyon ./AccountTests.exe | grep -E "^Randomness|^test cases|^assertions|All tests passed|FAILED")
  $C down -v
  ```
  **HARD RULE: every compose command carries `-p aphelyon_ci`; a bare `down -v` destroys the dev database.** Suite output hygiene: `<Suite>.exe | grep -E "^Randomness|^test cases|^assertions|All tests passed|FAILED"`; never `cat` a suite log. AccountTests takes ~17 minutes.
- **Game module untouched.** `Game/` builds through `arcbuild` and is not part of this plan; do not run it. `Game/Aphelyon.arcproj` stays at ABI 31 — this plan changes no engine header, so no restamp.
- **The dev machine's vcpkg** is `VCPKG_ROOT=D:\dev\_shared\tools\vcpkg` (`installed/x64-windows-static-md/` holds SDL3 only; `installed/x64-windows-static/` holds libpq 16.9). Task 2's install is a from-source v143 build (~15 min, `--no-binarycaching`); run it FOREGROUND with a 20-minute timeout and never in parallel with a Server build.
- **Ledger:** `.superpowers/sdd/2026-09-16-core-dll-split-plan3-gacha/progress.md` in the Arcane repo (gitignored).

---

## Plan-time rulings

| # | Ruling | Why |
|---|---|---|
| Q1 | **Plan 3 = the `/MD` migration PLUS the first compiled-Core consumer, `Arcane::Cli` via `Aphelyon::ServiceCli`.** Not a build-only flip. | Plan 2's Q1: "a CRT flip with no DLL boundary has no acceptance test beyond 'the exes start'". The consumer makes the boundary real and testable: `Cli::Result`'s maps are allocated in the DLL and freed in the exe, which is the one thing `/MD` buys. The alternatives the spec named — Combat adopting `Runtime` (a whole phase, the Combat Sphere) and `Aphelyon::Logger` onto `Base/Log` (`Base/Log` has one logger, no categories, no file sink: an engine design change first) — are both larger than this plan and neither is "the smallest true consumer". `Cli` also honours the directional rule directly: three copy-pasted `argv` loops are the exact "parallel infrastructure server-side" it forbids. |
| Q2 | **The Core-consumer helper configures the CURRENT project (`arcane_core_consumer()`, called inside a `project` block) and adds only the SDK-private header-only include dirs (glm, Astra, enkiTS, Manifold2D, Mosaic); spdlog and nlohmann stay the consumer's own.** A sibling `arcane_core_stage_dll()` adds the `ArcaneCore.dll` postbuild copy for exes. | `arcane_game_module` DECLARES a project because a game module has one shape; the services have five different shapes (three exes with different link lines, a static lib, a test-exe factory) so the helper must compose into them. Gacha vendors its own spdlog 1.17.0 and nlohmann at the engine's versions; putting the SDK's copies on the same include path is two copies of a header-only library on one path — a trap the helper refuses to set (a consumer without them gets a clear missing-header error naming `spdlog/spdlog.h`, not a silent version split). |
| Q3 | **The helper applies the engine's flavor contract: `/utf-8`, `/arch:AVX2`, and per-config `runtime` + `ARCANE_DEBUG` / `ARCANE_RELEASE`+`NDEBUG` / `ARCANE_DIST`+`NDEBUG` — the same lines `arcane_game_module` carries.** | `ArcaneCore.dll` is compiled `/arch:AVX2` workspace-wide (Arcane `premake5.lua:27`), so a process that loads it already requires AVX2 hardware; giving the exe the same flag costs nothing and keeps inline-header codegen shared across the boundary identical (arcane.lua's stated reason). `NDEBUG` for Release matches the DLL's own Release flavor (inline header layouts under `#ifndef NDEBUG` must agree). **Consequence, stated:** the Server's Release configuration defines `NDEBUG` for the first time (measured 2026-09-16: `Server/Auth/Auth.vcxproj` Release `PreprocessorDefinitions` carries no `NDEBUG`), which makes the three services' `#ifdef NDEBUG` "refuse to start in a Release build" guards LIVE — they were dead code, and `--no-rate-limit` worked in Release builds. That is the behaviour those guards were written for; Task 4's `ServiceCli` registers the `[DEBUG]` flags under `#ifndef NDEBUG` so a Release build refuses them as unknown arguments (exit 2) instead of silently ignoring them. |
| Q4 | **The Server's `outputdir` stays `<cfg>-windows-x86_64` (no `-md` suffix).** | The suffix exists in Arcane because its ThirdParty wrappers were once shared with this workspace; since the 2026-08-11 extraction the two repos vendor separate copies, so nothing collides. Renaming would ripple into the Jenkinsfile, `start-all.bat`, `db-seed-accounts.bat`, `CLAUDE.md` and `BUILD.md` for no build-correctness gain. |
| Q5 | **libpq on the `-md` triplet is a NEW install beside the old one; nothing removes `x64-windows-static` libpq except `clean.bat --deep`.** The spec's "already built on the dev machine" is corrected in Task 5 (measured: `installed/x64-windows-static-md/lib/` holds `SDL3-static.lib` only). | Keeping the old flavor installed is free and lets `git checkout` of a pre-Plan-3 commit still build. `setup-vcpkg-deps.bat` removes-then-installs only the `-md` package (idempotent rebuild); `clean --deep` removes both flavors. |
| Q6 | **Jenkins: the Server FAILS (exit 1) when `ARCANE_SDK` names an unbuilt SDK; it does not skip.** The `-md` libpq guard path replaces the static one. | The Game stage skips because the engine "has its own pipeline" and a Game build is additive; the Server IS this pipeline's reason to exist, and `ARCANE_SDK` was already mandatory for `generate` (premake errors without it). An unbuilt SDK is an agent-provisioning defect that must be red, loudly, at the cheapest stage. |
| Q7 | **`ServiceCli` refuses what the hand parsers ignored: unknown arguments, a missing option value, a non-numeric port, a port outside 1..65535 → usage + exit 2.** `-v` stays registered (verbose is the default) so existing launch lines keep working; `-q` clears it. Auth's port keeps its protocol.json fallback via `Result::Supplied("port")` — the option's help default is 7777, which IS protocol.json's `default_port`. | Rule 3 from the automation arc (a silently inert flag is a defect) and `Cli`'s own contract. `start-all.bat` passes `%2 %3 %4` through unchanged, so a mistyped flag on a dev launch line now fails the launch instead of starting a service with the wrong settings. |
| Q8 | **Living docs are amended; history is not.** Gacha `CLAUDE.md`, `Server/BUILD.md`, the scripts' banner text, `Core/Api.hpp`'s comment and the spec change; Plan 1/2 closeouts, the extraction spec and past plans keep their wording. | Plan 2's Q3. |

---

## File structure

| File | Change | Responsibility |
|---|---|---|
| Arcane `build/arcane.lua` | Modify (T1): append `arcane_core_consumer()` + `arcane_core_stage_dll()` after `arcane_game_module` | The SDK's Core-only consumer recipe, lifted from `premake5.lua`'s `ArcaneServer` block. |
| Arcane `ArcaneCore/src/Arcane/Core/Api.hpp` | Modify (T1): the `ARCANE_CORE_STATIC` comment | Stops describing a from-source Gacha build that Plan 2 retired. |
| Arcane `docs/specs/2026-09-15-core-dll-split-design.md` | Modify (T1: §11 "Plan 3" paragraph, §8 amendment's last sentence, §13 R12; T5: status line) | The living definition of Plan 3. |
| Gacha `Server/scripts/setup-vcpkg-deps.bat` | Modify (T2): triplet `x64-windows-static` → `x64-windows-static-md` in the check, banner, remove and install lines | libpq on the dynamic-CRT triplet. |
| Gacha `Server/scripts/clean.bat` | Modify (T2): `--deep` removes both flavors | Deep clean covers the new install. |
| Gacha `Server/scripts/generate.bat`, `scripts/doctor.bat` | Modify (T2): the overlay-triplet presence checks name the `-md` file | The doctor/generate warnings check the triplet that is actually used. |
| Gacha `Jenkinsfile` | Modify (T2: the libpq guard path; T3: the built-SDK guard in `Generate`) | CI provisions the `-md` libpq; refuses an unbuilt SDK. |
| Gacha `Server/premake5.lua` | Modify (T3): triplet, `THIRDPARTY_STATICRUNTIME = "off"`, `include(arcane.lua)`, every `staticruntime "on"` → `arcane_core_consumer()`, `arcane_core_stage_dll()` on the seven exes, the header comment | The workspace links `ArcaneCore.dll` on `/MD`. |
| Gacha `Server/Common/tests/CoreDllBoundaryTest.cpp` | Create (T3) | `[core-dll]`: DLL loaded + matching flavor, exe is dynamic-CRT (import-table scan), a `Cli::Result` crosses the heap. |
| Gacha `Server/Common/src/Util/ServiceCli.hpp`, `.cpp` | Create (T4) | `Aphelyon::ServiceCliSpec` / `ServiceArgs` / `ParseServiceArgs` over `Arcane::Cli`. |
| Gacha `Server/Common/tests/ServiceCliTest.cpp` | Create (T4) | The parser's contract (Q7). |
| Gacha `Server/Auth/src/Main.cpp`, `Server/Account/src/Main.cpp`, `Server/Combat/src/Main.cpp` | Modify (T4): the `argv` loop + `PrintUsage` → `ParseServiceArgs` | The three consumers. |
| Gacha `CLAUDE.md`, `Server/BUILD.md` | Modify (T3) | Every sentence that says the Server links nothing / uses the static triplet. |
| Arcane `docs/plans/2026-09-16-core-dll-split-plan3-gacha.md` (this file) | Modify (T5: Closeout) | The record. |

---

### Task 1: The SDK's Core-consumer helper, the `Api.hpp` comment, the spec's Plan 3 definition (Arcane)

**Files:**
- Modify: Arcane `build/arcane.lua` (append after `arcane_game_module`'s closing `end`, line 113)
- Modify: Arcane `ArcaneCore/src/Arcane/Core/Api.hpp:9-18` (the comment block)
- Modify: Arcane `docs/specs/2026-09-15-core-dll-split-design.md` — §8 amendment (lines 391-396), §11 "Plan 3" paragraph (lines 478-481), §13 (append R12 after R11, line 516)

**Interfaces:**
- Consumes: `ARCANE_SDK`, `ARCANE_TP`, `ARCANE_BIN` — the three locals arcane.lua already defines above `arcane_game_module`.
- Produces: `arcane_core_consumer()` and `arcane_core_stage_dll()`, both global premake functions callable inside a `project` block; Task 3 calls them from `Server/premake5.lua`.

- [ ] **Step 1: RED — the helper does not exist and the spec still defers Plan 3.** From the Arcane repo root:

```bash
grep -c "arcane_core_consumer" build/arcane.lua
grep -n "Plan 3 — Gacha repo, deferred" docs/specs/2026-09-15-core-dll-split-design.md
grep -c "already built on the dev machine" docs/specs/2026-09-15-core-dll-split-design.md
grep -c "^| R12" docs/specs/2026-09-15-core-dll-split-design.md
```
Expected: `0`, one line (~:478), `1`, `0`.

- [ ] **Step 2: Append the two helpers to `build/arcane.lua`.** After the final `end` of `arcane_game_module` (the file's last line), append:

```lua

-- ============================================================================
-- Core-only consumers (Core-DLL split, spec docs/specs/2026-09-15-core-dll-split-
-- design.md s1.2 / s8, Plan 3). An external exe or static lib that needs the
-- headless engine -- Cli, Guid, Base/Log, Project, ... -- and NOTHING from the
-- presentation DLL links ArcaneCore.dll alone. Aphelyon's three services,
-- their Common lib and their test exes are the consumers; the recipe is lifted
-- from the engine's own ArcaneServer block in premake5.lua (the proven
-- Core-only host).
--
-- arcane_core_consumer() configures the CURRENT project: call it INSIDE a
-- `project` block, in place of `staticruntime`. It composes into whatever
-- shape the project has (ConsoleApp, StaticLib, a factory function) rather
-- than declaring one, because Core consumers do not share a shape the way
-- game modules do. It adds:
--   * staticruntime "off" -- /MD, one CRT heap across the DLL boundary (the
--     whole reason a consumer links a DLL instead of compiling Core from
--     source: objects allocated in ArcaneCore.dll are freed by the caller);
--   * the Core include root + the SDK-PRIVATE header-only deps a Core header
--     closure can reach (glm, Astra, enkiTS, Manifold2D, Mosaic). spdlog and
--     nlohmann are deliberately NOT added: a consumer vendors its own copies
--     (Aphelyon does, at the engine's versions), and two copies of a
--     header-only library on one include path is a version split waiting to
--     happen -- a consumer without them gets a clear missing-header error;
--   * the import lib + libdir;
--   * the engine's flavor contract, the same lines arcane_game_module
--     carries: /utf-8, /arch:AVX2 (ArcaneCore.dll is built AVX2 workspace-wide,
--     so the process already requires it -- matching keeps inline header
--     codegen identical across the boundary), and per-config runtime +
--     ARCANE_DEBUG / ARCANE_RELEASE+NDEBUG / ARCANE_DIST+NDEBUG so inline
--     header layouts under #ifndef NDEBUG agree with the DLL's.
-- It ends with `filter {}` so the caller's following lines are unfiltered.
--
-- arcane_core_stage_dll() adds the postbuild copy of ArcaneCore.dll beside an
-- exe's output (the dev bin layout; ArcaneRuntime's own postbuild is the
-- template). StaticLib consumers do not call it.
-- ============================================================================
function arcane_core_consumer()
    staticruntime "off"

    includedirs {
        ARCANE_SDK .. "/ArcaneCore/src",
        ARCANE_TP .. "/glm",
        ARCANE_TP .. "/Astra/include",
        ARCANE_TP .. "/enkiTS/src",
        ARCANE_TP .. "/Manifold2D/include",
        ARCANE_TP .. "/Mosaic/include",
    }

    libdirs { ARCANE_BIN .. "/ArcaneCore" }
    links   { "ArcaneCore" }

    filter "system:windows"
        buildoptions { "/utf-8", "/arch:AVX2" }
    filter { "system:linux or system:macosx", "architecture:x86_64" }
        buildoptions { "-mavx2", "-mfma" }

    filter "configurations:Debug"
        defines { "ARCANE_DEBUG" }
        runtime "Debug"
    filter "configurations:Release"
        defines { "ARCANE_RELEASE", "NDEBUG" }
        runtime "Release"
    filter "configurations:Dist"
        defines { "ARCANE_DIST", "NDEBUG" }
        runtime "Release"
    filter {}
end

function arcane_core_stage_dll()
    postbuildcommands {
        '{COPYFILE} "' .. ARCANE_BIN .. '/ArcaneCore/ArcaneCore.dll" "%{cfg.buildtarget.directory}/ArcaneCore.dll"',
    }
end
```

- [ ] **Step 3: Replace the `Api.hpp` comment's stale Gacha paragraph.** In `ArcaneCore/src/Arcane/Core/Api.hpp`, replace lines 9-18 (from `// ARCANE_CORE_STATIC comes FIRST and decorates nothing: the Gacha Server` through `// importing.`) with:

```cpp
// ARCANE_CORE_STATIC comes FIRST and decorates nothing: the branch for a
// consumer that compiles Core FROM SOURCE into a static lib (neither exporting
// nor importing -- dllimport on a declaration whose definition the same build
// compiles is MSVC C2491). No in-tree consumer takes it today: the Gacha Server
// did until Core-DLL split Plan 2 retired its from-source project (2026-09-16),
// and since Plan 3 it links ArcaneCore.dll through build/arcane.lua's
// arcane_core_consumer() like every other consumer. Kept because the branch is
// the documented shape for a future static build (a platform without shared
// libraries), and it costs one #if.
```

- [ ] **Step 4: Rewrite the spec's Plan 3 definition.** In `docs/specs/2026-09-15-core-dll-split-design.md`:

  (a) §8 amendment, last sentence (line ~391, from `Plan 3's first task is a Core-only consumer helper in` through `SDK binaries the way the Game stage already does.`) → replace with:

```markdown
Plan 3's first task is a Core-only consumer helper in
`build/arcane.lua`, lifted from `ArcaneServer`'s own premake block (the proven
Core-only host recipe), plus the Jenkins provisioning flip to the `-md` libpq
(a from-source vcpkg build — measured 2026-09-16 at Plan 3 planning, the
`x64-windows-static-md` install held SDL3 only, so the ~15 min build §12 costed
is real) and a Server CI stage that REFUSES an unbuilt SDK (the Server is the
Gacha pipeline's reason to exist, so it fails rather than skipping the way the
Game stage does).
```

  (b) §11, the paragraph starting `**Plan 3 — Gacha repo, deferred.**` (lines ~478-481) → replace with:

```markdown
**Plan 3 — Gacha repo** (`docs/plans/2026-09-16-core-dll-split-plan3-gacha.md`).
The `/MD` migration as §8 originally described it — `Common`, the three
services and the four test exes on the dynamic CRT, libpq rebuilt on
`x64-windows-static-md`, every exe linking `ArcaneCore.dll` through
`build/arcane.lua`'s `arcane_core_consumer()` — **plus the first compiled-Core
consumer that makes the boundary testable**: `Arcane::Cli` replaces the three
hand-rolled `argv` loops through one `Aphelyon::ServiceCli` (the directional
rule applied: the engine's typed parser, the services' vocabulary on top). The
trigger was pulled deliberately rather than waited for (R12). Green = both
configs build with no `Arcane::` unresolved external and no `LNK4098`, a
`[core-dll]` test proves exe and DLL share a CRT flavor by import-table scan,
the three fast suites and AccountTests (ephemeral DB) pass, and each service
answers `--help` with exit 0 and a bogus flag with exit 2. Starts after Plan 2;
merges last.
```

  (c) §13, append after the R11 row:

```markdown
| R12 | Wait for Plan 3's trigger (the first service needing a compiled Core symbol) or pull it | **Pull it, with `Arcane::Cli` as the consumer** | The two candidates the §8 amendment named are both bigger than the migration itself (Combat adopting `Runtime` is the Combat Sphere phase; `Aphelyon::Logger` onto `Base/Log` needs categories + a file sink in `Base/Log` first). The services' three copy-pasted `argv` loops are "parallel infrastructure server-side" — the exact thing the directional rule forbids — and the engine's parser is the smallest true consumer: a `Cli::Result` allocated in the DLL and freed in the exe is the one operation `/MD` exists for, so it is the acceptance test Plan 2's Q1 said a bare CRT flip lacks. |
```

- [ ] **Step 5: GREEN — re-run Step 1's greps.**

```bash
grep -c "arcane_core_consumer\|arcane_core_stage_dll" build/arcane.lua
grep -c "Plan 3 — Gacha repo, deferred" docs/specs/2026-09-15-core-dll-split-design.md
grep -c "already built on the dev machine" docs/specs/2026-09-15-core-dll-split-design.md
grep -c "^| R12" docs/specs/2026-09-15-core-dll-split-design.md
grep -c "until Plan 2" ArcaneCore/src/Arcane/Core/Api.hpp
```
Expected: `4` (two definitions + two mentions in the comment block; ≥ 2 is the bar), `0`, `0`, `1`, `0`.

- [ ] **Step 6: Syntax check the Lua without touching the engine solution.** The in-repo ReferenceProject includes arcane.lua and self-locates the SDK, so a premake run there parses the new functions (they are only defined, not called):

```bash
cd ReferenceProject && ../ThirdParty/premake5/premake5.exe vs2026 | tail -3 && cd ..
git status --short ReferenceProject
```
Expected: `Done (…ms).` with no Lua error; `git status` shows NO tracked change under `ReferenceProject/` (its generated files are gitignored — if anything tracked shows modified, `git checkout -- ReferenceProject` and report it).

- [ ] **Step 7: Commit (Arcane).**

```bash
git add build/arcane.lua ArcaneCore/src/Arcane/Core/Api.hpp docs/specs/2026-09-15-core-dll-split-design.md
git commit -m "feat(sdk): arcane_core_consumer() -- the Core-only consumer recipe for external exes/libs; spec s8/s11/R12 define Plan 3 (Gacha /MD + Arcane::Cli as the first compiled-Core consumer)"
```
(+ the two trailers.) Record the sha in the ledger: Task 3 cites it as the SDK floor.

---

### Task 2: libpq on `x64-windows-static-md` — the scripts, the Jenkins guard, the build (Gacha)

**Files:**
- Modify: Gacha `Server/scripts/setup-vcpkg-deps.bat:56-58, 69, 76, 83`
- Modify: Gacha `Server/scripts/clean.bat:70`
- Modify: Gacha `Server/scripts/generate.bat:47-49`
- Modify: Gacha `scripts/doctor.bat:111-117`
- Modify: Gacha `Jenkinsfile:40`

**Interfaces:**
- Consumes: `vcpkg-triplets/x64-windows-static-md.cmake` (exists, byte-identical to the Arcane repo's copy: `VCPKG_CRT_LINKAGE dynamic`, `VCPKG_LIBRARY_LINKAGE static`, `VCPKG_PLATFORM_TOOLSET v143`).
- Produces: `$VCPKG_ROOT/installed/x64-windows-static-md/lib/{libpq,libpgcommon,libpgport,libssl,libcrypto,zlib}.lib` and `debug/lib/{…,zlibd}.lib`, the exact names `Server/premake5.lua`'s `links` lines already use; Task 3 points `VCPKG_TRIPLET` at them.

- [ ] **Step 1: RED — the `-md` libpq is absent.**

```bash
ls "$VCPKG_ROOT/installed/x64-windows-static-md/lib/libpq.lib" 2>&1 | tail -1
grep -c "x64-windows-static-md" Server/scripts/setup-vcpkg-deps.bat Server/scripts/clean.bat Server/scripts/generate.bat scripts/doctor.bat Jenkinsfile
```
Expected: `No such file or directory`; every count `0`.

- [ ] **Step 2: `setup-vcpkg-deps.bat` — the triplet.** Four edits:

  Line 56-58 (the overlay check) becomes:
```bat
if not exist "%OVERLAY%\x64-windows-static-md.cmake" (
    echo ERROR: Overlay triplet not found at %OVERLAY%\x64-windows-static-md.cmake
    echo Ensure vcpkg-triplets\x64-windows-static-md.cmake exists in the repo.
```
  Line 69 becomes:
```bat
echo triplet: x64-windows-static-md (v143 pinned for ABI stability; dynamic CRT -- the services link ArcaneCore.dll, Core-DLL split Plan 3)
```
  Line 76 becomes:
```bat
"%VCPKG%" remove libpq:x64-windows-static-md --recurse 2>nul
```
  Line 83 becomes:
```bat
"%VCPKG%" install libpq:x64-windows-static-md --overlay-triplets="%OVERLAY%" --no-binarycaching
```
  Also replace the header comment's lines 4-5 (`REM Rebuilds vcpkg dependencies (libpq) from source using the v143 overlay` / `REM triplet. …`) so the first sentence reads: `REM Rebuilds vcpkg dependencies (libpq) from source using the v143 overlay` / `REM triplet x64-windows-static-md (dynamic CRT, static libs: the services link` / `REM ArcaneCore.dll on /MD since Core-DLL split Plan 3, 2026-09-16). We pin v143` — then continue the existing sentence `deliberately to keep the libpq ABI stable …` unchanged.

- [ ] **Step 3: `clean.bat --deep` removes both flavors.** Line 70 becomes:

```bat
    "%VCPKG%" remove libpqxx:x64-windows-static libpq:x64-windows-static libpq:x64-windows-static-md --recurse 2>nul
```

- [ ] **Step 4: `generate.bat` and `doctor.bat` check the triplet that is used.** `Server/scripts/generate.bat:47-49`:

```bat
if not exist "%OVERLAY%\x64-windows-static-md.cmake" (
    echo WARNING: Overlay triplet not found at %OVERLAY%\x64-windows-static-md.cmake
    echo libpq may be built with the wrong toolset.
```
  `scripts/doctor.bat:111-117`:
```bat
if exist "%REPO_ROOT%\vcpkg-triplets\x64-windows-static-md.cmake" (
    echo   [PASS] overlay triplet present
    call :wiz "overlay triplet" pass "vcpkg-triplets\x64-windows-static-md.cmake present"
) else (
    echo   [FAIL] vcpkg-triplets\x64-windows-static-md.cmake missing -- re-pull from git
    call :wiz "overlay triplet" fail "vcpkg-triplets\x64-windows-static-md.cmake missing -- re-pull from git"
    set FAIL=1
)
```

- [ ] **Step 5: Jenkinsfile — the provisioning guard looks for the `-md` libpq.** Line 40 becomes (one line):

```groovy
                        bat 'if not exist "%VCPKG_ROOT%\\installed\\x64-windows-static-md\\lib\\libpq.lib" ( powershell -NoProfile -ExecutionPolicy Bypass -File scripts\\setup.ps1 -NonInteractive -SkipDoctor -SkipDb -Workspaces server ) else ( echo [provision] libpq present -- skipping vcpkg build )'
```
  And in the stage comment above it (lines 31-37), change `if libpq is absent from the agent's vcpkg` to `if the -md libpq is absent from the agent's vcpkg (Core-DLL split Plan 3: the services link ArcaneCore.dll on the dynamic CRT, so libpq comes from the x64-windows-static-md overlay triplet)`.

- [ ] **Step 6: Build it — or confirm the orchestrator's background build.** The from-source build (libpq + openssl + zlib, v143, `--no-binarycaching`) runs ~15 min, longer than the Bash tool's 10-minute foreground cap, so the orchestrator started the SAME command the script runs (`vcpkg install libpq:x64-windows-static-md --overlay-triplets=<repo>/vcpkg-triplets --no-binarycaching`) in the background at plan time. First check whether it has landed:

```bash
ls "$VCPKG_ROOT/installed/x64-windows-static-md/lib/libpq.lib" 2>&1 | tail -1
```
If present → skip to Step 7 (the script edits above are still exercised: the Jenkins agent and any fresh machine run them). If absent → the background build is still running or failed; ask the orchestrator for its log (`scratchpad/vcpkg-md.log`, last line `exit=<code>`) rather than starting a second concurrent vcpkg install (two installs into one `VCPKG_ROOT` race on the same package dir). Only if the orchestrator reports it FAILED, run the script yourself in the background and poll `ls` every 60 s until the lib appears or the log ends: `cd Server && MSYS_NO_PATHCONV=1 _APH_NOPAUSE=1 cmd //c "scripts\\setup-vcpkg-deps.bat" > ../vcpkg-md.log 2>&1` (`vcpkg-md.log` is untracked scratch at the repo root — delete it after reading; never stage it).

- [ ] **Step 7: GREEN — the libs exist under the names premake links.**

```bash
for l in libpq libpgcommon libpgport libssl libcrypto zlib; do ls "$VCPKG_ROOT/installed/x64-windows-static-md/lib/$l.lib" >/dev/null && echo "ok $l"; done
ls "$VCPKG_ROOT/installed/x64-windows-static-md/debug/lib/zlibd.lib" "$VCPKG_ROOT/installed/x64-windows-static-md/debug/lib/libpq.lib"
ls "$VCPKG_ROOT/installed/x64-windows-static/lib/libpq.lib"
```
Expected: six `ok` lines; both debug libs listed; the OLD static libpq still present (Q5). Record the vcpkg-reported libpq version in the ledger (16.9 expected, matching `installed/vcpkg/info/libpq_16.9_x64-windows-static.list`'s sibling).

- [ ] **Step 8: Commit (Gacha).**

```bash
git add Server/scripts/setup-vcpkg-deps.bat Server/scripts/clean.bat Server/scripts/generate.bat scripts/doctor.bat Jenkinsfile
git commit -m "chore(server): libpq moves to the x64-windows-static-md overlay triplet (dynamic CRT) -- setup/clean/doctor/generate + the Jenkins provisioning guard (Core-DLL split Plan 3)"
```
(+ the two trailers.)

---

### Task 3: The workspace links `ArcaneCore.dll` on `/MD` — premake, the boundary test, the SDK guard, the docs (Gacha)

**Files:**
- Modify: Gacha `Server/premake5.lua` (lines 13-19 triplet; 35-66 the include + SDK block; 81-85 the ThirdParty includes; every project's `staticruntime "on"` at 100, 151, 211, 305, 361, 456; postbuild blocks at 195-199, 278-293, 347-350, 373-378; the `aphelyon_test_project` body; the header comment 1-11 and 47-57)
- Create: Gacha `Server/Common/tests/CoreDllBoundaryTest.cpp`
- Modify: Gacha `Jenkinsfile` (the `Generate` stage, lines 43-47)
- Modify: Gacha `CLAUDE.md:15-16, 98, 204, 272, 278` and the Common Pitfalls list; `Server/BUILD.md:98-102, 127`

**Interfaces:**
- Consumes: `arcane_core_consumer()` / `arcane_core_stage_dll()` from Task 1 (SDK at Task 1's sha or later); the `-md` libpq from Task 2.
- Produces: seven exes that load `ArcaneCore.dll` from their own bin dir; `Common.lib` on `/MD`; the `[core-dll]` tag in CommonTests. Task 4 builds on this workspace shape without further premake edits.

- [ ] **Step 1: RED — the SDK is built, the workspace is still `/MT`, and the boundary test does not exist.**

```bash
git -C "$ARCANE_SDK" log --oneline -1 -- build/arcane.lua
ls "$ARCANE_SDK/bin/Debug-windows-x86_64-md/ArcaneCore/ArcaneCore.lib" "$ARCANE_SDK/bin/Release-windows-x86_64-md/ArcaneCore/ArcaneCore.lib"
grep -c 'staticruntime "on"' Server/premake5.lua
ls Server/Common/tests/CoreDllBoundaryTest.cpp 2>&1 | tail -1
```
Expected: the first prints Task 1's sha; both `.lib`s listed; `6`; `No such file or directory`.

- [ ] **Step 2: `Server/premake5.lua` — the workspace-level edits.**

  (a) Lines 18-19 become:
```lua
-- Core-DLL split Plan 3 (2026-09-16): the dynamic-CRT flavor of the same
-- v143 overlay pin -- the services link ArcaneCore.dll (/MD), so every static
-- lib they link, libpq included, must be /MD too (one CRT heap per process).
VCPKG_TRIPLET = "x64-windows-static-md"
VCPKG_INSTALLED = VCPKG_ROOT .. "/installed/" .. VCPKG_TRIPLET
```

  (b) Replace lines 35-66 (from `    -- Include directories` through `    IncludeDir["ArcaneCore"] = ARCANE_SDK .. "/ArcaneCore/src"`) with:
```lua
    -- Include directories
    IncludeDir = {}
    IncludeDir["Common"] = "%{wks.location}/Common/src"
    -- Strangler extraction (M0, 2026-06-11): wire framing, Protocol,
    -- Types, Crypto, RateLimiter (+ deps Logger, LruCache) live in
    -- ArcaneCore/src/Arcane as namespace Arcane. Since the 2026-08-11 repo
    -- extraction those sources live in the engine SDK checkout
    -- (github.com/T3mps/Arcane), consumed via ARCANE_SDK -- the same
    -- env-var contract game modules use for build/arcane.lua. The old
    -- Common paths are re-export shims. See docs/superpowers/specs/
    -- 2026-08-11-arcane-repo-extraction-design.md.
    --
    -- Core-DLL split Plan 3 (2026-09-16): every project here is a Core-only
    -- CONSUMER of the SDK -- it links ArcaneCore.dll through the SDK's own
    -- recipe, arcane_core_consumer() (build/arcane.lua, lifted from the
    -- engine's ArcaneServer block), on the dynamic CRT so objects the DLL
    -- allocates (an Arcane::Cli::Result, for one) are freed by the caller
    -- on the same heap. Plan 2 had left the workspace header-only; the first
    -- compiled-Core call (Arcane::Cli behind Common/src/Util/ServiceCli) is
    -- what pulled the trigger (spec docs/specs/2026-09-15-core-dll-split-
    -- design.md s8 amendment, s11 Plan 3, R12). The SDK must be BUILT:
    -- $ARCANE_SDK/bin/<cfg>-windows-x86_64-md/ArcaneCore/ArcaneCore.lib is
    -- the import lib; LNK1181 "cannot open ArcaneCore.lib" means build
    -- Arcane.slnx first.
    include(os.getenv("ARCANE_SDK") and (os.getenv("ARCANE_SDK"):gsub("\\", "/") .. "/build/arcane.lua")
            or error("ARCANE_SDK environment variable is not set.\n" ..
                     "Point it at the Arcane engine repo root, e.g.:\n" ..
                     "  setx ARCANE_SDK D:\\dev\\starworks\\Arcane\n" ..
                     "then restart the terminal and re-generate."))
    IncludeDir["ArcaneCore"] = ARCANE_SDK .. "/ArcaneCore/src"
```
  (`ARCANE_SDK` is now the GLOBAL arcane.lua sets — the old `local ARCANE_SDK` block is gone with these lines.)

  (c) Immediately BEFORE `group "Dependencies"` (line ~81 after the edit above), insert:
```lua
-- The vendored Catch2/rapidcheck wrappers read this global (their default is
-- the historical static CRT). Core-DLL split Plan 3: the whole workspace is
-- /MD, so the test libs must be too -- a /MT Catch2.lib linked into a /MD
-- test exe is LNK4098 + two CRT heaps.
THIRDPARTY_STATICRUNTIME = "off"
```

- [ ] **Step 3: Every project becomes a Core consumer.** In each of the six places (`Common` :100, `Auth` :151, `Account` :211, `Combat` :305, `AccountTests` :361, `aphelyon_test_project` :456 — line numbers as of Step 1; find them with `grep -n 'staticruntime "on"'`), replace the line `    staticruntime "on"` with:

```lua
    arcane_core_consumer()   -- /MD + ArcaneCore include/libdir/link (Core-DLL split Plan 3)
```
  (inside `aphelyon_test_project` the indentation is 8 spaces.) Then add the DLL staging to the exes — every project EXCEPT `Common`:
  - `Auth` (:195-199), `Combat` (:347-350): inside the existing `postbuildcommands { … }` block add, as the first entry, nothing — instead add a separate call on the line before `postbuildcommands {`: `    arcane_core_stage_dll()`.
  - `Account` (:278): same — `    arcane_core_stage_dll()` on the line before its `postbuildcommands {`.
  - `AccountTests` (:373): same, before its `postbuildcommands {`.
  - `aphelyon_test_project`: after `        links { "Common", "Catch2", "rapidcheck" }` add `        arcane_core_stage_dll()`.
  (premake accumulates `postbuildcommands` across calls, so the helper's copy and the project's own data staging both run.)

- [ ] **Step 4: The header comment.** Lines 1-11 of `Server/premake5.lua`: after line 6 (`--   - libpq: Account Postgres persistence (linked by the sqlpp23 connector)`) insert:
```lua
--   - built on the x64-windows-static-md overlay triplet (v143, dynamic CRT):
--     the workspace is /MD since Core-DLL split Plan 3 (2026-09-16) because
--     every project links ArcaneCore.dll (see the ARCANE_SDK note below).
```
  And in the `Common` project's banner (lines ~87-93), replace `Protocol/Types/Logger/Crypto/` / `RateLimiter/TcpSocket/LruCache are ArcaneCore HEADERS (included` / `from $ARCANE_SDK, nothing linked -- see the IncludeDir note above);` with `Protocol/Types/Logger/Crypto/` / `RateLimiter/TcpSocket/LruCache are ArcaneCore headers; ServiceCli wraps` / `the COMPILED Arcane::Cli (ArcaneCore.dll, linked -- see the ARCANE_SDK note);`.

- [ ] **Step 5: Write the failing boundary test.** Create `Server/Common/tests/CoreDllBoundaryTest.cpp`:

```cpp
// Core-DLL split Plan 3 (2026-09-16): the structural proofs that this exe is a
// real ArcaneCore.dll consumer, not a header-only includer with a link line.
//   1. the DLL is loaded and is the flavor this exe was built as;
//   2. this exe links the DYNAMIC CRT, read from its own import table -- a /MT
//      exe imports no CRT DLL at all (ScanFileCrtFlavor -> Unknown), so this
//      is the /MD migration's falsifier, not a comment;
//   3. a Cli::Result allocated inside the DLL is destroyed here, in the exe --
//      the one operation that needs a single CRT heap, which is the whole
//      reason Plan 3 exists (spec s8).
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Engine.hpp>
#include <Arcane/Cli/Cli.hpp>
#include <Arcane/Plugin/Module.hpp>

#include <filesystem>
#include <string>

TEST_CASE("[core-dll] ArcaneCore.dll is loaded and reports this exe's flavor")
{
    const std::string info = Arcane::BuildInfo();
    REQUIRE_FALSE(info.empty());
#if defined(APHELYON_DEBUG)
    REQUIRE(info.find("[Debug]") != std::string::npos);
#else
    REQUIRE(info.find("[Release]") != std::string::npos);
#endif
}

TEST_CASE("[core-dll] this exe and ArcaneCore.dll link the same dynamic CRT")
{
    const std::filesystem::path self = std::filesystem::path(Arcane::ExecutablePathUtf8());
    REQUIRE_FALSE(self.empty());

    std::string exeImport, dllImport;
    const Arcane::CrtFlavor exe = Arcane::Module::ScanFileCrtFlavor(self, &exeImport);
    const Arcane::CrtFlavor dll = Arcane::Module::ScanFileCrtFlavor(self.parent_path() / "ArcaneCore.dll", &dllImport);

    INFO("exe CRT import: " << exeImport << "  dll CRT import: " << dllImport);
    // Unknown = no CRT DLL import = a static-CRT image. Both must be Known and equal.
#if defined(APHELYON_DEBUG)
    REQUIRE(exe == Arcane::CrtFlavor::Debug);
#else
    REQUIRE(exe == Arcane::CrtFlavor::Release);
#endif
    REQUIRE(dll == exe);
}

TEST_CASE("[core-dll] a Cli::Result allocated in the DLL is freed in this exe")
{
    Arcane::Cli cli{ "probe", "heap-crossing probe" };
    cli.Option("name", "", "a value long enough to live on the heap, not in SSO");
    const char* argv[] = { "probe", "--name", "0123456789012345678901234567890123456789" };
    const Arcane::Cli::Result r = cli.Parse(3, const_cast<char**>(argv));
    REQUIRE(r.ok);
    REQUIRE(r.Get("name").size() == 40);
    REQUIRE(r.Supplied("name"));
}   // r's three maps (nodes allocated by Cli::Parse inside ArcaneCore.dll) are
    // destroyed here by this exe's inline destructor -- one heap, by construction.
```

- [ ] **Step 6: Regenerate + Rebuild both configs.** From `Server/`:

```bash
../ThirdParty/premake5/premake5.exe vs2026 | tail -2
MSYS_NO_PATHCONV=1 "/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" Aphelyon.slnx -p:Configuration=Debug -m -nologo -v:m -t:Rebuild > ../build-debug-t3.log 2>&1; echo "debug exit=$?"
MSYS_NO_PATHCONV=1 "/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" Aphelyon.slnx -p:Configuration=Release -m -nologo -v:m -t:Rebuild > ../build-release-t3.log 2>&1; echo "release exit=$?"
grep -E " error |LNK4098|LNK2019|LNK2001|LNK1181|Error\(s\)|Warning\(s\)" ../build-debug-t3.log ../build-release-t3.log | head -40
```
Expected: both `exit=0`; the grep prints NOTHING (minimal verbosity emits no summary line on a clean build) or only `0 Error(s)`. Any `LNK4098` → falsifier 2; any `LNK2019 … Arcane::` → falsifier 1; `LNK1181 cannot open ArcaneCore.lib` → the SDK is not built for that config (Global Constraints). The two logs are untracked scratch at the repo root — delete after reading.

- [ ] **Step 7: The DLL is beside every exe and every image is dynamic-CRT.**

```bash
for p in Auth Account Combat CommonTests AuthTests CombatTests AccountTests; do for c in Debug Release; do test -f "bin/$c-windows-x86_64/$p/ArcaneCore.dll" && echo "staged $c/$p" || echo "MISSING $c/$p"; done; done
grep -a -c "MSVCP140D.dll" bin/Debug-windows-x86_64/Auth/Auth.exe; grep -a -c "MSVCP140.dll" bin/Release-windows-x86_64/Auth/Auth.exe
grep -a -c "ArcaneCore.dll" bin/Debug-windows-x86_64/Auth/Auth.exe
```
Expected: 14 `staged` lines, no `MISSING`; the three counts all ≥ 1 (Debug imports the debug CRT DLL, Release the release one, and Auth.exe names `ArcaneCore.dll` in its import table — wait: Task 3 adds no Core CALL to Auth.exe yet, so the linker may drop the import; if the third count is `0` that is expected here and Task 4 re-checks it after `ServiceCli` lands. The CommonTests exe DOES call into the DLL — `grep -a -c "ArcaneCore.dll" bin/Debug-windows-x86_64/CommonTests/CommonTests.exe` must be ≥ 1.)

- [ ] **Step 8: GREEN — the boundary test and the fast suites.** From each bin dir:

```bash
(cd bin/Debug-windows-x86_64/CommonTests && ./CommonTests.exe "[core-dll]" | grep -E "^Randomness|^test cases|^assertions|All tests passed|FAILED")
(cd bin/Release-windows-x86_64/CommonTests && ./CommonTests.exe "[core-dll]" | grep -E "^Randomness|^test cases|^assertions|All tests passed|FAILED")
(cd bin/Debug-windows-x86_64/CommonTests && ./CommonTests.exe | grep -E "^Randomness|^test cases|^assertions|All tests passed|FAILED")
(cd bin/Debug-windows-x86_64/AuthTests   && ./AuthTests.exe   | grep -E "^Randomness|^test cases|^assertions|All tests passed|FAILED")
(cd bin/Debug-windows-x86_64/CombatTests && ./CombatTests.exe | grep -E "^Randomness|^test cases|^assertions|All tests passed|FAILED")
```
Expected: `[core-dll]` = 3 test cases, all passed, in BOTH configs (the Release run is the one that proves `NDEBUG`+`[Release]` agree); the full CommonTests = 69 test cases (66 + 3), AuthTests 12, CombatTests 1, all `All tests passed`. Record every seed in the ledger.

- [ ] **Step 9: Jenkins refuses an unbuilt SDK.** In `Jenkinsfile`, the `Generate` stage (lines 43-47) becomes:

```groovy
                stage('Generate') {
                    // Core-DLL split Plan 3: the Server links ArcaneCore.dll, so
                    // ARCANE_SDK must name a BUILT engine checkout (the import
                    // lib lives in the SDK's own bin/). Unlike the Game stage
                    // below, an unbuilt SDK FAILS here rather than skipping:
                    // the Server is this pipeline's reason to exist, and
                    // ARCANE_SDK was already mandatory for generate.
                    steps {
                        bat 'if not exist "%ARCANE_SDK%\\bin\\Debug-windows-x86_64-md\\ArcaneCore\\ArcaneCore.lib"   ( echo [server] ARCANE_SDK must name a BUILT engine checkout: Debug ArcaneCore.lib is missing -- build Arcane.slnx on this agent & exit /b 1 )'
                        bat 'if not exist "%ARCANE_SDK%\\bin\\Release-windows-x86_64-md\\ArcaneCore\\ArcaneCore.lib" ( echo [server] ARCANE_SDK must name a BUILT engine checkout: Release ArcaneCore.lib is missing -- build Arcane.slnx on this agent & exit /b 1 )'
                        bat 'cd Server && call GenerateProjects.bat'
                    }
                }
```
  And the file's header comment (lines 5-9): after `through the SDK's arcbuild.exe, on an agent that carries a BUILT` / `ARCANE_SDK checkout; it skips elsewhere.` append one line: `// The Server itself links ArcaneCore.dll (Core-DLL split Plan 3), so the` / `// Generate stage REFUSES an unbuilt SDK -- it never skips.`

- [ ] **Step 10: Docs.** Gacha `CLAUDE.md`:
  - Lines 15-16 (the tree comment): `├── vcpkg-triplets/ # vcpkg overlay triplets (v143 toolset; static-md = the` / `│                   #   dynamic-CRT flavor the Server + engine both build on)`.
  - Line 98 (the "Re-generate solution" paragraph): replace `-- the Server includes `$ARCANE_SDK/ArcaneCore/src` headers only; nothing compiles or links ArcaneCore (Core-DLL split Plan 2, 2026-09-16).` with `-- the Server links `ArcaneCore.dll` through the SDK's `build/arcane.lua` (`arcane_core_consumer()`), so the SDK must be BUILT: `$ARCANE_SDK/bin/<cfg>-windows-x86_64-md/ArcaneCore/ArcaneCore.lib` is the import lib (Core-DLL split Plan 3, 2026-09-16). Every project is `/MD`.`
  - Line 204 (the Common bullet): replace `which includes **ArcaneCore's header-only Net/Crypto/Util layer** from `$ARCANE_SDK/ArcaneCore/src` (nothing linked; the first compiled-Core call is Plan 3's trigger).` with `which consumes **ArcaneCore** from `$ARCANE_SDK`: the header-only Net/Crypto/Util layer plus the compiled `Arcane::Cli` behind `Common/src/Util/ServiceCli` — every exe links `ArcaneCore.dll` on the dynamic CRT (Core-DLL split Plan 3, 2026-09-16).`
  - Line 272 (the table row): `| libpq | vcpkg (v143 overlay triplet, dynamic CRT) | `vcpkg/installed/x64-windows-static-md/` |`.
  - Line 278: `The overlay triplet (`vcpkg-triplets/x64-windows-static-md.cmake` at the repo root) pins the v143 toolset and the dynamic CRT for vcpkg builds …` (rest of the sentence unchanged).
  - Common Pitfalls: add a bullet after the `ARCANE_SDK` one: `- **`LNK1181: cannot open input file 'ArcaneCore.lib'`** (or a missing `ArcaneCore.dll` beside a service exe): `ARCANE_SDK` names an UNBUILT engine checkout. Build `Arcane.slnx` (Debug and Release) there first; the Server links the DLL and stages it at post-build.`
  - The "Build" section (line ~86, `# From Server/`): add a line above the msbuild lines: `# ARCANE_SDK must be a BUILT engine checkout (both configs) -- the Server links ArcaneCore.dll`.

  `Server/BUILD.md`:
  - Line 98 row: `| **vcpkg** | libpq | Installed to `vcpkg/installed/x64-windows-static-md/` (dynamic CRT), linked via paths in `premake5.lua` |`.
  - Line 102: `The `vcpkg-triplets/x64-windows-static-md.cmake` file pins the v143 toolset and the dynamic CRT (the services link `ArcaneCore.dll`, so the whole workspace is `/MD`). This prevents ABI mismatches …` (rest unchanged).
  - Line 127: `vcpkg install <package>:x64-windows-static-md --overlay-triplets="vcpkg-triplets" --no-binarycaching`.

- [ ] **Step 11: Commit (Gacha).**

```bash
git add Server/premake5.lua Server/Common/tests/CoreDllBoundaryTest.cpp Jenkinsfile CLAUDE.md Server/BUILD.md
git commit -m "feat(server): the workspace links ArcaneCore.dll on /MD via arcane_core_consumer() -- libpq on the -md triplet, Catch2/rapidcheck dynamic-CRT, [core-dll] boundary proofs, Jenkins refuses an unbuilt SDK (Core-DLL split Plan 3)"
```
(+ the two trailers.)

---

### Task 4: `Aphelyon::ServiceCli` over `Arcane::Cli` — the three mains adopt it (Gacha)

**Files:**
- Create: Gacha `Server/Common/src/Util/ServiceCli.hpp`, `Server/Common/src/Util/ServiceCli.cpp`
- Create: Gacha `Server/Common/tests/ServiceCliTest.cpp`
- Modify: Gacha `Server/Auth/src/Main.cpp:1-11, 25-36, 48-83` · `Server/Account/src/Main.cpp:1-19, 33-44, 106-146` · `Server/Combat/src/Main.cpp:1-9, 23-34, 42-62`

**Interfaces:**
- Consumes: `Arcane::Cli` (`<Arcane/Cli/Cli.hpp>`: `Option(name, default, help)`, `Flag(name, help)`, `Builder::Short(char)`, `Builder::Type(CliType)`, `Parse(argc, argv) -> Result`, `Result::{ok, exitCode, Flag, Get, GetAs<T>, Supplied}`); `Aphelyon::g_rateLimitingEnabled` (`"Net/RateLimiter.hpp"`, a `using` of `Arcane::g_rateLimitingEnabled`); `Aphelyon::g_authIPBindingEnabled` (`"AuthServer.hpp"`).
- Produces:
  ```cpp
  namespace Aphelyon {
      struct ServiceCliSpec { std::string name, description; std::uint16_t defaultPort;
                              std::optional<std::uint16_t> defaultUdpPort; bool offersSeedAccounts; bool offersNoIpBind; };
      struct ServiceArgs    { bool ok; int exitCode; std::uint16_t port; bool portSupplied; std::uint16_t udpPort;
                              bool verbose; bool noRateLimit; bool noIpBind; bool seedAccounts; };
      ServiceArgs ParseServiceArgs(const ServiceCliSpec& spec, int argc, char** argv);
  }
  ```

- [ ] **Step 1: Write the failing test.** Create `Server/Common/tests/ServiceCliTest.cpp`:

```cpp
// Core-DLL split Plan 3 (2026-09-16): Aphelyon::ServiceCli is the services'
// shared argv contract over Arcane::Cli (the engine's typed parser -- the same
// one ArcaneRuntime/ArcaneServer/arcbuild use). What the three hand-rolled
// loops it replaced silently ignored is now REFUSED (plan ruling Q7): unknown
// arguments, a missing value, a non-numeric or out-of-range port -> usage +
// exit 2. --help/-h -> exit 0. Refusals print usage to stdout/stderr; that
// noise in the suite log is expected.
#include <catch2/catch_test_macros.hpp>

#include "Util/ServiceCli.hpp"

#include <vector>

using Aphelyon::ParseServiceArgs;
using Aphelyon::ServiceArgs;
using Aphelyon::ServiceCliSpec;

namespace
{
    // argv builder: a stable char** over string literals.
    struct Argv
    {
        std::vector<char*> v;
        explicit Argv(std::initializer_list<const char*> args)
        {
            v.push_back(const_cast<char*>("svc.exe"));
            for (const char* a : args) v.push_back(const_cast<char*>(a));
        }
        int    argc() const { return static_cast<int>(v.size()); }
        char** argv()       { return v.data(); }
    };

    ServiceCliSpec Auth()    { return { .name = "Auth",    .description = "test", .defaultPort = 7777, .defaultUdpPort = std::nullopt, .offersSeedAccounts = false, .offersNoIpBind = true }; }
    ServiceCliSpec Account() { return { .name = "Account", .description = "test", .defaultPort = 7771, .defaultUdpPort = std::nullopt, .offersSeedAccounts = true,  .offersNoIpBind = false }; }
    ServiceCliSpec Combat()  { return { .name = "Combat",  .description = "test", .defaultPort = 7772, .defaultUdpPort = 7778,         .offersSeedAccounts = false, .offersNoIpBind = false }; }

    ServiceArgs Parse(const ServiceCliSpec& s, std::initializer_list<const char*> args)
    {
        Argv a(args);
        return ParseServiceArgs(s, a.argc(), a.argv());
    }
}

TEST_CASE("[service-cli] no arguments yields the spec's defaults, verbose on")
{
    const ServiceArgs a = Parse(Combat(), {});
    REQUIRE(a.ok);
    REQUIRE(a.exitCode == 0);
    REQUIRE(a.port == 7772);
    REQUIRE_FALSE(a.portSupplied);
    REQUIRE(a.udpPort == 7778);
    REQUIRE(a.verbose);
    REQUIRE_FALSE(a.noRateLimit);
    REQUIRE_FALSE(a.noIpBind);
    REQUIRE_FALSE(a.seedAccounts);
}

TEST_CASE("[service-cli] -p / --port / --port=N override and mark the port supplied")
{
    SECTION("short")  { const auto a = Parse(Auth(), { "-p", "7000" });    REQUIRE(a.ok); REQUIRE(a.port == 7000); REQUIRE(a.portSupplied); }
    SECTION("long")   { const auto a = Parse(Auth(), { "--port", "7001" }); REQUIRE(a.ok); REQUIRE(a.port == 7001); REQUIRE(a.portSupplied); }
    SECTION("inline") { const auto a = Parse(Auth(), { "--port=7002" });    REQUIRE(a.ok); REQUIRE(a.port == 7002); REQUIRE(a.portSupplied); }
}

TEST_CASE("[service-cli] -q clears verbose; -v is accepted and changes nothing")
{
    REQUIRE_FALSE(Parse(Auth(), { "-q" }).verbose);
    REQUIRE_FALSE(Parse(Auth(), { "--quiet" }).verbose);
    REQUIRE(Parse(Auth(), { "-v" }).verbose);
    REQUIRE_FALSE(Parse(Auth(), { "-v", "-q" }).verbose);
}

TEST_CASE("[service-cli] --help and -h exit 0 without ok")
{
    for (const char* h : { "--help", "-h" })
    {
        const auto a = Parse(Auth(), { h });
        REQUIRE_FALSE(a.ok);
        REQUIRE(a.exitCode == 0);
    }
}

TEST_CASE("[service-cli] what the old loops ignored is refused with exit 2")
{
    SECTION("unknown argument")      { const auto a = Parse(Auth(), { "--bogus" });      REQUIRE_FALSE(a.ok); REQUIRE(a.exitCode == 2); }
    SECTION("missing value")         { const auto a = Parse(Auth(), { "-p" });           REQUIRE_FALSE(a.ok); REQUIRE(a.exitCode == 2); }
    SECTION("non-numeric port")      { const auto a = Parse(Auth(), { "-p", "abc" });    REQUIRE_FALSE(a.ok); REQUIRE(a.exitCode == 2); }
    SECTION("port 0")                { const auto a = Parse(Auth(), { "-p", "0" });      REQUIRE_FALSE(a.ok); REQUIRE(a.exitCode == 2); }
    SECTION("port above 65535")      { const auto a = Parse(Auth(), { "-p", "70000" });  REQUIRE_FALSE(a.ok); REQUIRE(a.exitCode == 2); }
    SECTION("udp port out of range") { const auto a = Parse(Combat(), { "-u", "65536" }); REQUIRE_FALSE(a.ok); REQUIRE(a.exitCode == 2); }
}

TEST_CASE("[service-cli] per-service options exist only where the spec offers them")
{
    SECTION("-u is Combat's")
    {
        const auto c = Parse(Combat(), { "-u", "8000" }); REQUIRE(c.ok); REQUIRE(c.udpPort == 8000);
        const auto a = Parse(Auth(),   { "-u", "8000" }); REQUIRE_FALSE(a.ok); REQUIRE(a.exitCode == 2);
    }
    SECTION("--seed-accounts is Account's")
    {
        const auto acc = Parse(Account(), { "--seed-accounts" }); REQUIRE(acc.ok); REQUIRE(acc.seedAccounts);
        const auto a   = Parse(Auth(),    { "--seed-accounts" }); REQUIRE_FALSE(a.ok); REQUIRE(a.exitCode == 2);
    }
}

#ifndef NDEBUG
TEST_CASE("[service-cli] the [DEBUG] toggles are registered in a non-NDEBUG build")
{
    const auto a = Parse(Auth(), { "--no-rate-limit", "--no-ip-bind" });
    REQUIRE(a.ok); REQUIRE(a.noRateLimit); REQUIRE(a.noIpBind);
    const auto c = Parse(Combat(), { "--no-ip-bind" });   // Auth's alone
    REQUIRE_FALSE(c.ok); REQUIRE(c.exitCode == 2);
}
#else
TEST_CASE("[service-cli] the [DEBUG] toggles are unknown arguments in an NDEBUG build")
{
    const auto a = Parse(Auth(), { "--no-rate-limit" });
    REQUIRE_FALSE(a.ok); REQUIRE(a.exitCode == 2);
    const auto b = Parse(Auth(), { "--no-ip-bind" });
    REQUIRE_FALSE(b.ok); REQUIRE(b.exitCode == 2);
}
#endif
```

- [ ] **Step 2: Run it to verify it fails to compile.** From `Server/`: `../ThirdParty/premake5/premake5.exe vs2026 | tail -1` (the glob picks the new file up), then build CommonTests only:

```bash
MSYS_NO_PATHCONV=1 "/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" Aphelyon.slnx -p:Configuration=Debug -m -nologo -v:m -t:CommonTests > ../build-t4-red.log 2>&1; echo "exit=$?"; grep -E "error C1083|ServiceCli" ../build-t4-red.log | head -3
```
Expected: `exit=1` with `error C1083: Cannot open include file: 'Util/ServiceCli.hpp'`.

- [ ] **Step 3: Write `ServiceCli.hpp`.** Create `Server/Common/src/Util/ServiceCli.hpp`:

```cpp
#pragma once

// Aphelyon service command line (Core-DLL split Plan 3, 2026-09-16).
//
// The three services used to carry three copy-pasted `for (i = 1; i < argc; …)`
// loops -- parallel infrastructure server-side, the thing the directional rule
// forbids -- that silently ignored unknown flags and std::stoi'd the port.
// This is the SHARED contract over the engine's typed parser, Arcane::Cli
// (ArcaneCore.dll -- the same parser ArcaneRuntime/ArcaneServer/arcbuild use):
// the engine supplies the mechanism, this header supplies the services'
// vocabulary (which options exist, their defaults, what they mean).
//
// Contract (plan ruling Q7): unknown arguments, a missing value, a non-numeric
// port or a port outside 1..65535 are REFUSED -- usage printed, exitCode 2.
// --help / -h prints usage, exitCode 0. Either way `ok` is false and main
// returns `exitCode`. -v is accepted for symmetry with the old launch lines
// (verbose is the default); -q clears it. The [DEBUG] toggles are registered
// only in a non-NDEBUG build, so a Release build refuses them as unknown
// arguments instead of silently ignoring them (the old loops ignored them AND
// the Release refusal guards in main were dead code: the Server's Release
// configuration only defines NDEBUG since Plan 3 -- see the plan's Q3).
#include <cstdint>
#include <optional>
#include <string>

namespace Aphelyon
{
    // What a service registers beyond the shared set (-p/--port, -v, -q,
    // --help, and --no-rate-limit in non-NDEBUG builds).
    struct ServiceCliSpec
    {
        std::string name;                              // "Auth" -- the usage banner reads "Aphelyon Auth"
        std::string description;                       // one line under the banner
        std::uint16_t defaultPort = 0;                 // --port's default (shown in usage)
        std::optional<std::uint16_t> defaultUdpPort;   // set => -u/--udp is registered (Combat)
        bool offersSeedAccounts = false;               // => --seed-accounts (Account)
        bool offersNoIpBind = false;                   // => --no-ip-bind, non-NDEBUG only (Auth)
    };

    struct ServiceArgs
    {
        bool ok = false;               // false => return exitCode from main
        int  exitCode = 0;             // 0 for --help, 2 for a refused line
        std::uint16_t port = 0;        // resolved: supplied or the spec's default
        bool portSupplied = false;     // Auth: false => take protocol.json's default_port instead
        std::uint16_t udpPort = 0;     // Combat only (0 when the spec has no UDP port)
        bool verbose = true;           // -q clears it
        bool noRateLimit = false;      // [DEBUG] --no-rate-limit
        bool noIpBind = false;         // [DEBUG] --no-ip-bind (Auth)
        bool seedAccounts = false;     // --seed-accounts (Account)
    };

    ServiceArgs ParseServiceArgs(const ServiceCliSpec& spec, int argc, char** argv);
}
```

- [ ] **Step 4: Write `ServiceCli.cpp`.** Create `Server/Common/src/Util/ServiceCli.cpp`:

```cpp
#include "Util/ServiceCli.hpp"

#include <Arcane/Cli/Cli.hpp>

#include <cstdio>
#include <string>

namespace Aphelyon
{
    namespace
    {
        // Arcane::Cli's Uint type has already refused non-numeric text; this
        // adds the port range. A refusal prints the reason the way Cli does
        // (stderr, then usage), so the operator sees one vocabulary.
        bool ResolvePort(const Arcane::Cli& cli, const Arcane::Cli::Result& r, const char* name, std::uint16_t& out)
        {
            const std::uint64_t v = r.GetAs<std::uint64_t>(name);
            if (v < 1 || v > 65535)
            {
                std::fprintf(stderr, "error: '--%s' wants a port in 1..65535, got '%s'\n", name, r.Get(name).c_str());
                cli.PrintUsage();
                return false;
            }
            out = static_cast<std::uint16_t>(v);
            return true;
        }
    }

    ServiceArgs ParseServiceArgs(const ServiceCliSpec& spec, int argc, char** argv)
    {
        Arcane::Cli cli{ "Aphelyon " + spec.name, spec.description };
        cli.Option("port", std::to_string(spec.defaultPort), "client TCP port").Short('p').Type(Arcane::CliType::Uint);
        if (spec.defaultUdpPort)
            cli.Option("udp", std::to_string(*spec.defaultUdpPort), "gameplay UDP port").Short('u').Type(Arcane::CliType::Uint);
        cli.Flag("verbose", "debug-level console logging (the default; accepted for symmetry with --quiet)").Short('v');
        cli.Flag("quiet",   "info-level console logging").Short('q');
        if (spec.offersSeedAccounts)
            cli.Flag("seed-accounts", "register the 10 dev test accounts (test, test1..test9) and exit");
#ifndef NDEBUG
        cli.Flag("no-rate-limit", "[DEBUG] disable all rate limiters");
        if (spec.offersNoIpBind)
            cli.Flag("no-ip-bind", "[DEBUG] disable session IP binding (laptop dev convenience)");
#endif

        const Arcane::Cli::Result r = cli.Parse(argc, argv);

        ServiceArgs a;
        if (!r.ok)
        {
            a.exitCode = r.exitCode;   // 0 (--help) or 2 (refused); usage already printed by Cli
            return a;
        }

        if (!ResolvePort(cli, r, "port", a.port)) { a.exitCode = 2; return a; }
        a.portSupplied = r.Supplied("port");
        if (spec.defaultUdpPort && !ResolvePort(cli, r, "udp", a.udpPort)) { a.exitCode = 2; return a; }

        a.verbose      = !r.Flag("quiet");
        a.seedAccounts = r.Flag("seed-accounts");   // false when not registered
        a.noRateLimit  = r.Flag("no-rate-limit");   // false when not registered (NDEBUG)
        a.noIpBind     = r.Flag("no-ip-bind");
        a.ok = true;
        return a;
    }
}
```

- [ ] **Step 5: Build CommonTests and run the new tag — GREEN.**

```bash
MSYS_NO_PATHCONV=1 "/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" Aphelyon.slnx -p:Configuration=Debug -m -nologo -v:m -t:CommonTests > ../build-t4-green.log 2>&1; echo "exit=$?"; grep -E " error |Error\(s\)" ../build-t4-green.log | head
(cd bin/Debug-windows-x86_64/CommonTests && ./CommonTests.exe "[service-cli]" | grep -E "^Randomness|^test cases|^assertions|All tests passed|FAILED")
```
Expected: `exit=0`; 7 test cases, `All tests passed`.

- [ ] **Step 6: Auth's `Main.cpp` adopts it.** Replace lines 1-11's includes so they read (keep the `#ifdef APHELYON_PLATFORM_WINDOWS` block and everything from `Aphelyon::AuthServer* g_server` on, except as edited below):

```cpp
#include <csignal>
#include <filesystem>
#include <string>
#include "AuthServer.hpp"
#include "Net/InternalRpcAuth.hpp"
#include "Util/Logger.hpp"
#include "Util/ServiceCli.hpp"
#include "Net/Protocol.hpp"
#include "Net/RateLimiter.hpp"
// AuthServer.hpp defines Aphelyon::g_authIPBindingEnabled, the H6 toggle.
```
  Delete the whole `void PrintUsage(const char* programName) { … }` function (lines 25-36). Replace lines 48-83 (from `    uint16_t port = 0;` through the closing `    }` of the `for` loop) with:

```cpp
    // Core-DLL split Plan 3: the engine's typed parser (Arcane::Cli, in
    // ArcaneCore.dll) behind the services' shared vocabulary. A refused line
    // (unknown flag, bad port) exits 2 here; --help exits 0.
    const Aphelyon::ServiceArgs args = Aphelyon::ParseServiceArgs(
        Aphelyon::ServiceCliSpec{
            .name = "Auth",
            .description = "authentication service: Login/Register/sessions (client TCP 7777, internal RPC 7770)",
            .defaultPort = 7777,            // == protocol.json settings.default_port; overridden below when not supplied
            .offersNoIpBind = true,
        },
        argc, argv);
    if (!args.ok)
        return args.exitCode;

    uint16_t port = args.port;
    const bool portOverride = args.portSupplied;
    const bool verbose = args.verbose;
#ifndef NDEBUG
    if (args.noRateLimit) Aphelyon::g_rateLimitingEnabled.store(false);
    if (args.noIpBind)    Aphelyon::g_authIPBindingEnabled.store(false);
#endif
```
  (`<cstring>` and `<iostream>` go: nothing else in the file uses them. The `if (!portOverride) port = … defaultPort;` block below stays as is.)

- [ ] **Step 7: Account's `Main.cpp` adopts it.** Includes (lines 1-19): remove `#include <cstring>` and add `#include "Util/ServiceCli.hpp"` after `#include "Util/Logger.hpp"` (keep `<iostream>` and `<vector>` — `SeedAccounts` uses them). Delete `PrintUsage` (lines 33-44). Replace lines 106-146 (from `    uint16_t port = 7771;` through the `for` loop's closing `    }`) with:

```cpp
    // Core-DLL split Plan 3: Arcane::Cli (ArcaneCore.dll) behind the services'
    // shared vocabulary. Audit M-V5-1's point stands: Account has its own
    // default port (7771) and never reads protocol.json's default_port.
    const Aphelyon::ServiceArgs args = Aphelyon::ParseServiceArgs(
        Aphelyon::ServiceCliSpec{
            .name = "Account",
            .description = "account, gacha and quest service (client TCP 7771, internal RPC 7773)",
            .defaultPort = 7771,
            .offersSeedAccounts = true,
        },
        argc, argv);
    if (!args.ok)
        return args.exitCode;

    const uint16_t port = args.port;
    const bool verbose = args.verbose;
    const bool seedAccounts = args.seedAccounts;
#ifndef NDEBUG
    if (args.noRateLimit) Aphelyon::g_rateLimitingEnabled.store(false);
#endif
```

- [ ] **Step 8: Combat's `Main.cpp` adopts it.** Includes (lines 1-9): drop `#include <iostream>`, add `#include "Util/ServiceCli.hpp"` after `#include "Util/Logger.hpp"`. Delete `PrintUsage` (lines 23-34). Replace lines 42-62 (from `    uint16_t lobbyPort = 7772;` through the `for` loop's closing `    }`) with:

```cpp
    // Core-DLL split Plan 3: Arcane::Cli (ArcaneCore.dll) behind the services'
    // shared vocabulary; -u/--udp is Combat's alone.
    const Aphelyon::ServiceArgs args = Aphelyon::ParseServiceArgs(
        Aphelyon::ServiceCliSpec{
            .name = "Combat",
            .description = "match lobby + gameplay service (lobby TCP 7772, gameplay UDP 7778)",
            .defaultPort = 7772,
            .defaultUdpPort = 7778,
        },
        argc, argv);
    if (!args.ok)
        return args.exitCode;

    const uint16_t lobbyPort = args.port;
    const uint16_t udpPort   = args.udpPort;
    const bool verbose = args.verbose;
#ifndef NDEBUG
    if (args.noRateLimit) Aphelyon::g_rateLimitingEnabled.store(false);
#endif
```

- [ ] **Step 9: Build both configs (incremental is fine now) and run the fast suites.**

```bash
MSYS_NO_PATHCONV=1 "/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" Aphelyon.slnx -p:Configuration=Debug   -m -nologo -v:m > ../build-t4-debug.log 2>&1; echo "debug exit=$?"
MSYS_NO_PATHCONV=1 "/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" Aphelyon.slnx -p:Configuration=Release -m -nologo -v:m > ../build-t4-release.log 2>&1; echo "release exit=$?"
grep -E " error |warning C4|LNK4098|LNK2019|Error\(s\)|Warning\(s\)" ../build-t4-debug.log ../build-t4-release.log | head -20
(cd bin/Debug-windows-x86_64/CommonTests && ./CommonTests.exe | grep -E "^Randomness|^test cases|^assertions|All tests passed|FAILED")
(cd bin/Release-windows-x86_64/CommonTests && ./CommonTests.exe "[service-cli],[core-dll]" | grep -E "^Randomness|^test cases|^assertions|All tests passed|FAILED")
(cd bin/Debug-windows-x86_64/AuthTests   && ./AuthTests.exe   | grep -E "^Randomness|^test cases|^assertions|All tests passed|FAILED")
(cd bin/Debug-windows-x86_64/CombatTests && ./CombatTests.exe | grep -E "^Randomness|^test cases|^assertions|All tests passed|FAILED")
```
Expected: both `exit=0`, the grep prints nothing (or only `0 Error(s)`); Debug CommonTests = 76 test cases (66 + 3 + 7) all passed; the Release run of the two tags = 10 cases all passed (this is where the `#else` branch of the NDEBUG test runs — it proves the Release build now defines `NDEBUG`, Q3); AuthTests 12, CombatTests 1.

- [ ] **Step 10: The witness — each service answers `--help` with 0 and a bogus flag with 2, in both configs, and imports the DLL.** Auth and Combat need only `data/protocol.json` (staged); Account's `--help`/refusal returns before any DB or env check. From `Server/`:

```bash
for c in Debug Release; do for s in Auth Account Combat; do
  (cd bin/$c-windows-x86_64/$s && ./$s.exe --help > /dev/null 2>&1; h=$?; ./$s.exe --bogus > /dev/null 2>&1; b=$?; ./$s.exe -p 0 > /dev/null 2>&1; z=$?; echo "$c/$s help=$h bogus=$b port0=$z")
done; done
for s in Auth Account Combat; do printf "%s imports ArcaneCore.dll: " $s; grep -a -c "ArcaneCore.dll" bin/Debug-windows-x86_64/$s/$s.exe; done
(cd bin/Release-windows-x86_64/Auth && ./Auth.exe --no-rate-limit > /dev/null 2>&1; echo "Release Auth --no-rate-limit exit=$?")
```
Expected: six lines `help=0 bogus=2 port0=2`; three counts ≥ 1; the last line `exit=2` (Release refuses the `[DEBUG]` flag as unknown — Q3/Q7 — where the old loop would have started the service).

- [ ] **Step 11: Commit (Gacha).**

```bash
git add Server/Common/src/Util/ServiceCli.hpp Server/Common/src/Util/ServiceCli.cpp Server/Common/tests/ServiceCliTest.cpp Server/Auth/src/Main.cpp Server/Account/src/Main.cpp Server/Combat/src/Main.cpp
git commit -m "feat(server): Aphelyon::ServiceCli -- the three services parse argv through Arcane::Cli (ArcaneCore.dll), the first compiled-Core consumer; unknown flags and bad ports are refused (Core-DLL split Plan 3)"
```
(+ the two trailers.) Delete the four `build-t4-*.log` scratch files.

---

### Task 5: Closeout — AccountTests on the ephemeral DB, spec status, the record (Gacha run, Arcane commit)

**Files:**
- Modify: Arcane `docs/specs/2026-09-15-core-dll-split-design.md:4` (status line)
- Modify: Arcane `docs/plans/2026-09-16-core-dll-split-plan3-gacha.md` (this file — the `<!-- CLOSEOUT -->` section)
- Modify: memory `C:\Users\Ethan Temprovich\.claude\projects\D--dev-starworks-Gacha\memory\project_arcane_core_shared_dll_direction.md` (description + a Plan 3 paragraph) — done by the orchestrator, not the implementer

**Interfaces:**
- Consumes: Tasks 1-4's commit shas and measurements from the ledger.
- Produces: the arc's record.

- [ ] **Step 1: AccountTests against the EPHEMERAL database only.** From the Gacha repo root, the Global Constraints recipe verbatim — `export POSTGRES_PORT=5433` BEFORE `up`; every line `-p aphelyon_ci`; ALWAYS `down -v` at the end (also on failure). Expected: `All tests passed` (194 test cases / 1296 assertions at Plan 2's close; Plan 3 adds none here). Record the seed. Confirm afterwards that the dev database is untouched: `docker ps --format '{{.Names}}' | grep -c aphelyon_ci` prints `0` and `docker volume ls --format '{{.Name}}' | grep -v aphelyon_ci` still lists the dev volume if one existed before.

- [ ] **Step 2: Status line.** `docs/specs/…-design.md:4`: replace `plan 3 (Gacha /MD) deferred to its trigger (s8 amendment).` with `plan 3 (Gacha /MD + Arcane::Cli as the first compiled-Core consumer) closed 2026-09-16 at Gacha \`<Task 4 sha>\` (SDK helper at Arcane \`<Task 1 sha>\`). The arc is complete: every host and every consumer links ArcaneCore.dll.`

- [ ] **Step 3: Closeout section.** Append after `<!-- CLOSEOUT -->` below: the five commit shas (Arcane T1, Gacha T2/T3/T4, Arcane T5); Task 2's vcpkg result (libpq version, wall time); Task 3's two-config build results (0 errors, 0 `LNK4098`, the `staged` count) and the `[core-dll]` seeds in both configs; Task 4's suite counts + seeds (Debug 76/12/1; Release 10) and the witness table (six `help=0 bogus=2 port0=2` lines, the three import counts, the Release `--no-rate-limit exit=2`); Task 5's AccountTests seed; one sentence confirming the dev database was never touched (every compose line carried `-p aphelyon_ci`); and the Q3 consequence stated as a fact: "the Server's Release configuration defines `NDEBUG` as of Task 3; the three services' Release refusal guards are live for the first time".

- [ ] **Step 4: Commit (Arcane).**

```bash
git add docs/specs/2026-09-15-core-dll-split-design.md docs/plans/2026-09-16-core-dll-split-plan3-gacha.md
git commit -m "docs(core-dll): close plan 3 -- spec status, closeout; the Core-DLL split arc is complete"
```
(+ the two trailers.) Do not push. Then the orchestrator updates the memory file: Plan 3 CLOSED at the two shas, both repos unpushed by N, the arc COMPLETE, and the next-plan pointer (Linux/CI milestone with ArcaneServer + the services as the Linux targets, the replication arc, or the 3D work — the user's call).

---

## Self-review (run at plan-writing time)

- **Spec coverage:** §8 "Gacha services → `/MD`" (Common + services `staticruntime "off"`, libpq on `-md`, the from-source project gone → T2/T3) ✓; §8 amendment "Plan 3's first task is a Core-only consumer helper in `build/arcane.lua`, lifted from `ArcaneServer`'s premake block" (T1), "the Jenkins provisioning flip to the `-md` libpq" (T2 Step 5), "a Server CI stage that depends on built SDK binaries" (T3 Step 9, as a refusal per Q6) ✓; §8 amendment's false "already built" claim corrected (T1 Step 4a) ✓; §11 Plan 3 rewritten from "deferred" to its definition (T1 Step 4b) ✓; §13 R12 records why the trigger was pulled (T1 Step 4c) ✓; §1.2/§6 "the services link Core" made true (T3) with a consumer that exercises a compiled export (T4) ✓; §12 "The Gacha vcpkg rebuild (~15 min)" — T2 ✓; R8 "Gacha last, its own PR" — Tasks 2-4 are Gacha commits after the Arcane helper ✓. Nothing else in the spec concerns Gacha.
- **Placeholder scan:** no TBD/TODO; every code step carries its full text; the only open values are Task 5's `<Task 1 sha>`/`<Task 4 sha>`, which exist only after those commits. Task 3 Step 7's "the third count may be 0 until Task 4" is a stated expectation, not a gap.
- **Type consistency:** `ServiceCliSpec`/`ServiceArgs`/`ParseServiceArgs` are spelled identically in the header (T4 S3), the implementation (T4 S4), the test (T4 S1) and the three mains (T4 S6-8); the designated-initializer field order in the mains (`name, description, defaultPort, [defaultUdpPort], [offersSeedAccounts], [offersNoIpBind]`) matches the struct's declaration order (C++20 requires it); `arcane_core_consumer`/`arcane_core_stage_dll` are spelled identically in T1 and T3; `Arcane::CrtFlavor::{Debug,Release}`, `Module::ScanFileCrtFlavor(path, std::string*)`, `ExecutablePathUtf8()`, `BuildInfo()` match `Plugin/Module.hpp` and `Base/Engine.hpp` as read on 2026-09-16; `Cli::Result::{ok, exitCode, Flag, Get, GetAs, Supplied}` and `Builder::{Short, Type}` match `Cli/Cli.hpp`.

<!-- CLOSEOUT -->

## Closeout

**Commits.**
- Task 1 (Arcane, `arcane_core_consumer()`/`arcane_core_stage_dll()` SDK helper + spec s8/s11/R12): `926cc053`
- Task 2 (Gacha, libpq moves to the `x64-windows-static-md` overlay triplet): `30005ac9`
- Task 3 (Gacha, the workspace links `ArcaneCore.dll` on `/MD` via `arcane_core_consumer()`): `154cdbfc`
- Task 4 (Gacha, `Aphelyon::ServiceCli` over `Arcane::Cli` — the three mains adopt it): `4c752293`
- Task 4 fix round 1 (Arcane, `arcane.lua` helpers disable C4251 for every consumer): `bb414f79`
- Task 5 (Arcane, this closeout): this commit

**Task 2 — libpq on `x64-windows-static-md` (verbatim from the Task 2 report).**
- vcpkg install (`libpq:x64-windows-static-md --overlay-triplets=<repo>/vcpkg-triplets --no-binarycaching`, run by the orchestrator in the background, no parallel Server build): **exit=0**, "All requested installations completed successfully in: 12 min" (openssl 8.1 min, zlib 8.3 s, libpq 3.4 min).
- libpq version: **16.9** on both `x64-windows-static-md` and the pre-existing `x64-windows-static` triplet (`vcpkg/installed/vcpkg/info/libpq_16.9_x64-windows-static*.list`).
- Six `.lib`s present on the new triplet (`libpq`, `libpgcommon`, `libpgport`, `libssl`, `libcrypto`, `zlib`), both release and debug variants; the old static-triplet `libpq.lib` untouched.
- Five scripts updated (`setup-vcpkg-deps.bat`, `clean.bat`, `generate.bat`, `doctor.bat`, `Jenkinsfile`) to reference the `-md` triplet; no edits to `premake5.lua`/`CLAUDE.md`/`BUILD.md` (Task 3's territory).

**Task 3 — the workspace links `ArcaneCore.dll` (both configurations).**
- Premake regenerate: clean, no warnings, all 9 vcxproj files generated.
- Debug `msbuild ... -t:Rebuild`: **exit=0**. Release `msbuild ... -t:Rebuild`: **exit=0**.
- `grep -E " error |LNK4098|LNK2019|LNK2001|LNK1181|Error\(s\)|Warning\(s\)"` over both logs: **no output at all** — 0 errors, 0 `LNK4098`, 0 unresolved externals. Neither of the Global Constraints' two falsifiers triggered.
- Staging: all **14 `staged` lines**, zero `MISSING` (Auth/Account/Combat/CommonTests/AuthTests/CombatTests/AccountTests, both configs).
- `[core-dll]` boundary suite, both configs: Debug seed `1505863082` — All tests passed (8 assertions in 3 test cases); Release seed `522435820` — All tests passed (8 assertions in 3 test cases).
- Fast suites (Debug, full run): CommonTests seed `2892176358` — 428 assertions in 69 test cases; AuthTests seed `1954335604` — 34 assertions in 12 test cases; CombatTests seed `555917096` — 1 assertion in 1 test case.
- Import-table check: `ArcaneCore.dll` in `Debug/CommonTests/CommonTests.exe`: 2 (the boundary test calls into the DLL). `Auth.exe` imports 0 `ArcaneCore.dll` symbols at this point — expected; Task 4 is the trigger that gives it a compiled-Core call site.

**Task 4 — `Aphelyon::ServiceCli` over `Arcane::Cli`, the first argv-parsing consumer.**
- TDD: RED — `error C1083: Cannot open include file: 'Util/ServiceCli.hpp'`; GREEN — `[service-cli]` seed `1354781014`, All tests passed (51 assertions in 7 test cases).
- Both configs `exit=0`; grep for error/LNK4098/LNK2019 over both logs: clean (the only matches pre-fix were 7 `warning C4251` lines from `Arcane/Cli/Cli.hpp`, resolved by the Task 4 fix round below).
- Suite counts + seeds: **Debug CommonTests** seed `3810777804` — All tests passed (479 assertions in **76** test cases, matching 66 pre-existing + 3 `[core-dll]` + 7 `[service-cli]`); **Release** `"[service-cli],[core-dll]"` seed `2480680819` — All tests passed (58 assertions in **10** test cases, the run that exercises the `#else` (`NDEBUG`) branch, proving Release now defines `NDEBUG`); **Debug AuthTests** seed `357980149` — All tests passed (34 assertions in **12** test cases); **Debug CombatTests** seed `1000697772` — All tests passed (1 assertion in **1** test case).
- Step 10 witness table (verbatim):
  ```
  Debug/Auth help=0 bogus=2 port0=2
  Debug/Account help=0 bogus=2 port0=2
  Debug/Combat help=0 bogus=2 port0=2
  Release/Auth help=0 bogus=2 port0=2
  Release/Account help=0 bogus=2 port0=2
  Release/Combat help=0 bogus=2 port0=2
  Auth imports ArcaneCore.dll: 1
  Account imports ArcaneCore.dll: 1
  Combat imports ArcaneCore.dll: 1
  Release Auth --no-rate-limit exit=2
  ```
  All six `help=0 bogus=2 port0=2` lines, all three import counts ≥ 1, and the Release refusal (`exit=2` — the old hand-rolled loop would have started the service) match exactly.
- **Fix round 1** (Arcane `bb414f79`): `arcane_game_module()` and `arcane_core_consumer()` in `build/arcane.lua` gained `disablewarnings { "4251" }`, propagating the engine workspace's own C4251 ruling to every external Core consumer. Verified: C4251 count in a Gacha Debug rebuild went from 7 to **0**; `CommonTests.exe "[service-cli],[core-dll]"` re-run afterward: seed `3062531209` — All tests passed (59 assertions in 10 test cases).

**Task 5 — AccountTests against the EPHEMERAL database.**
- Command sequence run verbatim from the Global Constraints recipe (`export POSTGRES_PORT=5433` before `up`; every `docker compose` line `-p aphelyon_ci`):
  ```bash
  C="docker compose -p aphelyon_ci -f Server/docker-compose.yml -f ci/docker-compose.ci.yml"
  export POSTGRES_PORT=5433
  $C up -d --wait --build
  $C exec -T postgres psql -U aphelyon -d aphelyon -v ON_ERROR_STOP=1 -f /sql/schema.sql
  $C exec -T postgres psql -U aphelyon -d aphelyon -v ON_ERROR_STOP=1 -f /sql/seed.sql
  (cd Server/bin/Debug-windows-x86_64/AccountTests && POSTGRES_PORT=5433 APHELYON_TEST_DB_URL=postgresql://aphelyon:aphelyon@localhost:5433/aphelyon ./AccountTests.exe | grep -E "^Randomness|^test cases|^assertions|All tests passed|FAILED")
  $C down -v
  ```
  One deviation from the plan's literal recipe was needed under Git Bash: MSYS path-conversion mangled the bare `-f /sql/schema.sql` / `-f /sql/seed.sql` container paths into a Windows path on the first attempt (`psql: error: C:/Program Files/Git/sql/schema.sql: No such file or directory`); the run was retried with `MSYS_NO_PATHCONV=1` exported alongside `POSTGRES_PORT`, which fixed it. The first, failed attempt never started AccountTests.exe and was torn down (`$C down -v`, still `-p aphelyon_ci`) before the retry — the ephemeral container/volume from that attempt is not the one counted below.
- Result: `Randomness seeded to: 849277911` — **All tests passed (1296 assertions in 194 test cases)**, matching Plan 2's close exactly (Plan 3 adds no AccountTests cases).
- Post-run checks: `docker ps -a --format '{{.Names}}' | grep -c aphelyon_ci` → `0`; `docker volume ls --format '{{.Name}}' | grep -c server_aphelyon_pgdata` → `1`; the dev container `aphelyon_postgres` also still present. **The dev database was never touched** — every compose invocation across both the failed MSYS-pathconv attempt and the passing run carried `-p aphelyon_ci`, and the pre-existing dev volume (`server_aphelyon_pgdata`) and dev container (`aphelyon_postgres`) are confirmed intact after teardown.

**Q3 consequence (fact, established by Task 4's Release suite run above).** The Server's Release configuration defines `NDEBUG` as of Task 3; the three services' Release refusal guards are live for the first time.
