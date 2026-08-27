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
$python = Get-Command python -ErrorAction Stop
$verifier = Join-Path $RepoRoot "scripts\verify_phase5_widgets_contract.py"
if (-not (Test-Path -LiteralPath $verifier -PathType Leaf)) {
    throw "Phase 5 Widgets verifier is missing: $verifier"
}

& $python.Source $verifier --root $RepoRoot
if ($LASTEXITCODE -ne 0) {
    throw "Phase 5 Widgets contract failed with exit code $LASTEXITCODE"
}
