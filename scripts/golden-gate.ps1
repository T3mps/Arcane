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
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\golden-gate.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\golden-gate.ps1 -Configuration Release
#
# Exit 0 iff all four comparisons resolve to a confirmed PASS. Exit 1
# otherwise (a genuine mismatch, a missing/undecodable reference, or a run
# whose outcome could not be determined at all).
#
# Windows PowerShell 5.1 compatible.

param(
    [string]$Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$configDirName = "$Configuration-windows-x86_64-md"

Write-Host "=== golden-gate: $Configuration ===" -ForegroundColor Cyan

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
}
Write-Host "-- ReferenceGame.dll restaged beside both hosts --" -ForegroundColor Green

# ---- The four combinations. ----
$combos = @(
    @{ Host = 'ArcaneRuntime'; Exe = 'ArcaneRuntime.exe'; Reference = 'runtime-scene'; Backend = 'dx12' }
    @{ Host = 'ArcaneRuntime'; Exe = 'ArcaneRuntime.exe'; Reference = 'runtime-scene'; Backend = 'vulkan' }
    @{ Host = 'ArcaneEditor'; Exe = 'ArcaneEditor.exe'; Reference = 'editor-ui'; Backend = 'dx12' }
    @{ Host = 'ArcaneEditor'; Exe = 'ArcaneEditor.exe'; Reference = 'editor-ui'; Backend = 'vulkan' }
)

$results = @()
$anyFailure = $false

foreach ($combo in $combos) {
    $hostName = $combo.Host
    $exeName = $combo.Exe
    $reference = $combo.Reference
    $backend = $combo.Backend
    $label = "$hostName/$backend/$reference"

    $exeDir = Join-Path $repoRoot "bin\$configDirName\$hostName"
    $exePath = Join-Path $exeDir $exeName

    Write-Host ""
    Write-Host "-- $label --" -ForegroundColor Cyan

    if (-not (Test-Path $exePath)) {
        Write-Error "$exePath does not exist -- build Arcane.slnx for $Configuration first."
        $results += [pscustomobject]@{ Combo = $label; Verdict = 'FAIL'; Detail = "exe not found: $exePath" }
        $anyFailure = $true
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
                    $diffPathToReport = Join-Path $exeDir $report.compare.diffPath
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
        Write-Host "PASS -- $label ($detail)" -ForegroundColor Green
    } else {
        $anyFailure = $true
        Write-Host "FAIL -- $label" -ForegroundColor Red
        Write-Host "  $detail" -ForegroundColor Red
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

if ($anyFailure) {
    Write-Host "golden-gate: FAILED" -ForegroundColor Red
    exit 1
}

Write-Host "golden-gate: all four comparisons PASSED" -ForegroundColor Green
exit 0
