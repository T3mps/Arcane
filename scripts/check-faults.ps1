# check-faults.ps1 -- Windows Event Log fault detector for Arcane executables.
#
# The engine's own crash capture (Diagnostics::Install, see
# ArcaneClient/src/Arcane/Host/Diagnostics.cpp) cannot see a fault that
# happens INSIDE a driver: nvwgf2umx.dll +0x1203ce sat unread in the
# Windows Event Log for six weeks (2026-07-09/10) before anyone noticed --
# every GPU-work session since had been run at the desk, away from this
# script, so nothing was watching. The Event Log is the only instrument
# that ever caught it. This script reads that instrument on demand instead
# of waiting for someone to remember to check Event Viewer by hand.
#
# Reports "Application Error" events (WER's crash provider) for any
# Arcane*.exe, newest first, and flags entries whose faulting module looks
# like a GPU driver (nv*/amd*) -- that shape is the Parsec-class signature:
# the app is ours, but the actual fault address is inside vendor code we
# don't control and can't patch, only avoid triggering.
#
# Usage:
#   powershell -File scripts\check-faults.ps1              # last 30 days (default)
#   powershell -File scripts\check-faults.ps1 -Days 60      # wider window
#
# Windows PowerShell 5.1 compatible.

param([int]$Days = 30)

# Get-WinEvent throws a terminating "No events were found..." error when the
# filter matches nothing -- on a clean machine (or a narrow -Days window)
# that is the NORMAL case, not a failure, so it must not abort the script.
$ErrorActionPreference = 'SilentlyContinue'

$ev = Get-WinEvent -FilterHashtable @{
    LogName='Application'; ProviderName='Application Error'
    StartTime=(Get-Date).AddDays(-$Days)
} -MaxEvents 500

if (-not $ev) { "No Application Error events in the last $Days days."; exit 0 }

# Application Error is WER's legacy provider: it has no schema, so
# Get-WinEvent hands back positional Properties rather than named fields.
# Verified against a live event's own rendered Message text on this box
# (2026-08-24) before trusting these indices:
#   [0] Faulting application name   [3] Faulting module name
#   [1] version                     [4] version
#   [2] time stamp                  [5] time stamp
#                                    [6] Exception code
#                                    [7] Fault offset
$rows = $ev | ForEach-Object {
    $p = $_.Properties
    [pscustomobject]@{ Time=$_.TimeCreated; App=$p[0].Value; Faulting=$p[3].Value; Offset=$p[7].Value }
} | Where-Object { $_.App -like 'Arcane*' }

if (-not $rows) { "No Arcane faults in the last $Days days."; exit 0 }

$rows | Sort-Object Time -Descending | Format-Table -AutoSize

$driver = $rows | Where-Object { $_.Faulting -like 'nv*' -or $_.Faulting -like 'amd*' }
if ($driver) { Write-Warning "GPU DRIVER faults present -- $($driver.Count). This is the Parsec-class signature." }
