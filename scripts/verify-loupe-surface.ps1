#Requires -Version 5.1
<#
.SYNOPSIS
    Verifies the installed Loupe product surface.

.DESCRIPTION
    Checks that the release-profile install contains the supported desktop/CLI
    entrypoints and retained production plugins, while rejecting only the
    explicitly deferred OCR and AudioBook surfaces.

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
    if ((Find-Artifact $Name).Count -eq 0) {
        throw "Missing required Loupe artifact: $Name"
    }
}

function Assert-Absent {
    param([string]$Name)
    $matches = Find-Artifact $Name
    if ($matches.Count -gt 0) {
        $paths = ($matches | ForEach-Object FullName) -join ", "
        throw "Forbidden inherited artifact present: $Name ($paths)"
    }
}

Assert-Present "Pdf4QtEditor"
Assert-Present "PdfTool"

foreach ($name in @("DimensionsPlugin", "ObjectInspectorPlugin", "OutputPreviewPlugin", "RedactPlugin", "SignaturePlugin", "SoftProofingPlugin", "EditorPlugin", "LoupePreflightPlugin", "ScannerPlugin", "Pdf4QtViewer", "Pdf4QtPageMaster", "Pdf4QtDiff", "Pdf4QtLaunchPad")) {
    Assert-Present $name
}

foreach ($name in @("AudioBookPlugin", "OcrPlugin", "LoupeOcrService")) {
    Assert-Absent $name
}

$desktopFiles = @($files | Where-Object { $_.Extension -eq ".desktop" })
if ($desktopFiles.Count -gt 0) {
    foreach ($name in @("Pdf4QtViewer", "Pdf4QtPageMaster", "Pdf4QtDiff", "Pdf4QtEditor")) {
        if (-not ($desktopFiles | Where-Object { $_.Name -match [regex]::Escape($name) })) {
            throw "Missing retained desktop entry: $name"
        }
    }
}

$loupeDesktop = @($desktopFiles | Where-Object { $_.Name -eq "io.github.mberrys.Loupe-pdf.desktop" })
if ($loupeDesktop.Count -gt 0) {
    $desktopText = Get-Content -LiteralPath $loupeDesktop[0].FullName -Raw
    if ($desktopText -notmatch "(?m)^Exec=Pdf4QtEditor(?:\.exe)? %f$") {
        throw "Loupe desktop entry does not launch Pdf4QtEditor: $($loupeDesktop[0].FullName)"
    }
}

Write-Output "Loupe surface verified: Editor, PdfTool, PageMaster, Viewer, Diff, LaunchPad, Scanner, and retained production plugins present; OCR and AudioBook surfaces absent."
