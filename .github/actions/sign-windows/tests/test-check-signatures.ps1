# Tests for check-signatures.ps1 (Windows): a signed and timestamped binary
# passes; an unsigned one, one in a subfolder, and an empty folder fail.
$ErrorActionPreference = 'Stop'
$check = Join-Path $PSScriptRoot '..\check-signatures.ps1'
$shell = (Get-Process -Id $PID).Path
$root = Join-Path ([IO.Path]::GetTempPath()) ("sigtest-" + [guid]::NewGuid())
$failures = 0

function Invoke-Check($folder) {
    & $shell -NoProfile -ExecutionPolicy Bypass -File $check -Folder $folder *> $null
    return $LASTEXITCODE
}
function Expect($name, $folder, $code) {
    $got = Invoke-Check $folder
    if (($code -eq 0) -eq ($got -eq 0)) { Write-Host "ok   $name" } else { Write-Host "FAIL $name (exit $got)"; $script:failures++ }
}

# A signed, timestamped exe from the machine: PowerShell 7 or Git, both embed signatures.
$signed = @("$env:ProgramFiles\PowerShell\7\pwsh.exe", "$env:ProgramFiles\Git\cmd\git.exe") |
    Where-Object { (Test-Path $_) -and ((Get-AuthenticodeSignature $_).Status -eq 'Valid') -and ($null -ne (Get-AuthenticodeSignature $_).TimeStamperCertificate) } |
    Select-Object -First 1

New-Item -ItemType Directory -Force "$root\good\sub", "$root\unsigned", "$root\nested\sub", "$root\empty" | Out-Null
if ($signed) {
    Copy-Item $signed "$root\good\sub\signed.exe"
    Expect 'a signed, timestamped exe passes' "$root\good" 0
    Copy-Item $signed "$root\nested\top.exe"
} else {
    Write-Host 'skip a signed, timestamped exe passes (no embedded-signed exe found on this machine)'
}
[IO.File]::WriteAllBytes("$root\unsigned\fake.dll", [byte[]](0x4D, 0x5A, 0, 0))
[IO.File]::WriteAllBytes("$root\nested\sub\fake.dll", [byte[]](0x4D, 0x5A, 0, 0))
Expect 'an unsigned dll fails' "$root\unsigned" 1
Expect 'an unsigned dll in a subfolder fails' "$root\nested" 1
Expect 'an empty folder fails' "$root\empty" 1

Remove-Item -Recurse -Force $root
# Exit explicitly: GitHub's pwsh wrapper exits with $LASTEXITCODE, which still
# holds the last deliberately failing check's 1.
if ($failures) { exit 1 }
exit 0
