#Requires -Version 5.1
<#
.SYNOPSIS
    Runs the Phase 5 Widgets completeness contract from a PowerShell-capable host.

.DESCRIPTION
    The Python verifier owns the canonical deterministic join. This Windows twin
    keeps the validation entry point platform-appropriate while preserving one
    evidence and policy implementation.
#>
param(
    [string]$RepoRoot = (Join-Path $PSScriptRoot "..")
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path
$verifier = Join-Path $RepoRoot "scripts\verify_phase5_widgets_contract.py"
if (-not (Test-Path -LiteralPath $verifier -PathType Leaf)) {
    throw "Phase 5 Widgets verifier is missing: $verifier"
}

$pythonCandidates = @(
    (Join-Path $RepoRoot ".venv\Scripts\python.exe")
)
if ($env:LOCALAPPDATA) {
    $pythonRoot = Join-Path $env:LOCALAPPDATA "Programs\Python"
    $pythonCandidates += Get-ChildItem -LiteralPath $pythonRoot -Directory -Filter "Python*" -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName "python.exe" }
}
$pythonCommand = Get-Command python -ErrorAction SilentlyContinue
if ($pythonCommand) {
    $pythonCandidates += $pythonCommand.Path
}
$pythonPath = $pythonCandidates |
    Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
    Select-Object -First 1
if (-not $pythonPath) {
    throw "Python interpreter is required to run the Phase 5 Widgets verifier."
}

& $pythonPath $verifier --root $RepoRoot
if ($LASTEXITCODE -ne 0) {
    throw "Phase 5 Widgets contract failed with exit code $LASTEXITCODE"
}
