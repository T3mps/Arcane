<#
.SYNOPSIS
    Re-vendor Astra's public headers from the standalone repo into ThirdParty/Astra.

.DESCRIPTION
    Astra is developed standalone (default D:\dev\starworks\Astra) and vendored
    here headers-only: Arcane/premake5.lua's only reference is
    IncludeDir["Astra"] = ThirdParty/Astra/include. Nothing else in the vendored
    tree is compiled by this repo.

    The mirror DELETES orphans, so a header removed upstream is removed here too
    (that is the point -- e.g. Archetype/ArchetypeGraph.hpp went away in the
    2026-07-29 sync and a copy-only update would have left a stale one behind).

    Every run stamps ThirdParty/Astra/VENDORED.txt with the source commit. That
    stamp is the anti-drift mechanism: "are we current?" becomes a one-line
    check instead of a 60-file diff. The vendored copy had silently drifted 40
    of 60 headers before this script existed.

.PARAMETER Source
    Path to the standalone Astra repo. Default D:\dev\starworks\Astra.

.PARAMETER DryRun
    List what would change and exit without writing anything.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\sync-astra.ps1
    powershell -ExecutionPolicy Bypass -File scripts\sync-astra.ps1 -DryRun
#>
[CmdletBinding()]
param(
    [string] $Source = 'D:\dev\starworks\Astra',
    [switch] $DryRun
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$srcInclude = Join-Path $Source 'include'
$dstAstra = Join-Path $repoRoot 'ThirdParty\Astra'
$dstInclude = Join-Path $dstAstra 'include'

if (-not (Test-Path $srcInclude)) {
    throw "Astra source headers not found at '$srcInclude'. Pass -Source <path-to-Astra-repo>."
}
if (-not (Test-Path $dstAstra)) {
    throw "Vendored Astra not found at '$dstAstra'."
}

# Source provenance. Not fatal if the source is not a git checkout -- the stamp
# just records "unknown" rather than blocking the sync.
$sha = 'unknown'
$branch = 'unknown'
$subject = ''
Push-Location $Source
try {
    $sha = (git rev-parse HEAD 2>$null)
    $branch = (git rev-parse --abbrev-ref HEAD 2>$null)
    $subject = (git log -1 --pretty=%s 2>$null)
    if (-not $sha) { $sha = 'unknown' }
    if (-not $branch) { $branch = 'unknown' }
} catch {
    Write-Warning "Could not read git metadata from '$Source'; stamping 'unknown'."
} finally {
    Pop-Location
}

Write-Host "Astra sync" -ForegroundColor Cyan
Write-Host "  source : $Source"
Write-Host "  commit : $sha ($branch)"
if ($subject) { Write-Host "  subject: $subject" }
Write-Host "  dest   : $dstInclude"
Write-Host ""

# /MIR mirrors including deletions. /NJH /NJS trim the banner/summary; /NP kills
# per-file progress. /XO is deliberately NOT used -- an upstream revert must
# come back even if its timestamp is older.
$roboArgs = @($srcInclude, $dstInclude, '/MIR', '/NFL', '/NDL', '/NJH', '/NJS', '/NP', '/R:2', '/W:1')
if ($DryRun) { $roboArgs += '/L' }

& robocopy.exe @roboArgs | Out-Null
$rc = $LASTEXITCODE

# robocopy: 0-7 are success variants (bit flags), >=8 is a real failure.
if ($rc -ge 8) {
    throw "robocopy failed with exit code $rc while mirroring '$srcInclude' -> '$dstInclude'."
}

if ($DryRun) {
    Write-Host "DRY RUN -- nothing written. robocopy rc=$rc" -ForegroundColor Yellow
    Write-Host "(rc 0 = already identical; 1 = files would copy; 2 = orphans would be purged; 3 = both)"
    exit 0
}

$stamp = @"
Astra vendored from the standalone repo. DO NOT EDIT THESE HEADERS IN PLACE --
change them in the Astra repo and re-run scripts\sync-astra.ps1, or the next
sync silently reverts your edit.

source  : $Source
commit  : $sha
branch  : $branch
subject : $subject
synced  : $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')
scope   : include/ only (Arcane compiles nothing else from this tree --
          see Arcane/premake5.lua IncludeDir["Astra"])
"@
$stampPath = Join-Path $dstAstra 'VENDORED.txt'
Set-Content -Path $stampPath -Value $stamp -Encoding utf8

$headerCount = (Get-ChildItem $dstInclude -Recurse -File -Filter *.hpp | Measure-Object).Count
Write-Host "Mirrored $headerCount headers (robocopy rc=$rc)." -ForegroundColor Green
Write-Host "Stamped $stampPath"
Write-Host ""
Write-Host "Next: Arcane\GenerateProjects.bat, rebuild, and run the full ~[gpu] gate." -ForegroundColor Cyan
