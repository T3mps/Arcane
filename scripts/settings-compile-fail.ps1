# settings-compile-fail.ps1 -- the build guard for spec s4.3 / s13: a settings
# field whose type has no cvar mapping must stop the BUILD (ReflectField's
# static_assert), not register a broken cvar. Compiles the fixture twice with
# the VS toolchain: the control must compile, the real case must fail on the
# static_assert text and name the field. Exit 0 = PASS.
[CmdletBinding()]
param([string]$Root)
$ErrorActionPreference = 'Stop'
if (-not $Root) { $Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path }

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { Write-Host 'settings-compile-fail: no Visual Studio with the C++ tools' -ForegroundColor Red; exit 2 }
$vcvars  = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$fixture = Join-Path $Root 'ArcaneTests\compile-fail\SettingsUnmappableField.cpp'
$obj     = Join-Path $env:TEMP 'settings-compile-fail.obj'
$inc = @('ArcaneCore\src', 'ThirdParty\Astra\include', 'ThirdParty\Mosaic\include', 'ThirdParty\spdlog\include',
         'ThirdParty\nlohmann', 'ThirdParty\glm') | ForEach-Object { '/I"' + (Join-Path $Root $_) + '"' }
$flags = '/nologo /std:c++latest /Zc:__cplusplus /EHsc /c /DNOMINMAX /DWIN32_LEAN_AND_MEAN ' + ($inc -join ' ')

function Invoke-Cl([string]$extra) {
    $out = cmd /c "call `"$vcvars`" >nul && cl $flags $extra /Fo`"$obj`" `"$fixture`" 2>&1"
    return @{ Code = $LASTEXITCODE; Text = ($out -join "`n") }
}

$control = Invoke-Cl '/DARC_COMPILE_FAIL_CONTROL'
if ($control.Code -ne 0) {
    Write-Host $control.Text
    Write-Host 'settings-compile-fail: the CONTROL failed -- the fixture or include paths are broken; the real case proves nothing' -ForegroundColor Red
    exit 1
}
$bad = Invoke-Cl ''
if ($bad.Code -eq 0) {
    Write-Host 'settings-compile-fail: an UNMAPPABLE settings field compiled -- the static_assert is gone' -ForegroundColor Red
    exit 1
}
if ($bad.Text -notmatch 'ARC_SETTINGS: a settings field must be') {
    Write-Host $bad.Text
    Write-Host 'settings-compile-fail: the build failed, but not on the settings static_assert' -ForegroundColor Red
    exit 1
}
if ($bad.Text -notmatch 'badField') {
    Write-Host $bad.Text
    Write-Host 'settings-compile-fail: the diagnostic does not name the field (badField)' -ForegroundColor Red
    exit 1
}
Write-Host 'settings-compile-fail: PASS (the control compiles; the unmappable field fails on the static_assert and names badField)' -ForegroundColor Green
exit 0
