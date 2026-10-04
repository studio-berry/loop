#Requires -Version 5.1
<#
.SYNOPSIS
    Qualifies the installed product against the native Windows accessibility
    backend using real UI Automation.

.DESCRIPTION
    The in-process smoke harness can only report the accessible tree; it cannot
    prove the operating-system accessibility bridge is active (QAccessible::isActive
    stays false until a real accessibility client attaches). This script drives
    tools/ProductQuickAccessibilitySmoke/inspect-windows-uia.ps1, which attaches
    a UI Automation client to the operator-path probe and reads the real
    accessibility tree, and it runs that probe from a staging copy of the
    INSTALLED tree so the native backend is exercised against the installed
    closure.

    The run fails closed unless the probe reports the native accessibility
    backend active on the native (non-software) graphics backend. The record it
    writes carries the native-accessibility claim, so a software-only smoke can
    never produce it.

    Windows-only: the UI Automation client, and a real window handle, are what
    make the claim.
#>
param(
    [string]$BuildDir = (Join-Path $PSScriptRoot "..\build"),
    [Parameter(Mandatory = $true)] [string]$InstallTree,
    [Parameter(Mandatory = $true)] [string]$OutputDirectory,
    [string]$StagingRoot = "",
    [string]$SourceSha = "",
    [string]$EvidenceJson = ""
)

. (Join-Path $PSScriptRoot "lib\loop-qt-runtime.ps1")
. (Join-Path $PSScriptRoot "lib\loop-quick-a11y-staging.ps1")
$null = Add-LoopQtRuntimeToPath

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$isWindowsHost = $IsWindows -or ($env:OS -match "Windows")
if (-not $isWindowsHost) {
    throw "Windows UI Automation is only available on Windows; the native accessibility qualification cannot run here."
}
if ($SourceSha -and $SourceSha -notmatch "^[0-9a-fA-F]{40}$") {
    throw "SourceSha must be a full 40-character Git SHA; got '$SourceSha'."
}

$driver = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\tools\ProductQuickAccessibilitySmoke\inspect-windows-uia.ps1")).Path

if (-not $StagingRoot) {
    $StagingRoot = Join-Path ([System.IO.Path]::GetTempPath()) "loop-quick-a11y-qualification"
}
$staged = New-LoopQuickA11yStagingTree -InstallTree $InstallTree -BuildDir $BuildDir -Stage (Join-Path $StagingRoot "uia-native")

# The driver refuses an existing output directory so a stale snapshot can never
# pass inspection; rebuild it from scratch on every run.
if (Test-Path -LiteralPath $OutputDirectory) {
    Remove-Item -LiteralPath $OutputDirectory -Recurse -Force
}

Write-Output "Running native UI Automation qualification executable=$($staged.Executable) staging_tree=$($staged.Stage)"
# The driver throws on any mismatch; with $ErrorActionPreference = 'Stop' a failed
# run aborts here. Do not read $LASTEXITCODE: the driver is a PowerShell script,
# not a native process, so a successful run leaves it unset (and StrictMode
# would reject the undefined read).
& $driver -Executable $staged.Executable -OutputDirectory $OutputDirectory

$reportPath = Join-Path $OutputDirectory "native-uia.json"
$stdoutPath = Join-Path $OutputDirectory "stdout.txt"
if (-not (Test-Path -LiteralPath $reportPath)) {
    throw "Native UI Automation did not write $reportPath."
}
$report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
if ($report.status -ne "pass") {
    throw "Native UI Automation report is not a pass: $($report.status)"
}
$observations = @($report.observations)

$stdout = if (Test-Path -LiteralPath $stdoutPath) { Get-Content -LiteralPath $stdoutPath -Raw } else { "" }
if ($stdout -notmatch "native_accessibility_active=1") {
    throw "The operator probe did not report the native accessibility backend active."
}

# The probe snapshots record the graphics backend it initialized; a native
# accessibility claim must not be made on the software rasterizer.
$graphicsApi = "absent"
foreach ($snapshotPath in (Get-ChildItem -LiteralPath $OutputDirectory -Filter "stage-*.json" | Sort-Object Name)) {
    $snapshot = Get-Content -LiteralPath $snapshotPath.FullName -Raw | ConvertFrom-Json
    if ($snapshot.native_accessibility_active -eq $true -and $snapshot.graphics_api) {
        $graphicsApi = [string]$snapshot.graphics_api
        break
    }
}
if ($graphicsApi -in @("software", "unknown", "null", "unrecognized", "absent")) {
    throw "Native UI Automation ran on a non-native graphics backend (graphics_api=$graphicsApi)."
}

if ($EvidenceJson) {
    $record = [ordered]@{
        schema_version = 1
        kind           = "loop-quick-accessibility-qualification"
        backend        = "native"
        claim          = "native-accessibility-backend"
        status         = "pass"
        source_sha     = if ($SourceSha) { $SourceSha.ToLowerInvariant() } else { $null }
        artifact       = [ordered]@{
            scope             = "installed-tree"
            run_root          = $staged.Stage
            install_tree      = $staged.InstallTree
            executable        = $staged.Executable
            executable_sha256 = (Get-FileHash -LiteralPath $staged.Executable -Algorithm SHA256).Hash.ToLowerInvariant()
            fixture_sha256    = $report.fixture_sha256
            driver_report     = $reportPath
        }
        observed       = [ordered]@{
            platform                        = [string]$report.platform
            native_accessibility_backend_active = $true
            graphics_api                    = $graphicsApi
            observation_count               = $observations.Count
        }
    }
    $evidenceParent = Split-Path -Parent $EvidenceJson
    if ($evidenceParent) { New-Item -ItemType Directory -Force -Path $evidenceParent | Out-Null }
    ($record | ConvertTo-Json -Depth 6) | Set-Content -LiteralPath $EvidenceJson -Encoding UTF8
    Write-Host "Wrote native accessibility qualification evidence: $EvidenceJson"
}

Write-Host "Native UI Automation qualification passed ($($observations.Count) observations, graphics_api=$graphicsApi)"
