#Requires -Version 5.1
<#
.SYNOPSIS
    Verifies an installed Loupe product profile against docs/product-surface.json.

.DESCRIPTION
    This compatibility entry point delegates all inventory and contract logic to
    scripts/verify_product_surface.py so PowerShell and Python cannot maintain
    separate product lists.
#>
param(
    [string]$InstallDir = "",
    [ValidateSet("developer", "loupe-release")]
    [string]$Profile = "loupe-release",
    [string]$ManifestPath = "",
    [string]$BuildDir = "",
    [string]$InstallManifestPath = "",
    [string]$PdfToolPath = "",
    [string]$DiscoveryJson = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$python = $null
$pythonArgs = @()
foreach ($candidateName in @("python", "python3", "py")) {
    $candidate = Get-Command $candidateName -ErrorAction SilentlyContinue
    if ($null -eq $candidate) {
        continue
    }
    $probeArgs = if ($candidateName -eq "py") { @("-3", "--version") } else { @("--version") }
    & $candidate.Source @probeArgs *> $null
    if ($LASTEXITCODE -eq 0) {
        $python = $candidate
        if ($candidateName -eq "py") {
            $pythonArgs = @("-3")
        }
        break
    }
}
if ($null -eq $python) {
    throw "Python 3 is required to verify the Loupe product surface."
}

$arguments = @(
    (Join-Path $repoRoot "scripts\verify_product_surface.py"),
    "--root", $repoRoot,
    "--profile", $Profile
)
if ($ManifestPath) { $arguments += @("--manifest-path", $ManifestPath) }
if ($BuildDir) { $arguments += @("--build-dir", $BuildDir) }
if ($InstallDir) { $arguments += @("--install-dir", $InstallDir) }
if ($InstallManifestPath) { $arguments += @("--install-manifest", $InstallManifestPath) }
if ($PdfToolPath) { $arguments += @("--pdf-tool", $PdfToolPath) }
if ($DiscoveryJson) { $arguments += @("--discovery-json", $DiscoveryJson) }

$commandArguments = @($pythonArgs) + @($arguments)
& $python.Source @commandArguments
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
