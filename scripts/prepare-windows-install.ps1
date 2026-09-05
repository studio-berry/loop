#Requires -Version 5.1
<#
.SYNOPSIS
    Prune the Windows install tree before MSI packaging.

.DESCRIPTION
    The Qt deploy script only needs QSQLITE for preflight history writes. Optional
    vendor SQL drivers reference libraries we do not ship and fail package-boundary
    inspection (qsqlibase.dll -> fbclient.dll, qsqloci.dll -> OCI.dll).
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$InstallDir
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $InstallDir -PathType Container)) {
    throw "Install directory not found: $InstallDir"
}

$sqlDir = Join-Path $InstallDir "plugins\sqldrivers"
if (Test-Path -LiteralPath $sqlDir -PathType Container) {
    Get-ChildItem -LiteralPath $sqlDir -File |
        Where-Object { $_.Name -ne "qsqlite.dll" } |
        Remove-Item -Force
}

Write-Host "Prepared Windows install tree under $InstallDir"
