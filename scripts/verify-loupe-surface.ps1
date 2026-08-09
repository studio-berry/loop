#Requires -Version 5.1
<#
.SYNOPSIS
    Verifies the installed slim Loupe product surface.

.DESCRIPTION
    Checks that the release-profile install contains the supported desktop/CLI
    entrypoints and retained preflight plugin, while rejecting inherited
    application binaries, optional plugins, and stale desktop entries.

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

$preflight = @($files | Where-Object { $_.BaseName -eq "LoupePreflightPlugin" })
if ($preflight.Count -eq 0) {
    throw "Missing required LoupePreflightPlugin artifact"
}

foreach ($name in @("Pdf4QtViewer", "Pdf4QtPageMaster", "Pdf4QtDiff", "Pdf4QtLaunchPad", "CodeGenerator", "JBIG2_Viewer", "PdfExampleGenerator", "AudioBookPlugin", "ScannerPlugin", "OcrPlugin")) {
    Assert-Absent $name
}

$desktopFiles = @($files | Where-Object { $_.Extension -eq ".desktop" })
foreach ($name in @("Pdf4QtViewer", "Pdf4QtPageMaster", "Pdf4QtDiff", "Pdf4QtLaunchPad")) {
    if ($desktopFiles | Where-Object { $_.Name -match [regex]::Escape($name) }) {
        throw "Forbidden desktop entry present: $name"
    }
}

$loupeDesktop = @($desktopFiles | Where-Object { $_.Name -eq "io.github.mberrys.Loupe-pdf.desktop" })
if ($loupeDesktop.Count -gt 0) {
    $desktopText = Get-Content -LiteralPath $loupeDesktop[0].FullName -Raw
    if ($desktopText -notmatch "(?m)^Exec=Pdf4QtEditor(?:\.exe)? %f$") {
        throw "Loupe desktop entry does not launch Pdf4QtEditor: $($loupeDesktop[0].FullName)"
    }
}

Write-Output "Loupe slim surface verified: Editor, PdfTool, and LoupePreflightPlugin present; inherited app/plugin surfaces absent."
