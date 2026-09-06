#Requires -Version 5.1
<#
.SYNOPSIS
    LGPL relink/replace evidence for a Windows MSI installed tree.

.DESCRIPTION
    Replaces a shipped Qt6Core.dll with a recipient-controlled copy and verifies
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

$editor = Join-Path $InstallDir "LoopEditor.exe"
if (-not (Test-Path -LiteralPath $editor)) {
    throw "LoopEditor not found under $InstallDir"
}

$qtCore = Get-ChildItem -LiteralPath $InstallDir -Filter "Qt6Core.dll" -Recurse -File | Select-Object -First 1
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
Copy-Item -LiteralPath $qtCore.FullName -Destination $backup -Force
Copy-Item -LiteralPath $backup -Destination $replacement -Force
Copy-Item -LiteralPath $replacement -Destination $qtCore.FullName -Force

$env:PATH = "$([Environment]::GetFolderPath('System'));$([Environment]::GetFolderPath('Windows'))"
$env:QT_QPA_PLATFORM = if ($env:QT_QPA_PLATFORM) { $env:QT_QPA_PLATFORM } else { "offscreen" }
Remove-Item Env:QT_PLUGIN_PATH -ErrorAction SilentlyContinue
Remove-Item Env:QML2_IMPORT_PATH -ErrorAction SilentlyContinue
Remove-Item Env:QML_IMPORT_PATH -ErrorAction SilentlyContinue
Remove-Item Env:QT_QPA_PLATFORM_PLUGIN_PATH -ErrorAction SilentlyContinue
Remove-Item Env:QTDIR -ErrorAction SilentlyContinue
Remove-Item Env:Qt6_DIR -ErrorAction SilentlyContinue
Remove-Item Env:LOOP_QT_ROOT -ErrorAction SilentlyContinue

$smokeOutput = & $editor --quick-smoke 2>&1
$smokeExit = $LASTEXITCODE

Copy-Item -LiteralPath $backup -Destination $qtCore.FullName -Force
Remove-Item -LiteralPath $backup, $replacement -Force -ErrorAction SilentlyContinue

if ($smokeExit -ne 0) {
    Write-Transcript "Qt relink test FAILED: LoopEditor --quick-smoke exit $smokeExit"
    Write-Transcript ($smokeOutput | Out-String)
    throw "Qt relink test failed"
}

Write-Transcript "Qt relink test PASSED: recipient-controlled Qt6Core replacement still launches"
