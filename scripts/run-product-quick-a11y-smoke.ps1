#Requires -Version 5.1
param(
    [string]$BuildDir = (Join-Path $PSScriptRoot "..\build"),
    [ValidateSet("native", "software")]
    [string]$Backend = "native",
    [ValidateSet("d3d11", "d3d12", "opengl", "vulkan", "metal", "software", "native")]
    [string]$ExpectedGraphicsApi = ""
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

# Inherit QT_QPA_PLATFORM from the caller. Do not force offscreen: that selects
# Qt's software scene graph and cannot stand as native-graphics evidence.
if ($Backend -eq "software") {
    $env:QT_QUICK_BACKEND = "software"
    if (-not $ExpectedGraphicsApi) {
        $ExpectedGraphicsApi = "software"
    }
} else {
    Remove-Item Env:QT_QUICK_BACKEND -ErrorAction SilentlyContinue
    if ($ExpectedGraphicsApi -eq "software") {
        throw "ProductQuickAccessibilitySmoke backend=native cannot expect graphics_api=software."
    }
}

Write-Output "Running ProductQuickAccessibilitySmoke backend=$Backend executable=$($executable.FullName)"
$smokeOutput = @(& $executable.FullName 2>&1)
$smokeOutput | ForEach-Object { Write-Output $_ }
$exitCode = $LASTEXITCODE
if ($exitCode -ne 0) {
    throw "ProductQuickAccessibilitySmoke failed with exit code $exitCode (backend=$Backend)"
}

$backendLine = $smokeOutput |
    Where-Object { $_.ToString() -match "graphics_api=([a-z0-9]+)" } |
    Select-Object -First 1
if (-not $backendLine) {
    throw "ProductQuickAccessibilitySmoke backend=$Backend did not report a selected graphics API."
}
if ($backendLine.ToString() -notmatch "graphics_api=([a-z0-9]+)") {
    throw "ProductQuickAccessibilitySmoke backend=$Backend reported an unparseable graphics API: $backendLine"
}
$reportedApi = $Matches[1]
if ($ExpectedGraphicsApi) {
    $nativeApis = @("d3d11", "d3d12", "opengl", "vulkan", "metal")
    if ($ExpectedGraphicsApi -eq "native") {
        if ($reportedApi -notin $nativeApis) {
            throw "ProductQuickAccessibilitySmoke backend=native selected software/null graphics ($reportedApi). Native evidence cannot run through the software scene graph."
        }
    } else {
        if ($Backend -eq "native" -and $reportedApi -in @("software", "null", "unknown")) {
            throw "ProductQuickAccessibilitySmoke backend=native selected software/null graphics ($reportedApi). Native evidence cannot run through the software scene graph."
        }
        if ($reportedApi -ne $ExpectedGraphicsApi) {
            throw "ProductQuickAccessibilitySmoke backend=$Backend selected an unexpected graphics API. Expected $ExpectedGraphicsApi; output: $backendLine"
        }
    }
}

Write-Host "ProductQuickAccessibilitySmoke passed (backend=$Backend graphics_api=$reportedApi)"
