<#
.SYNOPSIS
    Re-vendor Astra, Manifold2D, or Mosaic into ThirdParty/.

.DESCRIPTION
    One mirror for the three libraries Arcane vendors from standalone repos.
    Astra is include/ only. Manifold2D and Mosaic are include/ + src/ + LICENSE.
    A library's premake5.lua, ThirdParty/, vendor/, tests/ and docs/ are never
    copied. Manifold2D's premake5.lua in this tree is the consumer wrapper.

    The mirror DELETES orphans inside the mirrored directories (/MIR), so a
    header removed upstream is removed here too.

    Every real run stamps ThirdParty/<Library>/VENDORED.txt (UTF-8, no BOM).
    -Commit vendors that commit through a temporary detached git worktree, so
    the source working tree does not have to be checked out at it. The
    finally always removes that temp path (worktree remove --force, delete
    the directory if it remains, then worktree prune) and warns if
    `git worktree list` still names it. No other worktree is touched.

    After a real sync the script reads the Mosaic commit each upstream library
    pins (Manifold2D's ThirdParty/Mosaic/VENDORED.txt, Astra's vendor/Mosaic)
    and warns when it differs from Arcane's ThirdParty/Mosaic stamp. A missing
    stamp warns "unstamped". The reader accepts a UTF-8 BOM; the stamps this
    script writes do not have one.

    Line endings are copied as bytes (robocopy). The worktree checkout forces
    core.autocrlf=false so the bytes are the git blobs. The source repos set
    core.autocrlf=true, and a default checkout would rewrite LF blobs to CRLF
    and dirty every mirrored file. Afterwards git diff --stat is compared with
    git diff --ignore-cr-at-eol --stat; if more than half of the touched files
    differ only in line endings, the script fails (CRLF fan-out guard).

.PARAMETER Library
    Astra, Manifold2D, Mosaic, or All.

.PARAMETER Source
    Override the source repo for a single library. Not valid with -Library All.

.PARAMETER Commit
    Vendor this commit via a temporary worktree. Not valid with -Library All.

.PARAMETER DryRun
    List what would change (robocopy /L) and exit without writing anything.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\sync-vendor.ps1 -Library Astra -DryRun
    powershell -ExecutionPolicy Bypass -File scripts\sync-vendor.ps1 -Library Mosaic -Commit <sha>
    powershell -ExecutionPolicy Bypass -File scripts\sync-vendor.ps1 -Library All
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Astra', 'Manifold2D', 'Mosaic', 'All')]
    [string] $Library,

    [string] $Source,

    [string] $Commit,

    [switch] $DryRun
)

$ErrorActionPreference = 'Stop'
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -Scope Global -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$repoRoot = Split-Path -Parent $PSScriptRoot

$libs = @{
    Astra      = @{ Source = 'D:\dev\starworks\Astra';      Dest = 'ThirdParty\Astra';      Dirs = @('include');        Files = @() }
    Manifold2D = @{ Source = 'D:\dev\starworks\Manifold2D'; Dest = 'ThirdParty\Manifold2D'; Dirs = @('include','src'); Files = @('LICENSE') }
    Mosaic     = @{ Source = 'D:\dev\starworks\Mosaic';     Dest = 'ThirdParty\Mosaic';     Dirs = @('include','src'); Files = @('LICENSE') }
}

$scopeText = @{
    Astra      = 'include/ only (Arcane compiles nothing else from this tree)'
    Manifold2D = 'include/ + src/ + LICENSE (premake5.lua is the consumer wrapper and is never synced)'
    Mosaic     = 'include/ + src/ + LICENSE'
}

# Never a library's own premake, its nested third-party/vendor trees, or its
# tests and docs. The allowlist above is the real boundary; this rejects a
# table edit that would start copying them.
$banned = @('premake5.lua', 'ThirdParty', 'vendor', 'tests', 'docs')

function Invoke-Robocopy {
    param(
        [Parameter(Mandatory = $true)] [string[]] $RoboArgs,
        [switch] $ShowOutput
    )
    # Write-Host, not the success stream: a function's stdout is its return
    # value, and the caller ORs that into the robocopy code. Robocopy
    # overwrites its directory line with CR; split those so a captured log
    # does not glue the count to the path.
    if ($ShowOutput) {
        & robocopy.exe @RoboArgs | ForEach-Object {
            foreach ($line in ("$_" -split "`r")) {
                if ($line -ne '') { Write-Host ($line -replace "`t", '  ') }
            }
        }
    } else {
        & robocopy.exe @RoboArgs | Out-Null
    }
    $code = $LASTEXITCODE
    if ($null -eq $code) { $code = 0 }
    if ($code -ge 8) {
        throw "robocopy failed with exit code $code ($($RoboArgs -join ' '))."
    }
    return [int]$code
}

function Invoke-MirrorDir {
    param(
        [Parameter(Mandatory = $true)] [string] $From,
        [Parameter(Mandatory = $true)] [string] $To,
        [switch] $ListOnly
    )
    if (-not (Test-Path -LiteralPath $From)) {
        throw "source directory not found at '$From'."
    }
    # /MIR mirrors including deletions. /XO is deliberately not used: an
    # upstream revert must come back even when its timestamp is older.
    # /NJH /NJS /NP trim the banner. A real sync also hides the per-file
    # list (/NFL /NDL). A dry run (/L) drops those two so the would-copy
    # and would-purge names are printed, and writes nothing.
    $roboArgs = @($From, $To, '/MIR', '/NJH', '/NJS', '/NP', '/R:2', '/W:1')
    if ($ListOnly) {
        $roboArgs += '/L'
    } else {
        $roboArgs += '/NFL'
        $roboArgs += '/NDL'
    }
    return (Invoke-Robocopy -RoboArgs $roboArgs -ShowOutput:$ListOnly)
}

function Invoke-CopyOneFile {
    param(
        [Parameter(Mandatory = $true)] [string] $FromRoot,
        [Parameter(Mandatory = $true)] [string] $ToRoot,
        [Parameter(Mandatory = $true)] [string] $Name,
        [switch] $ListOnly
    )
    $srcFile = Join-Path $FromRoot $Name
    if (-not (Test-Path -LiteralPath $srcFile)) {
        throw "required file '$Name' not found at '$srcFile'."
    }
    # A single file, not /MIR of the repo root (that would sweep premake5.lua,
    # tests/ and docs/ into the vendor tree). Same /NFL /NDL split as the
    # directory mirror: hidden on a real copy, printed on /L.
    $roboArgs = @($FromRoot, $ToRoot, $Name, '/NJH', '/NJS', '/NP', '/R:2', '/W:1')
    if ($ListOnly) {
        $roboArgs += '/L'
    } else {
        $roboArgs += '/NFL'
        $roboArgs += '/NDL'
    }
    return (Invoke-Robocopy -RoboArgs $roboArgs -ShowOutput:$ListOnly)
}

function Get-GitMeta {
    param(
        [Parameter(Mandatory = $true)] [string] $Repo,
        [string] $At
    )
    $sha = 'unknown'
    $branch = 'unknown'
    $subject = ''
    if ($At) {
        $sha = & git -C $Repo rev-parse --verify "$At^{commit}"
        if ($LASTEXITCODE -ne 0 -or -not $sha) {
            throw "cannot resolve commit '$At' in '$Repo'."
        }
        $sha = "$sha".Trim()
        $branch = 'detached'
        $subject = & git -C $Repo log -1 --pretty=%s $sha
        if ($subject) { $subject = "$subject".Trim() } else { $subject = '' }
    } else {
        $shaOut = & git -C $Repo rev-parse HEAD 2>$null
        $brOut = & git -C $Repo rev-parse --abbrev-ref HEAD 2>$null
        $subOut = & git -C $Repo log -1 --pretty=%s 2>$null
        if ($shaOut) { $sha = "$shaOut".Trim() }
        if ($brOut) {
            $branch = "$brOut".Trim()
            if ($branch -eq 'HEAD' -or $branch -eq '') { $branch = 'detached' }
        }
        if ($subOut) { $subject = "$subOut".Trim() }
    }
    return @{ Sha = $sha; Branch = $branch; Subject = $subject }
}

function Write-VendorStamp {
    param(
        [Parameter(Mandatory = $true)] [string] $Name,
        [Parameter(Mandatory = $true)] [string] $DestRoot,
        [Parameter(Mandatory = $true)] [string] $SourcePath,
        [Parameter(Mandatory = $true)] $Meta,
        [Parameter(Mandatory = $true)] [string] $Scope
    )
    $lines = @(
        "$Name vendored from the standalone repo. DO NOT EDIT THESE FILES IN PLACE --",
        "change them in the $Name repo and re-run scripts\sync-vendor.ps1, or the next",
        "sync silently reverts your edit.",
        '',
        "source  : $SourcePath",
        "commit  : $($Meta.Sha)",
        "branch  : $($Meta.Branch)",
        "subject : $($Meta.Subject)",
        "synced  : $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')",
        "scope   : $Scope"
    )
    $text = ($lines -join "`n") + "`n"
    $utf8NoBom = New-Object System.Text.UTF8Encoding $false
    $stampPath = Join-Path $DestRoot 'VENDORED.txt'
    [System.IO.File]::WriteAllText($stampPath, $text, $utf8NoBom)
    return $stampPath
}

# commit : line, BOM-tolerant. Missing file or missing line returns $null
# (the caller warns "unstamped").
function Get-StampCommit {
    param([Parameter(Mandatory = $true)] [string] $Path)
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $start = 0
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        $start = 3
    }
    if ($start -ge $bytes.Length) { return $null }
    $text = [System.Text.Encoding]::UTF8.GetString($bytes, $start, $bytes.Length - $start)
    foreach ($line in ($text -split '\r?\n')) {
        if ($line -match '^\s*commit\s*:\s*([0-9a-fA-F]+)\s*$') {
            return $Matches[1].ToLowerInvariant()
        }
    }
    return $null
}

function Test-SameCommit {
    param([string] $A, [string] $B)
    if ($A -eq $B) { return $true }
    $short = $A
    $long = $B
    if ($B.Length -lt $A.Length) { $short = $B; $long = $A }
    return ($short.Length -ge 12 -and $long.StartsWith($short))
}

function Write-MosaicDrift {
    param(
        [Parameter(Mandatory = $true)] [string] $ArcaneRoot,
        [Parameter(Mandatory = $true)] [string] $ManifoldSource,
        [Parameter(Mandatory = $true)] [string] $AstraSource
    )
    $arcaneStamp = Join-Path $ArcaneRoot 'ThirdParty\Mosaic\VENDORED.txt'
    $arcaneCommit = Get-StampCommit -Path $arcaneStamp
    if (-not $arcaneCommit) {
        Write-Warning "Mosaic drift: Arcane is unstamped ($arcaneStamp)."
    }
    $pins = @(
        @{ Name = 'Manifold2D'; Path = (Join-Path $ManifoldSource 'ThirdParty\Mosaic\VENDORED.txt') },
        @{ Name = 'Astra';      Path = (Join-Path $AstraSource 'vendor\Mosaic\VENDORED.txt') }
    )
    foreach ($pin in $pins) {
        $pinned = Get-StampCommit -Path $pin.Path
        if (-not $pinned) {
            Write-Warning "Mosaic drift: $($pin.Name) is unstamped ($($pin.Path))."
            continue
        }
        if (-not $arcaneCommit) {
            Write-Warning "Mosaic drift: $($pin.Name) pins $pinned but Arcane is unstamped."
            continue
        }
        if (-not (Test-SameCommit -A $arcaneCommit -B $pinned)) {
            Write-Warning "Mosaic drift: $($pin.Name) pins $pinned but Arcane pins $arcaneCommit."
        }
    }
}

# $Work is this invocation's temp path only. remove --force, then delete the
# directory if it is still there, then prune, so a failed remove cannot leave
# a registration whose directory was deleted afterwards (prune ignores a
# path that is still on disk). Warn if the list still names it. Never pass
# any other path to worktree remove.
function Remove-SyncWorktree {
    param(
        [Parameter(Mandatory = $true)] [string] $SourcePath,
        [Parameter(Mandatory = $true)] [string] $Work
    )
    & git -C $SourcePath worktree remove --force -- $Work | Out-Null
    if (Test-Path -LiteralPath $Work) {
        Remove-Item -LiteralPath $Work -Recurse -Force -ErrorAction SilentlyContinue
    }
    & git -C $SourcePath worktree prune | Out-Null
    $needle = Split-Path -Leaf $Work
    $listed = @(& git -C $SourcePath worktree list)
    $hit = @($listed | Where-Object { $_ -and ($_ -like ('*' + $needle + '*')) })
    if ($hit.Count -gt 0) {
        Write-Warning @"
TEMPORARY WORKTREE STILL REGISTERED after remove --force, directory delete, and prune.
Path: $Work
git worktree list still names it:
$($hit -join "`n")
No other worktree was touched. Remove this one by hand:
  git -C "$SourcePath" worktree remove --force -- "$Work"
"@
    }
}

function Test-CrlfFanout {
    param(
        [Parameter(Mandatory = $true)] [string] $Root,
        [Parameter(Mandatory = $true)] [string[]] $RelPaths
    )
    $names = @()
    foreach ($rel in $RelPaths) {
        $listed = @(& git -C $Root --no-pager diff --name-only -- $rel)
        if ($LASTEXITCODE -gt 1) {
            throw "git diff --name-only failed for '$rel' (exit $LASTEXITCODE)."
        }
        foreach ($line in $listed) {
            $t = "$line".Trim()
            if ($t) { $names += $t }
        }
    }
    $names = @($names | Select-Object -Unique)
    Write-Host 'git diff --stat (synced paths):'
    & git -C $Root --no-pager diff --stat -- @RelPaths
    if ($names.Count -eq 0) {
        Write-Host 'CRLF guard: no content diff under the synced paths.'
        return
    }
    $eolOnly = @()
    foreach ($name in $names) {
        $ign = @(& git -C $Root --no-pager diff --ignore-cr-at-eol --numstat -- $name)
        if ($LASTEXITCODE -gt 1) {
            throw "git diff --ignore-cr-at-eol failed for '$name' (exit $LASTEXITCODE)."
        }
        $ignLines = @($ign | Where-Object { $_ -and "$($_)".Trim() -ne '' })
        if ($ignLines.Count -eq 0) { $eolOnly += $name }
    }
    if ($eolOnly.Count -gt ($names.Count * 0.5)) {
        $shown = ($eolOnly | Select-Object -First 40) -join "`n"
        throw @"
CRLF fan-out guard: $($eolOnly.Count) of $($names.Count) touched files differ only in line endings (git diff --ignore-cr-at-eol --stat is empty for them). Refusing the sync.
$shown
"@
    }
    Write-Host "CRLF guard: $($eolOnly.Count) of $($names.Count) touched files are line-endings only."
}

function Sync-OneLibrary {
    param(
        [Parameter(Mandatory = $true)] [string] $Name,
        [Parameter(Mandatory = $true)] $Spec,
        [Parameter(Mandatory = $true)] [string] $SourcePath,
        [string] $At,
        [switch] $ListOnly
    )
    foreach ($entry in @($Spec.Dirs + $Spec.Files)) {
        $leaf = Split-Path -Leaf $entry
        if ($banned -contains $leaf -or $banned -contains $entry) {
            throw "refusing to copy '$entry' from $Name (premake5.lua, ThirdParty, vendor, tests and docs are never synced)."
        }
    }
    $destRoot = Join-Path $repoRoot $Spec.Dest
    if (-not (Test-Path -LiteralPath $SourcePath -PathType Container)) {
        throw "$Name source repo not found at '$SourcePath'."
    }
    if (-not (Test-Path -LiteralPath $destRoot -PathType Container)) {
        throw "Vendored $Name not found at '$destRoot'."
    }

    $copyRoot = $SourcePath
    # Set as soon as the temp path is chosen, before worktree add. A failed
    # add can still create the directory or a registration, and finally must
    # clean that path even though the add did not succeed.
    $work = $null
    $rc = 0
    $meta = $null
    try {
        if ($At) {
            & git -C $SourcePath rev-parse --verify "$At^{commit}" | Out-Null
            if ($LASTEXITCODE -ne 0) {
                throw "commit '$At' not found in '$SourcePath'."
            }
            $work = Join-Path ([System.IO.Path]::GetTempPath()) ("arcane-sync-" + $Name + "-" + [guid]::NewGuid().ToString('n'))
            # autocrlf=false: copy the stored blobs. A default worktree under
            # the source repo's core.autocrlf=true checks CRLF out and the
            # fan-out guard then fires on an otherwise clean mirror.
            & git -c core.autocrlf=false -C $SourcePath worktree add --detach $work $At
            if ($LASTEXITCODE -ne 0) {
                throw "git worktree add --detach failed for $Name @ $At."
            }
            $copyRoot = $work
        }

        foreach ($dir in @($Spec.Dirs)) {
            $rc = $rc -bor (Invoke-MirrorDir -From (Join-Path $copyRoot $dir) -To (Join-Path $destRoot $dir) -ListOnly:$ListOnly)
        }
        foreach ($file in @($Spec.Files)) {
            if (-not $file) { continue }
            $rc = $rc -bor (Invoke-CopyOneFile -FromRoot $copyRoot -ToRoot $destRoot -Name $file -ListOnly:$ListOnly)
        }
        $meta = Get-GitMeta -Repo $SourcePath -At $At
    } finally {
        if ($work) {
            Remove-SyncWorktree -SourcePath $SourcePath -Work $work
        }
    }
    return @{ Rc = [int]$rc; Meta = $meta; DestRoot = $destRoot }
}

if ($Library -eq 'All' -and $Source) {
    throw '-Source applies to a single library. Do not combine -Source with -Library All.'
}
if ($Library -eq 'All' -and $Commit) {
    throw '-Commit applies to a single library. Do not combine -Commit with -Library All.'
}

# Mosaic first so a following drift check sees Arcane's updated stamp.
$order = if ($Library -eq 'All') { @('Mosaic', 'Manifold2D', 'Astra') } else { @($Library) }

$combined = 0
$destRels = @()
foreach ($name in $order) {
    $spec = $libs[$name]
    $sourcePath = if ($Source) { $Source } else { $spec.Source }
    Write-Host "$name sync" -ForegroundColor Cyan
    Write-Host "  source : $sourcePath"
    if ($Commit) { Write-Host "  commit : $Commit (requested)" }
    Write-Host "  dest   : $(Join-Path $repoRoot $spec.Dest)"
    Write-Host ''

    $synced = Sync-OneLibrary -Name $name -Spec $spec -SourcePath $sourcePath -At $Commit -ListOnly:$DryRun
    $combined = $combined -bor [int]$synced.Rc
    $meta = $synced.Meta
    Write-Host "  commit : $($meta.Sha) ($($meta.Branch))"
    if ($meta.Subject) { Write-Host "  subject: $($meta.Subject)" }
    Write-Host "  robocopy rc=$($synced.Rc)"
    Write-Host ''

    if (-not $DryRun) {
        $stampPath = Write-VendorStamp -Name $name -DestRoot $synced.DestRoot -SourcePath $sourcePath -Meta $meta -Scope $scopeText[$name]
        Write-Host "Stamped $stampPath" -ForegroundColor Green
        $destRels += ($spec.Dest -replace '\\', '/')
    }
}

if ($DryRun) {
    Write-Host "DRY RUN -- nothing written. robocopy rc=$combined" -ForegroundColor Yellow
    Write-Host '(rc 0 = already identical; 1 = files would copy; 2 = orphans would be purged; 3 = both)'
    exit 0
}

Test-CrlfFanout -Root $repoRoot -RelPaths $destRels

$manifoldSource = $libs.Manifold2D.Source
$astraSource = $libs.Astra.Source
if ($Library -eq 'Manifold2D' -and $Source) { $manifoldSource = $Source }
if ($Library -eq 'Astra' -and $Source) { $astraSource = $Source }
Write-MosaicDrift -ArcaneRoot $repoRoot -ManifoldSource $manifoldSource -AstraSource $astraSource

Write-Host "Mirrored $Library (robocopy rc=$combined)." -ForegroundColor Green
exit 0
