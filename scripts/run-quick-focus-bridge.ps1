#Requires -Version 5.1
<#
.SYNOPSIS
    Runs the qualification-only QWidget/Qt Quick focus and accessibility probe.

.DESCRIPTION
    This probe exercises a real QQuickWidget boundary inside a QWidget host.
    It is not product QML and it does not authorize shipping Qt Quick runtime
    files. A non-pass result is fatal so missing bridge evidence cannot be
    mistaken for a successful process startup.
#>
param(
    [string]$BuildDir = (Join-Path $PSScriptRoot "..\build"),
    [ValidateSet("native", "software")]
    [string]$Mode = "native"
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

switch ($Mode) {
    "native" {
        Remove-Item Env:QT_QUICK_BACKEND -ErrorAction SilentlyContinue
    }
    "software" {
        $env:QT_QUICK_BACKEND = "software"
    }
}

Write-Output "Running QWidget/Qt Quick focus bridge mode=$Mode executable=$($executable.FullName)"
& $executable.FullName "--focus-bridge"
$exitCode = $LASTEXITCODE
if ($exitCode -ne 0) {
    throw "QWidget/Qt Quick focus bridge mode=$Mode failed with exit code $exitCode."
}
