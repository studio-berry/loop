param(
    [ValidateSet("native", "software")]
    [string]$Backend = "native"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$binaryName = if ($IsWindows -or $env:OS -match "Windows") { "ProductQuickAccessibilitySmoke.exe" } else { "ProductQuickAccessibilitySmoke" }
$binaryPath = Join-Path $repoRoot "build/bin/$binaryName"

if (-not (Test-Path $binaryPath)) {
    $binaryPath = Join-Path $repoRoot "build/$binaryName"
}

if (-not (Test-Path $binaryPath)) {
    throw "ProductQuickAccessibilitySmoke binary not found at $binaryPath"
}

$env:QT_QPA_PLATFORM = "offscreen"
if ($Backend -eq "software") {
    $env:QT_QUICK_BACKEND = "software"
} else {
    Remove-Item Env:QT_QUICK_BACKEND -ErrorAction SilentlyContinue
}

& $binaryPath
if ($LASTEXITCODE -ne 0) {
    throw "ProductQuickAccessibilitySmoke failed with exit code $LASTEXITCODE (backend=$Backend)"
}

Write-Host "ProductQuickAccessibilitySmoke passed (backend=$Backend)"
