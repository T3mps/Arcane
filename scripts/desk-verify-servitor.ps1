# desk-verify-servitor.ps1 -- automates the mechanical half of Plan B's Task 14.
#
# Task 14 is the USER's acceptance checkpoint and needs a display and a real
# GPU, which is why it is not in CI. But most of its steps are mechanical:
# run the gate, break something, watch it fail, restore, bless, time it. This
# script does those and leaves you the parts that genuinely need eyes and a
# judgment call.
#
# WHAT IT WILL NOT DO, on purpose:
#   * It never blesses editor-ui. OWED DEFECT 2 is that the editor capture
#     depends on the machine's crash history (its Assets panel enumerates
#     Saved/Diagnostics/, which the editor itself writes into). Re-blessing
#     goes green today and red at the next crashed run, so blessing it would
#     manufacture a pass and destroy the evidence. Runtime-scene only.
#   * It never leaves the tree dirty. Every mutation is inside try/finally and
#     restored with `git checkout --`, and the last thing it does is assert
#     ReferenceProject/ is clean. Ctrl-C is safe.
#
# Usage:
#   scripts\desk-verify-servitor.ps1                    # phases A, B, C
#   scripts\desk-verify-servitor.ps1 -Phase A           # one phase
#   scripts\desk-verify-servitor.ps1 -Configuration Release
#
# TIME: each gate run does an unconditional /t:Rebuild of ReferenceProject
# (the single-slot Binaries/ precondition) plus four host launches. A full
# run is several gate invocations -- budget 20-40 minutes and do not walk
# away for the phase B timing step, which you are supposed to observe.
#
# Windows PowerShell 5.1 compatible.

param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [ValidateSet('All', 'A', 'B', 'C')]
    [string]$Phase = 'All'
)

$ErrorActionPreference = 'Stop'
$repoRoot   = Split-Path -Parent $PSScriptRoot
$gate       = Join-Path $repoRoot 'scripts\golden-gate.ps1'
$scene      = Join-Path $repoRoot 'ReferenceProject\Content\scenes\main.arcscene'
$configDir  = "$Configuration-windows-x86_64-md"
$runtimeDir = Join-Path $repoRoot "bin\$configDir\ArcaneRuntime"
$runtimeExe = Join-Path $runtimeDir 'ArcaneRuntime.exe'
$savedVerify = Join-Path $runtimeDir 'ReferenceProject\Saved\Verify'

# NOTE: the plan's Task 14 text says to break "test.arcscene". THAT FILE IS IN
# THE GACHA REPO, not this one. The reference scene here is main.arcscene and
# the mesh entity is "MeshCube".
$sceneRelative = 'ReferenceProject/Content/scenes/main.arcscene'
$refsRelative  = 'ReferenceProject/Verify/References'

function Write-Head($text) {
    Write-Host ""
    Write-Host "==================================================================" -ForegroundColor Cyan
    Write-Host "  $text" -ForegroundColor Cyan
    Write-Host "==================================================================" -ForegroundColor Cyan
}

function Write-Ask($text) {
    Write-Host ""
    Write-Host ">> YOUR CALL: $text" -ForegroundColor Magenta
}

function Assert-CleanReferenceProject {
    $dirty = & git -C $repoRoot status --porcelain -- ReferenceProject
    if ($dirty) {
        Write-Host "ReferenceProject/ IS DIRTY -- restoring:" -ForegroundColor Red
        $dirty | ForEach-Object { Write-Host "   $_" -ForegroundColor Red }
        & git -C $repoRoot checkout -- ReferenceProject
        $still = & git -C $repoRoot status --porcelain -- ReferenceProject
        if ($still) { throw "Could not restore ReferenceProject/ to clean. Fix by hand before continuing." }
        Write-Host "   restored." -ForegroundColor Green
    }
}

# Runs the gate and returns a hashtable: ExitCode, and the raw text so the
# caller can assert on lane verdicts rather than on the exit code alone --
# the same rule golden-gate.ps1 applies to the hosts it launches.
function Invoke-Gate([string]$why) {
    Write-Host ""
    Write-Host "-- running golden-gate ($Configuration) -- $why" -ForegroundColor DarkCyan
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $gate `
        -Configuration $Configuration -AdvisoryLanes ArcaneEditor 2>&1 | Out-String
    $code = $LASTEXITCODE
    Write-Host $out
    return @{ ExitCode = $code; Text = $out }
}

# The mutation: MeshCube's Transform position x, 0.4 -> 0.6. Visible, and a
# pure data edit that cannot break the scene's schema.
function Set-MeshCubeBroken {
    $json = Get-Content $scene -Raw
    if ($json -notmatch '(?s)"name":\s*"MeshCube".*?"Arcane::Transform":\s*\{\s*"position":\s*\[\s*0\.4') {
        throw "main.arcscene does not have MeshCube at position x=0.4 -- the scene changed. Re-derive this mutation before trusting the result."
    }
    $broken = [regex]::Replace($json,
        '(?s)("name":\s*"MeshCube".*?"Arcane::Transform":\s*\{\s*"position":\s*\[\s*)0\.4',
        '${1}0.6', 1)
    if ($broken -eq $json) { throw "Mutation did not apply -- refusing to continue with an unbroken scene." }
    Set-Content -Path $scene -Value $broken -Encoding UTF8 -NoNewline
    Write-Host "   MeshCube position x: 0.4 -> 0.6 (scene deliberately broken)" -ForegroundColor Yellow
}

function Restore-Scene {
    & git -C $repoRoot checkout -- $sceneRelative
    Write-Host "   scene restored from git." -ForegroundColor Green
}

Write-Head "Servitor desk verify -- $Configuration"
Write-Host "Repo:  $repoRoot"
Write-Host "Gate:  $gate"
Write-Host "Scene: $scene"
Assert-CleanReferenceProject

# =====================================================================
# PHASE A -- the gate does what it claims
# =====================================================================
if ($Phase -eq 'All' -or $Phase -eq 'A') {
    Write-Head "PHASE A1 -- baseline: the gate passes on an unmodified tree"
    Write-Host "EXPECTED (this is the corrected expectation -- the plan's text is stale):"
    Write-Host "  exit 0, TWO passing ArcaneRuntime comparisons (dx12 + vulkan),"
    Write-Host "  and TWO ArcaneEditor lanes reporting ADVISORY / KNOWN-RED."
    Write-Host "  The plan says 'four passing comparisons'. That is NO LONGER TRUE."

    $a1 = Invoke-Gate "A1 baseline"
    if ($a1.ExitCode -ne 0) {
        Write-Host "A1 FAILED: gate exited $($a1.ExitCode), expected 0." -ForegroundColor Red
        Write-Host "Stop here. A baseline that does not pass invalidates every step below." -ForegroundColor Red
        exit 1
    }
    Write-Host "A1 PASS -- gate exit 0 on a clean tree." -ForegroundColor Green
    Write-Ask "Did the two ArcaneEditor lanes print the OWED DEFECTS banner, and was it loud enough that you would actually notice it in a Jenkins log? If not, say so -- that is the stated cost of shipping them advisory, and it is one flag to reverse."

    Write-Head "PHASE A2 -- a gate never observed failing is not a gate"
    Write-Host "Breaking the scene, re-running, and asserting the RUNTIME lanes fail."
    try {
        Set-MeshCubeBroken
        $a2 = Invoke-Gate "A2 with a deliberately broken scene"
        if ($a2.ExitCode -eq 0) {
            Write-Host "A2 FAILED: the gate PASSED on a broken scene. This is the worst" -ForegroundColor Red
            Write-Host "possible result -- the gate is not gating. Investigate before anything else." -ForegroundColor Red
        } else {
            Write-Host "A2 PASS -- gate exit $($a2.ExitCode) on a broken scene (it caught it)." -ForegroundColor Green
        }
    } finally {
        Restore-Scene
        Assert-CleanReferenceProject
    }

    Write-Head "PHASE A3 -- read the diff image"
    Write-Host "Diff artifacts (from the A2 failing run) are under:"
    Write-Host "  $savedVerify"
    if (Test-Path $savedVerify) {
        Get-ChildItem $savedVerify -Filter '*-diff.png' -ErrorAction SilentlyContinue |
            ForEach-Object { Write-Host "   $($_.FullName)" -ForegroundColor Yellow }
    }
    Write-Ask "Open one runtime-scene diff PNG. RED should mark the real difference, YELLOW any antialiasing, and the background should be a WASHED-OUT GHOST of the reference -- not a copy of it. Is the cause obvious at a glance? If you have to hunt for it, the diff rendering is the finding."
}

# =====================================================================
# PHASE B -- blessing is cheap enough that nobody disables the gate
# =====================================================================
if ($Phase -eq 'All' -or $Phase -eq 'B') {
    Write-Head "PHASE B -- the bless round-trip, timed"
    Write-Host "Break -> gate fails -> --bless -> gate passes. Runtime-scene ONLY:"
    Write-Host "editor-ui is never blessed here (owed defect 2 -- its input is written"
    Write-Host "by the thing under test, so a bless manufactures a pass)."
    Write-Host ""
    Write-Host "THE SPEC'S WARNING, which this step exists to test: a gate nobody can"
    Write-Host "cheaply bless gets switched off in the first week. Time it honestly."

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        Set-MeshCubeBroken

        $b1 = Invoke-Gate "B1 -- confirm the break is caught"
        if ($b1.ExitCode -eq 0) {
            Write-Host "B1 unexpected PASS on a broken scene -- see phase A2. Aborting phase B." -ForegroundColor Red
        } else {
            Write-Host "B1 -- break caught (exit $($b1.ExitCode)). Now blessing runtime-scene on both backends." -ForegroundColor Green

            foreach ($backend in @('dx12', 'vulkan')) {
                Write-Host ""
                Write-Host "-- bless runtime-scene / $backend --" -ForegroundColor DarkCyan
                $blessArgs = @(
                    '--project', 'ReferenceProject',
                    '--offscreen',
                    '--backend', $backend,
                    '--frames', '60',
                    '--settle', '30',
                    '--compare', 'runtime-scene',
                    '--bless'
                )
                Write-Host "   $runtimeExe $($blessArgs -join ' ')" -ForegroundColor DarkGray
                $p = Start-Process -FilePath $runtimeExe -ArgumentList $blessArgs `
                    -WorkingDirectory $runtimeDir -NoNewWindow -Wait -PassThru
                if ($p.ExitCode -ne 0) {
                    Write-Host "   bless FAILED (exit $($p.ExitCode))" -ForegroundColor Red
                } else {
                    Write-Host "   blessed." -ForegroundColor Green
                }
            }

            $b2 = Invoke-Gate "B2 -- after blessing, the gate should pass again"
            if ($b2.ExitCode -eq 0) {
                Write-Host "B2 PASS -- blessing restored a green gate." -ForegroundColor Green
            } else {
                Write-Host "B2 FAILED -- the gate is still red after a bless (exit $($b2.ExitCode))." -ForegroundColor Red
                Write-Host "That is a real finding: it means blessing does not do what it claims." -ForegroundColor Red
            }
        }
    } finally {
        $sw.Stop()
        Write-Host ""
        Write-Host "-- restoring the scene AND the blessed references --" -ForegroundColor DarkCyan
        & git -C $repoRoot checkout -- $sceneRelative $refsRelative
        Assert-CleanReferenceProject
        Write-Host ""
        Write-Host "BLESS ROUND-TRIP ELAPSED: $([math]::Round($sw.Elapsed.TotalMinutes, 1)) minutes" -ForegroundColor Yellow
    }

    Write-Ask "Was that cheap enough that you would bless rather than disable the gate on a busy day? If it was awkward, SAY SO -- the spec's own prediction is that an expensive bless kills the gate in week one, and the Unreal research found the same failure mode independently."

    Write-Head "PHASE B2 -- where the bless landed"
    Write-Host "Confirm the bless wrote to the level the reference RESOLVED FROM."
    Write-Host ""
    Write-Host "CORRECTED EXPECTATION -- the plan says 'editor-ui backend-split,"
    Write-Host "runtime-scene shared'. THAT IS THE EXACT INVERSE of the truth:"
    Write-Host "  Verify/References/runtime-scene.png         <- dx12 (shared slot)"
    Write-Host "  Verify/References/vulkan/runtime-scene.png  <- backend override  ** SPLIT **"
    Write-Host "  Verify/References/editor-ui.png             <- ** SHARED **"
    Write-Host ""
    Write-Host "Actual tree:"
    Get-ChildItem (Join-Path $repoRoot 'ReferenceProject\Verify\References') -Recurse -Filter '*.png' |
        ForEach-Object { Write-Host "   $($_.FullName.Substring($repoRoot.Length + 1))" }
}

# =====================================================================
# PHASE C -- the layout seed, and the ignore form
# =====================================================================
if ($Phase -eq 'All' -or $Phase -eq 'C') {
    Write-Head "PHASE C -- layout seed and a clean tree after an editor session"
    Write-Host "The offscreen editor capture should show the SEEDED layout, visibly"
    Write-Host "different from BuildDefaultLayout."
    $editorRef = Join-Path $repoRoot 'ReferenceProject\Verify\References\editor-ui.png'
    Write-Host ""
    Write-Host "Reference to eyeball: $editorRef" -ForegroundColor Yellow
    Write-Ask "Open it. Does it show the seeded layout rather than the engine default?"

    Write-Host ""
    Write-Host "-- git status for ReferenceProject/ after this run --" -ForegroundColor DarkCyan
    $st = & git -C $repoRoot status --porcelain -- ReferenceProject
    if ($st) {
        Write-Host "DIRTY -- the Saved/* + !Saved/verify-layout.ini ignore form may not" -ForegroundColor Red
        Write-Host "hold now that Saved/Verify/ also receives artifacts. THIS IS A FINDING:" -ForegroundColor Red
        $st | ForEach-Object { Write-Host "   $_" -ForegroundColor Red }
    } else {
        Write-Host "CLEAN -- the ignore form holds." -ForegroundColor Green
    }
}

Write-Head "Desk verify finished"
Assert-CleanReferenceProject
Write-Host "ReferenceProject/ is clean." -ForegroundColor Green
Write-Host ""
Write-Host "NOT COVERED by this script, and named so they are not assumed closed:"
Write-Host "  * Section D -- driver reproduction: three WINDOWED [gpu] runs with Parsec"
Write-Host "    active, then scripts\check-faults.ps1 -Days 1. A negative result closes"
Write-Host "    it validly. Deliberately not automated: windowed GPU runs are the thing"
Write-Host "    the whole offscreen mode exists to avoid."
Write-Host "  * Section E -- cross-format parity (offscreen vs windowed). Optional, and"
Write-Host "    deliberately not automated for the same reason."
Write-Host "  * FastStats memory at 4K is arithmetic, not a measured run."
Write-Host "  * The Linux IsIdle() stub still returns true unconditionally."
Write-Host "  * Mesh picking is still unimplemented (CollectPickables has no MeshRenderer view)."
Write-Host ""
Write-Host "See docs\2026-08-26-servitor-closeout-and-desk-verify.md for the owed defects."
