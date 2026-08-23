#Requires -Version 5.1
<#
.SYNOPSIS
    Runs the optional Qt Quick shell backend smoke harness.

.DESCRIPTION
    This script verifies scene-graph initialization, not merely process startup
    or QT_QPA_PLATFORM=offscreen availability. The harness reports the selected
    GraphicsApi so CI logs retain the backend evidence.
#>
param(
    [string]$BuildDir = (Join-Path $PSScriptRoot "..\build"),
    [ValidateSet("native", "software", "warp")]
    [string]$Mode = "native",
    [ValidateSet("d3d11", "opengl", "software")]
    [string]$ExpectedGraphicsApi = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$resolvedBuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$executable = Get-ChildItem -LiteralPath $resolvedBuildDir -Recurse -File |
    Where-Object { $_.Name -eq "QuickShellSmoke.exe" -or $_.Name -eq "QuickShellSmoke" } |
    Sort-Object FullName |
    Select-Object -First 1

if (-not $executable) {
    throw "QuickShellSmoke executable was not found below $resolvedBuildDir. Configure with -DLOUPE_BUILD_QUICK_SHELL_SMOKE=ON and build the target first."
}

switch ($Mode) {
    "native" {
        Remove-Item Env:QT_QUICK_BACKEND -ErrorAction SilentlyContinue
        Remove-Item Env:QSG_RHI_PREFER_SOFTWARE_RENDERER -ErrorAction SilentlyContinue
    }
    "software" {
        $env:QT_QUICK_BACKEND = "software"
        Remove-Item Env:QSG_RHI_PREFER_SOFTWARE_RENDERER -ErrorAction SilentlyContinue
    }
    "warp" {
        Remove-Item Env:QT_QUICK_BACKEND -ErrorAction SilentlyContinue
        $env:QSG_RHI_PREFER_SOFTWARE_RENDERER = "1"
    }
}

Write-Output "Running QuickShellSmoke mode=$Mode executable=$($executable.FullName)"
$smokeOutput = @(& $executable.FullName 2>&1)
$smokeOutput | ForEach-Object { Write-Output $_ }
$exitCode = $LASTEXITCODE
if ($exitCode -ne 0) {
    throw "QuickShellSmoke mode=$Mode failed with exit code $exitCode."
}

if ($ExpectedGraphicsApi) {
    $backendLine = $smokeOutput |
        Where-Object { $_.ToString() -match "graphics_api=([a-z0-9]+)" } |
        Select-Object -First 1
    if (-not $backendLine) {
        throw "QuickShellSmoke mode=$Mode did not report a selected graphics API."
    }
    if ($backendLine.ToString() -notmatch "graphics_api=$ExpectedGraphicsApi(?:\s|$)") {
        throw "QuickShellSmoke mode=$Mode selected an unexpected graphics API. Expected $ExpectedGraphicsApi; output: $backendLine"
    }
}
