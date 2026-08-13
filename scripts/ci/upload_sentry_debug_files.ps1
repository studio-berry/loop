# Upload Loupe PDBs to Sentry so crashpad minidumps can be symbolicated.
#
# No-ops when SENTRY_AUTH_TOKEN is unset (fork PRs, local runs without a token).
# Requires packaging-tools.json -> sentryCli and scripts/ci/download_verified.ps1.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDir
)

$ErrorActionPreference = 'Stop'

$token = [string]$env:SENTRY_AUTH_TOKEN
if ([string]::IsNullOrWhiteSpace($token)) {
    Write-Host "upload_sentry_debug_files.ps1: SENTRY_AUTH_TOKEN is unset; skipping."
    exit 0
}

if (-not (Test-Path -LiteralPath $BuildDir)) {
    throw "upload_sentry_debug_files.ps1: build directory not found: $BuildDir"
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$pinsPath = Join-Path $repoRoot ".github\pins\packaging-tools.json"
$pins = Get-Content -Raw $pinsPath | ConvertFrom-Json
$cli = $pins.sentryCli
if (-not $cli -or -not $cli.assetId -or -not $cli.sha256 -or -not $cli.upstream) {
    throw "upload_sentry_debug_files.ps1: packaging-tools.json is missing a complete sentryCli pin."
}

$org = if ($env:SENTRY_ORG) { $env:SENTRY_ORG } else { "berry-studios" }
$project = if ($env:SENTRY_PROJECT) { $env:SENTRY_PROJECT } else { "loupe-pdf" }
$url = if ($env:SENTRY_URL) { $env:SENTRY_URL } else { "https://de.sentry.io" }

$cliPath = Join-Path $env:RUNNER_TEMP "sentry-cli-Windows-x86_64.exe"
if ([string]::IsNullOrWhiteSpace($env:RUNNER_TEMP)) {
    $cliPath = Join-Path ([System.IO.Path]::GetTempPath()) "sentry-cli-Windows-x86_64.exe"
}

& (Join-Path $PSScriptRoot "download_verified.ps1") `
    -GhRepo $cli.upstream `
    -AssetId ([string]$cli.assetId) `
    -OutFile $cliPath `
    -Sha256 $cli.sha256

$pdbFilter = '^(Pdf4Qt|PdfTool|pdf)'
$pdbs = @(Get-ChildItem -LiteralPath $BuildDir -Recurse -Filter *.pdb -File -ErrorAction SilentlyContinue |
    Where-Object { $_.BaseName -match $pdbFilter })

if ($pdbs.Count -eq 0) {
    throw "upload_sentry_debug_files.ps1: no Loupe PDBs under $BuildDir. Release builds need /Zi when PDF4QT_ENABLE_SENTRY is on."
}

Write-Host "upload_sentry_debug_files.ps1: uploading $($pdbs.Count) PDB(s) to $org/$project ($url)"
$env:SENTRY_URL = $url
& $cliPath debug-files upload --org $org --project $project --wait @($pdbs.FullName)
if ($LASTEXITCODE -ne 0) {
    throw "upload_sentry_debug_files.ps1: sentry-cli debug-files upload failed with exit code $LASTEXITCODE."
}
