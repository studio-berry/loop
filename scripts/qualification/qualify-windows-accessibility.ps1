param(
    [Parameter(Mandatory = $true)] [string]$Package,
    [Parameter(Mandatory = $true)] [string]$Kit,
    [Parameter(Mandatory = $true)] [string]$SourceSha,
    [Parameter(Mandatory = $true)] [string]$EvidenceDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
python scripts/ci/quick_a11y_kit.py --source-sha $SourceSha --output $Kit
if ($LASTEXITCODE -ne 0) { throw 'Accessibility kit identity mismatch.' }
foreach ($name in @('QT_ROOT_DIR', 'LOOP_QT_ROOT', 'QT_PLUGIN_PATH', 'QML2_IMPORT_PATH', 'QML_IMPORT_PATH')) {
    Remove-Item "Env:$name" -ErrorAction SilentlyContinue
}
$installedTree = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'LOOP'
$installLog = Join-Path $EvidenceDirectory 'msi-install.txt'
$installer = Start-Process msiexec.exe -ArgumentList @('/i', ('"' + $Package + '"'), '/qn', '/norestart', '/l*v', ('"' + $installLog + '"')) -WindowStyle Hidden -Wait -PassThru
if ($installer.ExitCode -notin @(0, 3010)) { throw "MSI install failed: $($installer.ExitCode)" }
try {
    $boundary = Join-Path $EvidenceDirectory 'package-boundary.json'
    python scripts/ci/inspect_package_dependencies.py --platform windows --package $Package --source-sha $SourceSha --payload-root $installedTree --output $boundary
    if ($LASTEXITCODE -ne 0) { throw 'Installed package dependency inspection failed.' }
    $native = Join-Path $EvidenceDirectory 'native.json'
    $software = Join-Path $EvidenceDirectory 'software.json'
    $accessibility = Join-Path $EvidenceDirectory 'native-accessibility.json'
    ./scripts/run-product-quick-a11y-smoke.ps1 -BuildDir $Kit -InstallTree $installedTree -Backend native -Platform windows -SourceSha $SourceSha -Package $Package -EvidenceJson $native
    ./scripts/run-product-quick-a11y-smoke.ps1 -BuildDir $Kit -InstallTree $installedTree -Backend software -Platform windows -SourceSha $SourceSha -Package $Package -EvidenceJson $software
    ./scripts/run-installed-quick-a11y-uia.ps1 -BuildDir $Kit -InstallTree $installedTree -OutputDirectory (Join-Path $EvidenceDirectory 'uia') -SourceSha $SourceSha -Package $Package -EvidenceJson $accessibility
    python scripts/ci/verify_quick_accessibility_evidence.py --native $native --software $software --native-accessibility $accessibility --require-native-accessibility --source-sha $SourceSha --install-tree $installedTree --package-boundary $boundary
    if ($LASTEXITCODE -ne 0) { throw 'Installed native accessibility/package evidence did not qualify.' }
} finally {
    $uninstallLog = Join-Path $EvidenceDirectory 'msi-uninstall.txt'
    $uninstaller = Start-Process msiexec.exe -ArgumentList @('/x', ('"' + $Package + '"'), '/qn', '/norestart', '/l*v', ('"' + $uninstallLog + '"')) -WindowStyle Hidden -Wait -PassThru
    if ($uninstaller.ExitCode -notin @(0, 3010)) { throw "MSI uninstall failed: $($uninstaller.ExitCode)" }
}
