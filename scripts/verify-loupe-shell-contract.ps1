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
    [string]$EditorUiPath = (Join-Path $PSScriptRoot "..\Pdf4QtLibGui\pdfeditormainwindow.ui")
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
if ($shell.gui_status -ne "deferred-until-0.0.3") {
    throw "GUI deferral gate changed without an explicit scope update: $($shell.gui_status)"
}
if ($shell.shell_surface -ne "Pdf4QtEditor") {
    throw "Loupe shell must remain Pdf4QtEditor: $($shell.shell_surface)"
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
$policyActions = @($actionPolicy.actions)
if ($actionPolicy.schema_version -ne 1 -or $actionPolicy.issue -ne 193) {
    throw "Unsupported Editor action policy version or issue number."
}
if ($actionPolicy.source_ui -ne "Pdf4QtLibGui/pdfeditormainwindow.ui") {
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
}

Write-Output "Loupe shell contract verified: $($shell.workspaces.Count) workspaces, $($uiIds.Count) Editor actions, $($shell.plugin_action_policy.Count) plugin policies; GUI remains deferred until 0.0.3."
