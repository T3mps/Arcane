# verify-arcbuild-backends.ps1 -- live Make/Ninja backend acceptance for
# arcbuild (multibackend hardening Task 7, spec docs/specs/
# 2026-09-20-arcbuild-multibackend-hardening-design.md s10/s11, and the
# original driver spec docs/specs/2026-09-13-arcbuild-driver-design.md).
#
# WHAT THIS PROVES that a Windows desk cannot get any other way: [build]'s
# unit/structural tests (BuildDriverTest.cpp) prove composition and decision
# policy with fakes; the opt-in [build-generator] Catch2 case proves the
# bundled Premake produces the artifacts BackendResolver expects. Neither
# spawns a REAL make.exe/ninja.exe against a REAL generated project and
# checks that the module actually compiles, rebuilds, and cleans -- that is
# this script's one job, plus the clean-precedence edge cases (spec s9)
# that only mean something against a real filesystem and a real child exit
# code.
#
# TWO DIFFERENT COMPILERS, not one. Premake 5.0.0-beta8's "ninja" action
# defaults to the MSVC ("msc") toolset on a native Windows target (confirmed
# by inspecting a real generated Fixture.ninja: its link rule is
# `link_msc`), while its "gmake" action defaults to GCC regardless of host
# OS (confirmed the same way: a real generated Fixture.make sets
# `CXX = g++`). arcbuild's Compose.cpp does not add a `--cc` override to
# either action, so this script needs a Visual Studio developer environment
# (cl.exe/link.exe) for the Ninja backend and a MinGW-w64 GCC/G++ toolchain
# for the Make backend -- two independent refusal checks below, each with
# its own clear message, never a single generic "compiler missing".
#
# UNIQUE TEMP DIRECTORIES ONLY, VALIDATED BEFORE DELETE. Every backend run
# and every edge case copies the committed, input-only ArcaneTests/data/
# arcbuild-fixture/ into its own directory under the SYSTEM temp root
# (never generates in place, never mutates the committed fixture). Cleanup
# (New-VerifyTempDir's caller always wraps its work in try/finally) resolves
# both the candidate directory and the system temp root with
# [System.IO.Path]::GetFullPath and refuses to delete anything that is not
# a strict descendant of that root -- the same guard the opt-in
# [build-generator] Catch2 case applies with weakly_canonical on the C++
# side.
#
# THE FOUR CLEAN-PRECEDENCE EDGE CASES (spec s9, BuildPipeline::Clean +
# MergeCleanResults):
#   1. no generated backend context (soft skip, logged) + successful
#      filesystem clean -> 0.
#   2. a real backend clean that fails (an invalid --config the generated
#      Makefile/build.ninja itself refuses) + successful filesystem clean
#      -> the backend's OWN exit code, unchanged.
#   3. a real backend clean that succeeds + a filesystem clean that fails
#      (a file held open with FileShare::None inside Binaries/, which
#      remove_all cannot delete) -> 2 (kExitRefused), never the backend's 0.
#   4. both fail at once -> still 2 -- the filesystem refusal outranks the
#      backend's own failing code, not just its success.
# Case 2's invalid-config trick never touches the filesystem, and case 3's
# locked file lives in Binaries/ (never Intermediate/<config>/, which is
# also the generated project's own objdir -- a lock placed THERE could make
# the backend's own clean recipe fail too and collapse the case-3/case-4
# distinction). This script does not re-prove that a failing Make/Ninja
# REBUILD's clean step suppresses its build step -- that is
# BuildDriverTest.cpp's FakeProcessRunner-based
# "arcbuild::ExecutePlan stops a plan at the first failing step" case,
# already covered without spawning anything.
#
# Usage:
#   Get-Command cl,ninja,make,mingw32-make -ErrorAction SilentlyContinue
#   .\scripts\verify-arcbuild-backends.ps1 -Configuration Debug
#
# Exit code: 0 if every check passed (including any that legitimately
# SKIPPED because a compiler was unavailable -- reported, never silent);
# 1 if any check produced an unexpected result.
#
# Windows PowerShell 5.1 compatible.

param(
    [string]$Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'

# A tool freshly added to the user PATH (e.g. by this task's winget installs)
# only reaches a shell that reads it AFTER the change -- winget's own
# "restart your shell" warning. APPENDING the Machine+User registry values
# to this session's own $env:Path (never replacing it) means this script
# sees a same-session install without forcing a terminal restart, while
# still honoring anything already on this session's PATH -- e.g. a
# Developer PowerShell / imported vcvars64.bat environment's cl.exe
# directory, which is process-scoped and would otherwise be wiped out by a
# registry-only reassignment. This never WRITES to either persisted scope,
# so it cannot mask a genuinely missing tool.
$env:Path = $env:Path + ';' +
    [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' +
    [Environment]::GetEnvironmentVariable('Path', 'User')

$repoRoot      = Split-Path -Parent $PSScriptRoot
$configDirName = "$Configuration-windows-x86_64-md"
$arcbuildExe   = Join-Path $repoRoot "bin\$configDirName\arcbuild\arcbuild.exe"
$fixtureSource = Join-Path $repoRoot 'ArcaneTests\data\arcbuild-fixture'

Write-Host "=== verify-arcbuild-backends: $Configuration ===" -ForegroundColor Cyan
Write-Host "repo    : $repoRoot"
Write-Host "arcbuild: $arcbuildExe"

if (-not (Test-Path $fixtureSource)) {
    Write-Error "fixture not found at '$fixtureSource' -- is this run from a checkout with ArcaneTests/data/arcbuild-fixture/ committed?"
    exit 2
}

if (-not (Test-Path $arcbuildExe)) {
    Write-Error @"
arcbuild.exe not found at '$arcbuildExe'.
Build it first (docs/specs/2026-09-13-arcbuild-driver-design.md's Build section), e.g.:
  msbuild arcbuild\arcbuild.vcxproj /p:Configuration=$Configuration /p:Platform=x64 /p:SolutionDir=$repoRoot\ /m /nologo /v:minimal
"@
    exit 2
}

# ---- Tool discovery ---------------------------------------------------------
#
# Make: mingw32-make first, then make (arcbuild's own Toolchain precedence,
# spec s7.2) -- reported explicitly by name+version, per this task's own
# self-review requirement (GnuWin32 Make 3.81 vs. a newer MinGW/MSYS2 Make
# is a real compatibility axis for the generated beta8 Makefile, not a
# cosmetic detail).
$cl   = Get-Command cl -ErrorAction SilentlyContinue
$ninja = Get-Command ninja -ErrorAction SilentlyContinue
$make = Get-Command mingw32-make -ErrorAction SilentlyContinue
if (-not $make) { $make = Get-Command make -ErrorAction SilentlyContinue }
$gxx  = Get-Command g++ -ErrorAction SilentlyContinue

function Get-MakeVersionLine {
    param($MakeCmd)
    if (-not $MakeCmd) { return '<not found>' }
    try {
        $first = & $MakeCmd.Source --version 2>$null | Select-Object -First 1
        return "$($MakeCmd.Name) -> $($MakeCmd.Source) ($first)"
    } catch {
        return "$($MakeCmd.Name) -> $($MakeCmd.Source) (version unknown)"
    }
}

Write-Host ""
Write-Host "--- tool discovery ---"
Write-Host "cl    : $(if ($cl)   { $cl.Source }   else { '<not found>' })"
Write-Host "ninja : $(if ($ninja){ $ninja.Source } else { '<not found>' })"
Write-Host "make  : $(Get-MakeVersionLine $make)"
Write-Host "g++   : $(if ($gxx)  { $gxx.Source }  else { '<not found>' })"

if (-not $ninja) {
    Write-Error "ninja not found on PATH -- winget install --id Ninja-build.Ninja, then add its directory to the user PATH."
    exit 2
}

if (-not $make) {
    Write-Error "make/mingw32-make not found on PATH -- install a GNU Make distribution (see this task's report for what this desk used) and add its directory to the user PATH."
    exit 2
}

# Ninja's generated project uses beta8's default MSVC toolset on a native
# Windows target (a real generated Fixture.ninja's link rule is
# `link_msc`) -- refuse clearly rather than fail confusingly partway
# through a build with a mysterious "cl: command not found" from ninja's
# own child process.
if (-not $cl) {
    Write-Error @"
cl.exe not found on PATH -- Ninja's generated build (beta8's default MSVC
toolset on Windows) needs a Visual Studio developer environment. Run this
script from "Developer PowerShell for VS 2026", or import vcvars64.bat's
environment into this session first, e.g.:
  `$vs = & "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  `$vcvars = Join-Path `$vs 'VC\Auxiliary\Build\vcvars64.bat'
  cmd /c "`"`$vcvars`" && set" | ForEach-Object {
      if (`$_ -match '^([^=]+)=(.*)`$') { [Environment]::SetEnvironmentVariable(`$matches[1], `$matches[2]) }
  }
"@
    exit 2
}

# The Make backend's OWN compiler dependency (gcc/g++, not cl.exe -- see
# this file's header). Unlike cl.exe above this does not abort the whole
# script: Make's generate + all four clean-precedence edge cases need no
# compiler at all (confirmed below), so they still run and are reported;
# only the real compiled build/rebuild step is skipped, exactly like Xcode/
# macOS is a documented live-validation limit rather than a silent gap.
$makeCompilerAvailable = [bool]$gxx
if (-not $makeCompilerAvailable) {
    Write-Warning "g++ not found on PATH -- the Make backend's real compiled build/rebuild step will be reported as SKIPPED (documented live-validation limit), not silently omitted. Generate and all four clean-precedence edge cases do not need a compiler and still run."
}

# ---- Result tracking ---------------------------------------------------------

$script:Results = New-Object System.Collections.Generic.List[object]

function Add-Result {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [bool]$Passed,
        [string]$Detail = ''
    )
    $script:Results.Add([pscustomobject]@{ Name = $Name; Passed = $Passed; Detail = $Detail })
    $status = if ($Passed) { 'PASS' } else { 'FAIL' }
    $color  = if ($Passed) { 'Green' } else { 'Red' }
    $suffix = if ($Detail) { " -- $Detail" } else { '' }
    Write-Host ("[{0}] {1}{2}" -f $status, $Name, $suffix) -ForegroundColor $color
}

function Add-Skipped {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [string]$Reason = ''
    )
    $script:Results.Add([pscustomobject]@{ Name = $Name; Passed = $true; Detail = "SKIPPED -- $Reason" })
    Write-Host ("[SKIP] {0} -- {1}" -f $Name, $Reason) -ForegroundColor Yellow
}

# ---- Unique temp directories, validated before delete ------------------------

$systemTempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())

function New-VerifyTempDir {
    param([Parameter(Mandatory)] [string]$Tag)

    $base = Join-Path $systemTempRoot 'arcbuild_verify_backends'
    New-Item -ItemType Directory -Force -Path $base | Out-Null

    for ($attempt = 0; $attempt -lt 1000; $attempt++) {
        $stamp     = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
        $candidate = Join-Path $base ("{0}_{1}_{2}_{3}" -f $Tag, $stamp, $PID, $attempt)
        if (-not (Test-Path $candidate)) {
            New-Item -ItemType Directory -Path $candidate | Out-Null
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    throw "could not allocate a unique temp directory for '$Tag'"
}

# Deletes ONLY a directory this script itself created: resolves both the
# candidate and the system temp root to their full paths and refuses unless
# the candidate is a STRICT descendant of that root (never the root itself).
function Remove-VerifyTempDir {
    param([string]$Path)

    if (-not $Path) { return }
    if (-not (Test-Path $Path)) { return }

    $resolvedRoot = [System.IO.Path]::GetFullPath($systemTempRoot).TrimEnd('\')
    $resolvedPath = [System.IO.Path]::GetFullPath($Path).TrimEnd('\')

    $isStrictDescendant =
        $resolvedPath.Length -gt $resolvedRoot.Length -and
        $resolvedPath.StartsWith($resolvedRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
        $resolvedPath[$resolvedRoot.Length] -eq '\'

    if (-not $isStrictDescendant) {
        Write-Warning "refusing to delete '$resolvedPath' -- it does not resolve below the validated system temp root '$resolvedRoot'"
        return
    }

    Remove-Item -LiteralPath $resolvedPath -Recurse -Force -ErrorAction SilentlyContinue
}

function Copy-Fixture {
    param([Parameter(Mandatory)] [string]$Destination)
    Copy-Item -Path (Join-Path $fixtureSource '*') -Destination $Destination -Recurse -Force
}

# ---- arcbuild invocation ------------------------------------------------------

function Invoke-Arcbuild {
    param(
        [Parameter(Mandatory)] [string]$Command,
        [Parameter(Mandatory)] [string]$ProjectRoot,
        [Parameter(Mandatory)] [string]$Action,
        [string]$Config = $Configuration
    )

    # arcbuild's Output.cpp prints everything (its own [arcbuild] lines AND
    # every streamed child line) through std::printf to stdout -- never
    # stderr -- so a plain capture needs no `2>&1` native-command dance.
    $captured = & $arcbuildExe $Command --project $ProjectRoot --action $Action --config $Config --sdk $repoRoot
    [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = $captured }
}

# ---- Backend acceptance: generate, and (compiler permitting) build/rebuild/clean

function Test-BackendAcceptance {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [string]$Action,
        [Parameter(Mandatory)] [string[]]$GenerateArtifacts,
        [Parameter(Mandatory)] [bool]$CompilerAvailable,
        [Parameter(Mandatory)] [string]$CompilerHint
    )

    Write-Host ""
    Write-Host "--- $Name backend: generate/build/rebuild/clean ---" -ForegroundColor Cyan

    $work = New-VerifyTempDir $Name.ToLowerInvariant()
    try {
        Copy-Fixture $work

        $gen = Invoke-Arcbuild -Command generate -ProjectRoot $work -Action $Action
        Add-Result "$Name generate exits 0" ($gen.ExitCode -eq 0) "exit=$($gen.ExitCode)"

        foreach ($artifact in $GenerateArtifacts) {
            Add-Result "$Name generate produced $artifact" (Test-Path (Join-Path $work $artifact))
        }

        if (-not $CompilerAvailable) {
            Add-Skipped "$Name real compiled build/rebuild" $CompilerHint
            return
        }

        $dllPath = Join-Path $work 'Binaries\Fixture.dll'

        # A real compile can fail for reasons that have nothing to do with
        # arcbuild (e.g. an engine header not yet portable to a given
        # compiler -- see this task's report for what this desk found with
        # the Make backend's default GCC toolset). So "the build succeeded"
        # is checked directly (exit 0, DLL on disk); if it did NOT succeed,
        # this falls back to the property arcbuild actually promises --
        # the composed child's REAL exit code reached the caller unchanged
        # -- by running the identical tool invocation directly and requiring
        # arcbuild's reported exit code to match it exactly. A mismatch
        # there (not a compile failure -- a DIFFERENT number) would be an
        # arcbuild defect and fails loudly.
        $build = Invoke-Arcbuild -Command build -ProjectRoot $work -Action $Action
        if ($build.ExitCode -eq 0) {
            Add-Result "$Name build succeeds and produces Binaries\Fixture.dll" (Test-Path $dllPath) "exit=0"
        } else {
            $direct = Get-DirectBuildExitCode -Name $Name -ProjectRoot $work -Config $Configuration
            Add-Result "$Name build: child exit code faithfully propagated (real compile did not succeed -- see report)" `
                ($build.ExitCode -eq $direct) `
                "arcbuild=$($build.ExitCode) direct-tool=$direct"
        }

        $rebuild = Invoke-Arcbuild -Command rebuild -ProjectRoot $work -Action $Action
        if ($rebuild.ExitCode -eq 0) {
            Add-Result "$Name rebuild succeeds and re-produces Binaries\Fixture.dll" (Test-Path $dllPath) "exit=0"
        } else {
            $direct = Get-DirectBuildExitCode -Name $Name -ProjectRoot $work -Config $Configuration
            Add-Result "$Name rebuild: child exit code faithfully propagated (real compile did not succeed -- see report)" `
                ($rebuild.ExitCode -eq $direct) `
                "arcbuild=$($rebuild.ExitCode) direct-tool=$direct"
        }

        # Clean does not depend on a prior successful compile -- the
        # generated recipe only ever touches the single expected output
        # path/objdir it knows about, a no-op when neither exists yet.
        $clean = Invoke-Arcbuild -Command clean -ProjectRoot $work -Action $Action
        Add-Result "$Name clean exits 0" ($clean.ExitCode -eq 0) "exit=$($clean.ExitCode)"
        Add-Result "$Name clean removed Binaries\" (-not (Test-Path (Join-Path $work 'Binaries')))
    } finally {
        Remove-VerifyTempDir $work
    }
}

# The same tool invocation Compose.cpp's ComposeMake/ComposeNinja compose for
# a plain Build operation, run directly (never through arcbuild) so a failed
# arcbuild build step can be checked for exit-code fidelity against ground
# truth rather than an assumption.
function Get-DirectBuildExitCode {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [string]$ProjectRoot,
        [Parameter(Mandatory)] [string]$Config
    )

    # No stream redirection here on purpose: PowerShell 5.1 wraps a native
    # command's stderr lines in NativeCommandError objects when redirected
    # (2>&1, *>, 2>$null all included), which -- combined with this script's
    # $ErrorActionPreference = 'Stop' -- would abort the script on the
    # child's ordinary compiler-error output. Piping only the success stream
    # to Out-Null leaves stderr to print normally while still suppressing
    # the noisy stdout build log; only $LASTEXITCODE is read back.
    if ($Name -eq 'Make') {
        & $make.Source -C $ProjectRoot "config=$($Config.ToLowerInvariant())" | Out-Null
        return $LASTEXITCODE
    } else {
        & $ninja.Source -C $ProjectRoot "Fixture_$Config" | Out-Null
        return $LASTEXITCODE
    }
}

# ---- The four clean-precedence edge cases (spec s9) ---------------------------

function Test-CleanPrecedence {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [string]$Action
    )

    Write-Host ""
    Write-Host "--- $Name backend: clean-precedence edge cases (spec s9) ---" -ForegroundColor Cyan

    # Case 1: no generated backend context at all (never generated) -- a
    # soft skip, logged -- plus filesystem clean over directories that do
    # not exist (remove_all of a missing path is 0 removed, no error) -> 0.
    $case1 = New-VerifyTempDir "$($Name.ToLowerInvariant())_softskip"
    try {
        Copy-Fixture $case1
        $result = Invoke-Arcbuild -Command clean -ProjectRoot $case1 -Action $Action
        $sawSoftSkip = ($result.Output -join "`n") -match 'running filesystem clean only'
        Add-Result "$Name clean: missing context soft-skip + fs success -> 0" `
            ($result.ExitCode -eq 0 -and $sawSoftSkip) `
            "exit=$($result.ExitCode) sawSoftSkipLine=$sawSoftSkip"
    } finally {
        Remove-VerifyTempDir $case1
    }

    # Case 2: a REAL generated context whose backend clean genuinely fails.
    #
    # An invalid --config cannot be used here: arcbuild's own Cli option
    # restricts --config to exactly {Debug, Release, Dist}
    # (Request.cpp's MakeCli, .Choices({...})) and refuses anything else
    # BEFORE Bootstrap/Compose/BackendResolver ever run -- discovered live
    # while writing this script (an earlier draft used --config Bogus and
    # got a same-valued but WRONG-REASON pass for Make, and an outright
    # mismatch for Ninja, once compared against the real child's own exit
    # code). Instead, Break-GeneratedCleanTarget corrupts the GENERATED
    # (never committed) backend files themselves, after a real, valid-config
    # generate, so the clean this case runs is genuinely driven through
    # BackendResolver -> Compose -> ProcessRunner -> a real failing child,
    # with a valid --config throughout. Ground truth is the identical
    # command run directly against the same corrupted files, so the
    # assertion is "arcbuild returns exactly what the child returned", never
    # a guessed constant.
    $case2 = New-VerifyTempDir "$($Name.ToLowerInvariant())_backendfail"
    try {
        Copy-Fixture $case2
        $gen = Invoke-Arcbuild -Command generate -ProjectRoot $case2 -Action $Action
        if ($gen.ExitCode -ne 0) {
            Add-Result "$Name clean: backend failure + fs success -> child code" $false "generate itself failed, exit=$($gen.ExitCode)"
        } else {
            Break-GeneratedCleanTarget -Name $Name -ProjectRoot $case2 -Config $Configuration
            $result   = Invoke-Arcbuild -Command clean -ProjectRoot $case2 -Action $Action
            $expected = Get-DirectCleanExitCode -Name $Name -ProjectRoot $case2 -Config $Configuration
            Add-Result "$Name clean: backend failure + fs success -> child code" `
                ($result.ExitCode -eq $expected -and $expected -ne 0) `
                "arcbuild=$($result.ExitCode) direct-tool=$expected"
        }
    } finally {
        Remove-VerifyTempDir $case2
    }

    # Case 3: a real backend clean that SUCCEEDS (valid config, nothing
    # built yet -- the generated clean recipe only ever touches the single
    # expected Fixture.dll/objdir it knows about) while a file held open
    # with FileShare::None inside Binaries/ (never Intermediate/<config>/,
    # which doubles as the generated project's own objdir -- see header)
    # makes remove_all fail -> 2, never the backend's own 0.
    $case3 = New-VerifyTempDir "$($Name.ToLowerInvariant())_fsfail"
    try {
        Copy-Fixture $case3
        $gen = Invoke-Arcbuild -Command generate -ProjectRoot $case3 -Action $Action
        if ($gen.ExitCode -ne 0) {
            Add-Result "$Name clean: backend success + fs failure -> 2" $false "generate itself failed, exit=$($gen.ExitCode)"
        } else {
            $binaries = Join-Path $case3 'Binaries'
            New-Item -ItemType Directory -Force -Path $binaries | Out-Null
            $lockFile = Join-Path $binaries 'verify-lock.txt'
            $stream = [System.IO.File]::Open($lockFile, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
            try {
                $result = Invoke-Arcbuild -Command clean -ProjectRoot $case3 -Action $Action
                Add-Result "$Name clean: backend success + fs failure -> 2" `
                    ($result.ExitCode -eq 2) `
                    "exit=$($result.ExitCode)"
            } finally {
                $stream.Dispose()
            }
        }
    } finally {
        Remove-VerifyTempDir $case3
    }

    # Case 4: BOTH fail at once (the same generated-file corruption AND the
    # Binaries/ lock) -- the filesystem refusal must still win, proving
    # precedence holds even when the backend ALSO failed (with its own,
    # different, nonzero code -- 1 for Ninja's "unknown target", not just a
    # coincidental match with kExitRefused), not only when the backend
    # happened to succeed (case 3).
    $case4 = New-VerifyTempDir "$($Name.ToLowerInvariant())_bothfail"
    try {
        Copy-Fixture $case4
        $gen = Invoke-Arcbuild -Command generate -ProjectRoot $case4 -Action $Action
        if ($gen.ExitCode -ne 0) {
            Add-Result "$Name clean: backend failure + fs failure -> 2" $false "generate itself failed, exit=$($gen.ExitCode)"
        } else {
            Break-GeneratedCleanTarget -Name $Name -ProjectRoot $case4 -Config $Configuration
            $binaries = Join-Path $case4 'Binaries'
            New-Item -ItemType Directory -Force -Path $binaries | Out-Null
            $lockFile = Join-Path $binaries 'verify-lock.txt'
            $stream = [System.IO.File]::Open($lockFile, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
            try {
                $result = Invoke-Arcbuild -Command clean -ProjectRoot $case4 -Action $Action
                Add-Result "$Name clean: backend failure + fs failure -> 2" `
                    ($result.ExitCode -eq 2) `
                    "exit=$($result.ExitCode)"
            } finally {
                $stream.Dispose()
            }
        }
    } finally {
        Remove-VerifyTempDir $case4
    }
}

# Corrupts a REAL, just-generated backend context so its clean genuinely
# fails, without ever touching Binaries/ or Intermediate/<config>/ (so
# filesystem clean still trivially succeeds in case 2) and without making
# BackendResolver's OWN existence checks fail (which would silently
# collapse this into case 1's soft skip instead of a real child failure).
function Break-GeneratedCleanTarget {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [string]$ProjectRoot,
        [Parameter(Mandatory)] [string]$Config
    )

    if ($Name -eq 'Make') {
        # BackendResolver::ResolveBackendContext(Make, ...) checks only that
        # the TOP-LEVEL Makefile exists (Backend.cpp) -- it delegates to the
        # per-project Fixture.make via a sub-make call. Removing Fixture.make
        # while leaving Makefile in place keeps resolution green but makes
        # the real sub-make invocation fail for real ("No rule to make
        # target 'Fixture.make'"), confirmed live on this desk.
        Remove-Item -LiteralPath (Join-Path $ProjectRoot 'Fixture.make') -Force
    } else {
        # BackendResolver::ResolveBackendContext(Ninja, ...) requires BOTH
        # build.ninja and <name>.ninja to exist (Backend.cpp) -- deleting
        # either would collapse this into case 1's soft skip. Instead,
        # rename every occurrence of the exact target ComposeNinja composes
        # ("<stem>_<Config>") so ninja itself refuses it as unknown -- a
        # real, confirmed-live child failure, not a resolution failure.
        $target = "Fixture_$Config"
        $broken = "${target}_broken"
        foreach ($file in @('Fixture.ninja', 'build.ninja')) {
            $path = Join-Path $ProjectRoot $file
            $content = Get-Content -LiteralPath $path -Raw
            $content = $content -replace [regex]::Escape($target), $broken
            Set-Content -LiteralPath $path -Value $content -NoNewline
        }
    }
}

# The real tool's own exit code for the SAME clean invocation arcbuild
# composes (Compose.cpp's ComposeMake/ComposeNinja) against the
# Break-GeneratedCleanTarget-corrupted files, run directly -- never assumed
# -- so case 2 above asserts "arcbuild passes the child's exit code through
# unchanged", not a guessed constant. See Get-DirectBuildExitCode above for
# why this pipes only the success stream to Out-Null rather than
# redirecting stderr.
function Get-DirectCleanExitCode {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [string]$ProjectRoot,
        [Parameter(Mandatory)] [string]$Config
    )

    if ($Name -eq 'Make') {
        & $make.Source -C $ProjectRoot "config=$($Config.ToLowerInvariant())" clean | Out-Null
        return $LASTEXITCODE
    } else {
        & $ninja.Source -C $ProjectRoot -t clean "Fixture_$Config" | Out-Null
        return $LASTEXITCODE
    }
}

# ---- Run everything ------------------------------------------------------------

Test-BackendAcceptance -Name 'Make' -Action 'gmake' `
    -GenerateArtifacts @('Makefile', 'Fixture.make') `
    -CompilerAvailable $makeCompilerAvailable `
    -CompilerHint 'g++ not found on PATH -- install a MinGW-w64 GCC toolchain (this is Premake beta8''s own default toolset for the gmake action on Windows; see this task''s report for what this desk used) and add its bin directory to PATH'

Test-BackendAcceptance -Name 'Ninja' -Action 'ninja' `
    -GenerateArtifacts @('build.ninja', 'Fixture.ninja') `
    -CompilerAvailable $true `
    -CompilerHint 'unreachable -- cl.exe absence already refused the whole script above'

Test-CleanPrecedence -Name 'Make' -Action 'gmake'
Test-CleanPrecedence -Name 'Ninja' -Action 'ninja'

# ---- Summary ---------------------------------------------------------------

Write-Host ""
Write-Host "=== summary ===" -ForegroundColor Cyan
$failed = @($script:Results | Where-Object { -not $_.Passed })
foreach ($r in $script:Results) {
    $status = if ($r.Passed) { 'PASS' } else { 'FAIL' }
    Write-Host ("  [{0}] {1}" -f $status, $r.Name)
}
Write-Host ("{0} checks, {1} failed" -f $script:Results.Count, $failed.Count)

if ($failed.Count -gt 0) {
    exit 1
}

exit 0
