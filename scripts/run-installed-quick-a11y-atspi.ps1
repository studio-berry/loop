param(
    [Parameter(Mandatory = $true)] [string]$BuildDir,
    [Parameter(Mandatory = $true)] [string]$InstallTree,
    [Parameter(Mandatory = $true)] [string]$OutputDirectory,
    [Parameter(Mandatory = $true)] [string]$SourceSha,
    [Parameter(Mandatory = $true)] [string]$Package,
    [Parameter(Mandatory = $true)] [string]$EvidenceJson
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $IsLinux) { throw 'Native AT-SPI qualification requires Linux.' }
if ($SourceSha -notmatch '^[0-9a-f]{40}$') { throw 'SourceSha must be a full lowercase Git commit.' }
. (Join-Path $PSScriptRoot 'lib/loop-quick-a11y-staging.ps1')
$stage = Join-Path ([IO.Path]::GetTempPath()) ('loop-atspi-' + [Guid]::NewGuid().ToString())
$staged = New-LoopQuickA11yStagingTree -InstallTree $InstallTree -BuildDir $BuildDir -Stage $stage
$env:LD_LIBRARY_PATH = $staged.LoaderPaths -join ':'
$driver = Join-Path $PSScriptRoot '../tools/ProductQuickAccessibilitySmoke/inspect-linux-atspi.py'
& /usr/bin/python3 $driver --executable $staged.Executable --output-directory $OutputDirectory
if ($LASTEXITCODE -ne 0) { throw 'Native AT-SPI inspection failed.' }
$reportPath = Join-Path $OutputDirectory 'native-atspi.json'
$report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
if ($report.status -ne 'pass') { throw 'Native AT-SPI report did not pass.' }
$record = [ordered]@{
    schema_version = 1
    kind = 'loop-quick-accessibility-qualification'
    backend = 'native'
    claim = 'native-accessibility-backend'
    status = 'pass'
    source_sha = $SourceSha
    artifact = [ordered]@{
        scope = 'installed-tree'
        run_root = $staged.Stage
        install_tree = $staged.InstallTree
        executable = $staged.Executable
        executable_sha256 = $report.executable_sha256
        fixture_sha256 = $report.fixture_sha256
        driver_report = $reportPath
        package = (Resolve-Path -LiteralPath $Package).Path
        package_sha256 = (Get-FileHash -LiteralPath $Package -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    observed = [ordered]@{
        platform = $report.platform
        native_accessibility_backend_active = $true
        graphics_api = $report.graphics_api
        observation_count = @($report.observations).Count
    }
}
($record | ConvertTo-Json -Depth 6) | Set-Content -LiteralPath $EvidenceJson -Encoding utf8
