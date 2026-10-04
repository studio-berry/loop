#Requires -Version 5.1
<#
.SYNOPSIS
    Runs the product Quick accessibility smoke against a build tree or a staged
    copy of the installed tree.

.DESCRIPTION
    Two backends are qualified separately and must never be conflated:

      native    the run carries no QT_QUICK_BACKEND override, so the scene graph
                uses the platform's preferred RHI (d3d11 on Windows, OpenGL or
                Vulkan on Linux). Run on a real platform (not offscreen), a
                native run must not silently fall back to the software
                rasterizer.
      software  the run forces QT_QUICK_BACKEND=software; it proves the software
                rasterizer path and is recorded with a distinct claim so it can
                never stand in for the native lane.

    With -InstallTree the harness runs from a fresh staging copy of the
    installed tree: the executable, LoopLibCore/LoopLibQuick and the deployed Qt
    closure all resolve from the installed layout rather than the developer
    build tree. This is the tree a user runs, and the tree the Widgets-free
    profile is verified against.

    The smoke binary is a qualification harness and is not part of the shipped
    product surface, so it is staged beside the installed closure instead of
    being installed into it; the shipped install tree is never mutated.

    The native accessibility *backend* claim is not made here: it needs a real
    OS accessibility client, which scripts/run-installed-quick-a11y-uia.ps1
    drives on Windows. This script records the accessible tree and the graphics
    backend of the operator path.

.PARAMETER InstallTree
    Root of an installed tree (the directory containing usr/bin and usr/lib,
    e.g. build/install). When set, the qualification runs against a staging copy
    of it.

.PARAMETER Platform
    Qt platform plugin to use. Defaults to offscreen (headless). Pass windows on
    Windows to exercise the real native scene graph; a non-offscreen native run
    must report a native graphics API.

.PARAMETER StagingRoot
    Parent directory for the per-backend staging copy. Defaults to a directory
    under the system temporary path. Each backend gets its own staging copy so
    the native and software runs never share a loader state.

.PARAMETER SourceSha
    Exact 40-character source SHA to record in the evidence record.

.PARAMETER Package
    Optional package artifact (MSI/AppImage) whose SHA-256 is recorded as the
    artifact identity. The package is usually built after this qualification, so
    it is optional; the install-tree identity and executable digest are always
    recorded.

.PARAMETER EvidenceJson
    When set, writes a machine-readable qualification record (backend, claim,
    source SHA, installed-tree identity, executable digest, observed backends).
#>
param(
    [string]$BuildDir = (Join-Path $PSScriptRoot "..\build"),
    [ValidateSet("native", "software")]
    [string]$Backend = "native",
    [string]$InstallTree = "",
    [string]$Platform = "offscreen",
    [string]$StagingRoot = "",
    [string]$SourceSha = "",
    [string]$Package = "",
    [string]$EvidenceJson = ""
)

. (Join-Path $PSScriptRoot "lib\loop-qt-runtime.ps1")
. (Join-Path $PSScriptRoot "lib\loop-quick-a11y-staging.ps1")
$null = Add-LoopQtRuntimeToPath

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($SourceSha -and $SourceSha -notmatch "^[0-9a-fA-F]{40}$") {
    throw "SourceSha must be a full 40-character Git SHA; got '$SourceSha'."
}

$isWindowsHost = $IsWindows -or ($env:OS -match "Windows")
$binaryName = if ($isWindowsHost) { "ProductQuickAccessibilitySmoke.exe" } else { "ProductQuickAccessibilitySmoke" }

function Find-SmokeExecutable
{
    param([string]$Root)
    $resolved = (Resolve-Path -LiteralPath $Root).Path
    return Get-ChildItem -LiteralPath $resolved -Recurse -File |
        Where-Object { $_.Name -eq $binaryName } |
        Sort-Object FullName |
        Select-Object -First 1
}

$buildExecutable = Find-SmokeExecutable $BuildDir
if (-not $buildExecutable) {
    throw "ProductQuickAccessibilitySmoke binary not found below $((Resolve-Path -LiteralPath $BuildDir).Path). Configure with -DLOOP_BUILD_PRODUCT_QUICK_ACCESSIBILITY_SMOKE=ON and build the target first."
}

$artifactScope = "build-tree"
$runRoot = (Resolve-Path -LiteralPath $BuildDir).Path
$runExecutable = $buildExecutable.FullName
$installedTreePath = $null
$loaderPaths = @()

if ($InstallTree) {
    if (-not $StagingRoot) {
        $StagingRoot = Join-Path ([System.IO.Path]::GetTempPath()) "loop-quick-a11y-qualification"
    }
    $stage = Join-Path $StagingRoot $Backend
    $staged = New-LoopQuickA11yStagingTree -InstallTree $InstallTree -BuildDir $BuildDir -Stage $stage
    $installedTreePath = $staged.InstallTree
    $runRoot = $staged.Stage
    $runExecutable = $staged.Executable
    $loaderPaths = $staged.LoaderPaths
    $artifactScope = "installed-tree"
}

$env:QT_QPA_PLATFORM = $Platform
if ($Backend -eq "software") {
    $env:QT_QUICK_BACKEND = "software"
} else {
    Remove-Item Env:QT_QUICK_BACKEND -ErrorAction SilentlyContinue
}

if (-not $isWindowsHost -and $loaderPaths.Count -gt 0) {
    $env:LD_LIBRARY_PATH = (@($loaderPaths + @($env:LD_LIBRARY_PATH)) | Where-Object { $_ }) -join ":"
}

Write-Output "Running ProductQuickAccessibilitySmoke backend=$Backend artifact_scope=$artifactScope executable=$runExecutable"
$smokeOutput = @(& $runExecutable 2>&1)
$exitCode = $LASTEXITCODE
$smokeOutput | ForEach-Object { Write-Output $_ }
if ($exitCode -ne 0) {
    throw "ProductQuickAccessibilitySmoke failed with exit code $exitCode (backend=$Backend, artifact_scope=$artifactScope)"
}

$joined = $smokeOutput -join "`n"
if ($joined -notmatch "status=pass") {
    throw "ProductQuickAccessibilitySmoke did not report status=pass (backend=$Backend)."
}

$graphicsMatch = [regex]::Match($joined, "graphics_api=([A-Za-z0-9]+)")
$graphicsApi = if ($graphicsMatch.Success) { $graphicsMatch.Groups[1].Value } else { "absent" }
$accessibilityMatch = [regex]::Match($joined, "native_accessibility_backend_active=(\d)")
$nativeAccessibilityActive = if ($accessibilityMatch.Success) { $accessibilityMatch.Groups[1].Value -eq "1" } else { $false }

if ($Backend -eq "software") {
    if ($graphicsApi -ne "software") {
        throw "Software backend run did not select the software rasterizer (graphics_api=$graphicsApi)."
    }
} else {
    if ($graphicsApi -in @("unknown", "null", "unrecognized", "absent")) {
        throw "Native backend run did not initialize a known graphics backend (graphics_api=$graphicsApi)."
    }
    if ($graphicsApi -eq "software") {
        throw "Native backend run on platform $Platform reported the software rasterizer; the native claim cannot be made."
    }
}

$claim = if ($Backend -eq "native") { "native-backend-operator-path" } else { "software-renderer-operator-path" }

if ($EvidenceJson) {
    if ($Package) {
        $resolvedPackage = Resolve-Path -LiteralPath $Package -ErrorAction SilentlyContinue
        if ($resolvedPackage) {
            $packagePath = $resolvedPackage.Path
            $packageDigest = (Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash.ToLowerInvariant()
        } else {
            $packagePath = $Package
            $packageDigest = $null
        }
    } else {
        $packagePath = $null
        $packageDigest = $null
    }

    $record = [ordered]@{
        schema_version = 1
        kind           = "loop-quick-accessibility-qualification"
        backend        = $Backend
        claim          = $claim
        status         = "pass"
        source_sha     = if ($SourceSha) { $SourceSha.ToLowerInvariant() } else { $null }
        artifact       = [ordered]@{
            scope          = $artifactScope
            run_root       = $runRoot
            install_tree   = $installedTreePath
            executable     = $runExecutable
            executable_sha256 = (Get-FileHash -LiteralPath $runExecutable -Algorithm SHA256).Hash.ToLowerInvariant()
            package        = $packagePath
            package_sha256 = $packageDigest
        }
        observed       = [ordered]@{
            graphics_api                          = $graphicsApi
            native_accessibility_backend_active   = $nativeAccessibilityActive
            qpa_platform                          = $Platform
        }
    }

    $evidenceParent = Split-Path -Parent $EvidenceJson
    if ($evidenceParent) { New-Item -ItemType Directory -Force -Path $evidenceParent | Out-Null }
    ($record | ConvertTo-Json -Depth 6) | Set-Content -LiteralPath $EvidenceJson -Encoding UTF8
    Write-Host "Wrote Quick accessibility qualification evidence: $EvidenceJson"
}

Write-Host "ProductQuickAccessibilitySmoke passed (backend=$Backend, artifact_scope=$artifactScope, platform=$Platform, graphics_api=$graphicsApi)"
