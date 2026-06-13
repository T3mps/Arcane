# Setup Wizard Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A graphical first-time-setup wizard (Tauri + Svelte) that drives the existing setup scripts so a contributor can clone the repo and run one committed `Setup.exe` to set everything up.

**Architecture:** Three layers, one source of truth for orchestration. A headless PowerShell orchestrator (`scripts/setup.ps1`) is the only place the setup sequence lives; it forwards inputs to the existing `.bat` leaf scripts via env vars, sets `_APH_NOPAUSE=1` to defuse the `pause`-hang, and emits both human log lines and `@@WIZ` machine markers. A Tauri Rust core spawns that script, streams its output, and parses the markers into events. A Svelte UI renders a doctor checklist, an options form, and a live run view. The prebuilt `Setup.exe` is committed at the repo root and kept current by a CI job.

**Tech Stack:** PowerShell 5.1 (orchestrator), Tauri 2.x (Rust core + WebView2), Svelte 5 + Vite + TypeScript (UI), Vitest (UI reducer test), `cargo test` (Rust parser test), GitHub Actions + `tauri-action` (build + commit the exe). Existing leaf scripts: `scripts/doctor.bat`, `Server/scripts/setup-vcpkg-deps.bat`, `Server/GenerateProjects.bat`, `Server/scripts/db-setup.bat`, `Arcane/scripts/setup-vcpkg-deps.bat`, `Arcane/GenerateProjects.bat`.

**Spec:** `docs/superpowers/specs/2026-06-12-setup-wizard-design.md`.

---

## Build-time prerequisites (for whoever BUILDS the wizard, not contributors)

- Node.js LTS (>= 20) + npm.
- Rust stable (`rustup`, MSVC toolchain) + the Tauri prerequisites (WebView2 runtime ships on Win 10/11; the Microsoft C++ Build Tools come with the VS install the repo already requires).
- These are needed only to build `Setup.exe`. Contributors who clone the repo run the committed exe and need none of this.

## Constraints carried into every task

- UTF-8 no BOM, ASCII-only comments in shell/PS/Rust/Svelte sources we author. Write/Edit tools.
- **Never** run `db-reset.bat`, `clean.bat --deep`, or `docker compose down -v`.
- PowerShell-script verification means: invoke the script with flags, capture stdout, assert on it (no Pester dependency). The orchestrator's `-DryRun` mode (Task 2) is what makes its logic testable without actually building vcpkg/Docker.
- Leaf scripts are canonical and are NOT reimplemented. The orchestrator only sequences them and forwards env vars.
- **API-adaptation rule (JS/Rust framework code):** the Tauri 2.x / Svelte 5 API snippets below were written from current conventions but the *authoritative* forms are whatever `npm create tauri-app` scaffolds at build time. If a snippet's import path, macro, or plugin name differs from the scaffolded version, adapt to the scaffold, keep the contract (command names, event names, the `@@WIZ` grammar) identical, and record the deviation in the commit body.
- Commit per task, `type(scope):` + the `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` trailer.
- Work happens on a feature branch off `main` (the executor creates it). The spec is already committed on `main`.

## File structure

```
scripts/doctor.bat                         MODIFIED  schema/seed check; opt-in @@WIZ markers
scripts/setup.ps1                          NEW       headless orchestrator (single source of truth)
scripts/setup.bat                          REPLACED  2-line shim -> setup.ps1
tools/setup-wizard/                         NEW       wizard source (lowercase; not the C++ Tools/)
  package.json, vite.config.ts, tsconfig.json, svelte.config.js
  index.html
  src/main.ts                              Svelte mount
  src/App.svelte                           screen router
  src/lib/wizard.svelte.ts                 event->state store + reducer (UNIT TESTED)
  src/lib/wizard.test.ts                   Vitest reducer test
  src/lib/screens/Doctor.svelte
  src/lib/screens/Options.svelte
  src/lib/screens/Run.svelte
  src-tauri/Cargo.toml
  src-tauri/tauri.conf.json                window + bundle config; product name "Aphelyon Setup"
  src-tauri/capabilities/default.json      shell/process permissions
  src-tauri/src/main.rs                    Tauri entry + commands
  src-tauri/src/orchestrator.rs            spawn setup.ps1, stream, parse @@WIZ (UNIT TESTED)
  tests/orchestrator-smoke.ps1             PS assertions for setup.ps1 -DryRun
  .gitignore                               ignore node_modules/, dist/, target/
Setup.exe                                  NEW (Task 6)  committed prebuilt, CI-maintained
.github/workflows/build-setup-wizard.yml   NEW (Task 6)  rebuild + commit Setup.exe
README.md                                  MODIFIED (Task 7) onboarding section
CLAUDE.md                                  MODIFIED (Task 7) wizard note
```

## The `@@WIZ` marker contract (shared by orchestrator, Rust parser, and UI)

Emitted on their own lines by the orchestrator and `doctor.bat`; everything else is raw log. Exact grammar:

```
@@WIZ step=<id> status=start
@@WIZ step=<id> status=ok
@@WIZ step=<id> status=fail msg="<reason>"
@@WIZ doctor item=<name> status=pass|warn|fail msg="<text>"
@@WIZ done status=ok|fail
```

- `<id>` is one of: `doctor`, `server-vcpkg`, `server-generate`, `server-db`, `arcane-vcpkg`, `arcane-generate`, `client`, `build`.
- `<name>` / `<text>` / `<reason>` are free text; `msg="..."` is always the last field and double-quoted (no embedded double quotes — replace any with `'`).
- A consumer MUST treat any line not starting with `@@WIZ ` as plain log text.

---

### Task 1: Fix and instrument `doctor.bat`

**Files:**
- Modify: `scripts/doctor.bat`

- [ ] **Step 1: Reproduce the current breakage**

Run: `scripts\doctor.bat`
Expected: it prints `[FAIL] Server\Account\migrations\ missing` and exits 1 (because that directory does not exist; the DB workflow is single-file `schema.sql` + `seed.sql`). This is the bug.

- [ ] **Step 2: Replace the migrations check with a schema/seed check**

In `scripts/doctor.bat`, find the block (around lines 137-144):

```bat
if exist "%REPO_ROOT%\Server\Account\migrations" (
    for /f %%n in ('dir /b /a-d "%REPO_ROOT%\Server\Account\migrations\*.sql" 2^>nul ^| find /c /v ""') do (
        echo   [PASS] Account\migrations\ ^(%%n .sql files^)
    )
) else (
    echo   [FAIL] Server\Account\migrations\ missing
    set FAIL=1
)
```

Replace it with:

```bat
call :check_file "%REPO_ROOT%\Server\Account\schema.sql" "Account\schema.sql"
call :check_file "%REPO_ROOT%\Server\Account\seed.sql"   "Account\seed.sql"
```

(`:check_file` already exists in the script and sets `FAIL=1` on a miss.)

- [ ] **Step 3: Add opt-in `@@WIZ doctor` markers**

The wizard needs structured rows, but plain console runs must stay clean. Add a tiny helper that emits a marker only when `_APH_WIZ=1`. Add this near the other helpers at the bottom (after `:check_port`):

```bat
:wiz
REM Emit a machine marker for the wizard. %~1=item name, %~2=status, %~3=msg.
if "%_APH_WIZ%"=="1" echo @@WIZ doctor item=%~1 status=%~2 msg="%~3"
goto :eof
```

Then add a `call :wiz "<name>" <status> "<text>"` next to each existing `[PASS]`/`[WARN]`/`[FAIL]` line. Do this for the load-bearing checks at minimum: Visual Studio, docker engine, git, vcpkg, overlay triplet, premake5, each vendored dep, docker-compose.yml, schema.sql, seed.sql, and each port. Example for the Visual Studio PASS/FAIL:

```bat
REM inside the "if defined VS_PATH" branch, after the [PASS] line:
call :wiz "Visual Studio" pass "!VS_VER!"
REM inside the else branch, after the [FAIL] line:
call :wiz "Visual Studio" fail "not detected -- install VS 2026 Desktop C++"
```

Keep the human `echo [PASS]/[WARN]/[FAIL]` lines exactly as they are; the `:wiz` calls are additive.

- [ ] **Step 4: Verify the fix (no migrations FAIL)**

Run: `scripts\doctor.bat`
Expected: NO `migrations` line; instead `[PASS] Account\schema.sql` and `[PASS] Account\seed.sql` (both files exist). Exit code reflects only real prereq state (0 if VS/docker/vcpkg present).

- [ ] **Step 5: Verify the markers are opt-in**

Run (cmd): `set _APH_WIZ=1 && scripts\doctor.bat | findstr "@@WIZ"`
Expected: one `@@WIZ doctor item=... status=...` line per instrumented check.
Run: `scripts\doctor.bat | findstr "@@WIZ"`
Expected: NO output (markers suppressed when `_APH_WIZ` is unset).

- [ ] **Step 6: Commit**

```bash
git add scripts/doctor.bat
git commit -m "fix(scripts): doctor checks schema.sql/seed.sql (not the removed migrations dir); add opt-in @@WIZ markers"
```

---

### Task 2: Headless orchestrator `scripts/setup.ps1` + `setup.bat` shim + smoke tests

**Files:**
- Create: `scripts/setup.ps1`
- Replace: `scripts/setup.bat` (currently a full .bat orchestrator; becomes a shim)
- Create: `tools/setup-wizard/tests/orchestrator-smoke.ps1`

- [ ] **Step 1: Write the smoke-test harness first (it drives the design)**

Create `tools/setup-wizard/tests/orchestrator-smoke.ps1`:

```powershell
# Smoke tests for scripts/setup.ps1 -DryRun. No vcpkg/Docker/premake are
# invoked in DryRun; we assert on the emitted @@WIZ step markers and flag
# handling only. Exit 1 on first failure.
$ErrorActionPreference = 'Stop'
# tools/setup-wizard/tests -> tools/setup-wizard -> tools -> <repo>
$repo = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$setup = Join-Path $repo 'scripts\setup.ps1'
$fail = 0

function Assert($cond, $msg) {
    if (-not $cond) { Write-Host "FAIL: $msg" -ForegroundColor Red; $script:fail = 1 }
    else            { Write-Host "ok:   $msg" -ForegroundColor Green }
}

# 1. Server+Arcane dry run emits the expected ordered step markers.
$out = & pwsh -NoProfile -File $setup -DryRun -NonInteractive -Workspaces server,arcane 2>&1 | Out-String
$steps = ([regex]::Matches($out, '@@WIZ step=([\w-]+) status=start')) | ForEach-Object { $_.Groups[1].Value }
Assert ($steps -join ',' -eq 'doctor,server-vcpkg,server-generate,server-db,arcane-vcpkg,arcane-generate') `
       "server+arcane step order: got [$($steps -join ',')]"
Assert ($out -match '@@WIZ done status=ok') "emits done=ok on a clean dry run"
Assert ($out -notmatch 'step=client')       "client step absent when not selected"

# 2. -SkipVcpkg drops the vcpkg steps.
$out2 = & pwsh -NoProfile -File $setup -DryRun -NonInteractive -Workspaces server,arcane -SkipVcpkg 2>&1 | Out-String
Assert ($out2 -notmatch 'step=server-vcpkg') "SkipVcpkg drops server-vcpkg"
Assert ($out2 -notmatch 'step=arcane-vcpkg') "SkipVcpkg drops arcane-vcpkg"
Assert ($out2 -match  'step=server-generate') "SkipVcpkg keeps generate"

# 3. Client selected emits the stub step.
$out3 = & pwsh -NoProfile -File $setup -DryRun -NonInteractive -Workspaces client 2>&1 | Out-String
Assert ($out3 -match 'step=client status=ok') "client stub emits ok"

# 4. Unknown workspace is rejected (exit 2).
& pwsh -NoProfile -File $setup -DryRun -NonInteractive -Workspaces bogus 2>&1 | Out-Null
Assert ($LASTEXITCODE -eq 2) "unknown workspace -> exit 2 (got $LASTEXITCODE)"

if ($fail) { Write-Host "`nSMOKE FAILED" -ForegroundColor Red; exit 1 }
Write-Host "`nSMOKE PASSED" -ForegroundColor Green; exit 0
```

(`pwsh` is PowerShell 7; if the build box only has Windows PowerShell 5.1, use `powershell` instead — both run the script. The harness uses whichever is available; document the choice in the commit.)

- [ ] **Step 2: Run the harness to confirm it fails (setup.ps1 doesn't exist yet)**

Run: `pwsh -NoProfile -File tools\setup-wizard\tests\orchestrator-smoke.ps1`
Expected: FAIL — `setup.ps1` not found / no markers.

- [ ] **Step 3: Write `scripts/setup.ps1`**

```powershell
#requires -Version 5.1
<#
  Aphelyon setup orchestrator -- the single source of truth for first-time
  setup. Sequences the existing leaf scripts (kept canonical); forwards inputs
  via environment variables they already read. Used headless by CI and as the
  backend the GUI wizard (tools/setup-wizard) spawns. Emits @@WIZ markers (see
  docs/superpowers/specs/2026-06-12-setup-wizard-design.md) plus plain logs.
#>
[CmdletBinding()]
param(
    [string[]] $Workspaces = @('server','arcane','client'),
    [string]   $VcpkgRoot,
    [int]      $DbPort = 5432,
    [switch]   $SkipVcpkg,
    [ValidateSet('Debug','Release','Dist')] [string] $Config = 'Debug',
    [switch]   $Build,
    [switch]   $SkipDoctor,
    [switch]   $DryRun,
    [switch]   $NonInteractive,
    [switch]   $DoctorOnly
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot   # scripts/ -> repo root
$KnownWorkspaces = @('server','arcane','client')

function Wiz([string]$line) { Write-Host "@@WIZ $line" }
function Info([string]$msg) { Write-Host $msg }

# Validate workspace names early.
foreach ($w in $Workspaces) {
    if ($KnownWorkspaces -notcontains $w) {
        Write-Error "Unknown workspace '$w' (valid: $($KnownWorkspaces -join ', '))"
        exit 2
    }
}

# Interactive prompts fill anything not supplied by a flag (skipped in CI/GUI).
if (-not $NonInteractive -and -not $DryRun -and -not $DoctorOnly) {
    # Only prompt for values the caller did not pin. Defaults shown in brackets.
    if (-not $PSBoundParameters.ContainsKey('Workspaces')) {
        $ans = Read-Host "Workspaces to set up [server,arcane,client]"
        if ($ans.Trim()) { $Workspaces = $ans.Split(',').Trim() }
    }
    if (-not $PSBoundParameters.ContainsKey('DbPort')) {
        $ans = Read-Host "Postgres host port [5432]"
        if ($ans.Trim()) { $DbPort = [int]$ans }
    }
    if (-not $SkipVcpkg.IsPresent) {
        $ans = Read-Host "Skip vcpkg dependency build? (y/N)"
        if ($ans -match '^(y|yes)$') { $SkipVcpkg = $true }
    }
}

# Forward inputs via env vars the leaf scripts/compose already read.
$env:_APH_NOPAUSE = '1'                  # defuse the leaf scripts' pause-hang
$env:POSTGRES_PORT = "$DbPort"           # docker-compose reads ${POSTGRES_PORT:-5432}
if ($VcpkgRoot) { $env:VCPKG_ROOT = $VcpkgRoot }

# Runs one leaf step: emit start, (optionally) invoke, emit ok/fail.
# In DryRun we never invoke -- we just emit the markers so the orchestration
# logic is testable without building anything.
function Step([string]$id, [scriptblock]$action) {
    Wiz "step=$id status=start"
    if ($DryRun) { Wiz "step=$id status=ok"; return }
    try {
        & $action
        if ($LASTEXITCODE -ne 0) { throw "exit $LASTEXITCODE" }
        Wiz "step=$id status=ok"
    } catch {
        $m = "$($_.Exception.Message)" -replace '"', "'"
        Wiz "step=$id status=fail msg=`"$m`""
        Wiz "done status=fail"
        exit 1
    }
}

# --- doctor ---------------------------------------------------------------
if (-not $SkipDoctor) {
    Wiz "step=doctor status=start"
    if ($DryRun) {
        Wiz "step=doctor status=ok"
    } else {
        $env:_APH_WIZ = '1'              # make doctor.bat emit @@WIZ doctor rows
        & cmd /c "`"$RepoRoot\scripts\doctor.bat`""
        $doctorExit = $LASTEXITCODE
        Remove-Item Env:\_APH_WIZ
        if ($doctorExit -ne 0) {
            Wiz "step=doctor status=fail msg=`"prerequisites missing -- see doctor rows`""
            Wiz "done status=fail"
            exit 1
        }
        Wiz "step=doctor status=ok"
    }
}
if ($DoctorOnly) { Wiz "done status=ok"; exit 0 }

# --- server ---------------------------------------------------------------
if ($Workspaces -contains 'server') {
    if (-not $SkipVcpkg) {
        Step 'server-vcpkg' { & cmd /c "`"$RepoRoot\Server\scripts\setup-vcpkg-deps.bat`"" }
    }
    Step 'server-generate' { & cmd /c "`"$RepoRoot\Server\GenerateProjects.bat`"" }
    Step 'server-db'       { & cmd /c "`"$RepoRoot\Server\scripts\db-setup.bat`"" }
}

# --- arcane ---------------------------------------------------------------
if ($Workspaces -contains 'arcane') {
    if (-not $SkipVcpkg) {
        Step 'arcane-vcpkg' { & cmd /c "`"$RepoRoot\Arcane\scripts\setup-vcpkg-deps.bat`"" }
    }
    Step 'arcane-generate' { & cmd /c "`"$RepoRoot\Arcane\GenerateProjects.bat`"" }
}

# --- client (stub) --------------------------------------------------------
if ($Workspaces -contains 'client') {
    Wiz "step=client status=start"
    Info "Client (Love2D) has no build/setup step yet -- run it with Client\run.bat."
    Wiz "step=client status=ok"
}

# --- optional build -------------------------------------------------------
if ($Build -and -not $DryRun) {
    Step 'build' {
        $msbuild = & "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe" `
            -latest -products * -requires Microsoft.Component.MSBuild `
            -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
        if (-not $msbuild) { throw "MSBuild not found via vswhere" }
        if ($Workspaces -contains 'server') { & $msbuild "$RepoRoot\Server\Aphelyon.slnx" "/p:Configuration=$Config" /m }
        if ($LASTEXITCODE -ne 0) { throw "Server build exit $LASTEXITCODE" }
        if ($Workspaces -contains 'arcane') { & $msbuild "$RepoRoot\Arcane\Arcane.slnx" "/p:Configuration=$Config" /m }
    }
} elseif ($Build -and $DryRun) {
    Wiz "step=build status=start"; Wiz "step=build status=ok"
}

Wiz "done status=ok"
Info ""
Info "Setup complete. Next: open Server\Aphelyon.slnx, run Server\scripts\start-all.bat, then Client\run.bat."
exit 0
```

- [ ] **Step 4: Replace `scripts/setup.bat` with a shim**

Overwrite `scripts/setup.bat` with:

```bat
@echo off
REM Thin shim -- the real orchestrator is scripts\setup.ps1. Forwards all args.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup.ps1" %*
exit /b %ERRORLEVEL%
```

- [ ] **Step 5: Run the smoke harness; iterate until green**

Run: `pwsh -NoProfile -File tools\setup-wizard\tests\orchestrator-smoke.ps1`
Expected: `SMOKE PASSED` (all four assertions ok). If `pwsh` is unavailable, run with `powershell`.

- [ ] **Step 6: Spot-check the doctor wrapper for real**

Run: `pwsh -NoProfile -File scripts\setup.ps1 -DoctorOnly -NonInteractive | findstr "@@WIZ"`
Expected: `@@WIZ step=doctor status=start`, a series of `@@WIZ doctor item=...` rows (forwarded from `doctor.bat`), then `@@WIZ step=doctor status=ok` (or `status=fail` + `done=fail` if a prereq is genuinely missing).

- [ ] **Step 7: Commit**

```bash
git add scripts/setup.ps1 scripts/setup.bat tools/setup-wizard/tests/orchestrator-smoke.ps1
git commit -m "feat(scripts): headless setup orchestrator (setup.ps1) + smoke tests; setup.bat becomes a shim"
```

---

### Task 3: Scaffold the Tauri + Svelte wizard

**Files:**
- Create: the `tools/setup-wizard/` project (scaffolded), `tools/setup-wizard/.gitignore`
- Modify: `tools/setup-wizard/src-tauri/tauri.conf.json`

- [ ] **Step 1: Scaffold**

From the repo root:

```bash
npm create tauri-app@latest setup-wizard -- --manager npm --template svelte-ts --identifier com.aphelyon.setup
```

Run it so it creates a `setup-wizard/` folder, then move its contents under `tools/setup-wizard/` (the `tests/` dir from Task 2 already lives there — preserve it). The scaffold pins Tauri 2.x, Svelte 5, Vite, TypeScript. Record the exact scaffolded versions in the commit body (they are the authoritative API per the adaptation rule).

- [ ] **Step 2: Write `tools/setup-wizard/.gitignore`**

```gitignore
node_modules/
dist/
src-tauri/target/
src-tauri/gen/
```

(The committed `Setup.exe` lives at the repo ROOT, not here, so it is not ignored by this file.)

- [ ] **Step 3: Configure the window + bundle in `src-tauri/tauri.conf.json`**

Set the product name and a single fixed-size window. Edit the `productName`, `app.windows[0]`, and `bundle` keys (exact schema is whatever the scaffold produced — keep its structure, change these values):

```json
{
  "productName": "Aphelyon Setup",
  "app": {
    "windows": [
      { "title": "Aphelyon Setup", "width": 720, "height": 560, "resizable": false }
    ]
  },
  "bundle": { "active": true, "targets": ["nsis"] }
}
```

- [ ] **Step 4: Verify dev build runs**

Run (from `tools/setup-wizard/`): `npm install` then `npm run tauri dev`
Expected: the default scaffold window opens (we replace its contents in Tasks 4-5). Close it.

- [ ] **Step 5: Verify a release build produces an exe**

Run (from `tools/setup-wizard/`): `npm run tauri build`
Expected: a `.exe` is produced under `src-tauri/target/release/` (e.g. `aphelyon-setup.exe` or per the productName). Note the exact output path — Task 6 copies it to the repo root as `Setup.exe`.

- [ ] **Step 6: Commit**

```bash
git add tools/setup-wizard
git commit -m "feat(setup-wizard): scaffold Tauri 2 + Svelte 5 project under tools/setup-wizard"
```

---

### Task 4: Rust core — spawn, stream, and parse `@@WIZ`

**Files:**
- Create: `tools/setup-wizard/src-tauri/src/orchestrator.rs`
- Modify: `tools/setup-wizard/src-tauri/src/main.rs` (or `lib.rs` per scaffold)
- Modify: `tools/setup-wizard/src-tauri/capabilities/default.json`
- Modify: `tools/setup-wizard/src-tauri/Cargo.toml`

- [ ] **Step 1: Write the marker parser with a failing unit test**

Create `tools/setup-wizard/src-tauri/src/orchestrator.rs`:

```rust
// Parses the orchestrator's @@WIZ marker lines (see the spec) into typed
// events. Any non-marker line is plain log text. This module is pure and
// unit-tested; spawning/streaming lives in run_setup below.

use serde::Serialize;

#[derive(Debug, Clone, Serialize, PartialEq)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum WizEvent {
    Step { id: String, status: String, msg: Option<String> },
    Doctor { item: String, status: String, msg: Option<String> },
    Done { status: String },
    Log { line: String },
}

/// Parse one line of orchestrator output into a WizEvent.
pub fn parse_line(line: &str) -> WizEvent {
    let Some(rest) = line.strip_prefix("@@WIZ ") else {
        return WizEvent::Log { line: line.to_string() };
    };
    let (head, msg) = split_msg(rest);
    let fields = parse_fields(head);
    let get = |k: &str| fields.iter().find(|(a, _)| a == k).map(|(_, b)| b.clone());

    if let Some(id) = get("step") {
        WizEvent::Step { id, status: get("status").unwrap_or_default(), msg }
    } else if let Some(item) = get("item") {
        WizEvent::Doctor { item, status: get("status").unwrap_or_default(), msg }
    } else if let Some(status) = get("done") {
        // grammar: `done status=...` -> "done" key has no value; status is separate
        WizEvent::Done { status }
    } else if head.starts_with("done") {
        WizEvent::Done { status: get("status").unwrap_or_default() }
    } else {
        WizEvent::Log { line: line.to_string() }
    }
}

// Splits trailing `msg="..."` (always last) from the key=value head.
fn split_msg(rest: &str) -> (&str, Option<String>) {
    if let Some(idx) = rest.find("msg=\"") {
        let head = rest[..idx].trim_end();
        let after = &rest[idx + 5..];
        let val = after.strip_suffix('"').unwrap_or(after);
        (head, Some(val.to_string()))
    } else {
        (rest, None)
    }
}

fn parse_fields(head: &str) -> Vec<(String, String)> {
    head.split_whitespace()
        .filter_map(|tok| tok.split_once('='))
        .map(|(k, v)| (k.to_string(), v.to_string()))
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_step_ok() {
        assert_eq!(parse_line("@@WIZ step=server-vcpkg status=ok"),
                   WizEvent::Step { id: "server-vcpkg".into(), status: "ok".into(), msg: None });
    }
    #[test]
    fn parses_step_fail_with_msg() {
        assert_eq!(parse_line("@@WIZ step=server-db status=fail msg=\"exit 1\""),
                   WizEvent::Step { id: "server-db".into(), status: "fail".into(), msg: Some("exit 1".into()) });
    }
    #[test]
    fn parses_doctor_row() {
        assert_eq!(parse_line("@@WIZ doctor item=Visual Studio status=fail msg=\"not detected\""),
                   WizEvent::Doctor { item: "Visual Studio".into(), status: "fail".into(), msg: Some("not detected".into()) });
    }
    #[test]
    fn parses_done() {
        assert_eq!(parse_line("@@WIZ done status=ok"), WizEvent::Done { status: "ok".into() });
    }
    #[test]
    fn plain_line_is_log() {
        assert_eq!(parse_line("Bringing up Postgres..."),
                   WizEvent::Log { line: "Bringing up Postgres...".into() });
    }
}
```

NOTE: `doctor item=Visual Studio` has a space in the value. `parse_fields` splits on whitespace, so `item` would capture only `Visual`. The test `parses_doctor_row` expects the full `"Visual Studio"`. Resolve by having `doctor.bat` emit `item` with NO spaces is wrong (names have spaces). Instead: treat `item=` as "everything up to ` status=`". Implement `get("item")` specially:

```rust
// Replace the item extraction: item runs from after "item=" to " status=".
fn doctor_item(head: &str) -> Option<String> {
    let start = head.find("item=")? + 5;
    let tail = &head[start..];
    let end = tail.find(" status=").unwrap_or(tail.len());
    Some(tail[..end].to_string())
}
```

Use `doctor_item(head)` instead of `get("item")` in `parse_line`. Keep `status` via `get("status")` (status values have no spaces).

- [ ] **Step 2: Run the parser test to confirm red, then green**

Run (from `src-tauri/`): `cargo test`
Expected first: `parses_doctor_row` FAILS (space-in-item) until `doctor_item` is wired; the others pass. After wiring `doctor_item`, run again: all 5 pass.

- [ ] **Step 3: Add the spawn/stream command in `main.rs`**

Add the shell/process plugin to `Cargo.toml` dependencies (use the version the scaffold's Tauri pins; e.g. `tauri-plugin-shell = "2"`), then in `main.rs` register the module and the commands. The command spawns `powershell -File <repo>/scripts/setup.ps1 <flags>`, reads stdout line by line, parses each via `parse_line`, and emits a `wiz` event per line to the window:

```rust
mod orchestrator;
use orchestrator::parse_line;
use tauri::{Emitter, Manager};
use std::process::{Command, Stdio};
use std::io::{BufRead, BufReader};

#[derive(serde::Deserialize)]
struct SetupArgs {
    workspaces: Vec<String>,
    vcpkg_root: Option<String>,
    db_port: u16,
    skip_vcpkg: bool,
    config: String,
    build: bool,
    doctor_only: bool,
}

fn repo_root() -> std::path::PathBuf {
    // The exe is committed at the repo root, so the repo root is the exe's dir.
    // In `tauri dev` it is the cwd; fall back to current_dir.
    std::env::current_exe().ok()
        .and_then(|p| p.parent().map(|d| d.to_path_buf()))
        .unwrap_or_else(|| std::env::current_dir().unwrap())
}

fn build_args(a: &SetupArgs) -> Vec<String> {
    let mut v = vec!["-NonInteractive".into(),
                     "-Workspaces".into(), a.workspaces.join(",")];
    if a.doctor_only { v.push("-DoctorOnly".into()); }
    if a.skip_vcpkg  { v.push("-SkipVcpkg".into()); }
    if a.build       { v.push("-Build".into()); }
    v.push("-DbPort".into());  v.push(a.db_port.to_string());
    v.push("-Config".into());  v.push(a.config.clone());
    if let Some(r) = &a.vcpkg_root { v.push("-VcpkgRoot".into()); v.push(r.clone()); }
    v
}

#[tauri::command]
async fn run_setup(window: tauri::Window, args: SetupArgs) -> Result<(), String> {
    let root = repo_root();
    let script = root.join("scripts").join("setup.ps1");
    let mut ps = vec!["-NoProfile".to_string(), "-ExecutionPolicy".into(),
                      "Bypass".into(), "-File".into(),
                      script.to_string_lossy().to_string()];
    ps.extend(build_args(&args));

    let mut child = Command::new("powershell")
        .args(&ps).current_dir(&root)
        .stdout(Stdio::piped()).stderr(Stdio::piped())
        .spawn().map_err(|e| e.to_string())?;

    let stdout = child.stdout.take().ok_or("no stdout")?;
    for line in BufReader::new(stdout).lines().map_while(Result::ok) {
        let ev = parse_line(&line);
        let _ = window.emit("wiz", ev);
    }
    let status = child.wait().map_err(|e| e.to_string())?;
    if status.success() { Ok(()) } else { Err(format!("setup exited {:?}", status.code())) }
}

#[tauri::command]
async fn run_doctor(window: tauri::Window) -> Result<(), String> {
    run_setup(window, SetupArgs {
        workspaces: vec!["server".into()], vcpkg_root: None, db_port: 5432,
        skip_vcpkg: false, config: "Debug".into(), build: false, doctor_only: true,
    }).await
}
```

Add a `cancel` command so closing/aborting does not orphan the spawned `powershell`. Track the child in Tauri managed state and kill it:

```rust
use std::sync::Mutex;
struct Running(Mutex<Option<std::process::Child>>);   // .manage(Running(Mutex::new(None)))

// In run_setup, after spawn: store the child so cancel can reach it.
// (spawn into the state instead of a local `child`, then take it back to wait.)
#[tauri::command]
fn cancel(state: tauri::State<Running>) {
    if let Some(mut c) = state.0.lock().unwrap().take() { let _ = c.kill(); }
}
```

In `run_setup`, put the spawned child into `state.0` before the read loop and `take()` it back to `wait()`; on the loop ending normally, `cancel` finds `None` and is a no-op. Register all three: `invoke_handler(tauri::generate_handler![run_setup, run_doctor, cancel])` and `.manage(Running(Mutex::new(None)))`. (Adapt `Emitter`/`Manager`/`State` imports and the builder call to the scaffold's Tauri 2 API per the adaptation rule.)

- [ ] **Step 4: Capabilities — allow spawning powershell**

In `src-tauri/capabilities/default.json`, ensure the window has permission to run the shell command. With `tauri-plugin-shell`, add the `shell:allow-execute` (or the scaffold's equivalent) permission scoped to `powershell`. Exact permission identifiers come from the plugin version the scaffold pinned — adapt, keep the scope limited to `powershell`.

- [ ] **Step 5: Build to confirm it compiles + the test passes**

Run (from `src-tauri/`): `cargo test` (parser: 5 pass) then from `tools/setup-wizard/`: `npm run tauri build` (compiles the command into the app).

- [ ] **Step 6: Commit**

```bash
git add tools/setup-wizard/src-tauri
git commit -m "feat(setup-wizard): Rust core - spawn setup.ps1, stream output, parse @@WIZ markers (unit-tested)"
```

---

### Task 5: Svelte UI — store + three screens

**Files:**
- Create: `tools/setup-wizard/src/lib/wizard.svelte.ts`, `src/lib/wizard.test.ts`
- Create: `src/lib/screens/Doctor.svelte`, `Options.svelte`, `Run.svelte`
- Modify: `src/App.svelte`, `src/main.ts`

- [ ] **Step 1: Write the event->state reducer with a failing Vitest test**

Create `tools/setup-wizard/src/lib/wizard.test.ts`:

```ts
import { describe, it, expect } from 'vitest';
import { reduce, initialState, type WizEvent } from './wizard.svelte';

describe('wizard reducer', () => {
  it('marks a step running then ok', () => {
    let s = initialState();
    s = reduce(s, { kind: 'step', id: 'server-db', status: 'start' } as WizEvent);
    expect(s.steps['server-db']).toBe('running');
    s = reduce(s, { kind: 'step', id: 'server-db', status: 'ok' } as WizEvent);
    expect(s.steps['server-db']).toBe('ok');
  });

  it('records a step failure message', () => {
    let s = initialState();
    s = reduce(s, { kind: 'step', id: 'server-db', status: 'fail', msg: 'exit 1' } as WizEvent);
    expect(s.steps['server-db']).toBe('fail');
    expect(s.error).toBe('exit 1');
  });

  it('collects doctor rows and a failing row blocks continue', () => {
    let s = initialState();
    s = reduce(s, { kind: 'doctor', item: 'docker', status: 'fail', msg: 'not running' } as WizEvent);
    expect(s.doctor.find(d => d.item === 'docker')?.status).toBe('fail');
    expect(s.doctorOk).toBe(false);
  });

  it('warnings do not block continue', () => {
    let s = initialState();
    s = reduce(s, { kind: 'doctor', item: 'git', status: 'warn' } as WizEvent);
    expect(s.doctorOk).toBe(true);
  });

  it('appends non-marker log lines', () => {
    let s = initialState();
    s = reduce(s, { kind: 'log', line: 'Bringing up Postgres...' } as WizEvent);
    expect(s.log.at(-1)).toBe('Bringing up Postgres...');
  });

  it('done=ok sets finished', () => {
    let s = initialState();
    s = reduce(s, { kind: 'done', status: 'ok' } as WizEvent);
    expect(s.finished).toBe('ok');
  });
});
```

- [ ] **Step 2: Write `wizard.svelte.ts` to pass it**

```ts
// Pure reducer over @@WIZ events (typed to match orchestrator.rs WizEvent).
// Kept framework-free so it is unit-testable; the Svelte screens hold an
// instance in a $state rune and call reduce() on each Tauri 'wiz' event.

export type WizEvent =
  | { kind: 'step'; id: string; status: string; msg?: string }
  | { kind: 'doctor'; item: string; status: string; msg?: string }
  | { kind: 'done'; status: string }
  | { kind: 'log'; line: string };

export type StepStatus = 'pending' | 'running' | 'ok' | 'fail';

export interface WizState {
  steps: Record<string, StepStatus>;
  doctor: { item: string; status: string; msg?: string }[];
  doctorOk: boolean;
  log: string[];
  error?: string;
  finished?: 'ok' | 'fail';
}

export function initialState(): WizState {
  return { steps: {}, doctor: [], doctorOk: true, log: [], finished: undefined };
}

export function reduce(s: WizState, e: WizEvent): WizState {
  switch (e.kind) {
    case 'step': {
      const status: StepStatus = e.status === 'start' ? 'running'
        : e.status === 'ok' ? 'ok' : e.status === 'fail' ? 'fail' : 'pending';
      const steps = { ...s.steps, [e.id]: status };
      return { ...s, steps, error: e.status === 'fail' ? e.msg ?? s.error : s.error };
    }
    case 'doctor': {
      const doctor = [...s.doctor.filter(d => d.item !== e.item),
                      { item: e.item, status: e.status, msg: e.msg }];
      const doctorOk = doctor.every(d => d.status !== 'fail');
      return { ...s, doctor, doctorOk };
    }
    case 'done':
      return { ...s, finished: e.status === 'ok' ? 'ok' : 'fail' };
    case 'log':
      return { ...s, log: [...s.log, e.line] };
  }
}
```

- [ ] **Step 3: Run Vitest red->green**

Run (from `tools/setup-wizard/`): `npm run test` (add a `"test": "vitest run"` script to `package.json` if the scaffold lacks one; `npm i -D vitest`).
Expected: 6 tests pass.

- [ ] **Step 4: Write the three screens + App router**

`src/App.svelte` holds the current screen (`$state`) and a shared `WizState`, subscribes to the Tauri `wiz` event once, and routes Doctor -> Options -> Run. Real Svelte 5 (runes):

```svelte
<script lang="ts">
  import { listen } from '@tauri-apps/api/event';
  import { invoke } from '@tauri-apps/api/core';
  import { reduce, initialState, type WizEvent, type WizState } from './lib/wizard.svelte';
  import Doctor from './lib/screens/Doctor.svelte';
  import Options from './lib/screens/Options.svelte';
  import Run from './lib/screens/Run.svelte';

  let screen = $state<'doctor' | 'options' | 'run'>('doctor');
  let st = $state<WizState>(initialState());

  listen<WizEvent>('wiz', (e) => { st = reduce(st, e.payload); });

  async function runDoctor() { st = initialState(); await invoke('run_doctor'); }
  async function runSetup(args: any) { screen = 'run'; await invoke('run_setup', { args }); }

  $effect(() => { runDoctor(); });   // run doctor on launch
</script>

{#if screen === 'doctor'}
  <Doctor {st} onContinue={() => (screen = 'options')} onRecheck={runDoctor} />
{:else if screen === 'options'}
  <Options onBack={() => (screen = 'doctor')} onRun={runSetup} />
{:else}
  <Run {st} />
{/if}
```

`src/lib/screens/Doctor.svelte` — list `st.doctor` rows green/red, disable Continue unless `st.doctorOk`:

```svelte
<script lang="ts">
  let { st, onContinue, onRecheck } = $props();
</script>
<h1>Prerequisites</h1>
<ul>
  {#each st.doctor as row}
    <li class={row.status}>
      <b>{row.item}</b> — {row.status}{#if row.msg} ({row.msg}){/if}
    </li>
  {/each}
</ul>
<button onclick={onRecheck}>Re-check</button>
<button disabled={!st.doctorOk} onclick={onContinue}>Continue</button>
<style>
  .pass { color: #2a2; } .warn { color: #b80; } .fail { color: #c22; }
</style>
```

`src/lib/screens/Options.svelte` — workspace checkboxes, VCPKG_ROOT, DB port, skip-vcpkg, config, build toggle; emits the args object:

```svelte
<script lang="ts">
  let { onBack, onRun } = $props();
  let server = $state(true), arcane = $state(true), client = $state(true);
  let vcpkgRoot = $state(''), dbPort = $state(5432), skipVcpkg = $state(false);
  let config = $state('Debug'), build = $state(false);
  function go() {
    const workspaces = [server && 'server', arcane && 'arcane', client && 'client'].filter(Boolean);
    onRun({ workspaces, vcpkgRoot: vcpkgRoot || null, dbPort, skipVcpkg, config, build, doctorOnly: false });
  }
</script>
<h1>Options</h1>
<label><input type="checkbox" bind:checked={server} /> Server (services + DB)</label>
<label><input type="checkbox" bind:checked={arcane} /> Arcane engine</label>
<label><input type="checkbox" bind:checked={client} /> Client</label>
<label>VCPKG_ROOT <input bind:value={vcpkgRoot} placeholder="auto-detect" /></label>
<label>DB port <input type="number" bind:value={dbPort} /></label>
<label><input type="checkbox" bind:checked={skipVcpkg} /> Skip vcpkg build</label>
<label>Build after <input type="checkbox" bind:checked={build} /></label>
<select bind:value={config}><option>Debug</option><option>Release</option><option>Dist</option></select>
<button onclick={onBack}>Back</button>
<button onclick={go}>Run setup</button>
```

`src/lib/screens/Run.svelte` — step checklist + live log + final state:

```svelte
<script lang="ts">
  import { invoke } from '@tauri-apps/api/core';
  let { st } = $props();
  const order = ['doctor','server-vcpkg','server-generate','server-db','arcane-vcpkg','arcane-generate','client','build'];
  const glyph = (s: string) => s === 'ok' ? '[x]' : s === 'fail' ? '[!]' : s === 'running' ? '[~]' : '[ ]';
</script>
<h1>Running setup</h1>
{#if !st.finished}<button onclick={() => invoke('cancel')}>Cancel</button>{/if}
<ul>
  {#each order.filter(id => st.steps[id]) as id}
    <li>{glyph(st.steps[id])} {id}</li>
  {/each}
</ul>
{#if st.finished === 'ok'}<p class="ok">Setup complete.</p>{/if}
{#if st.finished === 'fail'}<p class="fail">Setup failed: {st.error}</p>{/if}
<pre>{st.log.slice(-200).join('\n')}</pre>
<style>.ok{color:#2a2}.fail{color:#c22}</style>
```

- [ ] **Step 5: Manual smoke of the GUI against a dry run**

Temporarily point `run_setup` to add `-DryRun` (or add a hidden dev toggle), run `npm run tauri dev`, and confirm: doctor screen populates with green/red rows, Continue gates on failures, Options collects inputs, Run shows the step checklist ticking through. Remove the temporary `-DryRun` before committing. (Full real-run verification is Task 7.)

- [ ] **Step 6: Commit**

```bash
git add tools/setup-wizard/src tools/setup-wizard/package.json
git commit -m "feat(setup-wizard): Svelte UI - doctor/options/run screens over a unit-tested event reducer"
```

---

### Task 6: Distribution — committed `Setup.exe` + CI rebuild-and-commit

**Files:**
- Create: `Setup.exe` (repo root, bootstrap build)
- Create: `.github/workflows/build-setup-wizard.yml`

- [ ] **Step 1: Bootstrap-build and commit the first `Setup.exe`**

From `tools/setup-wizard/`: `npm install && npm run tauri build`. Copy the produced exe (path noted in Task 3 Step 5, e.g. `src-tauri/target/release/aphelyon-setup.exe`) to the repo root as `Setup.exe`:

```bash
cp tools/setup-wizard/src-tauri/target/release/aphelyon-setup.exe Setup.exe
git add Setup.exe
git commit -m "build(setup-wizard): commit prebuilt Setup.exe (bootstrap)"
```

(If the team prefers the NSIS installer over the raw exe, copy that artifact instead and name it `Setup.exe` — keep the committed name stable so the README link never breaks.)

- [ ] **Step 2: Write the CI workflow that keeps it fresh**

Create `.github/workflows/build-setup-wizard.yml`. It triggers ONLY on wizard-source changes (path filter excludes `Setup.exe`, so the bot's own commit cannot re-trigger it — the loop guard), builds on a Windows runner, and commits the refreshed exe back:

```yaml
name: build-setup-wizard
on:
  push:
    paths:
      - 'tools/setup-wizard/**'
      - '.github/workflows/build-setup-wizard.yml'
permissions:
  contents: write
jobs:
  build:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-node@v4
        with: { node-version: '20' }
      - uses: dtolnay/rust-toolchain@stable
      - name: Build wizard
        working-directory: tools/setup-wizard
        run: |
          npm ci
          npm run tauri build
      - name: Stage Setup.exe
        shell: pwsh
        run: |
          $exe = Get-ChildItem tools/setup-wizard/src-tauri/target/release -Filter *.exe |
                 Where-Object { $_.Name -notmatch 'setup-?\.exe$' } | Select-Object -First 1
          Copy-Item $exe.FullName Setup.exe -Force
      - name: Commit refreshed exe
        shell: pwsh
        run: |
          git config user.name  "StarworksBuilder"
          git config user.email "builder@starworks.dev"
          git add Setup.exe
          if (git diff --cached --quiet) { Write-Host "no change"; exit 0 }
          git commit -m "build(setup-wizard): refresh Setup.exe [skip ci]"
          git push
```

NOTE on the CI choice: this uses GitHub Actions because the Tauri toolchain (Rust+Node) is trivially available on `windows-latest`, and the build is isolated from the main Jenkins pipeline. If the team prefers to keep all CI in Jenkins (StarworksBuilder already has push rights), port these same steps to a Jenkins job gated on `tools/setup-wizard/**` and have it build with the agent's Rust+Node (install them on `windows-1` first). Either way the path filter / `[skip ci]` is the loop guard — do not trigger on `Setup.exe` changes.

- [ ] **Step 3: Verify the workflow's exe-selection locally**

Run the `Stage Setup.exe` PowerShell snippet locally against your `target/release` dir and confirm it picks the app exe (not a stray helper). Adjust the `-notmatch` filter to the actual artifact name from Task 3 Step 5 so it is unambiguous.

- [ ] **Step 4: Commit**

```bash
git add .github/workflows/build-setup-wizard.yml
git commit -m "ci(setup-wizard): rebuild + commit Setup.exe on wizard-source changes (path-filtered loop guard)"
```

---

### Task 7: Docs + full first-run verification

**Files:**
- Modify: `README.md`, `CLAUDE.md`

- [ ] **Step 1: README onboarding section**

Add a top-of-README "Getting started" block:

```markdown
## Getting started

1. Clone the repo (any git client).
2. Double-click **`Setup.exe`** in the repo root and follow the wizard.

The wizard checks prerequisites (Visual Studio + Docker Desktop must be
installed — it tells you if they're missing), then sets up the Server, Arcane
engine, and Client. Prefer the command line / CI? Run `scripts\setup.ps1`
(see `-?` for flags) — the wizard is just a GUI over that script.
```

- [ ] **Step 2: CLAUDE.md note**

Under the build/scripts area, add: "First-time setup: `Setup.exe` (repo root) is a Tauri+Svelte GUI over `scripts/setup.ps1`, the headless orchestrator that sequences the per-workspace setup scripts. The exe is prebuilt and CI-maintained; source in `tools/setup-wizard/`. CLI/CI use `scripts/setup.ps1` directly."

- [ ] **Step 3: Full unattended run (the real thing) on Server + Arcane**

Run: `pwsh -NoProfile -File scripts\setup.ps1 -NonInteractive -Workspaces server,arcane`
Expected: doctor passes, vcpkg deps build (or are already present), both solutions generate, Postgres comes up and `schema.sql`/`seed.sql` apply, ends with `@@WIZ done status=ok` and exit 0. (Honors the "never run db-reset/down -v" rule — `db-setup.bat` only brings the container up and applies schema.)

- [ ] **Step 4: GUI run-through (manual)**

Double-click the committed `Setup.exe`. Confirm: doctor rows render green/red and Continue gates on failures; Options collects inputs; Run streams the step checklist to completion with the live log; success state shows. Note this is the human acceptance gate.

- [ ] **Step 5: Commit**

```bash
git add README.md CLAUDE.md
git commit -m "docs: setup wizard onboarding (Setup.exe -> setup.ps1)"
```

---

## Exit criteria

- `doctor.bat` no longer fails on the removed migrations dir; checks `schema.sql`/`seed.sql`; emits opt-in `@@WIZ doctor` rows.
- `scripts/setup.ps1` orchestrates Server/Arcane/Client behind flags + interactive prompts, forwards `VCPKG_ROOT`/`POSTGRES_PORT`/`_APH_NOPAUSE`, and emits the `@@WIZ` contract; `-DryRun` smoke tests green; a real `-NonInteractive` Server+Arcane run completes.
- `setup.bat` is a shim to `setup.ps1`.
- Tauri+Svelte wizard builds; Rust `@@WIZ` parser unit-tested (5 cases); Svelte reducer unit-tested (6 cases); the three screens drive a real run.
- `Setup.exe` committed at the repo root; CI rebuilds + commits it on wizard-source changes with a loop guard.
- README + CLAUDE.md document the `clone -> Setup.exe` flow and the `setup.ps1` CLI/CI path.
- Tools is absent from the orchestrator; host prereqs are detect-and-guide only.

## Out of scope (per spec)

Auto-installing VS/Docker; a Linux setup path; Client build beyond the stub; Pester (PS verification is scripted output-assertions).
