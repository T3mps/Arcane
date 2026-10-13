# compare-test-counts.ps1 -Baseline <dir> -Candidate <dir> -- compares two
# downloaded sets of CI test-results artifacts (`gh run download <run> -p
# 'test-results-*' -D <dir>`), keyed <leg>-<Config>[-gpu]: the tests and
# skipped counts must agree and the candidate must have no failures. The
# container-parity check of docs/superpowers/specs/2026-10-12-local-ci-pipeline-design.md
# s3.2; sub-project B reuses it for Jenkins side by side. Exit 0 = every key matches.
# -Key <pattern[]> limits the comparison (a local run has no -gpu lane).
param([Parameter(Mandatory)][string]$Baseline, [Parameter(Mandatory)][string]$Candidate, [string[]]$Key = @())
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'ArcaneCi.psm1') -Force
$rows = Compare-JUnitDirs -Baseline $Baseline -Candidate $Candidate -Key $Key
$rows | Format-Table Key, BaselineTests, CandidateTests, BaselineSkipped, CandidateSkipped, CandidateFailures, Match -AutoSize | Out-Host
$bad = @($rows | Where-Object { -not $_.Match }).Count
if ($bad -gt 0) { Write-Host "compare-test-counts: $bad key(s) differ" -ForegroundColor Red; exit 1 }
Write-Host "compare-test-counts: all $($rows.Count) key(s) match"
exit 0
