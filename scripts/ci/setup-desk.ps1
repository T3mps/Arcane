# setup-desk.ps1 -- one-time desk setup for scripts\ci\local.ps1 (docs/ci.md,
# "Local runs"). Idempotent; safe to re-run.
#   * %USERPROFILE%\.wslconfig: [wsl2] memory=24GB, processors=16 (every other
#     key kept; a changed file is backed up first)
#   * checks Docker Desktop (installed, running, Linux containers) and Visual
#     Studio's MSBuild (vswhere)
# It never runs `wsl --shutdown` itself: a changed .wslconfig applies after
# you run it, and that stops every WSL distro and Docker Desktop.
# Exit 0 when the desk is ready (a pending wsl --shutdown still counts).
param([string]$Memory = '24GB', [int]$Processors = 16)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'ArcaneCi.psm1') -Force
$ok = $true

$path = Join-Path $env:USERPROFILE '.wslconfig'
$old = ''
if (Test-Path -LiteralPath $path) { $old = [System.IO.File]::ReadAllText($path) }
$new = Set-WslConfigLimits -Text $old -Memory $Memory -Processors $Processors
if ($new -ceq $old) {
    Write-Host "setup-desk: $path already has memory=$Memory, processors=$Processors"
} else {
    if ($old) {
        $backup = "$path.bak-$(Get-Date -Format yyyyMMdd-HHmmss)"
        Copy-Item -LiteralPath $path -Destination $backup
        Write-Host "setup-desk: backed up $path -> $backup"
    }
    [System.IO.File]::WriteAllText($path, $new, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "setup-desk: wrote $path (memory=$Memory, processors=$Processors)"
    Write-Host 'setup-desk: run `wsl --shutdown` to apply it -- it stops every running WSL distro and Docker Desktop' -ForegroundColor Yellow
}

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    Write-Host 'setup-desk: docker not found -- install Docker Desktop (WSL2 backend)' -ForegroundColor Red
    $ok = $false
} else {
    $info = Invoke-NativeQuiet -FilePath 'docker' -ArgumentList @('info', '--format', '{{.OSType}} {{.NCPU}} {{.MemTotal}}')
    # A wedged Docker Desktop (API 500) still exits 0, printing ' 0 0' (an empty OSType).
    if ($info.Code -ne 0 -or -not $info.Output -or "$($info.Output[0])" -notmatch '^\S') {
        Write-Host 'setup-desk: Docker Desktop is not running or not responding -- start (or restart) it' -ForegroundColor Red
        $ok = $false
    } elseif (("$($info.Output[0])").Trim() -notlike 'linux *') {
        Write-Host "setup-desk: Docker reports '$($info.Output[0])' -- switch Docker Desktop to Linux containers" -ForegroundColor Red
        $ok = $false
    } else {
        Write-Host "setup-desk: docker ok (OS, CPUs, memory bytes: $($info.Output[0]))"
    }
    # Resource Saver pauses the Docker VM after a few idle minutes -- shorter than a
    # Windows leg -- and some Docker Desktop versions resume wedged (API 500).
    $dd = Join-Path $env:APPDATA 'Docker\settings.json'
    if (Test-Path -LiteralPath $dd) {
        try {
            $settings = [System.IO.File]::ReadAllText($dd) | ConvertFrom-Json
            if ($settings.PSObject.Properties['useResourceSaver'] -and $settings.useResourceSaver) {
                Write-Host 'setup-desk: WARNING Docker Desktop Resource Saver is on -- a Linux leg after a long Windows leg may find Docker wedged; turn it off (Settings > Resources > Resource Saver) or update Docker Desktop' -ForegroundColor Yellow
            }
        } catch { }
    }
}

try { Write-Host "setup-desk: MSBuild $(Resolve-MSBuild)" }
catch { Write-Host "setup-desk: $($_.Exception.Message) -- install Visual Studio 2026 with Desktop development with C++" -ForegroundColor Red; $ok = $false }

if (-not $ok) { exit 1 }
exit 0
