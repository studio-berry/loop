#Requires -Version 5.1
<#
.SYNOPSIS
    Runs the optional S21 canvas-hosting benchmark.

.DESCRIPTION
    The benchmark uses a synthetic color/input surface to compare the current
    Widgets baseline with QQuickWidget, WindowContainer, and a direct QQuickItem.
    It is an admission probe, not PDF-rendering fidelity proof.
#>
param(
    [string]$BuildDir = (Join-Path $PSScriptRoot "..\build"),
    [ValidateSet("all", "widget-baseline", "qquickwidget", "window-container", "quick-item")]
    [string]$Candidate = "all"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$resolvedBuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$executable = Get-ChildItem -LiteralPath $resolvedBuildDir -Recurse -File |
    Where-Object { $_.Name -eq "CanvasBenchmark.exe" -or $_.Name -eq "CanvasBenchmark" } |
    Sort-Object FullName |
    Select-Object -First 1

if (-not $executable) {
    throw "CanvasBenchmark executable was not found below $resolvedBuildDir. Configure with -DLOUPE_BUILD_CANVAS_BENCHMARK=ON and build the target first."
}

Write-Output "Running CanvasBenchmark candidate=$Candidate executable=$($executable.FullName)"
& $executable.FullName "--candidate=$Candidate"
$exitCode = $LASTEXITCODE
if ($exitCode -ne 0) {
    throw "CanvasBenchmark candidate=$Candidate failed with exit code $exitCode."
}
