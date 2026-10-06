# Classify the scanner's seed dump against the FROZEN inventory (S6-1).
# CONSTANT row -> CONSTANT|<why>; SETTING/DERIVED row -> PENDING|<inventory line>; no row -> UNLISTED|line N.
param([string]$Seed = "$env:TEMP\constant-scan.txt",
      [string]$Inventory = "docs/superpowers/audits/2026-10-03-settings-inventory.md",
      [string]$Out = "scripts/constant-allowlist.txt")
$ErrorActionPreference = 'Stop'
$inv = Get-Content -LiteralPath $Inventory -Encoding UTF8
$rows = for ($i = 0; $i -lt $inv.Count; $i++) {
    $l = $inv[$i]; if (-not $l.StartsWith('| ')) { continue }
    $c = [regex]::Split($l.Trim().Trim('|'), '(?<!\\)\|') | ForEach-Object { $_.Trim() }
    if ($c.Count -lt 12) { continue }
    [pscustomobject]@{ Line=$i+1; Files=$c[0]; Symbols=$c[1]; Verdict=($c[3] -split ' ')[0]; Why=$c[11] }
}
$lines = @('# Numeric-constant allow-list (settings arc S6-1). path|symbol|status|note.',
         '# CONSTANT: reviewed, awaiting its ARC_CONSTANT marker (S6-42..44). PENDING: a SETTING/DERIVED row its S6 task deletes.',
         '# UNLISTED: not in the inventory; S6-42..44 classify it. S6-45 requires zero PENDING and zero UNLISTED.')
foreach ($s in Get-Content -LiteralPath $Seed) {
    $f, $sym, $null, $note = $s -split '\|'
    # Basename at a path boundary: 'Settings.hpp' must not match 'ViewportSettings.hpp'.
    $base = '(?<![\w.])' + [regex]::Escape([IO.Path]::GetFileName($f))
    $hit = $rows | Where-Object { $_.Files -match $base -and $_.Symbols -match "\b$([regex]::Escape($sym))\b" } | Select-Object -First 1
    if (-not $hit) {
        $n = [int]($note -replace '\D','')
        $hit = $rows | Where-Object {
            $_.Files -match $base -and ([regex]::Matches($_.Files, ':(\d+)(?:-(\d+))?') | Where-Object {
                $lo = [int]$_.Groups[1].Value; $hi = if ($_.Groups[2].Success) { [int]$_.Groups[2].Value } else { $lo }
                $n -ge $lo - 3 -and $n -le $hi + 3 }) } | Select-Object -First 1
    }
    if (-not $hit)                       { $lines += "$f|$sym|UNLISTED|$note" }
    elseif ($hit.Verdict -eq 'CONSTANT') { $lines += "$f|$sym|CONSTANT|$($hit.Why -replace '\|','/')" }
    else                                 { $lines += "$f|$sym|PENDING|inventory:$($hit.Line) $($hit.Verdict)" }
}
# ($lines, not $out: PowerShell names are case-insensitive, so $out IS the -Out parameter.)
# UTF-8 WITHOUT a BOM, LF endings: ConstantGuardTest reads line 1 as a '#' comment.
# Entries sorted ordinally (merge-friendly: later tasks add or delete whole lines in place).
$entries = [string[]]($lines | Select-Object -Skip 3)
[Array]::Sort($entries, [StringComparer]::Ordinal)
$lines = @($lines | Select-Object -First 3) + $entries
$outPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Out)
[IO.File]::WriteAllText($outPath, ($lines -join "`n") + "`n", (New-Object Text.UTF8Encoding($false)))
$lines | Select-Object -Skip 3 | Group-Object { ($_ -split '\|')[2] } | ForEach-Object { "$($_.Name): $($_.Count)" }
