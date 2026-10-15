# check-signatures.ps1 -Folder DIR: every .exe and .dll under DIR must be
# Authenticode-signed (Status Valid) and timestamped, or the release stops
# (spec s4 step 4; s12, Smart App Control). An empty folder is an error.
param([Parameter(Mandatory)][string]$Folder)
$files = @(Get-ChildItem -LiteralPath $Folder -Recurse -File -Include *.exe, *.dll)
if ($files.Count -eq 0) { Write-Host "::error::no .exe or .dll under $Folder; nothing was signed"; exit 1 }
$bad = @()
foreach ($f in $files) {
    $s = Get-AuthenticodeSignature -LiteralPath $f.FullName
    if ($s.Status -ne 'Valid') { $bad += "$($f.FullName): $($s.Status) $($s.StatusMessage)"; continue }
    if ($null -eq $s.TimeStamperCertificate) { $bad += "$($f.FullName): signed but not timestamped" }
}
if ($bad.Count -gt 0) { $bad | ForEach-Object { Write-Host "::error::$_" }; exit 1 }
Write-Host "$($files.Count) file(s) signed and timestamped"
