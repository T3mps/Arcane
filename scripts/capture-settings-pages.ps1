# capture-settings-pages.ps1 -- settings arc S4 gate: headless screenshots of the
# Preferences rich pages (theme Dark/Light/HighContrast, shortcuts plain and with
# a conflict, fonts at 16 and 20 px and at 1.25x, layouts with one saved
# layout). Automation only:
# --headless renders offscreen; no window focus, no SendInput. A scratch
# LOCALAPPDATA keeps the user's own preferences and layouts out of it.
#
# The page is opened through S3's automation hook, the Dev|Hidden cvars
# editor.settings.openAtBoot (preferences | project | both) and
# editor.settings.openCategory (a settings-tree path such as Appearance/Theme),
# read on the editor's first UI frame -- after --set has applied.
#
#   powershell -ExecutionPolicy Bypass -File scripts\capture-settings-pages.ps1 -OutDir <dir> [-Configuration Release]
#
# Writes theme-dark.png, theme-light.png, theme-highcontrast.png, shortcuts.png,
# shortcuts-conflict.png, fonts-default.png, fonts-size-20.png,
# fonts-scale-125.png and layouts.png into -OutDir, each with the host's stdout
# beside it (<name>.log). Exit 0 when all nine PNGs were written by a host that
# exited 0 and refused none of its --set values, else 1. The conflict capture
# ticks the Keyboard page's Conflicts only (editor.settings.keysConflictsOnly),
# so every red row -- the Graph context's too -- sits above the 1080p fold.
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [Parameter(Mandatory)][string]$OutDir
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exeDir = Join-Path $root "bin\$Configuration-windows-x86_64-md\ArcaneEditor"
$exe = Join-Path $exeDir 'ArcaneEditor.exe'
if (-not (Test-Path $exe)) { throw "capture-settings-pages: $exe is missing -- build $Configuration first." }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path   # the host runs from its exe dir; hand it an absolute path

# The preset's colors as --set pairs (the keys are full cvar names).
function Theme-Sets([string]$name) {
    $doc = Get-Content -Raw (Join-Path $root "data\EditorThemes\$name.arctheme") | ConvertFrom-Json
    $out = @()
    foreach ($p in $doc.colors.PSObject.Properties) { $out += '--set'; $out += "$($p.Name)=$($p.Value)" }
    return $out
}

function Capture([string]$name, [string]$category, [string[]]$extra) {
    # One scratch LOCALAPPDATA per capture: Paths derives EditorUserDir from it
    # (<LOCALAPPDATA>\Arcane\Editor), so the layouts page lists exactly "Studio".
    $lad = Join-Path $env:TEMP "s4-capture-lad-$name"
    if (Test-Path $lad) { Remove-Item -Recurse -Force $lad }
    New-Item -ItemType Directory -Force (Join-Path $lad 'Arcane\Editor\Layouts') | Out-Null
    Copy-Item (Join-Path $root 'ReferenceProject\Saved\verify-layout.ini') (Join-Path $lad 'Arcane\Editor\Layouts\Studio.ini')
    $png = Join-Path $OutDir "$name.png"
    if (Test-Path $png) { Remove-Item -Force $png }   # a stale PNG must never read as this run's output
    # --project is relative: from the exe dir it is the staged ReferenceProject.
    $argv = @('--project', 'ReferenceProject', '--headless', '--backend', 'dx12', '--window-size', '1920x1080',
              '--frames', '90', '--screenshot', $png,
              '--set', 'editor.settings.openAtBoot=preferences',
              '--set', "editor.settings.openCategory=$category") + $extra
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe; $psi.WorkingDirectory = $exeDir; $psi.UseShellExecute = $false
    $psi.Arguments = ($argv | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
    $psi.EnvironmentVariables['LOCALAPPDATA'] = $lad
    $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true   # the log sink writes to stderr
    $p = [System.Diagnostics.Process]::Start($psi)
    $stderr = $p.StandardError.ReadToEndAsync()   # drain both pipes at once, or a full one stalls the host
    $log = $p.StandardOutput.ReadToEnd(); $p.WaitForExit(); $log += $stderr.Result
    Set-Content -Encoding utf8 -Path (Join-Path $OutDir "$name.log") -Value $log
    # A --set the host refused (unknown name, bad value) leaves the page at its
    # default: the PNG would exist but show the wrong state, so it fails here.
    $refused = @($log -split "`n" | Where-Object { $_ -match 'cvar: --set ' })
    $ok = ($p.ExitCode -eq 0) -and (Test-Path $png) -and ($refused.Count -eq 0)
    $verdict = if (-not (Test-Path $png)) { 'MISSING' } elseif ($refused.Count -gt 0) { 'SET-REFUSED' } elseif ($p.ExitCode -ne 0) { 'EXIT' } else { 'PNG' }
    # Write-Host, not Write-Output: Capture's pipeline output is its verdict alone.
    Write-Host ("{0,-22} exit={1} {2}" -f $name, $p.ExitCode, $verdict)
    foreach ($line in $refused) { Write-Host ("    " + $line.Trim()) }
    return $ok
}

$results = @(
    Capture 'theme-dark'         'Appearance/Theme'           @()
    Capture 'theme-light'        'Appearance/Theme'           (Theme-Sets 'Light')
    Capture 'theme-highcontrast' 'Appearance/Theme'           (Theme-Sets 'HighContrast')
    Capture 'shortcuts'          'Keyboard'                   @()
    Capture 'shortcuts-conflict' 'Keyboard'                   @('--set', 'editor.keys.edit.copy=F', '--set', 'editor.settings.keysConflictsOnly=true')
    Capture 'fonts-default'      'Appearance/Fonts and Scale' @()
    Capture 'fonts-size-20'      'Appearance/Fonts and Scale' @('--set', 'editor.ui.fontSize=20')
    Capture 'fonts-scale-125'    'Appearance/Fonts and Scale' @('--set', 'editor.ui.scale=1.25')
    Capture 'layouts'            'Layout'                     @()
)
if ($results -contains $false) { exit 1 }
exit 0
