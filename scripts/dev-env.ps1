#Requires -Version 5.1
# Loop PDF development environment variables (Windows), the counterpart of
# scripts/dev-env.sh.
#
# Usage (Windows PowerShell or pwsh):
#   . .\scripts\dev-env.ps1
#
# Optional overrides before dot-sourcing:
#   $env:LOOP_QT_VERSION = "6.11.1"
#   $env:LOOP_QT_INSTALL_DIR = "C:\Qt-aqt"
#   $env:LOOP_BUILD_DIR = "C:\path\to\build-local"
#
# Why this exists: Loop's Qt runtime comes from LOOP_QT_ROOT, not from vcpkg, so
# no Qt DLL sits beside the build-tree executables. Without LOOP_QT_ROOT\bin on
# PATH, Windows stops in the loader with "Qt6Core.dll was not found" (and
# "Qt6Quick.dll was not found" for the editor/Quick binaries) instead of running
# the test. Exported: LOOP_REPO_ROOT, LOOP_QT_VERSION, LOOP_QT_ROOT, QT_ROOT_DIR,
# CMAKE_PREFIX_PATH, LOOP_BUILD_DIR, PATH, QT_QPA_PLATFORM.

[CmdletBinding()]
param()

# Two facts about dot-sourcing, both verified in Windows PowerShell 5.1: a
# dot-sourced script sees InvocationName ".", a `-File` run sees the script path,
# and an "&" call sees "&". No session options are set here (no Set-StrictMode, no
# $ErrorActionPreference): this file is dot-sourced, so either one would leak into
# the caller's session.
if ($MyInvocation.InvocationName -ne ".") {
    Write-Error "Dot-source this file instead of executing it:  . .\scripts\dev-env.ps1"
    exit 1
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$env:LOOP_REPO_ROOT = if ($env:LOOP_REPO_ROOT) { $env:LOOP_REPO_ROOT } else { $repoRoot }

$env:LOOP_QT_VERSION = if ($env:LOOP_QT_VERSION) { $env:LOOP_QT_VERSION } else { "6.11.1" }
$env:LOOP_QT_INSTALL_DIR = if ($env:LOOP_QT_INSTALL_DIR) { $env:LOOP_QT_INSTALL_DIR } else { "C:\Qt-aqt" }
$env:LOOP_QT_ROOT = if ($env:LOOP_QT_ROOT) {
    $env:LOOP_QT_ROOT
} else {
    Join-Path (Join-Path $env:LOOP_QT_INSTALL_DIR $env:LOOP_QT_VERSION) "msvc2022_64"
}
$env:QT_ROOT_DIR = $env:LOOP_QT_ROOT
$env:CMAKE_PREFIX_PATH = $env:LOOP_QT_ROOT

$env:LOOP_BUILD_DIR = if ($env:LOOP_BUILD_DIR) { $env:LOOP_BUILD_DIR } else { Join-Path $env:LOOP_REPO_ROOT "build-local" }
$env:QT_QPA_PLATFORM = if ($env:QT_QPA_PLATFORM) { $env:QT_QPA_PLATFORM } else { "offscreen" }

. (Join-Path $PSScriptRoot "lib\loop-qt-runtime.ps1")
$null = Add-LoopQtRuntimeToPath

# Run build-tree executables by name, the way scripts/dev-env.sh puts the build
# directory on PATH.
$env:PATH = "$($env:LOOP_BUILD_DIR)\usr\bin;$env:PATH"

Write-Host "LOOP_QT_ROOT=$($env:LOOP_QT_ROOT)"
Write-Host "LOOP_BUILD_DIR=$($env:LOOP_BUILD_DIR)"

Remove-Variable repoRoot
