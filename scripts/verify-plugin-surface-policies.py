#!/usr/bin/env python3
"""Verify all 12 plugin surface disposition policies are explicitly executed."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

RETIRED_FROM_INSTALL = frozenset(
    {
        "ActionListPlugin",
        "AudioBookPlugin",
        "DimensionsPlugin",
        "EditorPlugin",
        "LoupePreflightPlugin",
        "ObjectInspectorPlugin",
        "OcrPlugin",
        "OutputPreviewPlugin",
        "RedactPlugin",
        "ScannerPlugin",
        "SignaturePlugin",
        "SoftProofingPlugin",
    }
)
SHELL_TO_PRODUCT = {"STOP-SHIPPING": frozenset({"STOP-SHIPPING", "CLI-ONLY"})}


class PolicyError(ValueError):
    pass


def _load_json(path: Path) -> dict:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise PolicyError(f"cannot read {path.relative_to(ROOT)}: {exc}") from exc
    if not isinstance(data, dict):
        raise PolicyError(f"{path.relative_to(ROOT)} must contain an object")
    return data


def _plugin_product_rows(product: dict) -> dict[str, dict]:
    return {
        row["artifact"]: row
        for row in product.get("surfaces", [])
        if row.get("kind") == "plugin" and row.get("artifact")
    }


def _workspace_ids(product: dict) -> frozenset[str]:
    return frozenset(
        row["id"]
        for row in product.get("surfaces", [])
        if row.get("kind") == "workspace" and row.get("id")
    )


def _replacement_owner_ids(product: dict) -> frozenset[str]:
    owners = _workspace_ids(product)
    for row in product.get("surfaces", []):
        if (
            row.get("kind") == "application"
            and row.get("artifact_scope") == "install"
            and row.get("profiles", {}).get("loupe-release") == "present"
            and not row.get("replacement_surface")
            and row.get("id")
        ):
            owners = owners | {row["id"]}
    return owners


def _disposition_plugins(disposition: dict) -> dict[str, dict]:
    return {
        row["target"]: row
        for row in disposition.get("rows", [])
        if row.get("kind") == "plugin" and row.get("target")
    }


def _has_install_rule(plugin: str) -> bool:
    cmake = ROOT / "LoupeEditorPlugins" / plugin / "CMakeLists.txt"
    if not cmake.is_file():
        return False
    text = cmake.read_text(encoding="utf-8")
    return bool(re.search(rf"install\s*\(\s*TARGETS\s+{re.escape(plugin)}\b", text))


def _required_test_exists(required_test: str) -> bool:
    if required_test.startswith("PdfTool "):
        return True
    path = ROOT / required_test
    return path.is_file()


def _expect_shell_disposition(product_row: dict, shell_row: dict) -> None:
    product_disposition = product_row["disposition"]
    shell_disposition = shell_row["disposition"]
    allowed = SHELL_TO_PRODUCT.get(shell_disposition, frozenset({shell_disposition}))
    if product_disposition not in allowed:
        raise PolicyError(
            f"{shell_row['plugin']} shell/product disposition drift: "
            f"shell={shell_disposition}, product={product_disposition}"
        )


def validate_plugin_policies(root: Path) -> None:
    shell = _load_json(root / "docs" / "loupe-shell.json")
    product = _load_json(root / "docs" / "product-surface.json")
    disposition = _load_json(root / "docs/generated/phase5-widgets-disposition.json")
    product_plugins = _plugin_product_rows(product)
    disposition_plugins = _disposition_plugins(disposition)
    workspaces = _replacement_owner_ids(product)
    shell_plugins = shell.get("plugin_action_policy", [])
    if len(shell_plugins) != 12:
        raise PolicyError(f"expected 12 plugin policies, found {len(shell_plugins)}")

    crosswalk = disposition.get("crosswalk", {}).get("plugin_action_policy", [])
    unmatched = [row["plugin"] for row in crosswalk if row.get("status") not in {"matched", "explained-plugin-source-deleted"}]
    if unmatched:
        raise PolicyError(f"plugin inventory crosswalk missing targets: {sorted(unmatched)}")

    for shell_row in shell_plugins:
        plugin = shell_row["plugin"]
        if plugin not in product_plugins:
            raise PolicyError(f"unmanifested plugin: {plugin}")
        product_row = product_plugins[plugin]
        evidence = disposition_plugins.get(plugin)
        _expect_shell_disposition(product_row, shell_row)
        if not _required_test_exists(shell_row["required_test"]):
            raise PolicyError(f"{plugin} required_test missing: {shell_row['required_test']}")

        replacement = product_row.get("replacement_surface")
        if replacement and replacement not in workspaces:
            raise PolicyError(f"{plugin} replacement owner missing from product ledger: {replacement}")

        if plugin not in RETIRED_FROM_INSTALL:
            raise PolicyError(f"unclassified plugin policy group: {plugin}")

        if product_row.get("artifact_scope") != "build":
            raise PolicyError(f"{plugin} must be build-only")
        if product_row.get("profiles", {}).get("loupe-release") != "absent":
            raise PolicyError(f"{plugin} must be absent from loupe-release profile")
        if _has_install_rule(plugin):
            raise PolicyError(f"{plugin} still has install() rule")
        if evidence and evidence.get("disposition") not in {"DELETE", "HEADLESS-REPLACE", "BLOCKED", "RETAIN-NON-PRODUCT"}:
            raise PolicyError(f"{plugin} retired plugin has unexpected Phase 5 disposition")

    blocked = sorted(
        row["target"]
        for row in disposition_plugins.values()
        if row.get("disposition") == "BLOCKED"
    )
    if blocked and blocked != ["RedactPlugin"]:
        raise PolicyError(f"expected only RedactPlugin to be BLOCKED, found {blocked}")


def main() -> int:
    try:
        validate_plugin_policies(ROOT)
    except PolicyError as exc:
        print(f"Plugin surface policies FAILED: {exc}", file=sys.stderr)
        return 1
    print("Plugin surface policies verified: all 12 plugins are build-only and absent from loupe-release")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
