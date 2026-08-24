# check-faults.ps1 -- Windows Event Log fault detector for Arcane executables.
#
# The engine's own crash capture (Diagnostics::Install, see
# ArcaneClient/src/Arcane/Base/Diagnostics.cpp) cannot see a fault that
# happens INSIDE a driver: nvwgf2umx.dll +0x1203ce sat unread in the
# Windows Event Log for six weeks (2026-07-09/10) before anyone noticed --
# every GPU-work session since had been run at the desk, away from this
# script, so nothing was watching. The Event Log is the only instrument
# that ever caught it. This script reads that instrument on demand instead
# of waiting for someone to remember to check Event Viewer by hand.
#
# Reports "Application Error" events (WER's crash provider) for any
# Arcane*.exe, newest first, and flags entries whose faulting module looks
# like a GPU driver (nv*/amd*/ati*) -- that shape is the Parsec-class
# signature: the app is ours, but the actual fault address is inside vendor
# code we don't control and can't patch, only avoid triggering. `ati*` is
# included alongside `amd*` because AMD's D3D driver modules still ship
# under the legacy ATI name (atidxx64.dll, aticfx64.dll) even in current
# packages.
#
# "No results" is reported two different ways on purpose:
#   - Get-WinEvent finding zero matching events is EXPECTED and reported as
#     a clean, exit-0 message (fault-free is the normal/desired outcome).
#   - Any OTHER failure to read the log (log/provider missing, access
#     denied, service down, ...) is a DIFFERENT, noisy, exit-1 failure --
#     this is the one instrument that sees the driver-fault class, so it
#     must never silently collapse "couldn't check" into "nothing to see".
#
# Usage:
#   powershell -File scripts\check-faults.ps1              # last 30 days (default)
#   powershell -File scripts\check-faults.ps1 -Days 60      # wider window
#   powershell -File scripts\check-faults.ps1 -Days 0       # empty window (StartTime = now) --
#                                                            # degenerate/no-op sanity check, NOT "today"
#
# Windows PowerShell 5.1 compatible.

param([int]$Days = 30)

$ErrorActionPreference = 'Stop'

# Get-WinEvent throws a terminating error both when the log/provider is
# missing or unreadable (a real failure) AND when the filter simply matches
# no events (the normal clean-machine case). Those two must not collapse
# into the same outcome, so the match is narrowed to the specific
# "NoMatchingEventsFound" id (confirmed via $_.FullyQualifiedErrorId on
# this box) rather than a blanket $ErrorActionPreference = 'SilentlyContinue'
# that would have swallowed everything alike.
try {
    $ev = Get-WinEvent -FilterHashtable @{
        LogName='Application'; ProviderName='Application Error'
        StartTime=(Get-Date).AddDays(-$Days)
    } -MaxEvents 500 -ErrorAction Stop
} catch {
    if ($_.FullyQualifiedErrorId -like 'NoMatchingEventsFound*') {
        "No Application Error events in the last $Days days."
        exit 0
    }
    Write-Error "Could not read the Application event log: $($_.Exception.Message)"
    exit 1
}

$evCount = @($ev).Count
if ($evCount -ge 500) {
    Write-Warning "results truncated at $evCount events (the -MaxEvents cap) -- the oldest events in this window, possibly including the fault you care about, may be missing. Narrow -Days or raise the cap in this script."
}

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

$driver = $rows | Where-Object { $_.Faulting -like 'nv*' -or $_.Faulting -like 'amd*' -or $_.Faulting -like 'ati*' }
if ($driver) { Write-Warning "GPU DRIVER faults present -- $($driver.Count). This is the Parsec-class signature." }
