<#
.SYNOPSIS
    Shim: re-vendor Astra by calling sync-vendor.ps1 -Library Astra.

.DESCRIPTION
    Kept so existing callers (scripts\sync-astra.ps1 [-Source] [-DryRun]) keep
    working. The mirror, the VENDORED.txt stamp and the dry-run live in
    scripts\sync-vendor.ps1.

.PARAMETER Source
    Path to the standalone Astra repo. Default D:\dev\starworks\Astra.

.PARAMETER DryRun
    List what would change and exit without writing anything.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\sync-astra.ps1
    powershell -ExecutionPolicy Bypass -File scripts\sync-astra.ps1 -DryRun
#>
[CmdletBinding()]
param(
    [string] $Source = 'D:\dev\starworks\Astra',
    [switch] $DryRun
)

$ErrorActionPreference = 'Stop'
& "$PSScriptRoot\sync-vendor.ps1" -Library Astra -Source $Source -DryRun:$DryRun
exit $LASTEXITCODE
