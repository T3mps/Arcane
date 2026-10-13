# image-tag.ps1 [-Root DIR] -- the PowerShell twin of scripts/ci/image-tag.sh:
# prints the Linux CI image tag (build-<16 hex>) from the same inputs, the
# same checks and the same bytes (<path> NUL <bytes minus every CR> NUL, in
# ci/linux/image-inputs.txt order). scripts/ci/tests/test-image-tag.sh asserts
# both print the same tag. scripts\ci\local.ps1 uses it, so the desk resolves
# the tag without starting a container. Exit 1 + "image-tag: <reason>" on
# stderr when the inputs are refused (read image-tag.sh's header for the rules).
param([string]$Root)
$ErrorActionPreference = 'Stop'
if (-not $Root) { $Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }
$Root = (Resolve-Path -LiteralPath $Root).ProviderPath

function Fail([string]$Message) { [Console]::Error.WriteLine("image-tag: $Message"); exit 1 }
function Read-Text([string]$Rel) {
    $p = Join-Path $Root $Rel
    if (-not (Test-Path -LiteralPath $p)) { Fail "missing $p" }
    return ([System.IO.File]::ReadAllText($p)).Replace("`r", '')
}

$inputs = @()
foreach ($l in ((Read-Text 'ci/linux/image-inputs.txt') -split "`n")) {
    $t = ($l -replace '#.*$', '').Trim()
    if ($t) { $inputs += $t }
}

$stages = @{}
$refs = @()
foreach ($line in ((Read-Text 'ci/linux/Dockerfile') -split "`n")) {
    $t = $line.Trim()
    if (-not $t -or $t.StartsWith('#')) { continue }
    $tok = @($t -split '\s+')
    $kw = $tok[0].ToUpperInvariant()
    if ($kw -eq 'FROM') {
        for ($i = 1; $i -lt $tok.Count - 1; $i++) { if ($tok[$i] -ieq 'AS') { $stages[$tok[$i + 1]] = $true } }
        continue
    }
    if ($kw -eq 'ADD') { Fail "ci/linux/Dockerfile uses ADD (use a one-line COPY --from=scripts): $t" }
    if ($kw -ne 'COPY') { continue }
    if ($tok[$tok.Count - 1] -eq '\') { Fail "a COPY in ci/linux/Dockerfile continues onto the next line (keep it on one line): $t" }
    $from = ''
    $rest = @()
    for ($i = 1; $i -lt $tok.Count; $i++) {
        if ($tok[$i].StartsWith('--')) { if ($tok[$i].StartsWith('--from=')) { $from = $tok[$i].Substring(7) } }
        else { $rest += $tok[$i] }
    }
    if ($from -and $stages.ContainsKey($from)) { continue }
    if ($from -ne 'scripts') { Fail "every repo file must come in as COPY --from=scripts <file>... <dest>: $t" }
    for ($i = 0; $i -lt $rest.Count - 1; $i++) { $refs += "scripts/$($rest[$i])" }
}

foreach ($r in $refs) { if ($inputs -cnotcontains $r) { Fail "ci/linux/Dockerfile copies $r, which is not in ci/linux/image-inputs.txt" } }
if ($inputs -cnotcontains 'ci/linux/Dockerfile') { Fail 'ci/linux/image-inputs.txt must list ci/linux/Dockerfile' }
if ($inputs -cnotcontains 'ci/linux/image-inputs.txt') { Fail 'ci/linux/image-inputs.txt must list itself' }
foreach ($f in $inputs) {
    if ($f.StartsWith('scripts/') -and ($refs -cnotcontains $f)) { Fail "$f is listed in ci/linux/image-inputs.txt but ci/linux/Dockerfile does not copy it" }
    if (-not (Test-Path -LiteralPath (Join-Path $Root $f))) { Fail "missing input $f" }
}

$ms = New-Object System.IO.MemoryStream
foreach ($f in $inputs) {
    $name = [System.Text.Encoding]::UTF8.GetBytes($f)
    $ms.Write($name, 0, $name.Length)
    $ms.WriteByte(0)
    foreach ($b in [System.IO.File]::ReadAllBytes((Join-Path $Root $f))) { if ($b -ne 13) { $ms.WriteByte($b) } }
    $ms.WriteByte(0)
}
$sha = [System.Security.Cryptography.SHA256]::Create()
$hex = -join ($sha.ComputeHash($ms.ToArray()) | ForEach-Object { $_.ToString('x2') })
Write-Output "build-$($hex.Substring(0, 16))"
exit 0
