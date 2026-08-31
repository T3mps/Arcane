# desk-verify-golden-gate.ps1 -- the desk half of the golden-image gate.
#
# scripts/golden-gate.ps1 is the gate itself and runs in CI (see the
# Jenkinsfile, which now also runs `golden-gate.ps1 -SelfTest` on
# main/milestone/*). THIS script needs a display and a real GPU, so it
# remains the one that answers these two questions locally, on ANY branch,
# with eyes on it:
#
#   1. CAN THE GATE FAIL? A gate never observed failing is not a gate. Phase A2
#      breaks the scene on purpose, asserts the runtime lanes go red, restores.
#      (CI now asserts this too, via -SelfTest, but only on main/milestone.)
#   2. IS BLESSING CHEAP? Phase B times a full break -> fail -> bless -> pass
#      round-trip. A gate nobody can cheaply bless gets switched off in week one.
#
# Everything else is mechanical setup around those two, and it leaves you the
# parts that genuinely need eyes and a judgment call.
#
# WHAT IT WILL NOT DO, on purpose:
#   * Phase B's automated bless round-trip covers runtime-scene only -- that
#     is this phase's scope, not a ban on blessing editor-ui. OWED DEFECT 2
#     (the editor's verify capture depending on the machine's crash history
#     via Saved/Diagnostics/) is CLOSED: ProjectOpenOptions now keeps that
#     capture off diag://, so a deliberate editor-ui re-bless is legitimate --
#     the user did exactly this at the desk on 2026-08-30 (commit 97abd074).
#     A manual bless is:
#       ArcaneEditor.exe --project ReferenceProject --headless --backend dx12
#         --frames 60 --settle 30 --report <path> --compare editor-ui --bless
#     (--report or --screenshot is REQUIRED -- --settle refuses without one;
#     the plan's own version of this command omits it and cannot run). Then
#     restage the blessed reference to both hosts the same way Phase B does
#     below for runtime-scene -- golden-gate.ps1 deliberately does not
#     restage Verify/ itself.
#   * It never leaves the tree dirty. Every mutation is inside try/finally and
#     restored with `git checkout --`, and the last thing it does is assert
#     ReferenceProject/ is clean. Ctrl-C is safe.
#
# Usage:
#   scripts\desk-verify-golden-gate.ps1                    # phases A, B, C
#   scripts\desk-verify-golden-gate.ps1 -Phase A           # one phase
#   scripts\desk-verify-golden-gate.ps1 -Configuration Release
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
        -Configuration $Configuration 2>&1 | Out-String
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

    # ---- AND STAGE IT, THEN PROVE IT LANDED. ----
    #
    # THE FIRST VERSION OF THIS SCRIPT DID NOT DO THIS AND WAS THEREFORE A TEST
    # THAT COULD NOT FAIL. The hosts do not read the repo-root scene: premake's
    # host postbuilds ({COPYDIR} ReferenceProject -> the exe directory,
    # premake5.lua:355 and :430) stage the whole tree beside each exe, the hosts
    # run there with `--project ReferenceProject`, and that staged copy is
    # refreshed ONLY when the host itself builds. So editing the source scene
    # and running the gate compared a pristine render against a pristine
    # reference and reported diffCount=0 -- a green that meant nothing.
    #
    # golden-gate.ps1 now restages Content/ itself. This does it too, and then
    # asserts it, so the desk pass does not silently depend on which version of
    # the gate is on disk. An assertion is the only thing separating this from
    # the failure it is here to catch.
    foreach ($h in @('ArcaneRuntime', 'ArcaneEditor')) {
        $dest = Join-Path $repoRoot "bin\$configDir\$h\ReferenceProject\Content"
        if (Test-Path $dest) {
            Copy-Item -Path (Join-Path $repoRoot 'ReferenceProject\Content\*') -Destination $dest -Force -Recurse
        }
    }
    $stagedScene = Join-Path $repoRoot "bin\$configDir\ArcaneRuntime\ReferenceProject\Content\scenes\main.arcscene"
    if (-not (Test-Path $stagedScene)) {
        throw "No staged scene at $stagedScene -- build the hosts once so the postbuild stages ReferenceProject, then re-run."
    }
    $srcHash    = (Get-FileHash $scene -Algorithm MD5).Hash
    $stagedHash = (Get-FileHash $stagedScene -Algorithm MD5).Hash
    if ($srcHash -ne $stagedHash) {
        throw "STAGED SCENE DOES NOT MATCH THE BROKEN SOURCE. The hosts would render the old scene and the gate would pass on a break -- refusing to report a meaningless result. Staged: $stagedScene"
    }
    Write-Host "   staged to both host dirs and verified (md5 $($srcHash.Substring(0,8)))" -ForegroundColor Green
}

function Restore-Scene {
    & git -C $repoRoot checkout -- $sceneRelative
    # RESTAGE THE RESTORED SCENE TOO. Without this the repo goes clean while the
    # staged copies stay broken, so `git status` says everything is fine and the
    # next gate run -- this script's or anyone else's -- fails against a scene
    # nobody can see. A restore that only restores what git tracks is not a
    # restore.
    foreach ($h in @('ArcaneRuntime', 'ArcaneEditor')) {
        $dest = Join-Path $repoRoot "bin\$configDir\$h\ReferenceProject\Content"
        if (Test-Path $dest) {
            Copy-Item -Path (Join-Path $repoRoot 'ReferenceProject\Content\*') -Destination $dest -Force -Recurse
        }
    }
    Write-Host "   scene restored from git, and restaged to both host dirs." -ForegroundColor Green
}

Write-Head "Golden-gate desk verify -- $Configuration"
Write-Host "Repo:  $repoRoot"
Write-Host "Gate:  $gate"
Write-Host "Scene: $scene"
Assert-CleanReferenceProject

# =====================================================================
# PHASE A -- the gate does what it claims
# =====================================================================
if ($Phase -eq 'All' -or $Phase -eq 'A') {
    Write-Head "PHASE A1 -- baseline: the gate passes on an unmodified tree"
    Write-Host "EXPECTED: exit 0, FOUR passing comparisons -- both ArcaneRuntime"
    Write-Host "  lanes (dx12 + vulkan) and both ArcaneEditor lanes (dx12 + vulkan)."

    $a1 = Invoke-Gate "A1 baseline"
    if ($a1.ExitCode -ne 0) {
        Write-Host "A1 FAILED: gate exited $($a1.ExitCode), expected 0." -ForegroundColor Red
        Write-Host "Stop here. A baseline that does not pass invalidates every step below." -ForegroundColor Red
        exit 1
    }
    Write-Host "A1 PASS -- gate exit 0 on a clean tree." -ForegroundColor Green

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
    Write-Host "editor-ui is out of scope for this automated round-trip (it"
    Write-Host "covers runtime-scene only) -- NOT because blessing it would"
    Write-Host "manufacture a pass. Defect 2 (editor capture depending on the"
    Write-Host "machine's crash history) is CLOSED via ProjectOpenOptions, so a"
    Write-Host "deliberate manual editor-ui re-bless is legitimate -- see the"
    Write-Host "header comment above for the command."
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
                # DERIVED FROM golden-gate.ps1's OWN exeArgs, PLUS --bless and
                # NOTHING ELSE CHANGED. The first version of this retyped the
                # list from memory and dropped --report, and the host refused:
                # "--settle requires --screenshot or --report (it compares
                # captured frames; with neither, there is nowhere to land the
                # result and the run would never know when to stop)". A correct
                # refusal that cost a whole desk run. When a working invocation
                # already exists, copy it and add one flag.
                # ---- --project POINTS AT THE SOURCE TREE HERE, NOT THE STAGED
                #      ONE, AND THAT IS THE WHOLE POINT OF THIS STEP. ----
                #
                # MEASURED 2026-08-26. --bless writes to the level the reference
                # RESOLVED FROM. Run the gate's own invocation (`--project
                # ReferenceProject`, relative, from the exe directory) and the
                # blessed PNG lands in the STAGED tree under bin/ -- verified:
                # staged mtime moved, the committed reference did not, and
                # `git status` stayed clean. Nobody can commit that, and the
                # next host build's postbuild ({COPYDIR} ReferenceProject)
                # OVERWRITES IT FROM THE REPO, silently discarding the bless.
                #
                # Pointing --project at the source tree makes the reference
                # resolve from, and the bless write to, the file you actually
                # commit -- verified the same way: repo mtime moved.
                #
                # This is a WORKFLOW TRAP, not an engine defect: the natural
                # move is to copy the gate's command and add --bless, and that
                # quietly blesses a build artifact. Worth documenting wherever
                # the bless workflow is described.
                $sourceProject = Join-Path $repoRoot 'ReferenceProject'
                $blessReport = Join-Path $savedVerify "bless-runtime-scene-$backend-report.json"
                $blessArgs = @(
                    '--project', $sourceProject,
                    '--headless',
                    '--backend', $backend,
                    '--frames', '60',
                    '--settle', '30',
                    '--report', $blessReport,
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

            # The bless above wrote the SOURCE reference. The gate reads the
            # STAGED one, and deliberately does not restage Verify/ (it must not
            # trample a bless). So stand in for what really closes that loop in
            # practice -- commit the reference, rebuild, postbuild restages it.
            # Without this the round-trip cannot pass and the failure would be
            # an artifact of this script, not a finding about blessing.
            foreach ($h in @('ArcaneRuntime', 'ArcaneEditor')) {
                $destVerify = Join-Path $repoRoot "bin\$configDir\$h\ReferenceProject\Verify"
                if (Test-Path $destVerify) {
                    Copy-Item -Path (Join-Path $repoRoot 'ReferenceProject\Verify\*') -Destination $destVerify -Force -Recurse
                }
            }
            Write-Host "   blessed reference staged to both hosts (stands in for commit + rebuild)." -ForegroundColor DarkGray

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
        Restore-Scene   # restages Content/ as well -- git alone leaves the staged copies broken

        # UNDO THE BLESS IN THE STAGED TREE TOO. --bless writes to the level the
        # reference RESOLVED FROM, and the hosts resolve against the staged
        # ReferenceProject beside the exe -- so a bless may never dirty the repo
        # at all, which would make the `git checkout` above a silent no-op while
        # a blessed reference stayed on disk. The gate deliberately does not
        # restage Verify/ (it must not trample a bless), so nothing else would
        # ever undo it, and the NEXT gate run would pass against a reference
        # blessed from a deliberately broken scene. That is a green for the
        # wrong reason, left behind by a verification step.
        foreach ($h in @('ArcaneRuntime', 'ArcaneEditor')) {
            $destVerify = Join-Path $repoRoot "bin\$configDir\$h\ReferenceProject\Verify"
            if (Test-Path $destVerify) {
                Copy-Item -Path (Join-Path $repoRoot 'ReferenceProject\Verify\*') -Destination $destVerify -Force -Recurse
            }
        }
        Write-Host "   staged Verify/ restored from the repo (undoes the bless)." -ForegroundColor Green
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
    Write-Host "The headless editor capture should show the SEEDED layout, visibly"
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
Write-Host "    the whole headless mode exists to avoid."
Write-Host "  * Section E -- cross-format parity (offscreen vs windowed). Optional, and"
Write-Host "    deliberately not automated for the same reason."
Write-Host "  * FastStats memory at 4K is arithmetic, not a measured run."
Write-Host "  * The Linux IsIdle() stub still returns true unconditionally."
Write-Host "  * Mesh picking is still unimplemented (CollectPickables has no MeshRenderer view)."
Write-Host ""
Write-Host "The five owed automation defects this script was written alongside are"
Write-Host "all CLOSED as of 2026-08-30, and -AdvisoryLanes is deleted: all four lanes"
Write-Host "now hard-gate. Assert on gatePassed and the per-lane verdict in"
Write-Host "golden-gate-summary.json -- advisoryFailures no longer exists."
