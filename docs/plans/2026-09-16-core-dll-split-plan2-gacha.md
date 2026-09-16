# Core-DLL split, Plan 2 (Gacha) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Retire the Gacha Server's from-source `ArcaneCore` static project — measured dead weight, since no service or test calls a compiled Core symbol — and record in the spec that the `/MD` migration it was written to precede has no present consumer, with the trigger that revives it as Plan 3.

**Architecture:** The three services (Auth/Account/Combat), `Common` and the four test exes consume exactly six ArcaneCore headers (`Net/TcpSocket`, `Net/Protocol`, `Net/RateLimiter`, `Crypto/Crypto`, `Util/Logger`, `Util/LruCache`), all header-only, whose transitive includes reach no `ARCANE_CORE_API` declaration. The from-source `ArcaneCore` project compiles `Guid.cpp`, `Cli.cpp` and `Toolchain.cpp` that nothing in `Server/` references (grep: zero hits for `Arcane::Guid`, `Arcane::Cli`, `Toolchain`, `TaskExecutor`). The plan deletes that project, the workspace-wide `ARCANE_CORE_STATIC` define and the five `links "ArcaneCore"` lines, keeps the `$ARCANE_SDK/ArcaneCore/src` include dir, and proves the premise by the build: a link error naming a Core symbol would falsify it and STOPS the plan. The CRT stays static (`/MT`) and libpq stays on `x64-windows-static`: the `/MD` flip exists to share one heap across a DLL boundary, and there is no boundary. Spec §8/§11 are amended to say so, with Plan 3's trigger written down.

**Tech Stack:** premake5 (bundled at `ThirdParty/premake5/`), MSBuild (VS 18), Catch2/rapidcheck (vendored), Postgres 16 in Docker (the Jenkins ephemeral recipe), the Arcane SDK checkout via `ARCANE_SDK`.

**Spec:** `docs/specs/2026-09-15-core-dll-split-design.md` (Arcane repo) — §8 "Gacha services → `/MD`", §11 "Plan 2", §13 R8. Plan 1's Closeout (`docs/plans/2026-09-15-core-dll-split-plan1-arcane.md`, "Plan 2 hand-off") names the state inherited: the explicit Core file list (Gacha `84b63f44`), the workspace-wide `ARCANE_CORE_STATIC` define (`a3351eee`), `Game/Aphelyon.arcproj` at ABI 31 (`4faf5a18`).

## Global Constraints

- **Two repos, three commits.** Task 1 is an Arcane docs commit (`D:\dev\starworks\Arcane`, branch `main`, HEAD `cac776b2` at plan time); Task 2 is a Gacha commit (`D:\dev\starworks\Gacha`, branch `main`, HEAD `4faf5a18`); Task 3 is an Arcane docs commit. **Do not push either repo.** Trailers on every commit:
  ```
  Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01Djskfodj1WnyR2g5v9V1Qc
  ```
  Never stage the Arcane strays (`out.txt`, `ArcaneEditor/ArcaneEditor/`, `ArcaneAssetPipeline/ArcaneAs.*/`, `arcbuild/arcbuild/`) nor the Gacha in-flight files (`Game/Source/TestComponent.*`, `Game/Content/scenes/test.arcscene` — the latter shows modified; leave it). Stage by explicit path.
- **`ARCANE_SDK`** must point at an Arcane checkout at `cac776b2` or later (the Server needs only its headers; nothing is linked). Verify with `git -C "$ARCANE_SDK" rev-parse --short HEAD` before generating.
- **Gacha build ritual (bash, from `D:\dev\starworks\Gacha\Server`):** regenerate with `../ThirdParty/premake5/premake5.exe vs2026` (never `GenerateProjects.bat` under the Bash tool — it can hang); msbuild = `MSYS_NO_PATHCONV=1 "/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" Aphelyon.slnx -p:Configuration=<Debug|Release> -m -nologo -v:m > <log> 2>&1`, then `grep -E " error |Error\(s\)|Warning\(s\)" <log>`. Output lands in `Server/bin/<cfg>-windows-x86_64/<Project>/`. The CRT stays `staticruntime "on"` everywhere; `VCPKG_TRIPLET` stays `x64-windows-static`.
- **The falsifier.** If the Debug or Release link of ANY project reports an unresolved external whose name is in namespace `Arcane` (LNK2019/LNK2001), the plan's premise is wrong: STOP, report BLOCKED with the symbol and the project, do NOT re-add the project or an `ARCANE_CORE_STATIC` workaround. That symbol is the first compiled-Core consumer and is Plan 3's trigger, not this plan's fix.
- **Suites run FROM each exe's bin dir**, foreground, Debug: `CommonTests`, `AuthTests`, `CombatTests` (the Jenkins "Fast tests" set), then `AccountTests` against the EPHEMERAL CI database only — never the dev database. The ephemeral recipe, verbatim from the Jenkinsfile, from the Gacha repo root: `docker compose -p aphelyon_ci -f Server/docker-compose.yml -f ci/docker-compose.ci.yml up -d --wait --build`, then `... exec -T postgres psql -U aphelyon -d aphelyon -v ON_ERROR_STOP=1 -f /sql/schema.sql` and the same with `/sql/seed.sql`, run the suite with `APHELYON_TEST_DB_URL=postgresql://aphelyon:aphelyon@localhost:5433/aphelyon` and `POSTGRES_PORT=5433` in the environment, then ALWAYS tear down with `docker compose -p aphelyon_ci -f Server/docker-compose.yml -f ci/docker-compose.ci.yml down -v`. **HARD RULE: every compose command carries `-p aphelyon_ci`; a bare `down -v` destroys the dev database.** Suite output hygiene: `<Suite>.exe | grep -E "^Randomness|^test cases|^assertions|All tests passed|FAILED"`; never `cat` a suite log. AccountTests takes ~17 minutes.
- **Game module untouched.** `Game/` is built through `arcbuild` and is not part of this plan; do not run it.
- **Ledger:** `.superpowers/sdd/2026-09-16-core-dll-split-plan2-gacha/progress.md` in the Arcane repo (gitignored).

---

## Plan-time rulings

| # | Ruling | Why |
|---|---|---|
| Q1 | **Plan 2 is the retirement only; the `/MD` migration is Plan 3, written when a consumer exists.** | The spec's §8 premise ("the services link `ArcaneCore.dll`") was an assumption; measured 2026-09-16, the Server consumes six header-only files and no compiled symbol (grep: 0 hits for `Arcane::Guid`, `Arcane::Cli`, `Toolchain`, `TaskExecutor` in `Server/`). A CRT flip with no DLL boundary has no acceptance test beyond "the exes start"; the `arcane.lua` Core-consumer helper it needs takes its shape from the first real consumer (`ArcaneServer`'s premake block is the proven Core-only host recipe). Trigger for Plan 3: the first service that calls a compiled Core symbol — most likely Combat adopting `Runtime` in the Combat Sphere phase, or `Aphelyon::Logger` moving onto Core's `Base/Log`. |
| Q2 | **The include dir stays; the define goes.** `IncludeDir["ArcaneCore"]` and every `"%{IncludeDir.ArcaneCore}"` line are kept; `ARCANE_CORE_STATIC` is deleted. | The six headers reach no `ARCANE_CORE_API` declaration, so neither branch of `Core/Api.hpp` is exercised; an unused `dllimport` declaration would compile anyway. Keeping the define would misdescribe the workspace as a static Core build. |
| Q3 | **Living docs are amended; history is not.** `CLAUDE.md` (Gacha) and the spec (Arcane) change; Plan 1's Closeout, the extraction spec and past plans keep their wording. | The Closeout's hand-off paragraph is a record of what Plan 1 left; the spec is the authority the next reader plans from. |
| Q4 | **The falsifier is a STOP, not a fix.** | A Core link error during Task 2 means a compiled-Core consumer already exists; that is Plan 3's first fact, and patching around it here would hide it. |

---

## File structure

| File | Change | Responsibility |
|---|---|---|
| Arcane `docs/specs/2026-09-15-core-dll-split-design.md` | Modify (T1: §8 amendment paragraph, §11 Plan 2 paragraph, §13 new row R11; T3: status line) | The living definition of Plan 2 and Plan 3's trigger. |
| Gacha `Server/premake5.lua` | Modify (T2): delete lines 33-40 (workspace `ARCANE_CORE_STATIC` block), 84-144 (the `ArcaneCore` project), the `"ArcaneCore"` entry in the five `links` lines (227, 300, 381, 460, 542); reword the comments at 47-54 and 145-150 | The workspace no longer builds Core; it includes Core's header-only Net/Crypto/Util layer. |
| Gacha `CLAUDE.md` | Modify (T2): lines 24, 98, 204, 276, 287 | Every sentence that says the Server builds or links ArcaneCore. |
| Arcane `docs/plans/2026-09-16-core-dll-split-plan2-gacha.md` (this file) | Modify (T3: Closeout section) | The record. |

---

### Task 1: Spec amendment — the measured premise and Plan 3's trigger

**Files:**
- Modify: Arcane `docs/specs/2026-09-15-core-dll-split-design.md` — §8 (after the "Gacha services → `/MD`" paragraph, lines 364-373), §11 (the "Plan 2 — Gacha repo" paragraph, lines 449-453), §13 (append a row after R10)

**Interfaces:**
- Consumes: nothing.
- Produces: the amended §8/§11 text Task 2's commit message cites, and ruling R11 Task 3's status line refers to.

- [ ] **Step 1: RED — the spec still states the assumption.** From the Arcane repo root:

```bash
grep -n "The three services move from the from-source" docs/specs/2026-09-15-core-dll-split-design.md
grep -c "R11" docs/specs/2026-09-15-core-dll-split-design.md
```
Expected: the first prints one line (§8, ~:364); the second prints `0`.

- [ ] **Step 2: Append the amendment paragraph to §8.** Immediately after the paragraph that ends "does not remove the sources it compiles." (line ~373), insert:

```markdown
**Amendment, measured 2026-09-16 (Plan 2 planning).** The paragraph above
assumed the services link `ArcaneCore.dll`. They do not, and never did: the
Server's Core dependency is exactly six header-only files (`Net/TcpSocket`,
`Net/Protocol`, `Net/RateLimiter`, `Crypto/Crypto`, `Util/Logger`,
`Util/LruCache`), whose transitive includes reach no `ARCANE_CORE_API`
declaration, and nothing under `Server/` references the three compiled Core
sources the from-source project built (`Guid.cpp`, `Cli.cpp`, `Toolchain.cpp` —
grep: zero hits for `Arcane::Guid`, `Arcane::Cli`, `Toolchain`,
`TaskExecutor`). There is nothing to link, so the `/MD` flip — whose only
purpose is one CRT heap across a DLL boundary — has no present cause. **Plan 2
therefore retires the from-source project, the `ARCANE_CORE_STATIC` define and
the five dead `links` lines, keeps the header include, and leaves the CRT
static and libpq on `x64-windows-static`.** The migration this paragraph
describes becomes **Plan 3**, written when its consumer exists; its trigger is
the first service that calls a compiled Core symbol (most likely Combat
adopting `Runtime` in the Combat Sphere phase, or `Aphelyon::Logger` moving
onto `Base/Log`). Plan 3's first task is a Core-only consumer helper in
`build/arcane.lua`, lifted from `ArcaneServer`'s own premake block (the proven
Core-only host recipe), plus the Jenkins provisioning flip to the `-md` libpq
(already built on the dev machine) and a Server CI stage that depends on built
SDK binaries the way the Game stage already does.
```

- [ ] **Step 3: Rewrite the §11 "Plan 2" paragraph.** Replace lines ~449-453 (from "**Plan 2 — Gacha repo.**" to "merges last.") with:

```markdown
**Plan 2 — Gacha repo** (`docs/plans/2026-09-16-core-dll-split-plan2-gacha.md`).
Retire the from-source `ArcaneCore` project, the `ARCANE_CORE_STATIC` define and
the dead `links` lines from `Server/premake5.lua`; keep the header include; the
services stay static-CRT (§8 amendment: nothing links). Green = both configs
build with no `Arcane::` unresolved external, the three fast suites and
AccountTests (ephemeral DB) pass. Starts after Plan 1 has shipped; merges last.

**Plan 3 — Gacha repo, deferred.** The `/MD` migration as §8 originally
described it (link `ArcaneCore.dll`, libpq on `x64-windows-static-md`, `Common`
and the services on the dynamic CRT, the Jenkins lanes). Written when its
trigger fires (§8 amendment); it likely folds into that phase's own plan.
```

- [ ] **Step 4: Add ruling R11 to §13.** Append after the R10 row of the rulings table:

```markdown
| R11 | Do the services move to `/MD` in Plan 2 | **No — measured 2026-09-16: no service links a compiled Core symbol, so Plan 2 only retires the dead from-source project; the CRT flip is Plan 3, triggered by the first compiled-Core consumer** | The §8 premise was an assumption, not a measurement (six header-only includes; zero references to `Guid`/`Cli`/`Toolchain`/`TaskExecutor`). A CRT flip with no DLL boundary has no acceptance test, and the `arcane.lua` Core-consumer helper takes its shape from the first real consumer |
```

- [ ] **Step 5: GREEN.** Re-run Step 1's greps: the first still prints its line (the original paragraph is kept as history above the amendment), the second prints `1`. Also `grep -n "Plan 3" docs/specs/2026-09-15-core-dll-split-design.md | wc -l` prints at least `3`.

- [ ] **Step 6: Commit (Arcane).**

```bash
git add docs/specs/2026-09-15-core-dll-split-design.md
git commit -m "docs(core-dll): spec s8/s11/R11 -- Plan 2 retires the dead from-source Core project; the /MD migration is Plan 3, triggered by the first compiled-Core consumer (measured: six header-only includes, zero compiled references)"
```
(+ the two trailers.)

---

### Task 2: Retire the from-source `ArcaneCore` project (Gacha)

**Files:**
- Modify: Gacha `Server/premake5.lua:33-40` (delete), `:84-144` (delete), `:227`, `:300`, `:381`, `:460`, `:542` (drop `"ArcaneCore"` from `links`), `:47-54` and `:145-150` (comments)
- Modify: Gacha `CLAUDE.md:24`, `:98`, `:204`, `:276`, `:287`
- Test: the build in both configurations is the test (the falsifier in Global Constraints); then the four suites.

**Interfaces:**
- Consumes: `IncludeDir["ArcaneCore"] = ARCANE_SDK .. "/ArcaneCore/src"` (kept, `premake5.lua:63`).
- Produces: a workspace with no `ArcaneCore` project and no `ARCANE_CORE_STATIC`; `Common`, Auth, Account, Combat, AccountTests, CommonTests, AuthTests, CombatTests link `Common` (+ their existing system/vcpkg libs) only.

- [ ] **Step 1: RED — the premise, measured in this checkout.** From the Gacha repo root:

```bash
git -C "$ARCANE_SDK" rev-parse --short HEAD
grep -rn "Arcane::Guid\|Guid::\|Arcane::Cli\b\|CliType\|Toolchain\|TaskExecutor\|BuildInfo(" Server --include=*.cpp --include=*.hpp | grep -v "^\s*//" | wc -l
grep -rhoE "#include <Arcane/[A-Za-z/]+\.hpp>" Server --include=*.hpp --include=*.cpp | sort -u
grep -c "ArcaneCore" Server/premake5.lua
```
Expected: an SDK sha at or after `cac776b2`; `0`; exactly the six headers (`Crypto/Crypto`, `Net/Protocol`, `Net/RateLimiter`, `Net/TcpSocket`, `Util/Logger`, `Util/LruCache`); a count of `ArcaneCore` mentions (record it — it drops to the include-dir mentions only after Step 3). If the second grep is not `0`, STOP: the premise is wrong (see the falsifier ruling Q4) — report the hit as Plan 3's trigger.

- [ ] **Step 2: Delete the workspace define block.** Remove `Server/premake5.lua:33-40` entirely (the comment "-- Workspace-wide, so EVERY project that includes an Arcane Core header …" through `defines { "ARCANE_CORE_STATIC" }`). Nothing replaces it.

- [ ] **Step 3: Delete the `ArcaneCore` project block.** Remove `Server/premake5.lua:84-144` entirely (from the `-- ====` banner "ArcaneCore: server-flavor build of $ARCANE_SDK/ArcaneCore/src" through the `filter "configurations:Release"` … `optimize "on"` lines that precede the `Common` banner). Verify `grep -n 'project "ArcaneCore"' Server/premake5.lua` prints nothing.

- [ ] **Step 4: Drop the five dead links.** Edit these lines so `"ArcaneCore"` is gone:
  - `:227` `links { "Common", "ArcaneCore" }` → `links { "Common" }` (Auth)
  - `:300` same (Account)
  - `:381` same (Combat)
  - `:460` `links { "Common", "ArcaneCore", "Catch2", "rapidcheck" }` → `links { "Common", "Catch2", "rapidcheck" }` (AccountTests)
  - `:542` same (the `aphelyon_test_project` helper: CommonTests/AuthTests/CombatTests)
  (Line numbers shift after Steps 2-3; match on the text.)

- [ ] **Step 5: Reword the two comments.** Replace the block at (original) `:47-54` with:

```lua
    -- Strangler extraction (M0, 2026-06-11): wire framing, Protocol,
    -- Types, Crypto, RateLimiter (+ deps Logger, LruCache) live in
    -- ArcaneCore/src/Arcane as namespace Arcane. Since the 2026-08-11 repo
    -- extraction those sources live in the engine SDK checkout
    -- (github.com/T3mps/Arcane), consumed via ARCANE_SDK -- the same
    -- env-var contract game modules use for build/arcane.lua. The old
    -- Common paths are re-export shims. See docs/superpowers/specs/
    -- 2026-08-11-arcane-repo-extraction-design.md.
    --
    -- HEADER-ONLY consumption (Core-DLL split Plan 2, 2026-09-16): the six
    -- headers this workspace reaches (Net/TcpSocket, Net/Protocol,
    -- Net/RateLimiter, Crypto/Crypto, Util/Logger, Util/LruCache) are
    -- header-only and reach no ARCANE_CORE_API declaration, so nothing here
    -- compiles or links ArcaneCore -- the from-source static project that
    -- used to sit below was retired as dead weight. The day a service calls
    -- a COMPILED Core symbol (Runtime, Base/Log, Guid, Cli...) the link
    -- fails with an Arcane:: unresolved external: that is Plan 3's trigger
    -- (link ArcaneCore.dll, /MD, libpq on x64-windows-static-md -- spec
    -- docs/specs/2026-09-15-core-dll-split-design.md s8 amendment), not a
    -- reason to bring the static build back.
```
and the `Common` banner at (original) `:145-150` with:

```lua
-- ============================================================================
-- Common: Shared static library
-- Contains: ServiceClient, ServiceEndpoint, SessionCache, TcpServerBase,
--           State/Db/Persistence helpers. Protocol/Types/Logger/Crypto/
--           RateLimiter/TcpSocket/LruCache are ArcaneCore HEADERS (included
--           from $ARCANE_SDK, nothing linked -- see the IncludeDir note above);
--           the old Common paths are re-export shims.
-- ============================================================================
```

- [ ] **Step 6: Regenerate and build both configurations.** From `Server/`:

```bash
../ThirdParty/premake5/premake5.exe vs2026
MSYS_NO_PATHCONV=1 "/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" Aphelyon.slnx -p:Configuration=Debug   -m -nologo -v:m > /tmp/p2-debug.log 2>&1;   echo "exit=$?"; grep -E " error |Error\(s\)|Warning\(s\)" /tmp/p2-debug.log
MSYS_NO_PATHCONV=1 "/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" Aphelyon.slnx -p:Configuration=Release -m -nologo -v:m > /tmp/p2-release.log 2>&1; echo "exit=$?"; grep -E " error |Error\(s\)|Warning\(s\)" /tmp/p2-release.log
grep -c "LNK2019\|LNK2001" /tmp/p2-debug.log /tmp/p2-release.log
```
Expected: both `exit=0`, no ` error ` lines, `0` unresolved externals. **If an `LNK2019`/`LNK2001` names an `Arcane::` symbol: STOP (ruling Q4), report BLOCKED with the symbol and project.** Also confirm no project named `ArcaneCore` was built: `ls Server/bin/Debug-windows-x86_64/` lists no `ArcaneCore` dir newer than the build (a stale one from before is build output and is ignored).

- [ ] **Step 7: The fast suites (Debug), from each bin dir.**

```bash
for s in CommonTests AuthTests CombatTests; do (cd Server/bin/Debug-windows-x86_64/$s && ./$s.exe | grep -E "^Randomness|^test cases|^assertions|All tests passed|FAILED"); done
```
Expected: `All tests passed` for each (record the three case/assertion lines and seeds).

- [ ] **Step 8: AccountTests against the ephemeral CI database.** From the Gacha repo root, Docker Desktop running:

```bash
C="docker compose -p aphelyon_ci -f Server/docker-compose.yml -f ci/docker-compose.ci.yml"
$C up -d --wait --build
$C exec -T postgres psql -U aphelyon -d aphelyon -v ON_ERROR_STOP=1 -f /sql/schema.sql
$C exec -T postgres psql -U aphelyon -d aphelyon -v ON_ERROR_STOP=1 -f /sql/seed.sql
(cd Server/bin/Debug-windows-x86_64/AccountTests && POSTGRES_PORT=5433 APHELYON_TEST_DB_URL=postgresql://aphelyon:aphelyon@localhost:5433/aphelyon ./AccountTests.exe | grep -E "^Randomness|^test cases|^assertions|All tests passed|FAILED")
$C down -v
```
Expected: `All tests passed` (~17 min); the teardown runs whether or not the suite passed. `-p aphelyon_ci` on every line — the dev database is never touched.

- [ ] **Step 9: CLAUDE.md (Gacha).** Edit these sentences:
  - `:24` "Both `Game/` and the Server's `ArcaneCore` build consume it through the `ARCANE_SDK` environment variable" → "Both `Game/` and the Server (header-only: its six Core headers, nothing linked) consume it through the `ARCANE_SDK` environment variable".
  - `:98` "`ARCANE_SDK` must be set (the premake fails loudly if not) -- the `ArcaneCore` project compiles `$ARCANE_SDK/ArcaneCore/src` in its server flavor (static CRT)." → "`ARCANE_SDK` must be set (the premake fails loudly if not) -- the Server includes `$ARCANE_SDK/ArcaneCore/src` headers only; nothing compiles or links ArcaneCore (Core-DLL split Plan 2, 2026-09-16)."
  - `:204` "alongside **ArcaneCore** (the server-flavor build of `$ARCANE_SDK/ArcaneCore/src`). `Protocol`, `Types`, `Logger`, `Crypto`, `RateLimiter`, `TcpSocket`, `LruCache` live in ArcaneCore (namespace `Arcane`; …" → "which includes **ArcaneCore's header-only Net/Crypto/Util layer** from `$ARCANE_SDK/ArcaneCore/src` (nothing linked; the first compiled-Core call is Plan 3's trigger). `Protocol`, `Types`, `Logger`, `Crypto`, `RateLimiter`, `TcpSocket`, `LruCache` live there (namespace `Arcane`; …" — keep the rest of the bullet.
  - `:276` "| Arcane engine (Game/ + ArcaneCore) | Separate repo via `ARCANE_SDK` |" → "| Arcane engine (Game/ + the Server's Core headers) | Separate repo via `ARCANE_SDK` |".
  - `:287` "both `Server/premake5.lua` (ArcaneCore sources) and `Game/premake5.lua` (arcane.lua)" → "both `Server/premake5.lua` (ArcaneCore headers) and `Game/premake5.lua` (arcane.lua)".
  Then `grep -n -i "server-flavor\|from-source\|static CRT" CLAUDE.md` must print nothing about ArcaneCore.

- [ ] **Step 10: Commit (Gacha).**

```bash
git add Server/premake5.lua CLAUDE.md
git status --short   # must show ONLY the two staged files + the user's in-flight Game/ files unstaged
git commit -m "chore(server): retire the from-source ArcaneCore project -- the Server consumes six header-only Core headers and links nothing (Core-DLL split Plan 2; the /MD migration is Plan 3, spec s8 amendment)"
```
(+ the two trailers.)

---

### Task 3: Closeout

**Files:**
- Modify: Arcane `docs/specs/2026-09-15-core-dll-split-design.md:4` (status line)
- Modify: this plan (append a Closeout section after `<!-- CLOSEOUT -->`)

- [ ] **Step 1: Status line.** `docs/specs/…-design.md:4` currently reads `**Status:** Implemented -- plan 1 (Arcane) closed 2026-09-15 at \`c5abeb48\`; plan 2 (Gacha /MD) pending.` → `**Status:** Implemented -- plan 1 (Arcane) closed 2026-09-15 at \`c5abeb48\` (final fix wave \`1987655d\`, Astra resync \`cf7452a4\`, editor early-resolver fix \`cac776b2\`); plan 2 (Gacha, the from-source Core project retired) closed 2026-09-16 at Gacha \`<Task 2 sha>\`; plan 3 (Gacha /MD) deferred to its trigger (s8 amendment).`

- [ ] **Step 2: Closeout section.** Append after `<!-- CLOSEOUT -->` below: the three commit shas (Arcane T1, Gacha T2, Arcane T3), the Step 1 measurements (SDK sha, the six headers, the zero-hit grep), both build results (0 errors, 0 unresolved externals, warning counts), the four suite results with seeds, and one sentence confirming the dev database was never touched (every compose line carried `-p aphelyon_ci`).

- [ ] **Step 3: Commit (Arcane).**

```bash
git add docs/specs/2026-09-15-core-dll-split-design.md docs/plans/2026-09-16-core-dll-split-plan2-gacha.md
git commit -m "docs(core-dll): close plan 2 -- spec status, closeout"
```
(+ the two trailers.)

---

## Self-review (run at plan-writing time)

- **Spec coverage:** §8 "Gacha services → `/MD`" → amended by T1 (the measured premise) and its retirement half executed by T2; §11 "Plan 2" → rewritten by T1, executed by T2; R8 ("Gacha last, its own PR") → honoured (T2 is one Gacha commit after Plan 1 shipped); the deferred half → named Plan 3 with a trigger, not silently dropped. Nothing else in the spec concerns Gacha.
- **Placeholder scan:** no TBD/TODO; every edit carries its text; the one open value is Task 3's `<Task 2 sha>`, which only exists after Task 2 commits.
- **Type consistency:** no code types; the five `links` edits name the same projects the file declares (Auth, Account, Combat, AccountTests, the `aphelyon_test_project` helper); the ephemeral-DB recipe matches the Jenkinsfile verbatim including `-p aphelyon_ci` on the teardown.

<!-- CLOSEOUT -->
