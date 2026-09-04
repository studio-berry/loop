#Requires -Version 5.1
<#
.SYNOPSIS
    Validates an installed Loop-PDF tree on a clean machine (MIC-301).

.DESCRIPTION
    Asserts the shipped layout resolves, runs PdfTool preflight against a fixture,
    optionally exercises the OCR sidecar, launches the Editor, and scans the tree
    for payloads the default bundle is prohibited from shipping.

    This validates an *already installed* tree. To drive the full MSI lifecycle
    (install -> smoke -> upgrade -> uninstall), use Invoke-MsiSmokeTest.ps1, which
    calls this script.

.PARAMETER InstallDir
    Directory containing LoopEditor.exe and PdfTool.exe.

.PARAMETER ProfilesDir
    Override for the preflight profiles directory. When omitted the script probes
    the layouts CMake can produce (see Resolve-ProfilesDir) and reports which one
    matched -- that resolution is itself a MIC-301 finding worth recording.

.PARAMETER SourceSha
    Optional full source SHA to record in the smoke transcript. Package workflows
    pass the required exact SHA; clean-VM runs should pass it as well.

.PARAMETER AllowOcrSidecar
    Permit the LoopOcrService bundle (which carries a Python runtime) to be
    present. docs/PACKAGING_LICENSING.md requires the *default* bundle to be
    C++/Qt only, so this is off by default and the scan fails when it is found.

.PARAMETER AllowOcrPlugin
    Permit OcrPlugin.dll to be present. V1 ships OCR as CLI-only (PdfTool ocr) --
    the Editor OCR UI plugin is not part of the V1 release surface (MIC-343), so
    this is off by default and the scan fails when it is found.
#>
param(
    [string]$InstallDir = "${env:ProgramFiles}\LOOP",
    [string]$ProfilesDir = "",
    [string]$TestPdf = "",
    [string]$SourceSha = "",
    [switch]$SkipEditorLaunch,
    [switch]$AllowOcrSidecar,
    [switch]$AllowOcrPlugin
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not [string]::IsNullOrWhiteSpace($SourceSha)) {
    if ($SourceSha -notmatch "^[0-9a-fA-F]{40}$") {
        throw "SourceSha must be a full 40-character Git SHA."
    }
    Write-Host "Package source SHA: $($SourceSha.ToLowerInvariant())"
}

function Assert-FileExists {
    param([string]$Path, [string]$Label)
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Missing $Label`: $Path"
    }
}

function Resolve-ProfilesDir {
    <#
        LOOP_PREFLIGHT_PROFILES_DIR is ${LOOP_INSTALL_SHARE_DIR}/loop/profiles
        (PdfTool/CMakeLists.txt). LOOP_INSTALL_TO_USR=ON -- used by the Windows
        CI and MSI builds -- prefixes that with usr/, so the share tree can sit
        beside the bin directory (staged .../install/usr/bin keeps profiles at
        .../install/usr/share) or below an install root (MSI InstallDir
        C:\Program Files\LOOP keeps profiles at ...\LOOP\usr\share).
        $InstallDir may name either the bin directory or the install root, so
        walk upward from it instead of assuming one depth. Probe rather than
        assume.
    #>
    param([string]$InstallDir)

    $candidates = @()
    $anchor = $InstallDir
    for ($depth = 0; $depth -lt 4; $depth++) {
        if ([string]::IsNullOrWhiteSpace($anchor)) { break }
        $candidates += (Join-Path $anchor "share\loop\profiles")
        $candidates += (Join-Path $anchor "usr\share\loop\profiles")
        # A drive root has no parent; stop the walk there.
        $anchor = Split-Path -Parent $anchor
    }

    # Pre-MIC-301 assumption, kept so an old layout still resolves.
    $candidates += (Join-Path $env:ProgramFiles "share\loop\profiles")

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath (Join-Path $candidate "loop-default.json")) {
            return $candidate
        }
    }

    throw ("Could not locate loop-default.json. Probed:`n  " + ($candidates -join "`n  ") +
           "`nIf the installer lays profiles down elsewhere, pass -ProfilesDir and update" +
           " docs/PLATFORM_SUPPORT.md to match what actually ships.")
}

function Test-ForbiddenPayload {
    <#
        docs/PACKAGING_LICENSING.md: the default bundle is C++/Qt only -- no Ghostscript,
        no JRE/JDK, no Python. This was a manual checklist item with nothing enforcing it.
    #>
    param([string[]]$Roots, [switch]$AllowOcr)

    $rules = @(
        @{ Label = "Ghostscript"; Patterns = @("gswin*.exe", "gsdll*.dll", "gs.exe") },
        @{ Label = "Java runtime"; Patterns = @("java.exe", "javaw.exe", "jvm.dll", "*.jar") },
        @{ Label = "Python runtime"; Patterns = @("python*.exe", "python3*.dll", "*.whl") },
        @{ Label = "Widgets-bound Qt"; Patterns = @(
            "Qt6Widgets.dll", "Qt6Widgets*.dll",
            "Qt6QuickWidgets.dll", "Qt6QuickWidgets*.dll",
            "Qt6PrintSupport.dll", "Qt6PrintSupport*.dll"
        ) }
    )

    # The installer lays files down in more than one place: binaries under
    # INSTALLFOLDER and the profile/schema tree under a sibling share\ directory.
    # Scanning only INSTALLFOLDER leaves a payload in share\ completely unchecked.
    $scanned = @()
    $violations = @()
    foreach ($root in $Roots) {
        if ([string]::IsNullOrWhiteSpace($root)) { continue }
        if (-not (Test-Path -LiteralPath $root)) { continue }
        $resolved = (Resolve-Path -LiteralPath $root).Path
        # Skip a root already covered by (or nested inside) one we scanned.
        $alreadyCovered = $false
        foreach ($seen in $scanned) {
            if ($resolved -eq $seen -or $resolved.StartsWith($seen + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
                $alreadyCovered = $true
                break
            }
        }
        if ($alreadyCovered) { continue }
        $scanned += $resolved

        foreach ($rule in $rules) {
            foreach ($pattern in $rule.Patterns) {
                $hits = @(Get-ChildItem -LiteralPath $resolved -Filter $pattern -Recurse -File -ErrorAction SilentlyContinue)
                foreach ($hit in $hits) {
                    $isOcrSidecar = $hit.FullName -like "*\LoopOcrService\*"
                    if ($isOcrSidecar -and $AllowOcr) {
                        continue
                    }
                    $violations += "$($rule.Label): $($hit.FullName)"
                }
            }
        }
    }

    if ($scanned.Count -eq 0) {
        throw "Bundle policy scan found no directories to scan. Checked: $($Roots -join ', ')"
    }

    if ($violations.Count -gt 0) {
        $message = "Forbidden payload found in the installed tree (docs/PACKAGING_LICENSING.md):`n  " +
                   ($violations -join "`n  ")
        if (-not $AllowOcr) {
            $message += "`nIf these come from an intentional LoopOcrService bundle, re-run with -AllowOcrSidecar."
        }
        throw $message
    }

    Write-Host "OK: no Ghostscript / JRE / Python / Widgets-bound Qt payload in the default bundle (scanned: $($scanned -join ', '))"
}

# -InstallDir may name the directory holding the executables (staged tree
# .../install/usr/bin) or the install root above it (MSI C:\Program Files\LOOP
# with binaries at ...\LOOP\usr\bin). Normalize to the directory that actually
# holds the executables so every binary-relative path below resolves the same
# way for both spellings; a missing executable in both places stays a loud
# failure in Assert-FileExists rather than silently probing the wrong tree.
$binDir = $InstallDir
if (-not (Test-Path -LiteralPath (Join-Path $binDir "LoopEditor.exe")) -and
    (Test-Path -LiteralPath (Join-Path $binDir "usr\bin\LoopEditor.exe"))) {
    $binDir = Join-Path $binDir "usr\bin"
    Write-Host "Resolved product binaries to $binDir"
}

$pluginsDir = Join-Path $binDir "pdfplugins"

if ([string]::IsNullOrWhiteSpace($ProfilesDir)) {
    $ProfilesDir = Resolve-ProfilesDir -InstallDir $InstallDir
}
Write-Host "Smoke-testing install at $InstallDir"
Write-Host "Resolved preflight profiles to $ProfilesDir"

$requiredFiles = @(
    @{ Path = (Join-Path $binDir "LoopEditor.exe"); Label = "Editor" },
    @{ Path = (Join-Path $binDir "PdfTool.exe"); Label = "PdfTool" },
    @{ Path = (Join-Path $ProfilesDir "loop-default.json"); Label = "Default preflight profile" },
    @{ Path = (Join-Path $ProfilesDir "schemas\profile.schema.json"); Label = "Profile schema" },
    @{ Path = (Join-Path $ProfilesDir "schemas\report.schema.json"); Label = "Report schema" }
)

foreach ($item in $requiredFiles) {
    Assert-FileExists -Path $item.Path -Label $item.Label
    Write-Host "OK: $($item.Label)"
}

$pdfTool = Join-Path $binDir "PdfTool.exe"

$versionOutput = @(& $pdfTool --version 2>&1)
$versionExit = $LASTEXITCODE
$versionText = $versionOutput -join "`n"
if ($versionExit -ne 0 -or $versionText -notmatch '\b\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?\b') {
    throw "PdfTool version probe failed with exit code $versionExit`: $versionText"
}
Write-Host "OK: PdfTool reports a canonical semantic version"

$capabilitiesOutput = @(& $pdfTool capabilities --console-format json 2>&1)
$capabilitiesExit = $LASTEXITCODE
$capabilitiesText = $capabilitiesOutput -join "`n"
if ($capabilitiesExit -ne 0) {
    throw "PdfTool capabilities probe failed with exit code $capabilitiesExit`: $capabilitiesText"
}
try {
    $capabilities = $capabilitiesText | ConvertFrom-Json
} catch {
    throw "PdfTool capabilities probe returned invalid JSON: $capabilitiesText"
}
if ($capabilities.data.product.name -ne "PdfTool" -or
    $capabilities.data.product.version -ne $capabilities.version -or
    [string]::IsNullOrWhiteSpace($capabilities.version)) {
    throw "PdfTool capabilities reported an invalid product identity: $capabilitiesText"
}
Write-Host "OK: PdfTool capabilities report the Loop PdfTool identity"

$legacyEditor = Join-Path $binDir ("Lo" + "upeEditor.exe")
if (Test-Path -LiteralPath $legacyEditor) {
    throw "Legacy editor executable still present in the install: $legacyEditor"
}
Write-Host "OK: legacy editor executable absent"

# V1 ships OCR as CLI-only (PdfTool ocr); the Editor OCR UI plugin is not part of
# the V1 release surface (MIC-343). Its presence in a release bundle is packaging
# drift, not an optional extra -- fail loudly rather than silently reporting it.
$ocrPlugin = Join-Path $pluginsDir "OcrPlugin.dll"
if (Test-Path -LiteralPath $ocrPlugin) {
    if ($AllowOcrPlugin) {
        Write-Host "OK: OcrPlugin.dll present (explicitly allowed via -AllowOcrPlugin)"
    } else {
        $ocrPluginMessage = "OcrPlugin.dll found at $ocrPlugin. V1 ships OCR as CLI-only (MIC-343) -- " +
            "this plugin must not be in a release bundle. Build with -DLOOP_PLUGIN_OCR=OFF, " +
            "or re-run with -AllowOcrPlugin if this is an intentional non-V1 build."
        throw $ocrPluginMessage
    }
} else {
    Write-Host "OK: OcrPlugin.dll absent (V1 CLI-only OCR surface, MIC-343)"
}

if ([string]::IsNullOrWhiteSpace($TestPdf)) {
    $repoRoot = Split-Path -Parent $PSScriptRoot
    $candidate = Join-Path $repoRoot "loop-preflight\testdata\fixtures\bleed-adequate.pdf"
    if (Test-Path -LiteralPath $candidate) {
        $TestPdf = $candidate
    }
}

if ([string]::IsNullOrWhiteSpace($TestPdf) -or -not (Test-Path -LiteralPath $TestPdf)) {
    throw "Test PDF not found. Pass -TestPdf pointing at a sample document."
}

$profilePath = Join-Path $ProfilesDir "loop-default.json"
$editor = Join-Path $binDir "LoopEditor.exe"
# Strip Qt from PATH so preflight cannot silently resolve ICU/Qt deps from a
# developer or CI toolchain install — the bundle must be self-contained (MIC-301).
$qtRoots = @($env:QT_ROOT_DIR, $env:Qt6_DIR, $env:LOOP_QT_ROOT) |
    Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
$pathSeparator = [IO.Path]::PathSeparator
$pathParts = @($env:PATH -split [regex]::Escape([string]$pathSeparator) | Where-Object {
    $part = $_
    if ([string]::IsNullOrWhiteSpace($part)) { return $false }
    foreach ($root in $qtRoots) {
        if ($part.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) { return $false }
    }
    if ($part -match '[\\/]Qt[\\/].*[\\/]bin' ) { return $false }
    return $true
})
$savedPath = $env:PATH
$savedPluginPath = $env:QT_PLUGIN_PATH
$savedQmlPath = $env:QML2_IMPORT_PATH
$savedQmlImportPath = $env:QML_IMPORT_PATH
$savedQpaPluginPath = $env:QT_QPA_PLATFORM_PLUGIN_PATH
$savedQtdir = $env:QTDIR
$savedQtRootDir = $env:QT_ROOT_DIR
$savedQt6Dir = $env:Qt6_DIR
$savedLoopQtRoot = $env:LOOP_QT_ROOT
$savedQuickBackend = $env:QT_QUICK_BACKEND
$env:PATH = ($pathParts -join $pathSeparator)
Remove-Item Env:QT_PLUGIN_PATH -ErrorAction SilentlyContinue
Remove-Item Env:QML2_IMPORT_PATH -ErrorAction SilentlyContinue
Remove-Item Env:QML_IMPORT_PATH -ErrorAction SilentlyContinue
Remove-Item Env:QT_QPA_PLATFORM_PLUGIN_PATH -ErrorAction SilentlyContinue
Remove-Item Env:QTDIR -ErrorAction SilentlyContinue
Remove-Item Env:QT_ROOT_DIR -ErrorAction SilentlyContinue
Remove-Item Env:Qt6_DIR -ErrorAction SilentlyContinue
Remove-Item Env:LOOP_QT_ROOT -ErrorAction SilentlyContinue
try {
    $preflightOutput = & $pdfTool preflight $TestPdf --profile $profilePath --console-format json 2>&1
    $preflightExit = $LASTEXITCODE
    Remove-Item Env:QT_QUICK_BACKEND -ErrorAction SilentlyContinue
    $nativeOutput = @(& $editor --quick-smoke 2>&1)
    $nativeExit = $LASTEXITCODE
    if ($nativeExit -ne 0) {
        throw "LoopEditor native Quick startup failed with exit code $($nativeExit): $nativeOutput"
    }
    Write-Host "OK: LoopEditor native Quick startup"
    $env:QT_QUICK_BACKEND = "software"
    $softwareOutput = @(& $editor --quick-smoke 2>&1)
    $softwareExit = $LASTEXITCODE
    if ($softwareExit -ne 0) {
        throw "LoopEditor software Quick startup failed with exit code $($softwareExit): $softwareOutput"
    }
    Write-Host "OK: LoopEditor software Quick startup"
} finally {
    $env:PATH = $savedPath
    if ($null -ne $savedPluginPath) { $env:QT_PLUGIN_PATH = $savedPluginPath } else { Remove-Item Env:QT_PLUGIN_PATH -ErrorAction SilentlyContinue }
    if ($null -ne $savedQmlPath) { $env:QML2_IMPORT_PATH = $savedQmlPath } else { Remove-Item Env:QML2_IMPORT_PATH -ErrorAction SilentlyContinue }
    if ($null -ne $savedQmlImportPath) { $env:QML_IMPORT_PATH = $savedQmlImportPath } else { Remove-Item Env:QML_IMPORT_PATH -ErrorAction SilentlyContinue }
    if ($null -ne $savedQpaPluginPath) { $env:QT_QPA_PLATFORM_PLUGIN_PATH = $savedQpaPluginPath } else { Remove-Item Env:QT_QPA_PLATFORM_PLUGIN_PATH -ErrorAction SilentlyContinue }
    if ($null -ne $savedQtdir) { $env:QTDIR = $savedQtdir } else { Remove-Item Env:QTDIR -ErrorAction SilentlyContinue }
    if ($null -ne $savedQtRootDir) { $env:QT_ROOT_DIR = $savedQtRootDir } else { Remove-Item Env:QT_ROOT_DIR -ErrorAction SilentlyContinue }
    if ($null -ne $savedQt6Dir) { $env:Qt6_DIR = $savedQt6Dir } else { Remove-Item Env:Qt6_DIR -ErrorAction SilentlyContinue }
    if ($null -ne $savedLoopQtRoot) { $env:LOOP_QT_ROOT = $savedLoopQtRoot } else { Remove-Item Env:LOOP_QT_ROOT -ErrorAction SilentlyContinue }
    if ($null -ne $savedQuickBackend) { $env:QT_QUICK_BACKEND = $savedQuickBackend } else { Remove-Item Env:QT_QUICK_BACKEND -ErrorAction SilentlyContinue }
}
if ($preflightExit -ne 0 -and $preflightExit -ne 1) {
    throw "PdfTool preflight failed with unexpected exit code $preflightExit`: $preflightOutput"
}
Write-Host "OK: PdfTool preflight completed (exit $preflightExit)"

$ocrSidecar = Join-Path $binDir "LoopOcrService\LoopOcrService.exe"
if (Test-Path -LiteralPath $ocrSidecar) {
    $repoRoot = Split-Path -Parent $PSScriptRoot
    $mockSidecar = Join-Path $repoRoot "loop-ocr\tools\mock_ocr_sidecar.cmd"
    $scanFixture = Join-Path $repoRoot "loop-preflight\testdata\fixtures\image-dpi-low.pdf"
    if ((Test-Path -LiteralPath $mockSidecar) -and (Test-Path -LiteralPath $scanFixture)) {
        $ocrOutput = & $pdfTool ocr $scanFixture --console-format json --sidecar $mockSidecar 2>&1
        $ocrExit = $LASTEXITCODE
        if ($ocrExit -ne 0 -and $ocrExit -ne 1) {
            throw "PdfTool ocr failed with unexpected exit code $ocrExit`: $ocrOutput"
        }
        Write-Host "OK: PdfTool ocr completed with mock sidecar (exit $ocrExit)"
    }
    Write-Host "OK: LoopOcrService bundle present"
}

# Run the bundle-policy gate before the editor launch: it is a packaging
# assertion that does not depend on the GUI, and running it last meant any
# earlier failure silently skipped it entirely.
# Scan the binaries and the sibling share\ tree the installer also writes.
$shareRoot = $ProfilesDir
for ($i = 0; $i -lt 2; $i++) {
    $parentCandidate = Split-Path -Parent $shareRoot
    if ([string]::IsNullOrWhiteSpace($parentCandidate)) { break }
    $shareRoot = $parentCandidate
}
Test-ForbiddenPayload -Roots @($InstallDir, $shareRoot) -AllowOcr:$AllowOcrSidecar

if (-not $SkipEditorLaunch) {
    $editor = Join-Path $binDir "LoopEditor.exe"
    $editorProcess = Start-Process -FilePath $editor -ArgumentList @($TestPdf) -PassThru
    Start-Sleep -Seconds 5
    if ($editorProcess.HasExited) {
        throw "LoopEditor exited early with code $($editorProcess.ExitCode)"
    }
    Stop-Process -Id $editorProcess.Id -Force
    Write-Host "OK: LoopEditor launched without immediate crash"
}

Write-Host "Smoke test passed."
exit 0
