# golden-gate.ps1 -- the host-level golden-image gate (Task 12, plan-b comparator).
#
# THE HONEST SPLIT (see GoldenImageTest.cpp's own [gpu][golden] case and its
# header comment): an in-process Catch2 case proves the RENDER PATH, but it
# links neither RuntimeApp nor EditorApp, so it is silent about boot, settle,
# reporting and the CLI. THIS script is the one that actually launches both
# real hosts, on both backends, with --compare, and reads what they report --
# it is the gate that covers what an agent actually runs. Do not let a green
# [gpu][golden] Catch2 run stand in for a green run of this script.
#
# Four combinations, no --bless:
#   ArcaneRuntime --backend dx12    --compare runtime-scene
#   ArcaneRuntime --backend vulkan  --compare runtime-scene
#   ArcaneEditor  --backend dx12    --compare editor-ui
#   ArcaneEditor  --backend vulkan  --compare editor-ui
#
# THE VERDICT IS `exitReason` OUT OF THE REPORT JSON, NEVER THE RAW PROCESS
# EXIT CODE ALONE (ArcaneEditor/src/main.cpp's own exit-code table names the
# collisions this works around): "compare-failed" is exit 3 for a genuine
# mismatch but exit 4 for a bless-write I/O failure -- unreachable here since
# this script never passes --bless, but the string is still what carries the
# fact, not the number. On the editor, exit 3 ALSO collides with its
# pre-boot double-open refusal, which never reaches EditorApp::Run() and so
# never writes a report at all. That gives a ONE-DIRECTION disambiguator:
#   report PRESENT -> post-boot, definitively (whatever exitReason says).
#   report ABSENT  -> does NOT prove pre-boot. WriteTo itself can fail (full
#                     disk, permissions) on a run that otherwise got there.
# So a missing report is reported as "could not determine", never asserted
# as a pre-boot refusal, and this script surfaces stderr in that case --
# ShutdownGraphPath logs an ARC_ERROR line when the write itself fails, and
# that line is the only place left to find why the report is missing.
#
# THE PRECONDITION THIS SCRIPT ENFORCES: ReferenceProject/Binaries/ is a
# SINGLE SLOT shared by every configuration. Flip from Debug to Dist (or vice
# versa) without rebuilding it and the next host launch dies with "plugin:
# initial load failed" against a ReferenceGame.dll built for the wrong CRT --
# a debugging round already spent on exactly this. So step one, always, is a
# rebuild of ReferenceProject.slnx for the -Configuration this run targets,
# never a conditional skip.
#
# THE ADVISORY LANES (-AdvisoryLanes), and why this switch exists.
#
# The two ArcaneEditor lanes are RED, on both configurations, because of TWO
# OWED ENGINE DEFECTS THIS GATE ITSELF DISCOVERED on its first real use. They
# are owed to a future plan and are deliberately not fixed here. -AdvisoryLanes
# is how they ship without either of the two bad options:
#
#   * NOT hard-red. The Jenkins 'Golden gate' stage is SEQUENTIAL and sits
#     immediately before 'ReferenceProject (SDK build)' and 'Scripted
#     GPU-verify (--frames)'. A failing `bat` step aborts the enclosing
#     stage('Windows'), so shipping red would delete two pre-existing stages of
#     working coverage from EVERY build -- and would defeat that stage's own
#     "RELEASE THEN DEBUG is load-bearing" ordering, because Release is the
#     failing invocation, so the Debug one that restores the single-slot
#     Binaries/ would never run. A permanently red pipeline is also exactly the
#     "a gate nobody can cheaply bless gets switched off in the first week"
#     failure this branch's own spec and its own Unreal research both name.
#   * NOT held, and NOT dropped. Both ArcaneRuntime lanes pass 0-diff on both
#     backends and both configurations; that is real coverage, today. And a
#     lane that is not RUN cannot tell anyone when the owed fix works -- which
#     is why the editor entries stay in $combos and this switch changes only
#     whether they reach $anyFailure.
#
# THE SWITCH IS THE TRACKING ARTIFACT. There is no ticket to lose: the reason
# is printed on every single run (Write-OwedDefects below), and when both owed
# defects land, -AdvisoryLanes and the Jenkinsfile argument that passes it are
# DELETED, not edited. If you are reading this because a lane is quiet, that is
# the bug -- an advisory lane is meant to be impossible to miss.
#
# DO NOT RE-BLESS editor-ui to make this go away. The moving input is a
# directory the editor itself writes into, so re-blessing goes green today and
# red again at the next crashed, hung or killed editor run.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\golden-gate.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\golden-gate.ps1 -Configuration Release
#   powershell -ExecutionPolicy Bypass -File scripts\golden-gate.ps1 -AdvisoryLanes ArcaneEditor
#
# Exit 0 iff every HARD-GATING comparison resolves to a confirmed PASS. Exit 1
# otherwise (a genuine mismatch, a missing/undecodable reference, or a run
# whose outcome could not be determined at all).
#
# -AdvisoryLanes -- see "THE ADVISORY LANES" below. A host named there still
# runs, still prints its full verdict, and still leaves its diff artifact on
# disk; it just does not decide this script's exit code. It is a KNOWN-RED
# marker with a printed reason, NOT a skip and NOT a way to make a lane quiet.
#
# Windows PowerShell 5.1 compatible.

param(
    [string]$Configuration = 'Debug',
    # Host names (as they appear in $combos below) whose lanes are ADVISORY:
    # run, reported, archived -- but excluded from the exit code. Empty by
    # default, so a plain local run hard-gates all four lanes as it always did.
    [string[]]$AdvisoryLanes = @()
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$configDirName = "$Configuration-windows-x86_64-md"

Write-Host "=== golden-gate: $Configuration ===" -ForegroundColor Cyan

# ---- The four combinations. Declared HERE, ahead of the rebuild below, purely
#      so -AdvisoryLanes can be validated against the real host list BEFORE the
#      expensive /t:Rebuild -- a typo'd lane name should cost a second, not a
#      full ReferenceProject rebuild plus four host launches. ----
$combos = @(
    @{ Host = 'ArcaneRuntime'; Exe = 'ArcaneRuntime.exe'; Reference = 'runtime-scene'; Backend = 'dx12' }
    @{ Host = 'ArcaneRuntime'; Exe = 'ArcaneRuntime.exe'; Reference = 'runtime-scene'; Backend = 'vulkan' }
    @{ Host = 'ArcaneEditor'; Exe = 'ArcaneEditor.exe'; Reference = 'editor-ui'; Backend = 'dx12' }
    @{ Host = 'ArcaneEditor'; Exe = 'ArcaneEditor.exe'; Reference = 'editor-ui'; Backend = 'vulkan' }
)

# ---- -AdvisoryLanes: validate, then ANNOUNCE. ----
#
# An unrecognised name is REFUSED rather than ignored. Ignoring it fails
# "safe" -- the lane stays hard-gating -- but it fails SILENTLY, and the caller
# who typed `-AdvisoryLanes ArcaneEdtior` believes they configured something
# they did not. Same rule the hosts' own CLI applies to a flag that would
# otherwise be inert.
$knownHosts = $combos | ForEach-Object { $_.Host } | Sort-Object -Unique
foreach ($lane in $AdvisoryLanes) {
    if ($knownHosts -notcontains $lane) {
        Write-Host "-AdvisoryLanes: '$lane' is not one of the hosts this gate runs ($($knownHosts -join ', '))." -ForegroundColor Red
        exit 1
    }
}

# PRINTED ON EVERY RUN THAT USES THE SWITCH -- twice, once here and once in the
# summary -- because an advisory lane whose reason is not in front of you is
# just a lane nobody reads. See "THE ADVISORY LANES" in this file's header.
function Write-OwedDefects {
    Write-Host ""
    Write-Host "################################################################" -ForegroundColor Yellow
    Write-Host "## ADVISORY LANES ACTIVE: $($AdvisoryLanes -join ', ')" -ForegroundColor Yellow
    Write-Host "## These lanes RUN, print full verdicts, and leave their diff" -ForegroundColor Yellow
    Write-Host "## artifacts on disk. They do NOT decide this script's exit" -ForegroundColor Yellow
    Write-Host "## code, because they are red on TWO OWED ENGINE DEFECTS that" -ForegroundColor Yellow
    Write-Host "## THIS GATE FOUND and that are owed to a future plan:" -ForegroundColor Yellow
    Write-Host "##" -ForegroundColor Yellow
    Write-Host "## OWED DEFECT 1 -- --settle counts ATTEMPTS while the condition" -ForegroundColor Yellow
    Write-Host "##   it waits on (ShaderCompiler::IsIdle()) is denominated in" -ForegroundColor Yellow
    Write-Host "##   MILLISECONDS. At ~3.3 ms/attempt a 30-attempt budget is" -ForegroundColor Yellow
    Write-Host "##   spent in ~100 ms, so a fast enough build bails before" -ForegroundColor Yellow
    Write-Host "##   background compilation can drain. Release fails" -ForegroundColor Yellow
    Write-Host "##   settle-not-converged; Debug passes only because its" -ForegroundColor Yellow
    Write-Host "##   attempts are slower. MODE-WIDE, not editor-only. Raising" -ForegroundColor Yellow
    Write-Host "##   the attempt count treats the symptom, not the defect." -ForegroundColor Yellow
    Write-Host "##" -ForegroundColor Yellow
    Write-Host "## OWED DEFECT 2 -- the editor's verify capture includes the" -ForegroundColor Yellow
    Write-Host "##   Assets panel, which enumerates ReferenceProject/Saved/" -ForegroundColor Yellow
    Write-Host "##   Diagnostics/ -- a directory the editor itself writes crash" -ForegroundColor Yellow
    Write-Host "##   and hang captures into. The golden image therefore depends" -ForegroundColor Yellow
    Write-Host "##   on the machine's failure history (measured: 24 anti-" -ForegroundColor Yellow
    Write-Host "##   aliased pixels of a scrollbar thumb moved after two hang" -ForegroundColor Yellow
    Write-Host "##   captures landed). Fix is to keep the verify capture off" -ForegroundColor Yellow
    Write-Host "##   Saved/Diagnostics/. DO NOT RE-BLESS editor-ui: the moving" -ForegroundColor Yellow
    Write-Host "##   input is written by the thing under test, so re-blessing" -ForegroundColor Yellow
    Write-Host "##   goes green today and red at the next crashed editor run." -ForegroundColor Yellow
    Write-Host "##" -ForegroundColor Yellow
    Write-Host "## WHEN BOTH LAND, DELETE -AdvisoryLanes AND the Jenkinsfile" -ForegroundColor Yellow
    Write-Host "## argument that passes it. This switch IS the tracking item." -ForegroundColor Yellow
    Write-Host "################################################################" -ForegroundColor Yellow
    Write-Host ""
}

if ($AdvisoryLanes.Count -gt 0) { Write-OwedDefects }

# ---- THE PRECONDITION: rebuild ReferenceProject/Binaries/ for THIS config,
#      unconditionally, before a single host is launched. ----
$referenceProjectDir = Join-Path $repoRoot 'ReferenceProject'
$referenceSln = Join-Path $referenceProjectDir 'ReferenceProject.slnx'
if (-not (Test-Path $referenceSln)) {
    # ReferenceProject.slnx is gitignored (premake output, not a checked-in
    # file), so on a FRESH checkout -- exactly the state a CI agent starts
    # from -- it does not exist yet. Generating it here rather than just
    # refusing is what lets this stage sit right after "Tests (incl [gpu])"
    # in the Jenkinsfile, ahead of the separate "ReferenceProject (SDK
    # build)" stage that also builds it for Debug: without this, a
    # first-ever run on a clean agent would fail before a single host even
    # launched, on a precondition this script could satisfy itself.
    $premake5 = Join-Path $repoRoot 'ThirdParty\premake5\premake5.exe'
    if (-not (Test-Path $premake5)) {
        Write-Error "ReferenceProject.slnx is missing AND $premake5 does not exist -- cannot generate it."
        exit 1
    }
    Write-Host "-- ReferenceProject.slnx not found -- generating via premake5 vs2026 --"
    Push-Location $referenceProjectDir
    try {
        & $premake5 vs2026
        if ($LASTEXITCODE -ne 0) {
            Write-Error "premake5 vs2026 FAILED (exit $LASTEXITCODE) generating ReferenceProject.slnx."
            exit 1
        }
    } finally {
        Pop-Location
    }
    if (-not (Test-Path $referenceSln)) {
        Write-Error "premake5 vs2026 reported success but $referenceSln still does not exist."
        exit 1
    }
}

$msbuildWrapper = Join-Path $repoRoot 'ci\msbuild.cmd'
if (-not (Test-Path $msbuildWrapper)) {
    Write-Error "ci\msbuild.cmd not found at $msbuildWrapper."
    exit 1
}

Write-Host "-- rebuilding ReferenceProject for $Configuration (single-slot Binaries/ precondition) --"
& $msbuildWrapper $referenceSln -t:Rebuild "-p:Configuration=$Configuration" -m -v:minimal -nologo
if ($LASTEXITCODE -ne 0) {
    Write-Error "ReferenceProject rebuild FAILED (exit $LASTEXITCODE) -- refusing to run stale/mismatched hosts against it."
    exit 1
}
Write-Host "-- ReferenceProject rebuilt for $Configuration --" -ForegroundColor Green

# ---- ...AND THEN RESTAGE IT. The rebuild above writes ONLY
#      ReferenceProject/Binaries/ at the repo root. That is NOT the copy the
#      hosts load: premake5.lua's ArcaneRuntime/ArcaneEditor postbuilds
#      ({COPYDIR} "%{wks.location}/ReferenceProject" -> the exe's own
#      directory, lines 355 and 430) stage the WHOLE ReferenceProject tree --
#      Binaries/ included -- beside each exe, and the hosts run with
#      `--project ReferenceProject` from that exe directory, so
#      bin/<Config>-windows-x86_64-md/<Host>/ReferenceProject/Binaries/
#      ReferenceGame.dll is what actually gets LoadLibrary'd. That staged copy
#      is refreshed only when the HOST project itself builds, so the rebuild
#      above, on its own, leaves the precondition UNMET: measured on 2026-08-26,
#      a repo-root Release ReferenceGame.dll (MSVCP140.dll) sat beside a staged
#      copy still importing ucrtbased.dll -- i.e. a Debug-flavour plugin about
#      to be handed to a Release host, which PluginHost.cpp refuses outright
#      BEFORE LoadLibrary. On a fresh CI agent it is worse: the staged copy can
#      be absent entirely. So mirror the postbuild here, for exactly the two
#      hosts this script launches.
foreach ($stageHost in @('ArcaneRuntime', 'ArcaneEditor')) {
    $stagedBinaries = Join-Path $repoRoot "bin\$configDirName\$stageHost\ReferenceProject\Binaries"
    if (-not (Test-Path $stagedBinaries)) {
        New-Item -ItemType Directory -Path $stagedBinaries -Force | Out-Null
    }
    Copy-Item -Path (Join-Path $referenceProjectDir 'Binaries\*') -Destination $stagedBinaries -Force -Recurse
    $stagedDll = Join-Path $stagedBinaries 'ReferenceGame.dll'
    if (-not (Test-Path $stagedDll)) {
        Write-Error "restaging FAILED -- $stagedDll does not exist after the copy."
        exit 1
    }

    # ---- Content/ HAS THE IDENTICAL HAZARD, and it stayed unfixed until a desk
    #      pass tripped over it (2026-08-26). The reasoning above stopped at the
    #      DLL because a CRT mismatch was the symptom being chased -- but the
    #      postbuild this block mirrors stages the WHOLE tree, and Content/ is
    #      refreshed on exactly the same schedule: only when the HOST builds.
    #
    #      MEASURED: a deliberate, plainly visible edit to
    #      ReferenceProject/Content/scenes/main.arcscene (MeshCube moved)
    #      produced diffCount=0 on BOTH backends, because the hosts were still
    #      rendering an 18-hour-old staged scene. A GATE THAT CANNOT SEE A
    #      CONTENT CHANGE CANNOT GATE CONTENT: every green it reports after a
    #      scene edit is green about the PREVIOUS scene.
    #
    #      Verify/ is deliberately NOT restaged here. It holds the reference
    #      PNGs, which are also --bless's destination; refreshing them from the
    #      repo on every run would silently discard a bless the caller just
    #      made. The asymmetry is the point: Content/ is an INPUT the gate must
    #      read fresh, Verify/ is state the gate must not trample.
    $stagedContent = Join-Path $repoRoot "bin\$configDirName\$stageHost\ReferenceProject\Content"
    if (-not (Test-Path $stagedContent)) {
        New-Item -ItemType Directory -Path $stagedContent -Force | Out-Null
    }
    Copy-Item -Path (Join-Path $referenceProjectDir 'Content\*') -Destination $stagedContent -Force -Recurse

    # Assert the copy actually took. A silent no-op here restores exactly the
    # blindness this block exists to remove.
    $sourceScene = Join-Path $referenceProjectDir 'Content\scenes\main.arcscene'
    $stagedScene = Join-Path $stagedContent 'scenes\main.arcscene'
    if ((Test-Path $sourceScene) -and (Test-Path $stagedScene)) {
        if ((Get-FileHash $sourceScene -Algorithm MD5).Hash -ne (Get-FileHash $stagedScene -Algorithm MD5).Hash) {
            Write-Error "restaging FAILED -- staged scene still differs from source after the copy ($stagedScene)."
            exit 1
        }
    }
}
Write-Host "-- ReferenceGame.dll + Content/ restaged beside both hosts --" -ForegroundColor Green

$results = @()
$anyFailure = $false

foreach ($combo in $combos) {
    $hostName = $combo.Host
    $exeName = $combo.Exe
    $reference = $combo.Reference
    $backend = $combo.Backend
    $label = "$hostName/$backend/$reference"
    $isAdvisory = $AdvisoryLanes -contains $hostName

    $exeDir = Join-Path $repoRoot "bin\$configDirName\$hostName"
    $exePath = Join-Path $exeDir $exeName

    Write-Host ""
    if ($isAdvisory) {
        Write-Host "-- $label -- [ADVISORY: runs and reports, does not gate]" -ForegroundColor Cyan
    } else {
        Write-Host "-- $label --" -ForegroundColor Cyan
    }

    if (-not (Test-Path $exePath)) {
        # Write-Host, NOT Write-Error: $ErrorActionPreference = 'Stop' (above)
        # makes Write-Error TERMINATING, which made the three lines that follow
        # it DEAD CODE -- a single missing exe aborted the whole gate instead of
        # recording FAIL for this lane and testing the other three. The intent
        # was always "record it and carry on"; this is what actually does that.
        Write-Host "$exePath does not exist -- build Arcane.slnx for $Configuration first." -ForegroundColor Red
        # An advisory lane does not gate here either -- the rule is "this host
        # never decides the exit code", full stop, not "except when...". A
        # genuinely missing binary is the BUILD stage's job to catch (the
        # pipeline builds Arcane.slnx three configurations deep before this
        # stage runs), and the two hard-gating runtime lanes still decide this
        # script's verdict. It is still printed in red and still recorded.
        $verdict = if ($isAdvisory) { 'KNOWN-RED' } else { 'FAIL' }
        $results += [pscustomobject]@{ Combo = $label; Verdict = $verdict; Detail = "exe not found: $exePath" }
        if (-not $isAdvisory) { $anyFailure = $true }
        continue
    }

    # Report/stderr land in the exe's own Saved/ (project-gitignored, so a
    # local run never leaves a tracked artifact behind) and are named per
    # combo so four runs in the same exe dir never clobber each other.
    $savedVerifyDir = Join-Path $exeDir 'ReferenceProject\Saved\Verify'
    if (-not (Test-Path $savedVerifyDir)) {
        New-Item -ItemType Directory -Path $savedVerifyDir -Force | Out-Null
    }
    $reportPath = Join-Path $savedVerifyDir "golden-gate-$hostName-$backend-report.json"
    $stderrPath = Join-Path $savedVerifyDir "golden-gate-$hostName-$backend-stderr.txt"
    $stdoutPath = Join-Path $savedVerifyDir "golden-gate-$hostName-$backend-stdout.txt"
    if (Test-Path $reportPath) { Remove-Item $reportPath -Force }

    # The STALE DIFF goes too, for the same reason the stale report does, and
    # it was left out until a desk pass was misled by it (2026-08-26): the run
    # passed, wrote no diff, and the diff PNG sitting at the expected path from
    # an EARLIER failing run was presented as this run's evidence. A leftover
    # artifact that looks like output is worse than no output, because nobody
    # doubts it. Removing it makes "no diff on disk" mean exactly that.
    $staleDiff = Join-Path $savedVerifyDir "$reference-$backend-diff.png"
    if (Test-Path $staleDiff) { Remove-Item $staleDiff -Force }

    $exeArgs = @(
        '--project', 'ReferenceProject',
        '--offscreen',
        '--backend', $backend,
        '--frames', '60',
        '--settle', '30',
        '--report', $reportPath,
        '--compare', $reference
        # deliberately NO --bless -- this script only ever CHECKS.
    )

    # Run from the exe's OWN directory: plugin DLLs, shaders and
    # ReferenceProject/ are all staged relative to it (launch.ps1's own
    # header comment carries the same rule for the same reason).
    $proc = Start-Process -FilePath $exePath -ArgumentList $exeArgs -WorkingDirectory $exeDir `
        -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
    $exitCode = $proc.ExitCode

    $reportExists = Test-Path $reportPath
    $verdict = 'FAIL'
    $detail = ''
    $diffPathToReport = $null

    if ($reportExists) {
        # THE DISAMBIGUATOR'S PRESENT DIRECTION: a report on disk means this
        # run got at least as far as ShutdownGraphPath's report block, so
        # exitReason is the authoritative fact -- read it, never the raw
        # process exit code (main.cpp's "3 means two different things"
        # collision is exactly why).
        try {
            $report = Get-Content $reportPath -Raw | ConvertFrom-Json
        } catch {
            $verdict = 'FAIL'
            $detail = "report at $reportPath exists but failed to parse as JSON: $($_.Exception.Message)"
            $report = $null
        }

        if ($report) {
            $exitReason = $report.exitReason
            $comparePassed = $false
            if ($report.PSObject.Properties.Name -contains 'compare') {
                $comparePassed = [bool]$report.compare.passed
                if ($report.compare.PSObject.Properties.Name -contains 'diffPath' -and $report.compare.diffPath) {
                    # diffPath is `artifact.string()` built from Project::Root()
                    # (ReferenceImages.cpp), which is normally ABSOLUTE. PowerShell's
                    # Join-Path concatenates without resolving, so joining it to
                    # $exeDir produced a bogus path -- and, worse, setting this
                    # variable non-null SUPPRESSED the correct fallback below, so a
                    # failure went on to name a file that does not exist. Take the
                    # reported path as-is when it is already rooted.
                    if ([System.IO.Path]::IsPathRooted($report.compare.diffPath)) {
                        $diffPathToReport = $report.compare.diffPath
                    } else {
                        $diffPathToReport = Join-Path $exeDir $report.compare.diffPath
                    }
                }
            }

            if ($exitReason -eq 'frames-complete' -and $comparePassed) {
                $verdict = 'PASS'
                $detail = "exitReason=$exitReason diffCount=$($report.compare.diffCount)"
            } else {
                $verdict = 'FAIL'
                $diffCount = if ($report.PSObject.Properties.Name -contains 'compare') { $report.compare.diffCount } else { 'n/a' }
                $errorMessage = if ($report.PSObject.Properties.Name -contains 'compare') { $report.compare.errorMessage } else { '' }
                $detail = "exitReason=$exitReason comparePassed=$comparePassed diffCount=$diffCount errorMessage='$errorMessage'"
            }
        }
    } else {
        # THE DISAMBIGUATOR'S ABSENT DIRECTION: NOT proof of a pre-boot
        # refusal -- VerifyReport::WriteTo can itself fail post-boot (full
        # disk, permissions). "Could not determine" is the honest verdict,
        # and stderr is the one place a write failure's ARC_ERROR line
        # would have landed, so surface it rather than guessing.
        $verdict = 'FAIL'
        $stderrText = ''
        if (Test-Path $stderrPath) { $stderrText = (Get-Content $stderrPath -Raw).Trim() }
        $detail = "no report was written at $reportPath (process exit code $exitCode) -- COULD NOT DETERMINE pass/fail; " +
                  "this is NOT proof of a pre-boot refusal (VerifyReport::WriteTo can itself fail post-boot on a full disk " +
                  "or a permissions error), so treat this as unresolved, not as 'pre-boot'."
        if ($stderrText) {
            $detail += " stderr: $stderrText"
        } else {
            $detail += " (stderr was empty)"
        }
    }

    # THE DIFF ARTIFACT, computed the same way DiffArtifactPath.cpp builds it
    # (ReferenceImages.cpp), so a failure always names a concrete file to
    # open even when the report's own diffPath field came back empty (a
    # sizesMismatch/missing-reference failure that never got as far as a
    # pixel compare has nothing to put there) -- printed whenever it exists
    # on disk, regardless of which branch above produced the verdict.
    if (-not $diffPathToReport) {
        $diffPathToReport = Join-Path $exeDir "ReferenceProject\Saved\Verify\$reference-$backend-diff.png"
    }

    if ($verdict -eq 'PASS') {
        # An ADVISORY lane that PASSES is worth shouting about: it is the signal
        # that the owed defects may be fixed and the switch can come out.
        if ($isAdvisory) {
            Write-Host "PASS -- $label ($detail)" -ForegroundColor Green
            Write-Host "  ADVISORY LANE PASSED -- if both owed defects have landed, DELETE -AdvisoryLanes." -ForegroundColor Green
        } else {
            Write-Host "PASS -- $label ($detail)" -ForegroundColor Green
        }
    } else {
        # KNOWN-RED is a FAILING lane that does not gate. It is still printed in
        # full, and its diff artifact is still named -- the only thing the
        # advisory marker changes is $anyFailure.
        if ($isAdvisory) {
            $verdict = 'KNOWN-RED'
            Write-Host "KNOWN-RED -- $label (advisory: does NOT fail this gate)" -ForegroundColor Yellow
            Write-Host "  $detail" -ForegroundColor Yellow
        } else {
            $anyFailure = $true
            Write-Host "FAIL -- $label" -ForegroundColor Red
            Write-Host "  $detail" -ForegroundColor Red
        }
        if (Test-Path $diffPathToReport) {
            Write-Host "  DIFF ARTIFACT: $diffPathToReport" -ForegroundColor Yellow
        } else {
            Write-Host "  (no diff artifact on disk at the expected path: $diffPathToReport)" -ForegroundColor Yellow
        }
    }

    $results += [pscustomobject]@{ Combo = $label; Verdict = $verdict; Detail = $detail }
}

Write-Host ""
Write-Host "=== golden-gate summary ($Configuration) ===" -ForegroundColor Cyan
$results | Format-Table -AutoSize -Wrap

$knownRed = @($results | Where-Object { $_.Verdict -eq 'KNOWN-RED' })
if ($knownRed.Count -gt 0) {
    # Second printing, deliberately: the first was ~200 lines of build output
    # ago. The summary is the part a CI reader actually scrolls to.
    Write-OwedDefects
    Write-Host "golden-gate: $($knownRed.Count) ADVISORY lane(s) are KNOWN-RED and did not gate: $(($knownRed | ForEach-Object { $_.Combo }) -join ', ')" -ForegroundColor Yellow
}

if ($anyFailure) {
    Write-Host "golden-gate: FAILED" -ForegroundColor Red
    exit 1
}

if ($knownRed.Count -gt 0) {
    Write-Host "golden-gate: every HARD-GATING comparison PASSED (with $($knownRed.Count) advisory lane(s) KNOWN-RED, above)" -ForegroundColor Green
} else {
    Write-Host "golden-gate: all four comparisons PASSED" -ForegroundColor Green
}
exit 0
