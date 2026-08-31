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

# ---- The four combinations. ----
$combos = @(
    @{ Host = 'ArcaneRuntime'; Exe = 'ArcaneRuntime.exe'; Reference = 'runtime-scene'; Backend = 'dx12' }
    @{ Host = 'ArcaneRuntime'; Exe = 'ArcaneRuntime.exe'; Reference = 'runtime-scene'; Backend = 'vulkan' }
    @{ Host = 'ArcaneEditor'; Exe = 'ArcaneEditor.exe'; Reference = 'editor-ui'; Backend = 'dx12' }
    @{ Host = 'ArcaneEditor'; Exe = 'ArcaneEditor.exe'; Reference = 'editor-ui'; Backend = 'vulkan' }
)

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
                Write-Error ("-SelfTest: auto-heal of $sceneRelative did NOT clean the tree -- refusing " +
                    "to continue. Remaining status:`n" + ($postHealDirty -join "`n"))
                exit 1
            }
            Write-Host "-- -SelfTest: residue healed, ReferenceProject/ is clean -- continuing --" -ForegroundColor Green
        } else {
            Write-Error ("-SelfTest: refusing to run -- ReferenceProject/ is NOT clean, and this mode's " +
                "mutate-then-'git checkout --' restore cycle would silently discard whatever is there " +
                "now. Commit, stash, or discard these changes first, then re-run:`n" + ($preDirty -join "`n"))
            exit 1
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
        Write-Error "-SelfTest: could not find MeshCube's Arcane::Transform position at 0.4 in $sceneRelative -- the fixture moved; fix this script rather than reporting a pass."
        exit 1
    }
    $broken = [Regex]::Replace($sceneText,
        '(?s)("name":\s*"MeshCube".*?"Arcane::Transform":\s*\{\s*"position":\s*\[\s*)0\.4',
        '${1}0.6', 1)
    if ($broken -eq $sceneText) {
        Write-Error "-SelfTest: mutation did not apply to $sceneRelative -- refusing to run the lanes against an unbroken scene."
        exit 1
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
                Write-Error "restaging FAILED -- staged scene still differs from source after the copy ($stagedScene)."
                exit 1
            }
        }
    }
    Write-Host "-- ReferenceGame.dll + Content/ restaged beside both hosts --" -ForegroundColor Green

    $results = @()
    $anyFailure = $false
    $exeMissingCount = 0   # -SelfTest's assertion needs to know whether every lane genuinely launched, not just whether its Verdict is FAIL (see the assertion block below) -- a lane that never launched never had the chance to notice anything.

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
            # Write-Host, NOT Write-Error: $ErrorActionPreference = 'Stop' (above)
            # makes Write-Error TERMINATING, which made the three lines that follow
            # it DEAD CODE -- a single missing exe aborted the whole gate instead of
            # recording FAIL for this lane and testing the other three. The intent
            # was always "record it and carry on"; this is what actually does that.
            Write-Host "$exePath does not exist -- build Arcane.slnx for $Configuration first." -ForegroundColor Red
            $results += [pscustomobject]@{ Combo = $label; Verdict = 'FAIL'; Detail = "exe not found: $exePath" }
            $exeMissingCount++
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
} finally {
    if ($SelfTest) {
        # R12: forward slashes -- a backslash pathspec here would silently
        # no-op (see the mutation block above).
        & git -C $repoRoot checkout -- $sceneRelative
        $dirty = & git -C $repoRoot status --porcelain -- $sceneRelative
        if ($dirty) { Write-Error "-SelfTest: FAILED TO RESTORE $sceneRelative -- fix by hand before continuing."; exit 1 }

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
$summary = [pscustomobject]@{
    schemaVersion = 1
    configuration = $Configuration
    gatePassed    = (-not $anyFailure)
    selfTest      = [bool]$SelfTest
    lanes         = @($results | ForEach-Object {
        [pscustomobject]@{
            combo   = $_.Combo
            verdict = $_.Verdict
            detail  = $_.Detail
        }
    })
}
# R20: under -SelfTest, write to a SEPARATE file. Task 5 runs the self-test
# stage immediately after the ordinary gate stage on the same agent, so an
# in-place write here would overwrite a green build's gatePassed=true summary
# with an inverted one -- and every documented consumer of this gate is told
# to assert on gatePassed in golden-gate-summary.json. Do not suppress the
# write instead: the per-lane detail is exactly what makes a self-test
# failure diagnosable. The summary object also carries "selfTest" (Minor,
# review pass 2026-08-31) alongside the distinct filename, not instead of
# it -- a consumer that globs golden-gate*summary.json can then tell the two
# apart from the JSON itself, not only from which filename it happened to
# match.
$summaryFileName = if ($SelfTest) { 'golden-gate-selftest-summary.json' } else { 'golden-gate-summary.json' }
$summaryPath = Join-Path $repoRoot "bin\$configDirName\$summaryFileName"
try {
    $summaryDir = Split-Path -Parent $summaryPath
    if (-not (Test-Path $summaryDir)) { New-Item -ItemType Directory -Path $summaryDir -Force | Out-Null }
    # NOT `Set-Content -Encoding UTF8`: under Windows PowerShell 5.1 that
    # encoding emits a UTF-8 BOM, so both summary files would start with
    # EF BB BF and a stock JSON reader -- e.g. Python's json.load, the exact
    # "AGENT" consumer this file exists for -- fails with
    # "JSONDecodeError: Expecting value: line 1 column 1 (char 0)".
    # PowerShell's own ConvertFrom-Json tolerates the BOM, which is exactly
    # why this went unnoticed. WriteAllText with a BOM-less UTF8Encoding
    # writes the same bytes without it. $summaryPath is already absolute
    # (built off $repoRoot via Join-Path above), which WriteAllText requires.
    [System.IO.File]::WriteAllText($summaryPath, ($summary | ConvertTo-Json -Depth 5), (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "golden-gate: machine-readable verdict -> $summaryPath" -ForegroundColor Cyan
} catch {
    # Never let summary-writing decide the gate's verdict; say so and continue.
    Write-Host "golden-gate: WARNING -- could not write $summaryPath ($($_.Exception.Message))" -ForegroundColor Yellow
}

# ---- -SelfTest: every lane must have NOTICED. ----
# R11: this block sits HERE -- after the summary try/catch above closes,
# before $anyFailure is checked below -- and nowhere else. In -SelfTest mode
# all four lanes FAIL by design, so $anyFailure is $true; placing this block
# after the ordinary exit paths would make it dead code that never asserts
# anything. Placing it inside the summary try above is worse: that catch
# exists specifically to stop summary I/O from deciding the verdict, and it
# would swallow this block's own errors too.
#
# The restore itself no longer lives here (review pass 2026-08-31, Important
# #1) -- it now happens in the finally around the staging+host-launch block
# above, which protects the WHOLE mutation-to-restore window instead of just
# this tail. By the time this block runs, main.arcscene is ALREADY back to
# its committed state; this block only grades what the four lanes reported.
if ($SelfTest) {
    # Review pass 2026-08-31, Important #2: 'FAIL' is an umbrella, and only
    # some of what it covers means "the gate noticed a broken render."
    # exe-not-found (manufactured before any host launches -- see
    # $exeMissingCount above) and a short $results count (fewer lanes ran
    # than $combos declares) both mean the self-test never got the chance to
    # prove anything -- reporting PASS on either would be exactly the
    # vacuous self-test this mode exists to rule out. A POST-BOOT error (a
    # host that launched, then crashed or errored out instead of cleanly
    # comparing) still counts as noticing, per the spec -- this does not
    # weaken that rule, it only excludes lanes that never got as far as
    # trying.
    $notFailed = @($results | Where-Object { $_.Verdict -ne 'FAIL' })
    if ($exeMissingCount -gt 0) {
        Write-Host ""
        Write-Host "SELF-TEST FAILED -- $exeMissingCount lane(s) never launched (exe not found). A lane that never ran never had the chance to notice anything; this run proves nothing about the gate." -ForegroundColor Red
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
    } else {
        Write-Host ""
        Write-Host "SELF-TEST PASSED -- all $($results.Count) lane(s) launched and caught the broken scene." -ForegroundColor Green
        $selfTestOk = $true
    }
    # The self-test INVERTS the ordinary verdict: red lanes are the pass.
    if ($selfTestOk) { exit 0 } else { exit 1 }
}

if ($anyFailure) {
    Write-Host "golden-gate: FAILED" -ForegroundColor Red
    exit 1
}

Write-Host "golden-gate: all four comparisons PASSED" -ForegroundColor Green
exit 0
