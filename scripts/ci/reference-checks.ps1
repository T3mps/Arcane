# reference-checks.ps1 <Debug|Release|Dist> -- the Jenkins-gate checks that
# need no GPU, run on the hosted Windows legs after the tests
# (.github/workflows/ci.yml; audit C10). Sub-project B's runner calls the same
# script. Run AFTER scripts\ci\build.ps1 and scripts\run-tests.ps1:
#   * Debug and Release: arccook --project ReferenceProject --check -- the
#     cooked content is current (the Jenkinsfile's "Asset cook check").
#   * Debug: ReferenceProject built as its own workspace (premake vs2026 +
#     msbuild ReferenceProject.slnx) -- a game author's path without arcbuild
#     (the Jenkinsfile's "ReferenceProject (SDK build)").
# Exit: 0, or the first failing check's code.
param([Parameter(Mandatory, Position = 0)][ValidateSet('Debug', 'Release', 'Dist')][string]$Config)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'ArcaneCi.psm1') -Force
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$logs = Join-Path $root 'logs'

if ($Config -ne 'Dist') {
    $arccook = Join-Path $root "bin\$Config-windows-x86_64-md\arccook\arccook.exe"
    $code = Invoke-NativeLogged -FilePath $arccook -ArgumentList @('--project', (Join-Path $root 'ReferenceProject'), '--check') `
        -LogPath (Join-Path $logs "check-cook-$Config.log")
    if ($code -ne 0) { Write-Host "reference-checks: arccook --check FAILED ($code)" -ForegroundColor Red; exit $code }
}
if ($Config -eq 'Debug') {
    # premake runs inside ReferenceProject/, as the Jenkinsfile does.
    Push-Location (Join-Path $root 'ReferenceProject')
    try {
        $code = Invoke-NativeLogged -FilePath (Join-Path $root 'ThirdParty\premake5\premake5.exe') -ArgumentList @('vs2026') `
            -LogPath (Join-Path $logs 'check-sdk-generate.log')
    } finally { Pop-Location }
    if ($code -ne 0) { Write-Host "reference-checks: ReferenceProject premake FAILED ($code)" -ForegroundColor Red; exit $code }
    $code = Invoke-NativeLogged -FilePath (Resolve-MSBuild) `
        -ArgumentList @((Join-Path $root 'ReferenceProject\ReferenceProject.slnx'), '/p:Configuration=Debug', '/m', '/nologo', '/v:minimal') `
        -LogPath (Join-Path $logs 'check-sdk-build.log')
    if ($code -ne 0) { Write-Host "reference-checks: ReferenceProject standalone build FAILED ($code)" -ForegroundColor Red; exit $code }
}
Write-Host "reference-checks: $Config ok"
exit 0
