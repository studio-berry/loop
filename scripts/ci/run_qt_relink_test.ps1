#Requires -Version 5.1
<#
.SYNOPSIS
    LGPL relink/replace evidence for a Windows MSI installed tree.

.DESCRIPTION
    Copies a shipped Qt6Core.dll through a byte-identical replacement and verifies
    LoopEditor still launches via --quick-smoke. Restores the original library
    before exit.

.PARAMETER InstallDir
    Installed LOOP directory (64-bit Program Files\LOOP).

.PARAMETER SourceSha
    Optional exact source SHA recorded in the transcript.

.PARAMETER OutputPath
    Optional transcript path.
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$InstallDir,
    [string]$SourceSha = "",
    [string]$OutputPath = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Write-Transcript {
    param([string]$Message)
    if ($OutputPath) {
        Add-Content -LiteralPath $OutputPath -Value $Message -Encoding UTF8
    }
    Write-Host $Message
}

# Accept both a binary directory and the MSI install root above usr/bin.
$binDir = $InstallDir
if (-not (Test-Path -LiteralPath (Join-Path $binDir "LoopEditor.exe")) -and
    (Test-Path -LiteralPath (Join-Path $binDir "usr/bin/LoopEditor.exe"))) {
    $binDir = Join-Path $binDir "usr/bin"
}
$editor = Join-Path $binDir "LoopEditor.exe"
if (-not (Test-Path -LiteralPath $editor)) {
    throw "LoopEditor not found under $InstallDir"
}

$qtCore = Get-ChildItem -LiteralPath $binDir -Filter "Qt6Core.dll" -Recurse -File | Select-Object -First 1
if (-not $qtCore) {
    throw "Qt6Core.dll not found under $InstallDir"
}

Write-Transcript "Qt relink test: install_dir=$InstallDir"
if ($SourceSha) {
    Write-Transcript "source_sha=$SourceSha"
}
Write-Transcript "target_library=$($qtCore.FullName)"

$backup = "$($qtCore.FullName).loop-relink-bak"
$replacement = "$($qtCore.FullName).loop-relink-replacement"
# This is a byte-identical copy/launch check, not proof of a rebuilt Qt library.
Write-Transcript "replacement_kind=byte-identical-copy"
$environmentNames = @(
    "PATH", "QT_QPA_PLATFORM", "QT_PLUGIN_PATH", "QML2_IMPORT_PATH",
    "QML_IMPORT_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "QTDIR", "QT_ROOT_DIR",
    "Qt6_DIR", "LOOP_QT_ROOT", "CMAKE_PREFIX_PATH", "CMAKE_TOOLCHAIN_FILE", "VCPKG_ROOT"
)
$savedEnvironment = @{}
foreach ($name in $environmentNames) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, "Process")
}
Copy-Item -LiteralPath $qtCore.FullName -Destination $backup -Force
try {
    Copy-Item -LiteralPath $backup -Destination $replacement -Force
    Copy-Item -LiteralPath $replacement -Destination $qtCore.FullName -Force
    $env:PATH = "$([Environment]::GetFolderPath('System'));$([Environment]::GetFolderPath('Windows'))"
    $env:QT_QPA_PLATFORM = if ($env:QT_QPA_PLATFORM) { $env:QT_QPA_PLATFORM } else { "offscreen" }
    foreach ($name in $environmentNames | Where-Object { $_ -notin @("PATH", "QT_QPA_PLATFORM") }) {
        [Environment]::SetEnvironmentVariable($name, $null, "Process")
    }
    $smokeOutput = & $editor --quick-smoke 2>&1
    $smokeExit = $LASTEXITCODE
} finally {
    try {
        Copy-Item -LiteralPath $backup -Destination $qtCore.FullName -Force
        Remove-Item -LiteralPath $backup, $replacement -Force -ErrorAction SilentlyContinue
    } finally {
        foreach ($name in $environmentNames) {
            [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], "Process")
        }
    }
}

if ($smokeExit -ne 0) {
    Write-Transcript "Qt relink test FAILED: LoopEditor --quick-smoke exit $smokeExit"
    Write-Transcript ($smokeOutput | Out-String)
    throw "Qt relink test failed"
}

Write-Transcript "Qt relink test PASSED: byte-identical Qt6Core copy still launches"
