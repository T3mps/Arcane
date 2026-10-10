# namespace-compile-fail.ps1 -- FA2. Each forbidden spelling is its own TU and
# must fail to compile. The control TU must compile. Exit 0 = PASS.
[CmdletBinding()]
param([string]$Root)
$ErrorActionPreference = 'Stop'
if (-not $Root) { $Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path }

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { Write-Host 'namespace-compile-fail: no Visual Studio with the C++ tools' -ForegroundColor Red; exit 2 }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

$incNames = @(
    'ArcaneCore\src',
    'ThirdParty\nlohmann',
    'ThirdParty\picosha2',
    'ThirdParty\spdlog\include',
    'ThirdParty\glm',
    'ThirdParty\stb',
    'ThirdParty\Astra\include',
    'ThirdParty\enkiTS\src',
    'ThirdParty\Manifold2D\include',
    'ThirdParty\Mosaic\include'
)
$inc = ($incNames | ForEach-Object { '/I"' + (Join-Path $Root $_) + '"' }) -join ' '
$flags = "/nologo /std:c++latest /Zc:__cplusplus /EHsc /MD /c /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS $inc"

$dir = Join-Path $Root 'ArcaneTests\compile-fail\namespace'
$bat = Join-Path $env:TEMP 'namespace-compile-fail.bat'
$log = Join-Path $env:TEMP 'namespace-compile-fail.log'
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("@echo off")
$lines.Add("call `"$vcvars`" >nul")
$lines.Add("if errorlevel 1 exit /b 2")

function Add-Compile([string]$name, [bool]$mustFail) {
    $src = Join-Path $dir $name
    $obj = Join-Path $env:TEMP ("nsfacade-" + [IO.Path]::GetFileNameWithoutExtension($name) + ".obj")
    $script:lines.Add("echo --- $name")
    $script:lines.Add("cl $flags /Fo`"$obj`" `"$src`"")
    if ($mustFail) {
        $script:lines.Add("if not errorlevel 1 (")
        $script:lines.Add("  echo namespace-compile-fail: $name compiled; that spelling must not exist")
        $script:lines.Add("  exit /b 1")
        $script:lines.Add(")")
    } else {
        $script:lines.Add("if errorlevel 1 (")
        $script:lines.Add("  echo namespace-compile-fail: the CONTROL failed to compile")
        $script:lines.Add("  exit /b 1")
        $script:lines.Add(")")
    }
}

Add-Compile 'Control.cpp' $false
foreach ($name in @(
    'EcsEntity.cpp', 'Physics2DWorld.cpp', 'ArcanePhys.cpp', 'PhysicsResource.cpp',
    'PhysicsSystem.cpp', 'RigidBody.cpp', 'PhysicsWorld.cpp', 'Body.cpp',
    'BodyHandle.cpp', 'WorldMember.cpp')) {
    Add-Compile $name $true
}
$lines.Add("echo namespace-compile-fail: PASS (control compiles; 10 forbidden spellings do not)")
$lines.Add("exit /b 0")
[System.IO.File]::WriteAllLines($bat, $lines)
cmd /c "`"$bat`" > `"$log`" 2>&1"
$code = $LASTEXITCODE
Get-Content $log | Write-Host
exit $code
