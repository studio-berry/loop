#Requires -Version 5.1
<#
.SYNOPSIS
    Verifies an installed Loupe product surface against docs/product-surface.json.

.DESCRIPTION
    The product-surface manifest is the source of truth. This check derives
    expected/forbidden first-party artifacts and packaging entrypoints from
    the selected profile instead of maintaining a second hardcoded inventory.
    It is an artifact/contract check; it does not build or launch the GUI. It
    invokes the installed PdfTool discovery command to verify CLI capabilities.
#>
param(
    [string]$InstallDir = (Get-Location).Path,
    [ValidateSet("developer", "loupe-release")]
    [string]$Profile = "loupe-release",
    [string]$ManifestPath = (Join-Path $PSScriptRoot "..\docs\product-surface.json")
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $InstallDir -PathType Container)) {
    throw "Install directory does not exist: $InstallDir"
}
if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) {
    throw "Product-surface manifest does not exist: $ManifestPath"
}

$manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
if ([int]$manifest.schema_version -ne 1) {
    throw "Unsupported product-surface manifest schema: $($manifest.schema_version)"
}
if ($manifest.adr -ne "docs/adr/adr-005-product-surface-pruning-classification.md") {
    throw "Product-surface manifest is not linked to ADR-005."
}
if (@($manifest.profiles) -notcontains $Profile) {
    throw "Manifest does not define profile: $Profile"
}

function Get-ProfileValue {
    param(
        [Parameter(Mandatory = $true)]$Object,
        [Parameter(Mandatory = $true)][string]$Name
    )
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        throw "Manifest object is missing profile property: $Name"
    }
    return $property.Value
}

$files = @(Get-ChildItem -LiteralPath $InstallDir -Recurse -File)
$installSurfaces = @($manifest.surfaces | Where-Object {
    $_.artifact_scope -eq "install" -and $null -ne $_.artifact
})

function Find-Artifact {
    param([string]$Name)
    return @($files | Where-Object {
        $_.BaseName -eq $Name -or
        $_.BaseName -eq "lib$Name" -or
        $_.Name -eq $Name -or
        $_.Name -eq "$Name.exe" -or
        $_.Name -eq "$Name.dll" -or
        $_.Name -eq "lib$Name.so" -or
        $_.Name -eq "lib$Name.dylib"
    })
}

function Assert-Present {
    param([string]$Name)
    $matches = @(Find-Artifact $Name)
    if ($matches.Count -eq 0) {
        throw "Missing required Loupe artifact from manifest: $Name"
    }
}

function Assert-Absent {
    param([string]$Name)
    $matches = @(Find-Artifact $Name)
    if ($matches.Count -gt 0) {
        $paths = ($matches | ForEach-Object FullName) -join ", "
        throw "Forbidden artifact present for manifest profile $($Profile): $Name ($paths)"
    }
}

foreach ($surface in $installSurfaces) {
    $state = Get-ProfileValue $surface.profiles $Profile
    if ($state -eq "present") {
        Assert-Present $surface.artifact
    } elseif ($state -eq "absent") {
        Assert-Absent $surface.artifact
    } else {
        throw "Invalid profile state '$state' for surface '$($surface.id)'."
    }
}


# Catch first-party drift in either direction. Third-party Qt/vcpkg libraries
# are intentionally outside this product-artifact namespace.
$knownArtifacts = @($installSurfaces | Where-Object {
    (Get-ProfileValue $_.profiles $Profile) -eq "present"
} | ForEach-Object artifact)
$firstPartyFiles = @($files | Where-Object {
    $_.Extension -in @(".exe", ".dll", ".so", ".dylib") -and
    $_.BaseName -match "^(lib)?(Pdf4Qt(Editor|Viewer|PageMaster|Diff|LaunchPad)|PdfTool|[A-Za-z]+Plugin)$"
})
foreach ($file in $firstPartyFiles) {
    $artifactName = $file.BaseName -replace "^lib", ""
    if ($knownArtifacts -notcontains $artifactName) {
        throw "Unmanifested first-party artifact in install tree: $($file.FullName)"
    }
}

# .desktop entries are a freedesktop.org / Linux packaging concept; CMakeLists.txt
# only installs them outside the WIN32 branch, so they never exist on a Windows
# install surface. $IsLinux is a PowerShell 7+ automatic variable.
if ($IsLinux) {
    $expectedDesktop = @(Get-ProfileValue $manifest.packaging.desktop_entries $Profile)
    $desktopFiles = @($files | Where-Object { $_.Extension -eq ".desktop" })
    $actualDesktop = @($desktopFiles | ForEach-Object Name | Sort-Object)
    $expectedDesktopSorted = @($expectedDesktop | Sort-Object)
    if (($actualDesktop -join "`n") -ne ($expectedDesktopSorted -join "`n")) {
        throw "Desktop entry inventory drift for $($Profile). Expected: $($expectedDesktopSorted -join ', '); found: $($actualDesktop -join ', ')"
    }

    $launcher = $manifest.packaging.loupe_launcher
    $loupeDesktop = @($desktopFiles | Where-Object { $_.Name -eq "io.github.mberrys.Loupe-pdf.desktop" })
    if ($loupeDesktop.Count -eq 1) {
        $desktopText = Get-Content -LiteralPath $loupeDesktop[0].FullName -Raw
        $expectedExec = "^Exec=" + [regex]::Escape($launcher.executable) + "(?:\.exe)? %f\r?$"
        if ($desktopText -notmatch "(?m)$expectedExec") {
            throw "Loupe desktop entry does not launch $($launcher.executable): $($loupeDesktop[0].FullName)"
        }
        foreach ($association in @($launcher.file_associations)) {
            if ($desktopText -notmatch "(?m)^MimeType=.*$([regex]::Escape($association))") {
                throw "Loupe desktop entry is missing file association $association."
            }
        }
    }
}

$discoveryParts = @($manifest.cli.discovery_command -split "\s+")
$discoveryExecutable = $discoveryParts[0]
$pdfTool = @(Find-Artifact $discoveryExecutable | Select-Object -First 1)
if ($pdfTool.Count -ne 1) {
    throw "CLI discovery executable is missing from the installed surface: $discoveryExecutable"
}
$discoveryArguments = @($discoveryParts | Select-Object -Skip 1)
$discoveryOutput = & $pdfTool[0].FullName @discoveryArguments 2>&1
if ($LASTEXITCODE -ne 0) {
    throw "CLI discovery command failed with exit code $($LASTEXITCODE): $($manifest.cli.discovery_command)"
}
try {
    $discoveryDocument = ($discoveryOutput -join "`n") | ConvertFrom-Json
} catch {
    throw "CLI discovery command did not return valid JSON: $($manifest.cli.discovery_command)"
}
$discoveryData = $discoveryDocument.data
if ($null -eq $discoveryData) {
    throw "CLI discovery output is missing the data envelope."
}
$capabilityProperty = $discoveryData.PSObject.Properties[$manifest.cli.capability_field]
if ($null -eq $capabilityProperty) {
    throw "CLI discovery output is missing capability field: $($manifest.cli.capability_field)"
}
$reportedCapabilities = @($capabilityProperty.Value)
foreach ($requiredCapability in @($manifest.cli.required_build_capabilities)) {
    if ($reportedCapabilities -notcontains $requiredCapability) {
        throw "CLI discovery output is missing required build capability: $requiredCapability"
    }
}

$appxManifest = @($files | Where-Object { $_.Name -eq "AppxManifest.xml" })
if ($appxManifest.Count -gt 0) {
    $appxText = Get-Content -LiteralPath $appxManifest[0].FullName -Raw
    foreach ($application in @(Get-ProfileValue $manifest.packaging.appx_applications $Profile)) {
        if ($appxText -notmatch ('<Application\s+Id="' + [regex]::Escape($application) + '"')) {
            throw "AppX manifest is missing manifest application: $application"
        }
    }
    $allApplications = @(@($manifest.packaging.appx_applications.developer) + @($manifest.packaging.appx_applications.'loupe-release') | Sort-Object -Unique)
    foreach ($application in $allApplications) {
        if ((Get-ProfileValue $manifest.packaging.appx_applications $Profile) -notcontains $application -and
            $appxText -match ('<Application\s+Id="' + [regex]::Escape($application) + '"')) {
            throw "AppX manifest exposes application not allowed by $($Profile): $application"
        }
    }
    $editorApplication = [regex]::Match($appxText, '<Application\s+Id="Pdf4QtEditor".*?</Application>', [System.Text.RegularExpressions.RegexOptions]::Singleline).Value
    foreach ($association in @($launcher.file_associations)) {
        if ($association -eq "application/pdf" -and $editorApplication -notmatch '<uap:FileType>\.pdf</uap:FileType>') {
            throw "AppX PDF file association is not owned by the Loupe Editor application."
        }
    }
}

Write-Output "Loupe surface verified from $ManifestPath for profile $($Profile): $($knownArtifacts.Count) first-party artifacts, $($desktopFiles.Count) desktop entries, and the declared Editor launcher contract."
