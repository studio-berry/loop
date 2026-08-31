#!/usr/bin/env python3
"""Verify Loupe shell contract (Python twin for Linux agents without pwsh)."""

from __future__ import annotations

import json
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PRODUCT_SURFACE_PATH = ROOT / "docs" / "product-surface.json"
SHELL_PATH = ROOT / "docs" / "loupe-shell.json"
ACTION_POLICY_PATH = ROOT / "docs" / "loupe-shell-actions.json"

VALID_DISPOSITIONS = frozenset({"KEEP", "ADVANCED", "ABSORB", "HIDE", "OPEN", "STOP-SHIPPING"})
VALID_TARGETS = frozenset({"Document", "Preflight", "Production", "Inspect", "Fix", "Pages", "Compare", "Advanced"})
VALID_LEGACY = frozenset({"MIGRATE", "CONSOLIDATE", "HEADLESS", "RETIRE"})
PLUGIN_FIELDS = ("owner", "replacement_target", "required_test", "evidence_artifact", "deletion_condition")
LEGACY_FIELDS = (
    "path",
    "disposition",
    "owner",
    "replacement_target",
    "required_test",
    "evidence_artifact",
    "deletion_condition",
    "rationale",
)
EXPECTED_LEGACY_UI_COUNT = 2


class ContractError(ValueError):
    pass


def load_json(path: Path) -> dict:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ContractError(f"cannot read {path.relative_to(ROOT)}: {exc}") from exc
    if not isinstance(data, dict):
        raise ContractError(f"{path.relative_to(ROOT)} must be an object")
    return data


def main() -> int:
    product_surface = load_json(PRODUCT_SURFACE_PATH)
    shell = load_json(SHELL_PATH)
    action_policy = load_json(ACTION_POLICY_PATH)

    if product_surface.get("shell_contract") != "docs/loupe-shell.json":
        raise ContractError("product-surface manifest is not linked to loupe-shell.json")
    if shell.get("schema_version") != 1 or shell.get("issue") != 193:
        raise ContractError("unsupported shell contract version or issue")
    if shell.get("gui_status") not in frozenset({"gated-by-quick-admission", "quick-admitted"}):
        raise ContractError(f"unsupported gui_status: {shell.get('gui_status')}")
    if shell.get("shell_surface") != "LoupeEditor":
        raise ContractError("shell_surface must be LoupeEditor")

    expected_workspaces = sorted(
        [
            "loupe-document",
            "loupe-preflight",
            "loupe-production-preview",
            "loupe-pages-production",
            "loupe-inspect",
            "loupe-fix",
            "loupe-compare",
        ]
    )
    manifest_ids = sorted(
        surface["id"]
        for surface in product_surface.get("surfaces", [])
        if surface.get("kind") == "workspace"
    )
    shell_ids = sorted(workspace["manifest_surface"] for workspace in shell.get("workspaces", []))
    if manifest_ids != expected_workspaces or shell_ids != expected_workspaces:
        raise ContractError("workspace inventory drift")

    actions = action_policy.get("actions", [])
    policy_ids = [action["id"] for action in actions]
    if len(policy_ids) != len(set(policy_ids)):
        raise ContractError("duplicate editor action ids")
    expected_count = int(action_policy.get("expected_action_count", 0))
    if expected_count != len(actions):
        raise ContractError(
            f"Editor action count mismatch: expected_action_count={expected_count}, policy={len(actions)}"
        )

    plugin_surfaces = {
        surface["artifact"]: surface
        for surface in product_surface.get("surfaces", [])
        if surface.get("kind") == "plugin"
    }
    for plugin_action in shell.get("plugin_action_policy", []):
        plugin = plugin_action["plugin"]
        if plugin not in plugin_surfaces:
            raise ContractError(f"unmanifested plugin: {plugin}")
        for field in PLUGIN_FIELDS:
            if field not in plugin_action:
                raise ContractError(f"plugin {plugin} missing {field}")
            if field != "replacement_target" and not str(plugin_action[field]).strip():
                raise ContractError(f"plugin {plugin} empty {field}")
        if plugin_action["disposition"] not in VALID_DISPOSITIONS:
            raise ContractError(f"invalid plugin disposition for {plugin}")
        if plugin_action["target"] not in VALID_TARGETS:
            raise ContractError(f"invalid plugin target for {plugin}")
        expected = (
            "STOP-SHIPPING"
            if plugin_surfaces[plugin]["disposition"] == "CLI-ONLY"
            else plugin_surfaces[plugin]["disposition"]
        )
        if plugin_action["disposition"] != expected:
            raise ContractError(
                f"plugin disposition drift for {plugin}: expected {expected}, found {plugin_action['disposition']}"
            )

    legacy = shell.get("legacy_surface_disposition", [])
    ledger_paths: list[str] = []
    for entry in legacy:
        for field in LEGACY_FIELDS:
            if field not in entry:
                raise ContractError(f"legacy entry missing {field}")
            if field != "replacement_target" and not str(entry[field]).strip():
                raise ContractError(f"legacy {entry.get('path')} empty {field}")
        if entry["disposition"] not in VALID_LEGACY:
            raise ContractError(f"invalid legacy disposition for {entry['path']}")
        path = entry["path"]
        if path in ledger_paths:
            raise ContractError(f"duplicate legacy surface ledger entry: {path}")
        ledger_paths.append(path)
        if not (ROOT / path).is_file():
            raise ContractError(f"missing legacy ui file: {path}")

    if len(legacy) != EXPECTED_LEGACY_UI_COUNT:
        raise ContractError(
            f"legacy_surface_disposition must contain {EXPECTED_LEGACY_UI_COUNT} entries, found {len(legacy)}"
        )

    repo_ui = sorted(
        str(p.relative_to(ROOT)).replace("\\", "/")
        for p in ROOT.rglob("*.ui")
        if p.is_file()
    )
    ledger_only = sorted(set(ledger_paths) - set(repo_ui))
    repo_only = sorted(set(repo_ui) - set(ledger_paths))
    if ledger_only or repo_only:
        raise ContractError(f"legacy inventory drift ledger_only={ledger_only} repo_only={repo_only}")

    gui_message = (
        "Quick product shell admitted."
        if shell.get("gui_status") == "quick-admitted"
        else "product GUI remains gated by S21/S22 Quick admission."
    )
    print(
        f"Loupe shell contract verified: {len(shell['workspaces'])} workspaces, "
        f"{len(actions)} Editor actions, {len(shell['plugin_action_policy'])} plugin policies, "
        f"{len(legacy)} legacy UI dispositions; {gui_message}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ContractError as exc:
        print(f"Loupe shell contract FAILED: {exc}", file=sys.stderr)
        raise SystemExit(1)
