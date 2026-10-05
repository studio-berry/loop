#Requires -Version 5.1
<#
.SYNOPSIS
    Stages a copy of an installed Loop tree plus the Quick accessibility smoke
    binary so the harness runs against the installed closure.

.DESCRIPTION
    The product Quick accessibility smoke binary is a qualification harness and
    is deliberately not part of the shipped product surface (product-surface
    verification treats an extra first-party install artifact as drift). To
    qualify the INSTALLED tree without shipping the harness, stage a fresh copy
    of the installed tree and drop the harness into that copy's bin directory:
    on Windows the loader searches the executable's own directory first, so the
    staged LoopLibCore/LoopLibQuick and deployed Qt DLLs win over the build tree;
    on Linux the copied binary keeps its build RUNPATH, so the caller also puts
    the staged lib/bin directories on the loader path.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function New-LoopQuickA11yStagingTree
{
    param(
        [Parameter(Mandatory = $true)] [string]$InstallTree,
        [Parameter(Mandatory = $true)] [string]$BuildDir,
        [Parameter(Mandatory = $true)] [string]$Stage
    )

    $resolvedInstall = (Resolve-Path -LiteralPath $InstallTree).Path
    if (-not (Test-Path -LiteralPath $resolvedInstall)) {
        throw "Installed tree does not exist: $InstallTree"
    }

    $isWindowsHost = $IsWindows -or ($env:OS -match "Windows")
    $binaryName = if ($isWindowsHost) { "ProductQuickAccessibilitySmoke.exe" } else { "ProductQuickAccessibilitySmoke" }
    $resolvedBuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
    $harness = Get-ChildItem -LiteralPath $resolvedBuildDir -Recurse -File |
        Where-Object { $_.Name -eq $binaryName } |
        Sort-Object FullName |
        Select-Object -First 1
    if (-not $harness) {
        throw "ProductQuickAccessibilitySmoke binary not found below $resolvedBuildDir. Configure with -DLOOP_BUILD_PRODUCT_QUICK_ACCESSIBILITY_SMOKE=ON and build the target first."
    }

    if (Test-Path -LiteralPath $Stage) {
        Remove-Item -LiteralPath $Stage -Recurse -Force
    }
    # Copy to a destination that does not exist so the installed tree's contents
    # land directly in $Stage (no extra install/ level, matching the staging the
    # package workflows use).
    Copy-Item -LiteralPath $resolvedInstall -Destination $Stage -Recurse -Force

    $stageBin = Join-Path $Stage "usr/bin"
    if (-not (Test-Path -LiteralPath $stageBin)) {
        throw "Installed tree $resolvedInstall has no usr/bin directory; cannot stage the harness."
    }
    Copy-Item -LiteralPath $harness.FullName -Destination (Join-Path $stageBin $binaryName) -Force

    $stageLib = Join-Path $Stage "usr/lib"
    $loaderPaths = @($stageBin)
    if (Test-Path -LiteralPath $stageLib) { $loaderPaths = @($stageLib) + $loaderPaths }

    return [pscustomobject]@{
        Stage             = $Stage
        Executable        = (Join-Path $stageBin $binaryName)
        InstallTree       = $resolvedInstall
        LoaderPaths       = $loaderPaths
        IsWindowsHost     = $isWindowsHost
    }
}
