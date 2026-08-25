#Requires -Version 5.1
<#
.SYNOPSIS
    Verifies the non-visual Loupe shell contract and Editor action policy.

.DESCRIPTION
    This check intentionally does not build or launch the GUI. It verifies the
    state/workspace contract, the #192 product-surface linkage, plugin routing,
  legacy surface disposition inventory, and complete action coverage before GUI
    wiring is allowed.
#>
param(
    [string]$RepoRoot = (Join-Path $PSScriptRoot ".."),
    [string]$ProductSurfacePath = (Join-Path $PSScriptRoot "..\docs\product-surface.json"),
    [string]$ShellContractPath = (Join-Path $PSScriptRoot "..\docs\loupe-shell.json"),
    [string]$ActionPolicyPath = (Join-Path $PSScriptRoot "..\docs\loupe-shell-actions.json"),
    [string]$EditorUiPath = (Join-Path $PSScriptRoot "..\LoupeLibGui\pdfeditormainwindow.ui")
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

foreach ($path in @($ProductSurfacePath, $ShellContractPath, $ActionPolicyPath, $EditorUiPath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required shell contract input is missing: $path"
    }
}

$productSurface = Get-Content -LiteralPath $ProductSurfacePath -Raw | ConvertFrom-Json
$shell = Get-Content -LiteralPath $ShellContractPath -Raw | ConvertFrom-Json
$actionPolicy = Get-Content -LiteralPath $ActionPolicyPath -Raw | ConvertFrom-Json
$editorUi = Get-Content -LiteralPath $EditorUiPath -Raw

if ($productSurface.shell_contract -ne "docs/loupe-shell.json") {
    throw "Product-surface manifest is not linked to the #193 shell contract."
}
if ($shell.schema_version -ne 1 -or $shell.issue -ne 193) {
    throw "Unsupported shell contract version or issue number."
}
$allowedGuiStatuses = @("gated-by-quick-admission", "quick-admitted")
if ($allowedGuiStatuses -notcontains $shell.gui_status) {
    throw "Unsupported gui_status: $($shell.gui_status)"
}
if ($shell.shell_surface -ne "LoupeEditor") {
    throw "Loupe shell must remain LoupeEditor: $($shell.shell_surface)"
}
if ($shell.ui_foundation.candidate -ne "Qt Quick Controls" -or $shell.ui_foundation.decision_issue -ne 178) {
    throw "Shell foundation is not linked to the Qt Quick Controls decision issue."
}

$expectedWorkspaceIds = @(
    "loupe-document",
    "loupe-preflight",
    "loupe-production-preview",
    "loupe-pages-production",
    "loupe-inspect",
    "loupe-fix",
    "loupe-compare"
)
$manifestWorkspaceIds = @($productSurface.surfaces | Where-Object { $_.kind -eq "workspace" } | ForEach-Object id | Sort-Object)
$shellWorkspaceIds = @($shell.workspaces | ForEach-Object manifest_surface | Sort-Object)
if (($manifestWorkspaceIds -join ",") -ne (($expectedWorkspaceIds | Sort-Object) -join ",")) {
    throw "Product-surface workspace inventory does not match the shell contract."
}
if (($shellWorkspaceIds -join ",") -ne (($expectedWorkspaceIds | Sort-Object) -join ",")) {
    throw "Shell workspace inventory does not match the #192 product-surface contract."
}
$validDispositions = @("KEEP", "ADVANCED", "ABSORB", "HIDE", "OPEN", "STOP-SHIPPING")
$validTargets = @("Document", "Preflight", "Production", "Inspect", "Fix", "Pages", "Compare", "Advanced")
$validLegacyDispositions = @("MIGRATE", "CONSOLIDATE", "HEADLESS", "RETIRE")
$policyActions = @($actionPolicy.actions)
if ($actionPolicy.schema_version -ne 1 -or $actionPolicy.issue -ne 193) {
    throw "Unsupported Editor action policy version or issue number."
}
if ($actionPolicy.source_ui -ne "LoupeLibGui/pdfeditormainwindow.ui") {
    throw "Editor action policy points to an unexpected source UI."
}
$policyIds = @($policyActions | ForEach-Object id)
$duplicateIds = @($policyIds | Group-Object | Where-Object Count -gt 1 | ForEach-Object Name)
if ($duplicateIds.Count -gt 0) {
    throw "Duplicate Editor action policy IDs: $($duplicateIds -join ', ')"
}
foreach ($action in $policyActions) {
    if ($validDispositions -notcontains $action.disposition) {
        throw "Invalid disposition for Editor action $($action.id): $($action.disposition)"
    }
    if ($validTargets -notcontains $action.target) {
        throw "Invalid target for Editor action $($action.id): $($action.target)"
    }
}

$uiIds = @([regex]::Matches($editorUi, '<action name="([^"]+)"') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
$missingActions = @($uiIds | Where-Object { $policyIds -notcontains $_ })
$extraActions = @($policyIds | Where-Object { $uiIds -notcontains $_ })
if ($missingActions.Count -gt 0 -or $extraActions.Count -gt 0) {
    throw "Editor action policy mismatch. Missing: $($missingActions -join ', '); extra: $($extraActions -join ', ')"
}
if ([int]$actionPolicy.expected_action_count -ne $uiIds.Count -or $policyActions.Count -ne $uiIds.Count) {
    throw "Editor action count mismatch. UI=$($uiIds.Count), policy=$($policyActions.Count), expected=$($actionPolicy.expected_action_count)"
}

$pluginPolicyFields = @("owner", "replacement_target", "required_test", "evidence_artifact", "deletion_condition")
foreach ($pluginAction in @($shell.plugin_action_policy)) {
    $pluginSurface = @($productSurface.surfaces | Where-Object { $_.kind -eq "plugin" -and $_.artifact -eq $pluginAction.plugin })
    if ($pluginSurface.Count -ne 1) {
        throw "Plugin policy names an unmanifested plugin: $($pluginAction.plugin)"
    }
    if ($validDispositions -notcontains $pluginAction.disposition) {
        throw "Invalid plugin disposition for $($pluginAction.plugin): $($pluginAction.disposition)"
    }
    if ($validTargets -notcontains $pluginAction.target) {
        throw "Invalid plugin target for $($pluginAction.plugin): $($pluginAction.target)"
    }
    foreach ($field in $pluginPolicyFields) {
        if (-not ($pluginAction.PSObject.Properties.Name -contains $field)) {
            throw "Plugin policy $($pluginAction.plugin) is missing required field: $field"
        }
        if ($field -ne "replacement_target" -and [string]::IsNullOrWhiteSpace([string]$pluginAction.$field)) {
            throw "Plugin policy $($pluginAction.plugin) has empty required field: $field"
        }
    }
    $expectedShellDisposition = if ($pluginSurface[0].disposition -eq "CLI-ONLY") { "STOP-SHIPPING" } else { $pluginSurface[0].disposition }
    if ($pluginAction.disposition -ne $expectedShellDisposition) {
        throw "Plugin shell disposition diverges from product-surface manifest for $($pluginAction.plugin): expected $expectedShellDisposition, found $($pluginAction.disposition)"
    }
}

$legacyLedger = @($shell.legacy_surface_disposition)
$legacyPaths = @()
$legacyFields = @("path", "disposition", "owner", "replacement_target", "required_test", "evidence_artifact", "deletion_condition", "rationale")
foreach ($entry in $legacyLedger) {
    foreach ($field in $legacyFields) {
        if (-not ($entry.PSObject.Properties.Name -contains $field)) {
            throw "Legacy surface entry is missing required field: $field"
        }
        if ($field -ne "replacement_target" -and [string]::IsNullOrWhiteSpace([string]$entry.$field)) {
            throw "Legacy surface $($entry.path) has empty required field: $field"
        }
    }
    if ($validLegacyDispositions -notcontains $entry.disposition) {
        throw "Invalid legacy disposition for $($entry.path): $($entry.disposition)"
    }
    $fullPath = Join-Path $RepoRoot $entry.path
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        throw "Legacy surface ledger references missing file: $($entry.path)"
    }
    if ($legacyPaths -contains $entry.path) {
        throw "Duplicate legacy surface ledger entry: $($entry.path)"
    }
    $legacyPaths += $entry.path
}

if ($legacyLedger.Count -ne 48) {
    throw "legacy_surface_disposition must contain exactly 48 tracked .ui forms, found $($legacyLedger.Count)"
}

$repoUiFiles = @(Get-ChildItem -LiteralPath $RepoRoot -Recurse -Filter "*.ui" -File | ForEach-Object {
    $_.FullName.Substring($RepoRoot.Length + 1).Replace("\", "/")
} | Sort-Object)
$ledgerOnly = @($legacyPaths | Where-Object { $repoUiFiles -notcontains $_ })
$repoOnly = @($repoUiFiles | Where-Object { $legacyPaths -notcontains $_ })
if ($ledgerOnly.Count -gt 0 -or $repoOnly.Count -gt 0) {
    throw "Legacy surface inventory drift. Ledger-only: $($ledgerOnly -join ', '); repo-only: $($repoOnly -join ', ')"
}

$guiMessage = if ($shell.gui_status -eq "quick-admitted") {
    "Quick product shell admitted."
} else {
    "product GUI remains gated by S21/S22 Quick admission."
}

Write-Output "Loupe shell contract verified: $($shell.workspaces.Count) workspaces, $($uiIds.Count) Editor actions, $($shell.plugin_action_policy.Count) plugin policies, $($legacyLedger.Count) legacy UI dispositions; $guiMessage"
