# launch.ps1 -- convenience launcher for Arcane workspace executables.
#
# Launches any compiled project exe from any build config under Arcane\bin,
# with the exe's OWN directory as the working directory (ArcaneRuntime/Sandbox
# resolve plugin DLLs and shaders relative to it -- launching from elsewhere
# breaks them, which is why plain double-click-from-Explorer works but a naive
# shell invocation from another directory does not).
#
# Usage (from anywhere):
#   .\launch.ps1                              # interactive: pick project + config
#   .\launch.ps1 -List                        # show everything that's built
#   .\launch.ps1 ArcaneRuntime                # best available config (Dist > Release > Debug)
#   .\launch.ps1 ArcaneRuntime Dist           # explicit config (fuzzy: d / dist / Dist all work)
#   .\launch.ps1 ArcaneRuntime Dist --backend vulkan --frames 180   # extra args pass through to the exe
#   .\launch.ps1 ArcaneTests Debug -Wait "~[gpu]"          # console apps: -Wait streams output here
#   .\launch.ps1 ArcaneRuntime -DryRun        # print what would launch, don't launch
#
# Windows PowerShell 5.1 compatible.

param(
    [Parameter(Position = 0)] [string]$Project,
    [Parameter(Position = 1)] [string]$Config,
    [Parameter(ValueFromRemainingArguments = $true)] [string[]]$ExeArgs,
    [switch]$List,
    [switch]$Wait,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

$binRoot = Join-Path (Split-Path -Parent $PSScriptRoot) 'bin'
if (-not (Test-Path $binRoot)) {
    Write-Host "No bin directory at $binRoot -- build something first (msbuild Arcane.slnx)." -ForegroundColor Red
    exit 1
}

# Discover: config dirs (e.g. Dist-windows-x86_64-md) -> project dirs containing an exe.
$configDirs = Get-ChildItem $binRoot -Directory
$inventory = @()   # objects: Config (short), ConfigDir, Project, ExePath
foreach ($cd in $configDirs) {
    $shortCfg = ($cd.Name -split '-')[0]
    foreach ($pd in (Get-ChildItem $cd.FullName -Directory)) {
        $exes = @(Get-ChildItem $pd.FullName -Filter '*.exe' -File -ErrorAction SilentlyContinue)
        foreach ($exe in $exes) {
            $inventory += [pscustomobject]@{
                Config    = $shortCfg
                ConfigDir = $cd.Name
                Project   = $pd.Name
                Exe       = $exe.Name
                ExePath   = $exe.FullName
                WorkDir   = $pd.FullName
            }
        }
    }
}

if ($inventory.Count -eq 0) {
    Write-Host "No built executables found under $binRoot." -ForegroundColor Red
    exit 1
}

if ($List) {
    $inventory | Sort-Object Config, Project | Format-Table Config, Project, Exe, ConfigDir -AutoSize
    exit 0
}

# Interactive picker when no project was named.
if (-not $Project) {
    $choices = $inventory | Sort-Object Project, Config
    Write-Host "Built executables:" -ForegroundColor Cyan
    for ($i = 0; $i -lt $choices.Count; $i++) {
        Write-Host ("  [{0,2}] {1,-9} {2}" -f $i, $choices[$i].Config, $choices[$i].Project)
    }
    $pick = Read-Host "Launch which? (number, blank cancels)"
    if ($pick -eq '') { exit 0 }
    $sel = $choices[[int]$pick]
    if (-not $sel) { Write-Host "No such entry." -ForegroundColor Red; exit 1 }
}
else {
    # Fuzzy project match (prefix, case-insensitive), then config preference.
    $matches_ = @($inventory | Where-Object { $_.Project -like "$Project*" })
    if ($matches_.Count -eq 0) {
        Write-Host "No built project matches '$Project'. Built projects:" -ForegroundColor Red
        ($inventory | Select-Object -ExpandProperty Project | Sort-Object -Unique) -join ', ' | Write-Host
        exit 1
    }
    if ($Config) {
        $matches_ = @($matches_ | Where-Object { $_.Config -like "$Config*" })
        if ($matches_.Count -eq 0) {
            Write-Host "'$Project' is not built in a config matching '$Config'." -ForegroundColor Red
            exit 1
        }
    }
    # Preference order when config unspecified (or fuzzy-matched several).
    $pref = @('Dist', 'Release', 'Debug')
    $sel = $null
    foreach ($p in $pref) {
        $hit = @($matches_ | Where-Object { $_.Config -eq $p })
        if ($hit.Count -gt 0) { $sel = $hit[0]; break }
    }
    if (-not $sel) { $sel = $matches_[0] }
}

Write-Host ("Launching {0} [{1}] from {2}" -f $sel.Exe, $sel.Config, $sel.WorkDir) -ForegroundColor Green
if ($ExeArgs -and $ExeArgs.Count -gt 0) {
    Write-Host ("  args: {0}" -f ($ExeArgs -join ' '))
}

if ($DryRun) {
    Write-Host "(dry run -- not launched)" -ForegroundColor Yellow
    exit 0
}

$startArgs = @{
    FilePath         = $sel.ExePath
    WorkingDirectory = $sel.WorkDir
}
if ($ExeArgs -and $ExeArgs.Count -gt 0) { $startArgs['ArgumentList'] = $ExeArgs }
if ($Wait) {
    # Console apps: stream into this console and return the exe's exit code.
    $startArgs['NoNewWindow'] = $true
    $startArgs['Wait'] = $true
    $startArgs['PassThru'] = $true
    $proc = Start-Process @startArgs
    exit $proc.ExitCode
}
Start-Process @startArgs
