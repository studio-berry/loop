#Requires -Version 5.1
param(
    [string]$BuildDir = (Join-Path $PSScriptRoot "..\build"),
    [ValidateSet("native", "software")]
    [string]$Backend = "native"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$resolvedBuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$binaryName = if ($IsWindows -or $env:OS -match "Windows") { "ProductQuickAccessibilitySmoke.exe" } else { "ProductQuickAccessibilitySmoke" }
$executable = Get-ChildItem -LiteralPath $resolvedBuildDir -Recurse -File |
    Where-Object { $_.Name -eq $binaryName } |
    Sort-Object FullName |
    Select-Object -First 1

if (-not $executable) {
    throw "ProductQuickAccessibilitySmoke binary not found below $resolvedBuildDir. Configure with -DLOOP_BUILD_PRODUCT_QUICK_ACCESSIBILITY_SMOKE=ON and build the target first."
}

$env:QT_QPA_PLATFORM = "offscreen"
if ($Backend -eq "software") {
    $env:QT_QUICK_BACKEND = "software"
} else {
    Remove-Item Env:QT_QUICK_BACKEND -ErrorAction SilentlyContinue
}

Write-Output "Running ProductQuickAccessibilitySmoke backend=$Backend executable=$($executable.FullName)"
& $executable.FullName
if ($LASTEXITCODE -ne 0) {
    throw "ProductQuickAccessibilitySmoke failed with exit code $LASTEXITCODE (backend=$Backend)"
}

Write-Host "ProductQuickAccessibilitySmoke passed (backend=$Backend)"
