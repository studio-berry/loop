#Requires -Version 5.1
<#
.SYNOPSIS
    Verifies the non-visual Loupe shell contract and Editor action policy.

.DESCRIPTION
    This check intentionally does not build or launch the GUI. It verifies the
    state/workspace contract, the #192 product-surface linkage, plugin routing,
    and complete action coverage before GUI wiring is allowed.
#>
param(
    [string]$RepoRoot = (Join-Path $PSScriptRoot ".."),
    [string]$ProductSurfacePath = (Join-Path $PSScriptRoot "..\docs\product-surface.json"),
    [string]$ShellContractPath = (Join-Path $PSScriptRoot "..\docs\loupe-shell.json"),
    [string]$ActionPolicyPath = (Join-Path $PSScriptRoot "..\docs\loupe-shell-actions.json"),
    [string]$ShellSchemaPath = (Join-Path $PSScriptRoot "..\docs\schemas\loupe-shell.schema.json"),
    [string]$EditorUiPath = (Join-Path $PSScriptRoot "..\LoupeLibGui\pdfeditormainwindow.ui")
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

foreach ($path in @($ProductSurfacePath, $ShellContractPath, $ActionPolicyPath, $ShellSchemaPath, $EditorUiPath)) {
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
if ($shell.gui_status -ne "deferred-until-0.1.1") {
    throw "GUI deferral gate changed without an explicit scope update: $($shell.gui_status)"
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
$validLegacyClassifications = @("MIGRATE", "CONSOLIDATE", "HEADLESS", "RETIRE")
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
    $expectedShellDisposition = if ($pluginSurface[0].disposition -eq "CLI-ONLY") { "STOP-SHIPPING" } else { $pluginSurface[0].disposition }
    if ($pluginAction.disposition -ne $expectedShellDisposition) {
        throw "Plugin shell disposition diverges from product-surface manifest for $($pluginAction.plugin): expected $expectedShellDisposition, found $($pluginAction.disposition)"
    }
    foreach ($field in @("owner", "required_test", "evidence_artifact", "deletion_condition")) {
        if ([string]::IsNullOrWhiteSpace([string]$pluginAction.$field)) {
            throw "Plugin policy is missing $field for $($pluginAction.plugin)"
        }
    }
    if ($null -eq $pluginAction.replacement_target) {
        if ($pluginSurface[0].replacement_surface -ne $null) {
            throw "Plugin replacement target is missing for $($pluginAction.plugin)"
        }
    } elseif ([string]$pluginAction.replacement_target -ne [string]$pluginSurface[0].replacement_surface) {
        throw "Plugin replacement target diverges from product-surface manifest for $($pluginAction.plugin)"
    }
}

$pluginArtifacts = @($productSurface.surfaces | Where-Object { $_.kind -eq "plugin" } | ForEach-Object artifact | Sort-Object)
$policyPluginArtifacts = @($shell.plugin_action_policy | ForEach-Object plugin | Sort-Object)
if (($pluginArtifacts -join ",") -ne ($policyPluginArtifacts -join ",")) {
    throw "Plugin disposition coverage mismatch. Product surface: $($pluginArtifacts -join ', '); shell policy: $($policyPluginArtifacts -join ', ')"
}

$legacyEntries = @($shell.legacy_surface_disposition)
if ($legacyEntries.Count -ne 48) {
    throw "Legacy UI disposition must contain exactly 48 entries; found $($legacyEntries.Count)"
}
$repoUiPaths = @(git -C $RepoRoot ls-files "*.ui" | ForEach-Object { $_.Trim() } | Sort-Object)
$ledgerUiPaths = @($legacyEntries | ForEach-Object path | Sort-Object)
if (($repoUiPaths -join ",") -ne ($ledgerUiPaths -join ",")) {
    throw "Legacy UI disposition does not exactly cover repository inventory. Repository=$($repoUiPaths.Count), ledger=$($ledgerUiPaths.Count)"
}
$duplicateLegacyPaths = @($ledgerUiPaths | Group-Object | Where-Object Count -gt 1 | ForEach-Object Name)
if ($duplicateLegacyPaths.Count -gt 0) {
    throw "Duplicate legacy UI disposition paths: $($duplicateLegacyPaths -join ', ')"
}
foreach ($entry in $legacyEntries) {
    if ($validLegacyClassifications -notcontains $entry.classification) {
        throw "Invalid legacy UI classification for $($entry.path): $($entry.classification)"
    }
    foreach ($field in @("owner", "required_test", "evidence_artifact", "deletion_condition")) {
        if ([string]::IsNullOrWhiteSpace([string]$entry.$field)) {
            throw "Legacy UI disposition is missing $field for $($entry.path)"
        }
    }
}

Write-Output "Loupe shell contract verified: $($shell.workspaces.Count) workspaces, $($uiIds.Count) Editor actions, $($shell.plugin_action_policy.Count) plugin policies, $($legacyEntries.Count) legacy UI dispositions."
