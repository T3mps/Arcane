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
#   powershell -ExecutionPolicy Bypass -File scripts\golden-gate.ps1 -SelfTest
#       Prove the gate can FAIL: break the scene, assert all four lanes go red,
#       restore. The ONE mode that writes to the tree -- Content/ only, never
#       Verify/, never a bless. The mutation-to-restore window is a single
#       try/finally (opened where the staging loop below begins) that covers
#       restaging, all four host launches, and any crash or Ctrl-C in
#       between -- not just the tail after the lanes finish -- so the restore
#       genuinely runs on every exit path out of that window, not only the
#       happy one. Writes its verdict to golden-gate-selftest-summary.json
#       (NOT golden-gate-summary.json, so a self-test run never overwrites a
#       green build's gatePassed=true artifact), with an added "selfTest":
#       true field so a consumer scanning golden-gate*summary.json can tell
#       the two apart from the JSON itself, not only from the filename.
#
#       IT ALSO PINS THE WIRE CONTRACT, and that half is not optional: it runs
#       `ArcaneTests.exe "[verdict]"`, which publishes what Arcane::ToString
#       and VerifyReport's schema constants ACTUALLY produce into
#       automation-vocabulary.txt beside the exe, then diffs that against
#       $script:VerdictNames / $script:ReportSchemaMin / $script:ReportSchemaMax
#       below. A missing ArcaneTests.exe FAILS the self-test rather than
#       warning past it -- by that point this mode has already launched four
#       hosts out of the same bin/<Config>/ tree, so its absence means the
#       build is incomplete, not that the check does not apply.
#
#       PRECONDITIONS. (1) ReferenceProject/ must be clean: this mode
#       refuses to start otherwise, because its restore ends in
#       `git checkout --`, which would otherwise silently discard whatever
#       uncommitted edits were sitting in Content/. SELF-HEAL EXCEPTION: if
#       the only dirty path is main.arcscene, and it is EXACTLY this mode's
#       own 0.4 -> 0.6 mutation (checked via git's own diff output, never a
#       raw string compare -- the file is CRLF, the committed blob LF), it
#       is healed via the same `git checkout --` and the run continues,
#       since that is unmistakably a prior -SelfTest killed before its own
#       `finally` ran; anything less exact still refuses, unchanged. (2) It
#       assumes the ORDINARY gate already passes on the tree it is about to
#       mutate -- in
#       the Jenkins pipeline this holds because the self-test stage runs
#       immediately after a green "Golden gate" stage on the same agent, but
#       -SelfTest does NOT verify that baseline itself. Run it against an
#       already-red tree and it reports a false PASS, for the same reason a
#       control group that started broken proves nothing.
#
#       THE CONTROL RUN. Nothing had ever proven this mode's OWN assertion
#       logic was capable of reporting FAILED -- a self-test that can only
#       ever pass is exactly the defect the arc it belongs to exists to
#       rule out elsewhere -- which is why the procedure below was run.
#       The shipped interface always mutates,
#       so "run it against an unmutated tree" cannot be done literally; the
#       interface-only equivalent is to make the mutated render compare
#       CLEAN by blessing the mutated state as its own reference.
#
#       The first version of this procedure blessed
#       ReferenceProject/Verify/References itself while the scene was
#       mutated, then reverted only the scene -- leaving those four blessed
#       PNGs modified in the SOURCE tree, which the dirty-tree precheck
#       above (:207-215) then refuses to run against. Written and broken in
#       the same commit. The procedure below blesses the STAGED copies
#       instead, which that precheck never looks at, so the tree stays
#       clean throughout:
#         1. `git status --porcelain -- ReferenceProject` must be empty.
#         2. By hand, apply the exact -SelfTest edit -- MeshCube's
#            Arcane::Transform position, first element, 0.4 -> 0.6 -- to
#            BOTH staged scene copies, never the git-tracked source:
#              bin\<Config>-windows-x86_64-md\ArcaneRuntime\ReferenceProject\Content\scenes\main.arcscene
#              bin\<Config>-windows-x86_64-md\ArcaneEditor\ReferenceProject\Content\scenes\main.arcscene
#         3. Bless three reference slots against that mutated render,
#            running each host FROM ITS OWN EXE DIRECTORY with
#            `--project ReferenceProject` (relative, so it resolves to the
#            staged tree just edited, not the source). editor-ui is a
#            SHARED slot (one editor-ui.png serves both backends);
#            runtime-scene is backend-split:
#              ArcaneRuntime.exe --project ReferenceProject --headless --backend dx12   --frames 60 --settle 30 --report <path> --compare runtime-scene --bless
#              ArcaneRuntime.exe --project ReferenceProject --headless --backend vulkan --frames 60 --settle 30 --report <path> --compare runtime-scene --bless
#              ArcaneEditor.exe  --project ReferenceProject --headless --backend dx12   --frames 60 --settle 30 --report <path> --compare editor-ui    --bless
#            (--report or --screenshot is REQUIRED -- --settle refuses
#            without one.) This is the key move: Verify/ is deliberately
#            never restaged by the staging loop below (:304-308), so a
#            bless made against the staged tree PERSISTS across the
#            -SelfTest run in step 4 instead of being clobbered by it.
#         4. Run `-SelfTest`. It mutates the SOURCE 0.4 -> 0.6 and restages
#            it, so every lane now renders the same 0.6 state step 3 just
#            blessed against, and all four report PASS on a scene that is,
#            by construction, broken. EXPECTED: this script prints
#            "SELF-TEST FAILED -- the gate did NOT notice a broken scene."
#            and exits 1. That failure is the proof.
#         5. Restore: copy ReferenceProject/Verify/References back over
#            both staged trees (undoing step 3's bless) and delete the
#            scratch --report files from step 3. Confirm with an ordinary
#            `golden-gate.ps1 -Configuration Debug` run that all four lanes
#            are green again.
#
#       RUN, 2026-08-31, Debug, both backends, on real GPU hardware.
#       Forward direction: the ordinary -SelfTest run drove all four lanes
#       to FAIL by exitReason=compare-failed -- runtime lanes 11799
#       differing pixels, editor lanes 4080 -- and printed "SELF-TEST
#       PASSED -- all 4 lane(s) launched and caught the broken scene", exit
#       0. The lanes failed by genuinely COMPARING, not by erroring and not
#       via the exe-not-found path. Control direction: with the staged
#       references blessed against the mutated state (steps 1-3 above), all
#       four lanes reported PASS on a broken scene and the assertion
#       correctly refused to call that a pass -- "SELF-TEST FAILED", exit
#       1. Afterwards the tree was verified restored: all six staged
#       reference PNGs md5-identical to the committed source, `git status`
#       clean, and an ordinary gate run green on all four lanes.
#
# Exit 0 iff every HARD-GATING comparison resolves to a confirmed PASS. Exit 1
# otherwise (a genuine mismatch, a missing/undecodable reference, or a run
# whose outcome could not be determined at all). -SelfTest INVERTS this: it
# exits 0 iff all four lanes launched and went FAIL as expected (see its own
# exit block).
#
# Windows PowerShell 5.1 compatible.

param(
    [string]$Configuration = 'Debug',
    # SELF-TEST: prove this gate is CAPABLE OF FAILING. A gate never observed
    # failing is not a gate. Mutates ReferenceProject's scene, runs the four
    # lanes, and asserts ALL FOUR go FAIL -- then restores. This is the ONE
    # mode in which this script writes to the tree; it touches Content/ only,
    # never Verify/, never blesses, and restores in a finally block so an
    # error or a Ctrl-C still leaves the tree clean.
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$configDirName = "$Configuration-windows-x86_64-md"

Write-Host "=== golden-gate: $Configuration ===" -ForegroundColor Cyan

# ---- THE SUMMARY FILE, RESOLVED AND CLEARED BEFORE ANYTHING CAN REFUSE. ----
#
# R20: under -SelfTest, write to a SEPARATE file. The Jenkins pipeline runs the
# self-test stage immediately after the ordinary gate stage on the same agent,
# so an in-place write would overwrite a green build's gatePassed=true summary
# with an inverted one -- and every documented consumer of this gate is told to
# assert on gatePassed in golden-gate-summary.json.
$summaryFileName = if ($SelfTest) { 'golden-gate-selftest-summary.json' } else { 'golden-gate-summary.json' }
$summaryPath = Join-Path $repoRoot "bin\$configDirName\$summaryFileName"

# A GATE THAT REFUSES TO RUN MUST NOT LEAVE THE PREVIOUS RUN'S VERDICT ON DISK.
# This script's own header tells consumers to assert on gatePassed and NEVER on
# the exit code alone -- and there are a dozen-plus refusal paths BEFORE the
# summary is written (a failed ReferenceProject rebuild, a dirty tree under
# -SelfTest, a malformed exclusion file, a restaging failure). Every one of them
# used to leave yesterday's `gatePassed: true` sitting there for a consumer
# following that instruction to read as today's answer. Jenkins never saw it --
# it consumes the `bat` exit code -- so this bit exactly the AGENT consumer the
# file exists for.
#
# BOTH HALVES, deliberately. Exit-GateRefusal below replaces the bare `exit 1`
# on every refusal path with a gatePassed=false summary that says WHY, keeping
# the header's stated contract literally true. Deleting here as well covers what
# that cannot: $ErrorActionPreference = 'Stop' turns any unhandled terminating
# error into an exit that runs no code of ours at all, and a stale green would
# survive it.
if (Test-Path $summaryPath) {
    Remove-Item $summaryPath -Force -ErrorAction SilentlyContinue
}

# Renders the machine-readable verdict. ONE writer for both the ordinary
# end-of-run summary and a refusal, so the two can never drift into different
# shapes -- a consumer parses the same document either way and reads
# refusalReason to tell them apart.
function Write-GateSummaryFile {
    param(
        [bool]$GatePassed,
        [object[]]$Lanes,
        [string]$RefusalReason
    )
    $summary = [pscustomobject]@{
        # 2: `verdict` widened from PASS/FAIL to the seven-value vocabulary, and
        # `skipReason` was added.
        # 3: `refusalReason` added, and a summary is now written on the refusal
        # paths that previously wrote none. Empty string on an ordinary run --
        # EMPTY, NOT ABSENT, the same absence-must-be-absence contract
        # VerifyReport upholds for its own optional blocks, so a consumer never
        # has to distinguish "this build predates the field" from "this run was
        # not a refusal". `gatePassed` keeps its name, type and meaning through
        # both moves and remains the safe single thing to assert on.
        schemaVersion = 3
        configuration = $Configuration
        gatePassed    = $GatePassed
        selfTest      = [bool]$SelfTest
        refusalReason = $RefusalReason
        lanes         = @($Lanes)
    }
    try {
        $summaryDir = Split-Path -Parent $summaryPath
        if (-not (Test-Path $summaryDir)) { New-Item -ItemType Directory -Path $summaryDir -Force | Out-Null }
        # NOT `Set-Content -Encoding UTF8`: under Windows PowerShell 5.1 that
        # encoding emits a UTF-8 BOM, so the summary would start with EF BB BF
        # and a stock JSON reader -- e.g. Python's json.load, the exact "AGENT"
        # consumer this file exists for -- fails with "JSONDecodeError:
        # Expecting value: line 1 column 1 (char 0)". PowerShell's own
        # ConvertFrom-Json tolerates the BOM, which is exactly why this went
        # unnoticed. WriteAllText with a BOM-less UTF8Encoding writes the same
        # bytes without it. $summaryPath is already absolute (built off
        # $repoRoot via Join-Path above), which WriteAllText requires.
        [System.IO.File]::WriteAllText($summaryPath, ($summary | ConvertTo-Json -Depth 5), (New-Object System.Text.UTF8Encoding($false)))
        Write-Host "golden-gate: machine-readable verdict -> $summaryPath" -ForegroundColor Cyan
    } catch {
        # Never let summary-writing decide the gate's verdict; say so and continue.
        Write-Host "golden-gate: WARNING -- could not write $summaryPath ($($_.Exception.Message))" -ForegroundColor Yellow
    }
}

# Every pre-lane refusal goes through here. NOT Write-Error: under
# $ErrorActionPreference = 'Stop' that is itself a script-terminating error, so
# a `Write-Error ...; exit 1` pair never reaches its own exit -- and would never
# reach a summary write placed after it either.
function Exit-GateRefusal {
    param([Parameter(Mandatory)][string]$Reason)
    Write-Host ""
    Write-Host "golden-gate: REFUSING TO RUN -- $Reason" -ForegroundColor Red
    Write-GateSummaryFile -GatePassed $false -Lanes @() -RefusalReason $Reason
    exit 1
}

#   VERDICT REACHABILITY. A value no synthetic condition can produce does not
#   belong in the vocabulary. Each is reachable as follows -- the first is
#   automated by -SelfTest; the rest are manual recipes, each OBSERVED once
#   during this arc rather than merely asserted:
#     Failed            -- `-SelfTest` (MeshCube position x: 0.4 -> 0.6)
#     NotRun            -- rename a host exe aside; the preflight reports it up front
#     Indeterminate     -- delete the lane's reference image, which makes the host
#                          exit `compare-missing-reference`. IT MUST BE THE STAGED
#                          COPY under bin/<config>/<host>/ReferenceProject/Verify/
#                          References/, not the source tree: the hosts read the
#                          staged references, so deleting the source alone changes
#                          nothing. And EVERY LEVEL of the fallback chain must go:
#                          removing only vulkan/runtime-scene.png makes the lane
#                          resolve the SHARED runtime-scene.png and report Failed
#                          (the vulkan render genuinely differs from it -- which is
#                          why a backend-specific reference exists at all). Both
#                          wrong recipes were tried before this one; neither was
#                          guessed.
#     Skipped           -- add a live entry to scripts/automation-exclusions.json
#     PassedOnFallback  -- a lane whose ExpectedLevel is 'backend' but which resolves
#                          the shared reference. ArcaneRuntime/dx12/runtime-scene does
#                          this today: there is no dx12/runtime-scene.png, only
#                          vulkan/. It is observed on every ordinary run.
#     Errored           -- exceed a launch bound, so the host is killed and the bound
#                          names itself. NOT via "gpu-stall": that string is a
#                          Diagnostics crash-envelope KIND, never a VerifyReport
#                          exitReason, so it cannot reach this cascade. See the
#                          Errored branch's own comment.
#     Passed            -- the ordinary green run
#
# ---- The four combinations. ----
# ExpectedLevel: which reference this lane is SUPPOSED to resolve against.
# Nothing in the report can infer this -- a resolvedLevel of "shared" looks
# identical whether that was the design or an oversight -- so it is declared
# here and compared in the verdict block below. A lane declaring "shared" and
# resolving "shared" is a plain Passed, not PassedOnFallback.
$combos = @(
    @{ Host = 'ArcaneRuntime'; Exe = 'ArcaneRuntime.exe'; Reference = 'runtime-scene'; Backend = 'dx12';   ExpectedLevel = 'backend' }
    @{ Host = 'ArcaneRuntime'; Exe = 'ArcaneRuntime.exe'; Reference = 'runtime-scene'; Backend = 'vulkan'; ExpectedLevel = 'backend' }
    @{ Host = 'ArcaneEditor';  Exe = 'ArcaneEditor.exe';  Reference = 'editor-ui';     Backend = 'dx12';   ExpectedLevel = 'shared'  }
    @{ Host = 'ArcaneEditor';  Exe = 'ArcaneEditor.exe';  Reference = 'editor-ui';     Backend = 'vulkan'; ExpectedLevel = 'shared'  }
)

# THE VERDICT VOCABULARY. This literal set is the PowerShell half of a contract
# whose other half is Arcane::Verdict (ArcaneClient/src/Arcane/Host/Verdict.hpp),
# pinned by VerdictTest.cpp. PowerShell cannot include that header -- so the
# [verdict] cases PUBLISH what Arcane::ToString actually produces into
# automation-vocabulary.txt beside ArcaneTests.exe, and -SelfTest reads that
# file back and DIFFS IT AGAINST THIS LIST, order included. Change either side
# and the other must change in the same commit; -SelfTest is what makes that a
# failure rather than a hope.
$script:VerdictNames = @(
    'Passed', 'PassedOnFallback', 'Failed', 'Errored', 'NotRun', 'Skipped', 'Indeterminate'
)
# THE VERIFY-REPORT SCHEMA RANGE this script claims to understand -- the
# PowerShell copy of VerifyReport::kOldestSupportedSchemaVersion and
# kSchemaVersion (ArcaneClient/src/Arcane/Host/VerifyReport.hpp). Read at the
# report parse site below, so the range is a mechanism here and not only a
# constant the header tests exercise. Pinned by -SelfTest against what the
# engine actually publishes, exactly like $script:VerdictNames above -- these
# are the second and third numbers in the same wire contract, and a
# hand-maintained copy nothing compares is what this arc exists to abolish.
$script:ReportSchemaMin = 3
$script:ReportSchemaMax = 4
# Green SATISFIES the gate. Skipped is deliberately absent: it does not fail a
# gate, but it must not count toward "at least one lane passed" either, or an
# all-skipped run reports success having verified nothing.
$script:GreenVerdicts = @('Passed', 'PassedOnFallback')
# Any of these makes the gate red.
$script:RedVerdicts   = @('Failed', 'Errored', 'NotRun', 'Indeterminate')

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
        Exit-GateRefusal "ReferenceProject.slnx is missing AND $premake5 does not exist -- cannot generate it."
    }
    Write-Host "-- ReferenceProject.slnx not found -- generating via premake5 vs2026 --"
    Push-Location $referenceProjectDir
    try {
        & $premake5 vs2026
        if ($LASTEXITCODE -ne 0) {
            Exit-GateRefusal "premake5 vs2026 FAILED (exit $LASTEXITCODE) generating ReferenceProject.slnx."
        }
    } finally {
        Pop-Location
    }
    if (-not (Test-Path $referenceSln)) {
        Exit-GateRefusal "premake5 vs2026 reported success but $referenceSln still does not exist."
    }
}

$msbuildWrapper = Join-Path $repoRoot 'ci\msbuild.cmd'
if (-not (Test-Path $msbuildWrapper)) {
    Exit-GateRefusal "ci\msbuild.cmd not found at $msbuildWrapper."
}

Write-Host "-- rebuilding ReferenceProject for $Configuration (single-slot Binaries/ precondition) --"
& $msbuildWrapper $referenceSln -t:Rebuild "-p:Configuration=$Configuration" -m -v:minimal -nologo
if ($LASTEXITCODE -ne 0) {
    Exit-GateRefusal "ReferenceProject rebuild FAILED (exit $LASTEXITCODE) -- refusing to run stale/mismatched hosts against it."
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

# ---- -SelfTest: refuse on a dirty tree. MUST run first, before anything
#      below touches ReferenceProject/ at all. The mutate-then-restore cycle
#      ends in `git checkout -- <scene>`, which silently discards whatever
#      was there before -- including a desk user's own in-progress edits to
#      main.arcscene. desk-verify-golden-gate.ps1's Assert-CleanReferenceProject
#      (:80-90) guards exactly this; this mode originally shipped without
#      it, so the guard is added here, ahead of everything else.
#
#      SELF-HEAL EXCEPTION, for exactly one shape of dirt: a PRIOR -SelfTest
#      run killed abruptly (taskkill, a hung/crashed host, power loss) never
#      gets to run its own `finally` (:541-575), so its 0.4 -> 0.6 scene
#      mutation can be left sitting in the tree -- and this precheck would
#      otherwise wedge every later run until a human runs `git checkout` by
#      hand. If the ONLY dirty path is main.arcscene, and it is EXACTLY that
#      mutation -- confirmed via `git diff --numstat` (1 insertion, 1
#      deletion), `git diff -U0` (the one removed line reads "0.4", the one
#      added line reads "0.6", whitespace/trailing-comma tolerant), and a
#      regex re-check that the working file's MeshCube Arcane::Transform
#      block still shows the mutated 0.6 -- it is healed with the same `git
#      checkout --` this mode already uses to restore, and the run
#      continues. This checks git's OWN normalised diff output, never a raw
#      string compare: main.arcscene is `i/lf w/crlf attr/text=auto`, so the
#      committed blob is LF and the working file is CRLF, and comparing
#      contents directly would never match -- a heal that can never fire is
#      worse than none. Anything less exact -- more than one dirty path, a
#      different file, a different value, an edit outside MeshCube's own
#      block, an untracked file, a STAGED change -- still refuses, exactly
#      as before. ----
$sceneRelative = 'ReferenceProject/Content/scenes/main.arcscene'
$scenePath     = Join-Path $repoRoot 'ReferenceProject\Content\scenes\main.arcscene'
if ($SelfTest) {
    $preDirty = @(& git -C $repoRoot status --porcelain -- ReferenceProject)
    if ($preDirty) {
        $healEligible = $false

        # Condition 1: exactly one dirty path, an UNSTAGED modification
        # (" M ", never "M  " -- a staged change still refuses) of
        # main.arcscene itself.
        if (($preDirty.Count -eq 1) -and
            ($preDirty[0] -match ('^ M ' + [Regex]::Escape($sceneRelative) + '$'))) {

            # Condition 2: git's own numstat -- exactly 1 insertion, 1 deletion.
            $numstat = @(& git -C $repoRoot diff --numstat -- $sceneRelative)
            if (($numstat.Count -eq 1) -and ($numstat[0] -match '^(\d+)\s+(\d+)\s+')) {
                $insCount = [int]$Matches[1]
                $delCount = [int]$Matches[2]
                if ($insCount -eq 1 -and $delCount -eq 1) {

                    # Condition 3: git's own -U0 diff (normalises line endings) --
                    # exactly one removed "0.4" and one added "0.6".
                    $diffLines    = @(& git -C $repoRoot diff -U0 -- $sceneRelative)
                    $removedLines = @($diffLines | Where-Object { $_ -match '^-(?!--)' })
                    $addedLines   = @($diffLines | Where-Object { $_ -match '^\+(?!\+\+)' })
                    if ($removedLines.Count -eq 1 -and $addedLines.Count -eq 1) {
                        $removedValue = $removedLines[0].Substring(1).Trim().TrimEnd(',').Trim()
                        $addedValue   = $addedLines[0].Substring(1).Trim().TrimEnd(',').Trim()
                        if ($removedValue -eq '0.4' -and $addedValue -eq '0.6') {

                            # Condition 4: the changed element is MeshCube's OWN --
                            # not some other entity that happens to hold a 0.4.
                            $workingText = Get-Content $scenePath -Raw
                            if ($workingText -match '(?s)"name":\s*"MeshCube".*?"Arcane::Transform":\s*\{\s*"position":\s*\[\s*0\.6') {
                                $healEligible = $true
                            }
                        }
                    }
                }
            }
        }

        if ($healEligible) {
            Write-Host ""
            Write-Host "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" -ForegroundColor Red
            Write-Host "-SelfTest: FOUND RESIDUE FROM A PREVIOUSLY-KILLED -SelfTest RUN." -ForegroundColor Red
            Write-Host "ReferenceProject/Content/scenes/main.arcscene still carries this mode's own" -ForegroundColor Red
            Write-Host "MeshCube 0.4 -> 0.6 mutation -- an earlier -SelfTest run was killed (taskkill," -ForegroundColor Red
            Write-Host "a crashed/hung host, or power loss) before its own 'finally' could restore it." -ForegroundColor Red
            Write-Host "Restoring it now via 'git checkout --' and continuing." -ForegroundColor Red
            Write-Host "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" -ForegroundColor Red
            & git -C $repoRoot checkout -- $sceneRelative
            $postHealDirty = & git -C $repoRoot status --porcelain -- ReferenceProject
            if ($postHealDirty) {
                Exit-GateRefusal ("-SelfTest: auto-heal of $sceneRelative did NOT clean the tree -- refusing " +
                    "to continue. Remaining status:`n" + ($postHealDirty -join "`n"))
            }
            Write-Host "-- -SelfTest: residue healed, ReferenceProject/ is clean -- continuing --" -ForegroundColor Green
        } else {
            Exit-GateRefusal ("-SelfTest: ReferenceProject/ is NOT clean, and this mode's " +
                "mutate-then-'git checkout --' restore cycle would silently discard whatever is there " +
                "now. Commit, stash, or discard these changes first, then re-run:`n" + ($preDirty -join "`n"))
        }
    }
}

# ---- -SelfTest: break the scene, so the lanes have something to catch. ----
# R12: git checkout's pathspec needs FORWARD slashes -- a backslash pathspec
# is a silent no-op there (see the restore block near the end of this
# script, and the self-heal block just above). Join-Path wants its native
# backslash form. Keep both, the way desk-verify-golden-gate.ps1 does
# (:56, :65). $sceneRelative/$scenePath are now defined above the precheck
# so the self-heal block can share them instead of duplicating the path.
#
# The two STAGED scene copies under
# bin\<Config>-windows-x86_64-md\{ArcaneRuntime,ArcaneEditor}\ReferenceProject\Content\
# need no healing of their own: the staging loop below unconditionally
# deletes and re-copies Content/ beside both hosts on every run, so any
# stale staged copy is always overwritten before a single lane launches.
$broken = $null
if ($SelfTest) {
    # The SAME mutation desk-verify-golden-gate.ps1's Set-MeshCubeBroken uses
    # (scripts\desk-verify-golden-gate.ps1:105-117) -- MeshCube's
    # Arcane::Transform position, first element, 0.4 -> 0.6. One mutation
    # vocabulary, not two: a second one would drift from this one exactly the
    # way hand-copied logic drifted elsewhere in this tree.
    #
    # The regex is anchored to MeshCube's own entity block and capped to ONE
    # replacement ([Regex]::Replace's trailing count arg): main.arcscene also
    # has an unrelated "0.45" literal (:164) that a loosely-anchored global
    # -replace would corrupt into 0.65, mutating the scene a second,
    # undeclared way. NOTE the anchor is still a LAZY, UNBOUNDED match
    # (".*?" between MeshCube's name and its Transform): if MeshCube's own
    # position ever stops being 0.4 while some LATER entity in the file
    # happens to have a 0.4 in the same shape, both the guard and the
    # replace below will walk forward and silently retarget that later
    # entity instead -- reporting a clean mutation while breaking the wrong
    # thing. The `-notmatch` guard only catches the case where NO match
    # exists at all, not a match against the wrong entity.
    $sceneText = Get-Content $scenePath -Raw
    if ($sceneText -notmatch '(?s)"name":\s*"MeshCube".*?"Arcane::Transform":\s*\{\s*"position":\s*\[\s*0\.4') {
        Exit-GateRefusal "-SelfTest: could not find MeshCube's Arcane::Transform position at 0.4 in $sceneRelative -- the fixture moved; fix this script rather than reporting a pass."
    }
    $broken = [Regex]::Replace($sceneText,
        '(?s)("name":\s*"MeshCube".*?"Arcane::Transform":\s*\{\s*"position":\s*\[\s*)0\.4',
        '${1}0.6', 1)
    if ($broken -eq $sceneText) {
        Exit-GateRefusal "-SelfTest: mutation did not apply to $sceneRelative -- refusing to run the lanes against an unbroken scene."
    }
}

# ---- Staging + host launches. ALWAYS runs, self-test or not -- this try is
#      the fix for the review finding that the mode's OWN header claimed
#      "always restored in a finally" while the actual try opened 292 lines
#      after the mutation, leaving the two restaging failures below, the
#      Remove-Item/Copy-Item restaging itself, and all four
#      Start-Process -Wait host launches -- the mode's entire wall clock --
#      completely unprotected. When -SelfTest is set, the mutation now
#      happens as the FIRST statement inside this try, so a hung host, a
#      locked staged file, an AV hold, or a Ctrl-C anywhere in this block
#      still restores. (The try opens around the whole shared body, rather
#      than immediately around the bare Set-Content call, so this SAME code
#      serves both modes without duplicating it: for an ordinary run
#      $SelfTest is $false, the mutation and the finally's restore are both
#      no-ops, and nothing here behaves any differently than before this
#      switch existed.)
try {
    if ($SelfTest) {
        Set-Content -Path $scenePath -Value $broken -Encoding UTF8 -NoNewline
        Write-Host "-- -SelfTest: scene deliberately broken (MeshCube position x: 0.4 -> 0.6) --" -ForegroundColor Yellow
    }

    foreach ($stageHost in @('ArcaneRuntime', 'ArcaneEditor')) {
        $stagedBinaries = Join-Path $repoRoot "bin\$configDirName\$stageHost\ReferenceProject\Binaries"
        if (-not (Test-Path $stagedBinaries)) {
            New-Item -ItemType Directory -Path $stagedBinaries -Force | Out-Null
        }
        Copy-Item -Path (Join-Path $referenceProjectDir 'Binaries\*') -Destination $stagedBinaries -Force -Recurse
        $stagedDll = Join-Path $stagedBinaries 'ReferenceGame.dll'
        if (-not (Test-Path $stagedDll)) {
            Exit-GateRefusal "restaging FAILED -- $stagedDll does not exist after the copy."
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
        #      MIRROR, NOT MERGE. `Copy-Item -Force -Recurse` overwrites what both
        #      trees share and SILENTLY KEEPS anything the staged tree has that the
        #      source does not, so "restaged" delivered "the source's files PLUS
        #      whatever has accumulated" -- which is not reading fresh, whatever the
        #      paragraph above claims. Measured 2026-08-30: a stray
        #      `New Mesh.arcmesh` (untracked, absent from source, left behind by an
        #      editor session's + Mesh button) sat in BOTH staged hosts. Nothing
        #      sorts the asset list -- neither AssetRegistry's
        #      recursive_directory_iterator nor the Assets panel -- so one extra row
        #      at the top shifted every row below it and moved the editor-ui golden
        #      image by 8152 pixels, on both backends, deterministically. It cost a
        #      full desk round to find. Clearing first is what makes the claim true.
        $stagedContent = Join-Path $repoRoot "bin\$configDirName\$stageHost\ReferenceProject\Content"
        if (Test-Path $stagedContent) {
            Remove-Item -Path $stagedContent -Recurse -Force
        }
        New-Item -ItemType Directory -Path $stagedContent -Force | Out-Null
        Copy-Item -Path (Join-Path $referenceProjectDir 'Content\*') -Destination $stagedContent -Force -Recurse

        # Assert the copy actually took. A silent no-op here restores exactly the
        # blindness this block exists to remove.
        $sourceScene = Join-Path $referenceProjectDir 'Content\scenes\main.arcscene'
        $stagedScene = Join-Path $stagedContent 'scenes\main.arcscene'
        if ((Test-Path $sourceScene) -and (Test-Path $stagedScene)) {
            if ((Get-FileHash $sourceScene -Algorithm MD5).Hash -ne (Get-FileHash $stagedScene -Algorithm MD5).Hash) {
                Exit-GateRefusal "restaging FAILED -- staged scene still differs from source after the copy ($stagedScene)."
            }
        }
    }
    Write-Host "-- ReferenceGame.dll + Content/ restaged beside both hosts --" -ForegroundColor Green

    $results = @()

    # ---- EXCLUSIONS. Loaded once; a malformed file is a REFUSAL. ----
    # An ABSENT file legitimately means "no exclusions". A PRESENT but malformed
    # one is refused rather than silently treated as empty -- a parse error that
    # disabled the whole mechanism would hide every entry in it.
    $exclusionsPath = Join-Path $repoRoot 'scripts\automation-exclusions.json'
    $exclusions = @()
    $expiredExclusions = @()
    $today = (Get-Date).ToString('yyyy-MM-dd')
    if (Test-Path $exclusionsPath) {
        try {
            $parsed = Get-Content $exclusionsPath -Raw | ConvertFrom-Json
        } catch {
            Exit-GateRefusal ("automation-exclusions.json is present but not valid JSON: $($_.Exception.Message). " +
                "A malformed exclusion file must not read as 'no exclusions'.")
        }
        if ($null -ne $parsed) {
            $exclusions = @($parsed)
            foreach ($e in $exclusions) {
                foreach ($required in @('target', 'reason', 'expires')) {
                    # PARENTHESISED deliberately. Unary -not binds tighter than
                    # -contains, so `-not $e.PSObject.Properties.Name -contains $required`
                    # parses as `($false) -contains $required` -- always false, so the
                    # missing-property half would never fire.
                    if ((-not ($e.PSObject.Properties.Name -contains $required)) -or (-not $e.$required)) {
                        Exit-GateRefusal "automation-exclusions.json: every entry needs a non-empty '$required'."
                    }
                }
                # UNKNOWN KEYS ARE REFUSED, naming the key -- the same rule
                # ExclusionList.cpp's ParseExclusions applies, in the same
                # words, because this file has TWO readers and a rule only one
                # of them enforces is not a rule. An omitted axis means ALL
                # (see the matcher below), so a misspelled `"backend"` for
                # `"backends"` is dropped in silence and the entry then excludes
                # MORE THAN IT WAS AUTHORED TO -- the exact silent-widening
                # failure the out-of-range-date check next to it already
                # refuses.
                #
                # ADDING A KEY: extend this list IN THE SAME COMMIT that
                # introduces the key, and extend ExclusionList.cpp's
                # kKnownKeys there too. Both readers ship in this repo
                # alongside the file, so there is no cross-version
                # compatibility to protect and no reason for them to disagree.
                $knownKeys = @('target', 'reason', 'expires', 'backends', 'hosts', 'configurations')
                foreach ($prop in $e.PSObject.Properties.Name) {
                    if ($knownKeys -cnotcontains $prop) {
                        Exit-GateRefusal ("automation-exclusions.json: unknown key '$prop' on the entry targeting " +
                            "'$($e.target)' -- known keys are $($knownKeys -join ', ').")
                    }
                }
                # MONTH AND DAY RANGES ARE CHECKED, not just the shape.
                # ExclusionList.cpp's IsIsoDate got exactly this and this half
                # did not, so the two readers disagreed about the same file:
                # "2026-13-01" sorts lexicographically AFTER every real 2026
                # date, so the comparison below would have honoured it as live
                # for nearly a year while ArcaneTests refused it outright. A
                # shape-only regex cannot catch a transposed month.
                if ($e.expires -notmatch '^\d{4}-(0[1-9]|1[0-2])-(0[1-9]|[12]\d|3[01])$') {
                    Exit-GateRefusal "automation-exclusions.json: 'expires' must be an ISO date YYYY-MM-DD with a real month and day, got '$($e.expires)'."
                }
                # Lexicographic compare -- that is why the ISO format is mandatory.
                if ($today -gt $e.expires) { $expiredExclusions += $e }
            }
        }
    }

    # ---- PREFLIGHT. Check every lane's preconditions BEFORE launching any. ----
    # Without this, a missing exe for lane 4 is discovered only after lanes 1-3
    # have each spent a full host launch. This is the "will never be ready" half
    # of Gauntlet's IsReadyToStart contract; the "not ready yet" half is retry
    # machinery for device farms and is deliberately not implemented.
    $preflightFailures = @{}
    foreach ($combo in $combos) {
        $label = "$($combo.Host)/$($combo.Backend)/$($combo.Reference)"
        $exeDir  = Join-Path $repoRoot "bin\$configDirName\$($combo.Host)"
        $exePath = Join-Path $exeDir $combo.Exe
        if (-not (Test-Path $exePath)) {
            $preflightFailures[$label] = "exe not found: $exePath"
        }
    }
    if ($preflightFailures.Count -gt 0) {
        Write-Host ""
        Write-Host "-- PREFLIGHT: $($preflightFailures.Count) of $($combos.Count) lane(s) cannot run --" -ForegroundColor Red
        foreach ($k in $preflightFailures.Keys) {
            Write-Host "   $k -- $($preflightFailures[$k])" -ForegroundColor Red
        }
        Write-Host "   Build first:  msbuild Arcane.slnx /p:Configuration=$Configuration /m" -ForegroundColor Yellow
    }

    foreach ($combo in $combos) {
        $hostName = $combo.Host
        $exeName = $combo.Exe
        $reference = $combo.Reference
        $backend = $combo.Backend
        $expectedLevel = $combo.ExpectedLevel
        $label = "$hostName/$backend/$reference"

        $exeDir = Join-Path $repoRoot "bin\$configDirName\$hostName"
        $exePath = Join-Path $exeDir $exeName

        Write-Host ""
        Write-Host "-- $label --" -ForegroundColor Cyan

        $excluded = $null
        foreach ($e in $exclusions) {
            # -cne, NOT -ne. PowerShell's -ne is CASE-INSENSITIVE, while
            # ExclusionList.cpp:148 compares std::string with != and is
            # case-SENSITIVE -- so `"arcaneeditor/vulkan/editor-ui"` matched
            # here and not there, one file read two ways. The scoping AXES are
            # deliberately case-insensitive on both sides (see AxisMatches);
            # the target is deliberately exact on both. Case sensitivity is the
            # stricter of the two behaviours, and the C++ side is the one with
            # a test pinning it.
            if ($e.target -cne $label) { continue }
            # An omitted axis means ALL, never NONE. d3d12 aliases dx12 so the
            # report's spelling is not a silent miss.
            $bMatch = $true; $hMatch = $true; $cMatch = $true
            if (($e.PSObject.Properties.Name -contains 'backends') -and $e.backends) {
                $want = @($e.backends | ForEach-Object { $v = "$_".ToLower(); if ($v -eq 'd3d12') { 'dx12' } else { $v } })
                $have = $backend.ToLower(); if ($have -eq 'd3d12') { $have = 'dx12' }
                $bMatch = $want -contains $have
            }
            if (($e.PSObject.Properties.Name -contains 'hosts') -and $e.hosts) {
                $hMatch = @($e.hosts | ForEach-Object { "$_".ToLower() }) -contains $hostName.ToLower()
            }
            if (($e.PSObject.Properties.Name -contains 'configurations') -and $e.configurations) {
                $cMatch = @($e.configurations | ForEach-Object { "$_".ToLower() }) -contains $Configuration.ToLower()
            }
            if ($bMatch -and $hMatch -and $cMatch) { $excluded = $e; break }
        }
        if ($excluded) {
            # Checked before the exe even has to exist: an excluded lane needs
            # no binary. Skipped does not fail the gate -- but it does not
            # satisfy it either, so an all-excluded run is still red.
            $results += [pscustomobject]@{
                Combo = $label; Verdict = 'Skipped'
                Detail = "excluded until $($excluded.expires)"
                SkipReason = "$($excluded.reason) (expires $($excluded.expires))"
            }
            # No second "-- $label --" header here: the loop already printed one
            # a few lines above, before the exclusion was even looked up, so an
            # excluded lane used to announce itself twice.
            Write-Host "Skipped -- $($excluded.reason) (expires $($excluded.expires))" -ForegroundColor DarkGray
            continue
        }

        # The decision was already made in the preflight above -- this only
        # records it. NotRun, not Failed: the lane never got the chance to
        # notice anything, a distinction a bespoke $exeMissingCount used to
        # carry and the vocabulary now makes unnecessary.
        if ($preflightFailures.ContainsKey($label)) {
            $results += [pscustomobject]@{ Combo = $label; Verdict = 'NotRun'; Detail = $preflightFailures[$label]; SkipReason = $null }
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
            '--headless',
            '--backend', $backend,
            '--frames', '60',
            '--settle', '30',
            '--report', $reportPath,
            '--compare', $reference
            # deliberately NO --bless -- this script only ever CHECKS what the
            # hosts render. (-SelfTest's one exception, mutating Content/, is a
            # source-tree edit made before any host launches, and is restored
            # before this script exits -- it is not a --bless.)
        )

        # Run from the exe's OWN directory: plugin DLLs, shaders and
        # ReferenceProject/ are all staged relative to it (launch.ps1's own
        # header comment carries the same rule for the same reason).
        # TWO INDEPENDENT BOUNDS. Total duration answers "is this run too long";
        # inactivity answers "has it stopped making progress". They are different
        # questions -- a host looping silently trips the second while the first
        # still has budget -- and the failure names WHICH bound was hit, so the
        # caller is told which knob would change the outcome (the same rule
        # SettleBail already follows).
        $totalBudgetSec      = 600
        $inactivityBudgetSec = 120

        $proc = Start-Process -FilePath $exePath -ArgumentList $exeArgs -WorkingDirectory $exeDir `
            -NoNewWindow -PassThru `
            -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath

        $startedAt = Get-Date
        $lastSize  = -1
        $lastCpu   = [TimeSpan]::Zero
        $lastGrew  = Get-Date
        $timeoutBound = $null
        while (-not $proc.HasExited) {
            Start-Sleep -Milliseconds 500

            # PROGRESS = stdout grew OR the process burned CPU. stdout ALONE is
            # not a progress signal for this workload: these hosts emit nothing
            # per-frame during the phase this bound guards. RuntimeFrame.cpp has
            # no unconditional per-frame logging (its ARC_WARN/ARC_INFO calls are
            # gated behind --settle attempts, which only begin once the --frames
            # budget is already spent), RuntimeApp.cpp logs only discrete
            # start/end events, and ShaderCompiler's cache is in-process only so
            # every launch recompiles from scratch. A cold or GPU-contended lane
            # is therefore EXPECTED to sit silent -- killing it and calling it
            # inactivity would be the gate lying about why the lane failed.
            # TotalProcessorTime advances for a working process and stays flat
            # for a wedged one, which is the question this bound actually asks.
            $size = 0
            if (Test-Path $stdoutPath) { $size = (Get-Item $stdoutPath).Length }
            $cpu = $lastCpu
            try { $cpu = $proc.TotalProcessorTime } catch { }
            if ($size -ne $lastSize -or $cpu -gt $lastCpu) {
                $lastSize = $size
                $lastCpu  = $cpu
                $lastGrew = Get-Date
            }

            if (((Get-Date) - $startedAt).TotalSeconds -gt $totalBudgetSec) {
                $timeoutBound = "total-duration ($totalBudgetSec s)"
                break
            }
            if (((Get-Date) - $lastGrew).TotalSeconds -gt $inactivityBudgetSec) {
                $timeoutBound = "inactivity ($inactivityBudgetSec s with no new output)"
                break
            }
        }

        if ($timeoutBound) {
            try { $proc.Kill() } catch { }
        }
        # WaitForExit() before reading ExitCode, on BOTH paths: Start-Process
        # -PassThru without -Wait can hand back a Process whose ExitCode is not
        # yet populated even once HasExited reads true, and after a Kill() the
        # exit is asynchronous. This costs nothing when the process is already
        # gone and removes the race either way.
        $exited = $proc.WaitForExit(10000)
        if (-not $exited) {
            # A process that survived Kill() would make $proc.ExitCode throw
            # InvalidOperationException, and under $ErrorActionPreference =
            # 'Stop' that aborts the ENTIRE gate instead of reddening one lane.
            # One wedged lane must not cost the other three their verdicts.
            $exitCode = -1
            if (-not $timeoutBound) { $timeoutBound = "unkillable (still running 10 s after Kill())" }
        } else {
            $exitCode = $proc.ExitCode
        }

        $reportExists = Test-Path $reportPath
        $verdict = 'Indeterminate'
        $detail = ''
        $skipReason = $null
        $diffPathToReport = $null

        if ($timeoutBound) {
            # A killed host may still have written a partial report, but the
            # bound that stopped it is the more useful fact, so it wins. Naming
            # WHICH bound is the point: "raise --frames" and "the host stopped
            # progressing" are different problems.
            $verdict = 'Errored'
            if ($timeoutBound -like 'unkillable*') {
                # Folded in from Task 10's review: the shared template below reads
                # "raise that bound", which is not the action for an unkillable
                # process. Actionable failure text is the point of this arc.
                $detail = "the host survived Kill() -- $timeoutBound. Find what is holding the process open (a stuck driver call or a debugger attach are the usual causes)."
            } else {
                $detail = "killed after exceeding its $timeoutBound bound -- raise that bound, or find why the host stopped progressing"
            }
        }
        elseif ($reportExists) {
            # THE DISAMBIGUATOR'S PRESENT DIRECTION: a report on disk means this
            # run got at least as far as ShutdownGraphPath's report block, so
            # exitReason is the authoritative fact -- read it, never the raw
            # process exit code (main.cpp's "3 means two different things"
            # collision is exactly why).
            try {
                $report = Get-Content $reportPath -Raw | ConvertFrom-Json
            } catch {
                # Indeterminate: a report we cannot read tells us nothing about
                # the render, which is not the same as the render being wrong.
                $verdict = 'Indeterminate'
                $detail = "report at $reportPath exists but failed to parse as JSON: $($_.Exception.Message)"
                $report = $null
            }

            # ---- THE SCHEMA RANGE, ACTUALLY CHECKED. ----
            # VerifyReport::kSchemaVersion / kOldestSupportedSchemaVersion exist
            # to answer "is this document readable at all", and until now the
            # only thing that ever asked was VerifyReport's own tests. This
            # script parses the document for real -- and reads
            # compare.maxLocalDifference, WHICH ONLY EXISTS AT v4 -- so it is
            # the mechanism's natural first consumer. A future v5 that repurposed
            # a field would otherwise be read confidently and wrongly here, which
            # is the whole failure a version range exists to prevent.
            #
            # INDETERMINATE, not Failed: an unreadable document says nothing
            # about the render. The reason names the version so the next reader
            # is told what to widen.
            if ($report) {
                $reportSchema = $null
                if ($report.PSObject.Properties.Name -contains 'schemaVersion') {
                    $reportSchema = $report.schemaVersion
                }
                $schemaInt = 0
                if ($null -eq $reportSchema) {
                    # NOT a [int] cast on the raw value: [int]$null is silently 0
                    # in PowerShell, which would read a report with NO
                    # schemaVersion at all as version 0 and then reject it with a
                    # number the file never contained.
                    $verdict = 'Indeterminate'
                    $detail = "report at $reportPath carries NO schemaVersion field -- cannot establish it is readable, so nothing it says is trusted here"
                    $report = $null
                } elseif (-not [int]::TryParse([string]$reportSchema, [ref]$schemaInt)) {
                    $verdict = 'Indeterminate'
                    $detail = "report at $reportPath has a non-integer schemaVersion '$reportSchema' -- cannot establish it is readable"
                    $report = $null
                } elseif ($schemaInt -lt $script:ReportSchemaMin -or $schemaInt -gt $script:ReportSchemaMax) {
                    $verdict = 'Indeterminate'
                    $detail = "report at $reportPath is schemaVersion $schemaInt, outside this gate's supported range " +
                              "$($script:ReportSchemaMin)..$($script:ReportSchemaMax) (VerifyReport::kOldestSupportedSchemaVersion..kSchemaVersion). " +
                              "A document this script cannot claim to understand is not evidence about the render."
                    $report = $null
                }
            }

            if ($report) {
                $exitReason = $report.exitReason
                $comparePassed = $false
                $resolvedLevel = ''
                $maxLocal = 'n/a'
                if ($report.PSObject.Properties.Name -contains 'compare') {
                    $comparePassed = [bool]$report.compare.passed
                    if ($report.compare.PSObject.Properties.Name -contains 'resolvedLevel') {
                        $resolvedLevel = [string]$report.compare.resolvedLevel
                    }
                    if ($report.compare.PSObject.Properties.Name -contains 'maxLocalDifference') {
                        $maxLocal = $report.compare.maxLocalDifference
                    }
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

                # Precedence, first match wins. Mirrors the spec's section 2,
                # with two corrections made against the PRODUCER (RuntimeApp.cpp
                # :1124-1165, mirrored in EditorApp.cpp) rather than the spec:
                #
                #  1. "gpu-stall" is NOT in this list, though the spec's section 1
                #     lists it as an eighth exitReason. It is not one. It is a
                #     crash-envelope KIND produced by Diagnostics::DeriveKind
                #     (Diagnostics.cpp:214) in a separate .arcdiag structure, and
                #     is never assigned to VerifyReport's exitReason. A branch
                #     that matches nothing is a declared knob that is never
                #     applied -- the exact defect this arc exists to abolish --
                #     so it is omitted deliberately. Do not re-add it from the
                #     spec. Errored stays reachable via the three below.
                #  2. Three reasons that mean "could not tell" used to fall
                #     through to Failed. See the Indeterminate branch below.
                if ($exitReason -in @('device-lost', 'render-failed', 'validation-errors')) {
                    # The subject ran and then DIED. Not the same next action as
                    # a pixel mismatch, which is why it is not Failed.
                    $verdict = 'Errored'
                    $detail = "exitReason=$exitReason -- the host died before it could answer"
                }
                elseif ($exitReason -in @('compare-missing-reference', 'settle-not-converged', 'stopped-early', 'compare-blessed')) {
                    # The render may be perfectly correct; nothing established
                    # otherwise. Red, but NOT "the render is wrong" -- and the
                    # distinction is the reason this vocabulary exists.
                    #   compare-missing-reference: no reference to compare against.
                    #   settle-not-converged:      the compare NEVER RAN. RuntimeApp.cpp:1158
                    #                              picks this over "compare-failed" precisely
                    #                              when m_compareEvaluated is false. The gate
                    #                              passes --settle 30 on every lane, so this is
                    #                              live, not hypothetical.
                    #   stopped-early:             the run ended before the frame budget, so the
                    #                              capture is not of the frame we asked about.
                    #   compare-blessed:           a --bless run verified nothing. The gate never
                    #                              blesses, so this is unreachable here -- but
                    #                              mapping it to Failed would simply be false.
                    $verdict = 'Indeterminate'
                    $detail = "exitReason=$exitReason -- ran, but nothing established the render's correctness"
                }
                elseif ($exitReason -eq 'frames-complete' -and $comparePassed) {
                    # ONE DIRECTION ONLY -- declared 'backend', resolved
                    # 'shared'. That is what "fell back" means: the lane wanted
                    # its own reference and had to inherit one. The old
                    # `$resolvedLevel -ne $expectedLevel` also fired the OTHER
                    # way: bless a dx12/editor-ui.png and that lane (declared
                    # 'shared') would resolve 'backend' and be reported as
                    # having fallen back for GAINING a reference of its own.
                    # The spec's wording is one-directional; so is this now.
                    if ($expectedLevel -eq 'backend' -and $resolvedLevel -eq 'shared') {
                        $verdict = 'PassedOnFallback'
                        $detail = "exitReason=$exitReason diffCount=$($report.compare.diffCount) maxLocalDifference=$maxLocal resolvedLevel=$resolvedLevel (expected $expectedLevel)"
                    } else {
                        $verdict = 'Passed'
                        $detail = "exitReason=$exitReason diffCount=$($report.compare.diffCount) maxLocalDifference=$maxLocal resolvedLevel=$resolvedLevel"
                    }
                }
                else {
                    $verdict = 'Failed'
                    $diffCount = if ($report.PSObject.Properties.Name -contains 'compare') { $report.compare.diffCount } else { 'n/a' }
                    $errorMessage = if ($report.PSObject.Properties.Name -contains 'compare') { $report.compare.errorMessage } else { '' }
                    $detail = "exitReason=$exitReason comparePassed=$comparePassed diffCount=$diffCount maxLocalDifference=$maxLocal errorMessage='$errorMessage'"
                }
            }
        } else {
            # THE DISAMBIGUATOR'S ABSENT DIRECTION: NOT proof of a pre-boot
            # refusal -- VerifyReport::WriteTo can itself fail post-boot (full
            # disk, permissions). "Could not determine" is the honest verdict,
            # and stderr is the one place a write failure's ARC_ERROR line
            # would have landed, so surface it rather than guessing.
            $verdict = 'Indeterminate'
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

        if ($verdict -in $script:GreenVerdicts) {
            $colour = if ($verdict -eq 'Passed') { 'Green' } else { 'Yellow' }
            Write-Host "$verdict -- $label ($detail)" -ForegroundColor $colour
        } elseif ($verdict -eq 'Skipped') {
            Write-Host "Skipped -- $label ($skipReason)" -ForegroundColor DarkGray
        } else {
            Write-Host "$verdict -- $label" -ForegroundColor Red
            Write-Host "  $detail" -ForegroundColor Red
            if (Test-Path $diffPathToReport) {
                Write-Host "  DIFF ARTIFACT: $diffPathToReport" -ForegroundColor Yellow
            } else {
                Write-Host "  (no diff artifact on disk at the expected path: $diffPathToReport)" -ForegroundColor Yellow
            }
            # Every failure names the exact command that reproduces it. The gate
            # reported lane names and diff paths but never "run this", which put
            # the burden of reconstructing an eight-argument invocation on
            # whoever is already dealing with a red build.
            $quoted = ($exeArgs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
            Write-Host "  REPRODUCE:  cd `"$exeDir`"; .\$exeName $quoted" -ForegroundColor Cyan
        }

        $results += [pscustomobject]@{ Combo = $label; Verdict = $verdict; Detail = $detail; SkipReason = $skipReason }
    }

    # A stale exclusion is ITSELF the failure -- not the thing it excludes. This
    # is what makes the expiry a mechanism rather than a date in a comment.
    # Reported as a synthetic lane so it appears in the machine-readable summary
    # exactly like any other verdict, and counts toward $redCount.
    foreach ($e in $expiredExclusions) {
        $results += [pscustomobject]@{
            Combo = "automation-exclusions/$($e.target)"
            Verdict = 'Failed'
            Detail = "exclusion EXPIRED on $($e.expires) (today is $today) -- reason was: $($e.reason). Either the problem is fixed (delete the entry) or it is not (give it a new date and a fresh justification)."
            SkipReason = $null
        }
        Write-Host ""
        Write-Host "Failed -- automation-exclusions/$($e.target)" -ForegroundColor Red
        Write-Host "  EXPIRED on $($e.expires), today is $today. Reason was: $($e.reason)" -ForegroundColor Red
    }
} finally {
    if ($SelfTest) {
        # R12: forward slashes -- a backslash pathspec here would silently
        # no-op (see the mutation block above).
        & git -C $repoRoot checkout -- $sceneRelative
        $dirty = & git -C $repoRoot status --porcelain -- $sceneRelative
        if ($dirty) { Exit-GateRefusal "-SelfTest: FAILED TO RESTORE $sceneRelative -- fix by hand before continuing." }

        # AND RESTAGE THE RESTORED SCENE TOO. Without this the source goes
        # clean while the staged copies -- what the hosts actually read --
        # stay broken, so `git status` says everything is fine and the next
        # run (this script's or anyone else's) launches against a scene
        # nobody can see is wrong. desk-verify-golden-gate.ps1:155-159 says
        # it in as many words: "a restore that only restores what git
        # tracks is not a restore." Nothing downstream reads these staged
        # copies today -- the next gate invocation re-stages Content/ itself
        # unconditionally -- so this is defensive, not load-bearing, but two
        # scripts sharing one mutation vocabulary by explicit ruling must
        # not hold opposite definitions of "restored." Wrapped so a
        # staging-copy failure is reported but never masks the git restore's
        # own verdict above -- the git restore is the one thing this block
        # must be trusted on.
        foreach ($h in @('ArcaneRuntime', 'ArcaneEditor')) {
            $stagedRestoreDest = Join-Path $repoRoot "bin\$configDirName\$h\ReferenceProject\Content"
            if (Test-Path $stagedRestoreDest) {
                try {
                    Copy-Item -Path (Join-Path $referenceProjectDir 'Content\*') -Destination $stagedRestoreDest -Force -Recurse
                } catch {
                    Write-Host "-- -SelfTest: WARNING -- could not restage the restored scene to $stagedRestoreDest ($($_.Exception.Message)) --" -ForegroundColor Yellow
                }
            }
        }
        Write-Host "-- -SelfTest: scene restored (source + both staged copies) --" -ForegroundColor DarkGray
    }
}

Write-Host ""
Write-Host "=== golden-gate summary ($Configuration) ===" -ForegroundColor Cyan
$results | Format-Table -AutoSize -Wrap

# ---- THE MACHINE-READABLE VERDICT. ----
#
# WHY THIS EXISTS. This gate's whole purpose is to be consumed by an AGENT --
# that is what the arc it belongs to was built for. A process exit code and
# English prose alone leave a consumer with no per-lane detail to inspect
# programmatically.
#
# Found by a desk pass on 2026-08-26, by asking what THIS TOOL'S ACTUAL
# CONSUMER sees rather than what a person sees.
#
# So: one aggregated file, per configuration, next to the build output.
# Consumers should assert on `gatePassed` and the per-lane `verdict` fields,
# never on the exit code alone.
# gatePassed: at least one lane actually PASSED, and no lane is red. The
# "at least one" half is the vacuity guard -- without it a run where every
# lane was skipped or excluded reports success having verified nothing.
$greenCount = @($results | Where-Object { $_.Verdict -in $script:GreenVerdicts }).Count
$redCount   = @($results | Where-Object { $_.Verdict -in $script:RedVerdicts }).Count
$gatePassed = ($greenCount -gt 0) -and ($redCount -eq 0)

# Written through the SAME writer every refusal path uses (defined at the top of
# this script), so an ordinary verdict and a refusal are the same document shape
# and a consumer never has to branch on which one it got. `refusalReason` is
# empty here: this run reached its lanes. The per-lane detail is exactly what
# makes a failure -- gate or self-test -- diagnosable, which is why the write is
# never suppressed, only redirected to a separate filename under -SelfTest.
Write-GateSummaryFile -GatePassed $gatePassed -RefusalReason '' -Lanes @($results | ForEach-Object {
    [pscustomobject]@{
        combo      = $_.Combo
        verdict    = $_.Verdict
        detail     = $_.Detail
        skipReason = $_.SkipReason
    }
})

# ---- -SelfTest: every lane must have NOTICED. ----
# R11: this block sits HERE -- after the summary is written above, before the
# ordinary gatePassed check below -- and nowhere else. In -SelfTest mode all
# four lanes FAIL by design, so an ordinary run would already have exited red;
# placing this block after those exit paths would make it dead code that never
# asserts anything. Placing it inside Write-GateSummaryFile's own try is worse:
# that catch exists specifically to stop summary I/O from deciding the verdict,
# and it would swallow this block's own errors too.
#
# The restore itself no longer lives here (review pass 2026-08-31, Important
# #1) -- it now happens in the finally around the staging+host-launch block
# above, which protects the WHOLE mutation-to-restore window instead of just
# this tail. By the time this block runs, main.arcscene is ALREADY back to
# its committed state; this block only grades what the four lanes reported.
if ($SelfTest) {
    # Review pass 2026-08-31, Important #2: the old umbrella 'FAIL' covered
    # things that did NOT mean "the gate noticed a broken render." The verdict
    # vocabulary now names them: exe-not-found is NotRun (manufactured before
    # any host launches), and a short $results count (fewer lanes ran
    # than $combos declares) both mean the self-test never got the chance to
    # prove anything -- reporting PASS on either would be exactly the
    # vacuous self-test this mode exists to rule out. A POST-BOOT error (a
    # host that launched, then crashed or errored out instead of cleanly
    # comparing) still counts as noticing, per the spec -- this does not
    # weaken that rule, it only excludes lanes that never got as far as
    # trying.
    # THE OTHER HALF OF THE WIRE CONTRACT. VerdictTest.cpp pins Arcane::Verdict's
    # string set; this pins the copy this script carries. PowerShell cannot
    # include the header, so the two are pinned independently and must be changed
    # in the same commit. Sourced from the BUILT EXE so it cannot go stale
    # against a rebuilt engine.
    # Its OWN flag, deliberately NOT $selfTestOk: the lane-grading cascade below
    # ends in an else that assigns $selfTestOk unconditionally, so writing the
    # probe result there would be silently clobbered whenever the hosts behave
    # normally -- which is EXACTLY the drift case this probe exists to catch.
    #
    # AND IT COMPARES THE TWO LISTS, which the exit-code probe never did. Running
    # `ArcaneTests.exe "[verdict]"` and checking only its exit status proves the
    # C++ side agrees WITH ITSELF -- VerdictTest.cpp's literals against
    # Arcane::ToString. It says nothing about $script:VerdictNames above. Rename
    # Indeterminate to Unknown in Verdict.hpp/.cpp and update VerdictTest.cpp in
    # the same commit -- exactly what Verdict.hpp's own header instructs -- and
    # the probe still exits 0 while this script keeps the stale word. The spec
    # says the self-test "asserts its set matches the one the header produces"
    # and its risk table claims the vocabulary is "pinned from both sides"; the
    # exit code alone did not deliver that. So the [verdict] cases now PUBLISH
    # what the engine actually produces (VerdictTest.cpp writes
    # automation-vocabulary.txt beside the exe) and this reads it back and
    # diffs it, order included.
    $vocabOk = $true
    $verdictProbe = Join-Path $repoRoot "bin\$configDirName\ArcaneTests\ArcaneTests.exe"
    $probeDir     = Split-Path -Parent $verdictProbe
    $vocabFile    = Join-Path $probeDir 'automation-vocabulary.txt'
    if (-not (Test-Path $verdictProbe)) {
        # A MISSING SUITE IS A FAILURE, NOT AN EXEMPTION. By this point
        # -SelfTest has already launched four hosts out of this same
        # bin/$configDirName/ tree, so ArcaneTests.exe missing from it means the
        # BUILD IS INCOMPLETE -- not that the check does not apply here. This
        # used to print a yellow warning and leave $vocabOk true, so
        # "SELF-TEST PASSED" stayed reachable with the vocabulary never
        # cross-checked at all: a check that silently stops checking, inside the
        # mode whose entire purpose is proving a check can bite.
        Write-Host ""
        Write-Host "SELF-TEST FAILED -- ArcaneTests.exe is absent at $verdictProbe, so the verdict vocabulary was NEVER cross-checked." -ForegroundColor Red
        Write-Host "This mode already launched its hosts out of bin\$configDirName, so a missing suite there means the build is incomplete." -ForegroundColor Red
        Write-Host "Build first:  msbuild Arcane.slnx /p:Configuration=$Configuration /m" -ForegroundColor Yellow
        $vocabOk = $false
    } else {
        # DELETE THE PUBLISHED FILE FIRST. Left in place, a copy from an older
        # build would answer for a vocabulary this engine no longer has -- the
        # stale-artifact failure this script already learned once with leftover
        # diff PNGs.
        if (Test-Path $vocabFile) { Remove-Item $vocabFile -Force }

        # Start-Process with explicit redirection, NOT `& $exe ... 2>&1`. Under
        # Windows PowerShell 5.1 that idiom wraps every stderr line from a
        # NATIVE exe in an ErrorRecord, and with $ErrorActionPreference = 'Stop'
        # set at the top of this script a single engine log line reaching stderr
        # becomes a script-terminating NativeCommandError -- killing the whole
        # self-test on output that was never an error. -WorkingDirectory does
        # what the old Push-Location did: this suite runs FROM the exe
        # directory, where its fixtures, data/ and automation-exclusions.json
        # are staged.
        $probeStdout = Join-Path $probeDir 'golden-gate-verdict-probe-stdout.txt'
        $probeStderr = Join-Path $probeDir 'golden-gate-verdict-probe-stderr.txt'
        $probeProc = Start-Process -FilePath $verdictProbe -ArgumentList @('[verdict]') `
            -WorkingDirectory $probeDir -NoNewWindow -Wait -PassThru `
            -RedirectStandardOutput $probeStdout -RedirectStandardError $probeStderr
        $probeCode = $probeProc.ExitCode
        $probeOut = ''
        foreach ($streamFile in @($probeStdout, $probeStderr)) {
            if (Test-Path $streamFile) {
                $text = (Get-Content $streamFile -Raw)
                if ($text) { $probeOut += $text }
            }
        }

        if ($probeCode -ne 0) {
            Write-Host ""
            Write-Host "SELF-TEST FAILED -- the [verdict] cases do not pass (exit $probeCode), so this script's copy of the vocabulary cannot be trusted." -ForegroundColor Red
            Write-Host $probeOut
            $vocabOk = $false
        } elseif (-not (Test-Path $vocabFile)) {
            Write-Host ""
            Write-Host "SELF-TEST FAILED -- the [verdict] cases passed but published no $vocabFile." -ForegroundColor Red
            Write-Host "VerdictTest.cpp's publishing case is what this script diffs against; without it the vocabulary is unpinned." -ForegroundColor Red
            Write-Host $probeOut
            $vocabOk = $false
        } else {
            $published = @(Get-Content $vocabFile | ForEach-Object { $_.Trim() } | Where-Object { $_ })
            $publishedVerdicts = @($published | Where-Object { $_ -cmatch '^verdict=' } |
                                   ForEach-Object { $_ -creplace '^verdict=', '' })

            # Joined and compared with -cne (case-SENSITIVE; plain -ne is not).
            # Comparing the joins pins ORDER as well as membership, which is the
            # contract VerdictTest.cpp's own first case states: "exactly these
            # seven, in this order".
            $mine   = ($script:VerdictNames -join '|')
            $theirs = ($publishedVerdicts -join '|')
            if ($mine -cne $theirs) {
                Write-Host ""
                Write-Host "SELF-TEST FAILED -- the verdict vocabulary has DRIFTED between Arcane::Verdict and this script." -ForegroundColor Red
                Write-Host "  engine publishes: $theirs" -ForegroundColor Red
                Write-Host "  this script has:  $mine" -ForegroundColor Red
                # BACKTICK, not backslash. PowerShell's escape character is the
                # backtick; `\$script:VerdictNames` in a double-quoted string
                # would still interpolate the array and print seven words where
                # a variable NAME was meant.
                Write-Host "Change BOTH in the same commit: Verdict.hpp/.cpp + VerdictTest.cpp, and `$script:VerdictNames in this file." -ForegroundColor Red
                $vocabOk = $false
            }

            # The report schema range rides the same file, for the same reason:
            # this script reads compare.maxLocalDifference, which only exists at
            # v4, and now range-checks every report it parses. Two integers
            # copied by hand and compared by nothing is the same defect in
            # miniature.
            foreach ($pin in @(
                @{ Key = 'reportSchemaMin'; Mine = $script:ReportSchemaMin; Variable = 'ReportSchemaMin'; Header = 'VerifyReport::kOldestSupportedSchemaVersion' },
                @{ Key = 'reportSchemaMax'; Mine = $script:ReportSchemaMax; Variable = 'ReportSchemaMax'; Header = 'VerifyReport::kSchemaVersion' }
            )) {
                $line = @($published | Where-Object { $_ -cmatch ('^' + $pin.Key + '=') }) | Select-Object -First 1
                if (-not $line) {
                    Write-Host ""
                    Write-Host "SELF-TEST FAILED -- $vocabFile publishes no '$($pin.Key)', so this script's copy of $($pin.Header) is unpinned." -ForegroundColor Red
                    $vocabOk = $false
                    continue
                }
                $theirValue = ($line -creplace ('^' + $pin.Key + '='), '')
                if ("$($pin.Mine)" -cne $theirValue) {
                    Write-Host ""
                    Write-Host "SELF-TEST FAILED -- $($pin.Key) has DRIFTED: the engine publishes $theirValue, this script has $($pin.Mine)." -ForegroundColor Red
                    Write-Host "Change BOTH in the same commit: $($pin.Header) and this script's `$script:$($pin.Variable)." -ForegroundColor Red
                    $vocabOk = $false
                }
            }
        }
    }

    # The ordinary -SelfTest mutation breaks the scene, so every lane that ran
    # must report Failed -- the verdict meaning "the subject ran and did not meet
    # its contract". NotRun, Skipped, Indeterminate and Errored all mean the lane
    # never got to notice anything, and reporting a pass on any of them would be
    # exactly the vacuous self-test this mode exists to rule out. That
    # distinction used to need a bespoke $exeMissingCount; the vocabulary now
    # carries it, so this asserts on Failed DIRECTLY rather than on "not green".
    $notFailed = @($results | Where-Object { $_.Verdict -ne 'Failed' })
    $unknown   = @($results | Where-Object { $_.Verdict -notin $script:VerdictNames })

    if ($unknown.Count -gt 0) {
        Write-Host ""
        Write-Host "SELF-TEST FAILED -- $($unknown.Count) lane(s) reported a verdict outside the declared vocabulary:" -ForegroundColor Red
        $unknown | ForEach-Object { Write-Host "  $($_.Combo) reported '$($_.Verdict)'" -ForegroundColor Red }
        $selfTestOk = $false
    } elseif ($results.Count -ne $combos.Count) {
        Write-Host ""
        Write-Host "SELF-TEST FAILED -- expected $($combos.Count) lane result(s), got $($results.Count). A self-test that cannot even count its own lanes cannot be trusted to grade them." -ForegroundColor Red
        $selfTestOk = $false
    } elseif ($notFailed.Count -gt 0) {
        Write-Host ""
        Write-Host "SELF-TEST FAILED -- the gate did NOT notice a broken scene." -ForegroundColor Red
        $notFailed | ForEach-Object { Write-Host "  $($_.Combo) reported $($_.Verdict)" -ForegroundColor Red }
        Write-Host "A gate that cannot fail is not a gate. Fix the gate, not this check." -ForegroundColor Red
        $selfTestOk = $false
    } elseif (-not $vocabOk) {
        # The lanes graded correctly, but the wire contract is broken. Without
        # this arm the else below would report PASSED over the top of the
        # probe's own FAILED line -- healthy hosts masking a drifted vocabulary.
        Write-Host ""
        Write-Host "SELF-TEST FAILED -- the lanes graded correctly, but the vocabulary pin above did not hold. Arcane::Verdict and this script have drifted apart." -ForegroundColor Red
        $selfTestOk = $false
    } else {
        Write-Host ""
        Write-Host "SELF-TEST PASSED -- all $($results.Count) lane(s) launched and caught the broken scene." -ForegroundColor Green
        $selfTestOk = $true
    }
    # The self-test INVERTS the ordinary verdict: red lanes are the pass.
    if ($selfTestOk) { exit 0 } else { exit 1 }
}

if (-not $gatePassed) {
    if ($greenCount -eq 0) {
        Write-Host "golden-gate: FAILED -- NO lane passed. $($results.Count) lane(s) ran; a gate that verified nothing is not a green gate." -ForegroundColor Red
    } else {
        Write-Host "golden-gate: FAILED -- $redCount lane(s) red." -ForegroundColor Red
    }
    exit 1
}

Write-Host "golden-gate: $greenCount lane(s) passed, 0 red" -ForegroundColor Green
exit 0
