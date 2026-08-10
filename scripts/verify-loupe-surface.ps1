#Requires -Version 5.1
<#
.SYNOPSIS
    Verifies the installed Loupe product surface.

.DESCRIPTION
    Checks that the release-profile install contains the supported Loupe
    binaries and retained production plugins, while ensuring compatibility
    applications are not exposed as separate product entrypoints.

    This is an artifact-level check. It does not build, launch, or validate the
    runtime behavior of the binaries.

.PARAMETER InstallDir
    Directory containing the staged install tree. Defaults to the current
    directory.
#>
param(
    [string]$InstallDir = (Get-Location).Path
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $InstallDir -PathType Container)) {
    throw "Install directory does not exist: $InstallDir"
}

$files = @(Get-ChildItem -LiteralPath $InstallDir -Recurse -File)

function Find-Artifact {
    param([string]$Name)
    return @($files | Where-Object { $_.BaseName -eq $Name -or $_.Name -eq $Name })
}

function Assert-Present {
    param([string]$Name)
    $matches = @(Find-Artifact $Name)
    if ($matches.Count -eq 0) {
        throw "Missing required Loupe artifact: $Name"
    }
}

function Assert-Absent {
    param([string]$Name)
    $matches = @(Find-Artifact $Name)
    if ($matches.Count -gt 0) {
        $paths = ($matches | ForEach-Object FullName) -join ", "
        throw "Forbidden inherited artifact present: $Name ($paths)"
    }
}

Assert-Present "Pdf4QtEditor"
Assert-Present "PdfTool"

$pdfToolArtifact = @(Find-Artifact "PdfTool") | Select-Object -First 1
if ($null -eq $pdfToolArtifact) {
    throw "Could not resolve the installed PdfTool executable."
}
$pdfTool = $pdfToolArtifact.FullName
$capabilityOutput = @(& $pdfTool capabilities --command ocr --console-format json 2>$null)
$capabilityExit = $LASTEXITCODE
if ($capabilityExit -ne 0) {
    throw "PdfTool capabilities --command ocr failed with exit code $capabilityExit"
}

try {
    $capabilities = ($capabilityOutput -join "`n") | ConvertFrom-Json
} catch {
    throw "PdfTool OCR capability discovery did not return valid JSON: $($_.Exception.Message)"
}

if ($capabilities.status -ne "success" -or $capabilities.exit_code -ne 0) {
    throw "PdfTool OCR capability discovery returned an unsuccessful result."
}

$ocrCommands = @($capabilities.data.commands | Where-Object { $_.id -eq "ocr" })
if ($ocrCommands.Count -ne 1 -or -not ($capabilities.data.build_capabilities -contains "ocr")) {
    throw "Release PdfTool does not advertise the CLI-only ocr command."
}
Write-Output "OK: PdfTool ocr is available in the release surface"

foreach ($name in @("DimensionsPlugin", "ObjectInspectorPlugin", "OutputPreviewPlugin", "RedactPlugin", "SignaturePlugin", "SoftProofingPlugin", "EditorPlugin", "LoupePreflightPlugin", "ScannerPlugin", "Pdf4QtViewer", "Pdf4QtPageMaster", "Pdf4QtDiff", "Pdf4QtLaunchPad")) {
    Assert-Present $name
}

foreach ($name in @("AudioBookPlugin", "OcrPlugin", "LoupeOcrService")) {
    Assert-Absent $name
}

$desktopFiles = @($files | Where-Object { $_.Extension -eq ".desktop" })
if ($desktopFiles.Count -ne 1 -or $desktopFiles[0].Name -ne "io.github.mberrys.Loupe-pdf.desktop") {
    $names = ($desktopFiles | ForEach-Object Name) -join ", "
    throw "Release package must contain only the Loupe desktop entry; found: $names"
}

$loupeDesktop = @($desktopFiles | Where-Object { $_.Name -eq "io.github.mberrys.Loupe-pdf.desktop" })
if ($loupeDesktop.Count -gt 0) {
    $desktopText = Get-Content -LiteralPath $loupeDesktop[0].FullName -Raw
    if ($desktopText -notmatch "(?m)^Exec=Pdf4QtEditor(?:\.exe)? %f\r?$") {
        throw "Loupe desktop entry does not launch Pdf4QtEditor: $($loupeDesktop[0].FullName)"
    }
}

if ($files | Where-Object { $_.Extension -eq ".desktop" -and $_.Name -match "Pdf4Qt(Viewer|PageMaster|Diff|Editor)" }) {
    throw "Release package contains inherited application desktop entries."
}

$appxManifest = @($files | Where-Object { $_.Name -eq "AppxManifest.xml" })
if ($appxManifest.Count -gt 0) {
    $appxText = Get-Content -LiteralPath $appxManifest[0].FullName -Raw
    if ($appxText -notmatch '<Application\s+Id="Pdf4QtEditor"') {
        throw "AppX manifest does not declare the Loupe Editor application."
    }
    foreach ($name in @("Pdf4QtViewer", "Pdf4QtPageMaster", "Pdf4QtDiff", "Pdf4QtLaunchPad")) {
        if ($appxText -match ('<Application\s+Id="' + [regex]::Escape($name) + '"')) {
            throw "AppX manifest exposes compatibility application: $name"
        }
    }
}

Write-Output "Loupe surface verified: Loupe, Loupe CLI compatibility, hidden Viewer/PageMaster/Diff/LaunchPad binaries, Scanner, and retained production plugins present; separate app entries absent."
