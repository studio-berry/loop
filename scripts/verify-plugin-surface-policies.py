#!/usr/bin/env python3
"""Verify all 12 plugin surface disposition policies are explicitly executed."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

RETIRED_FROM_INSTALL = frozenset({"AudioBookPlugin", "OcrPlugin"})
ABSORB_INSTALLED = frozenset(
    {
        "ActionListPlugin",
        "DimensionsPlugin",
        "EditorPlugin",
        "LoupePreflightPlugin",
        "OutputPreviewPlugin",
        "SoftProofingPlugin",
    }
)
ADVANCED_INSTALLED = frozenset({"ObjectInspectorPlugin", "SignaturePlugin", "ScannerPlugin"})
BLOCKED_PLUGIN = "RedactPlugin"
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
        raise PolicyError(f"missing plugin CMakeLists for {plugin}")
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
    unmatched = [row["plugin"] for row in crosswalk if row.get("status") != "matched"]
    if unmatched:
        raise PolicyError(f"plugin inventory crosswalk missing targets: {sorted(unmatched)}")

    for shell_row in shell_plugins:
        plugin = shell_row["plugin"]
        if plugin not in product_plugins:
            raise PolicyError(f"unmanifested plugin: {plugin}")
        if plugin not in disposition_plugins:
            raise PolicyError(f"plugin missing from Phase 5 disposition evidence: {plugin}")
        product_row = product_plugins[plugin]
        evidence = disposition_plugins[plugin]
        _expect_shell_disposition(product_row, shell_row)
        if not _required_test_exists(shell_row["required_test"]):
            raise PolicyError(f"{plugin} required_test missing: {shell_row['required_test']}")

        replacement = product_row.get("replacement_surface")
        if replacement and replacement not in workspaces:
            raise PolicyError(f"{plugin} replacement owner missing from product ledger: {replacement}")

        if plugin in RETIRED_FROM_INSTALL:
            if product_row.get("artifact_scope") != "build":
                raise PolicyError(f"{plugin} must be build-only")
            if product_row.get("profiles", {}).get("loupe-release") != "absent":
                raise PolicyError(f"{plugin} must be absent from loupe-release profile")
            if _has_install_rule(plugin):
                raise PolicyError(f"{plugin} still has install() rule")
            if evidence["disposition"] not in {"DELETE", "HEADLESS-REPLACE"}:
                raise PolicyError(f"{plugin} retired plugin has unexpected Phase 5 disposition")
            continue

        if plugin in ABSORB_INSTALLED:
            if product_row.get("disposition") != "ABSORB":
                raise PolicyError(f"{plugin} must remain ABSORB in product ledger")
            if product_row.get("artifact_scope") != "install":
                raise PolicyError(f"{plugin} must remain installed until workspace absorption is proven")
            if product_row.get("profiles", {}).get("loupe-release") != "present":
                raise PolicyError(f"{plugin} must remain in loupe-release profile")
            if not _has_install_rule(plugin):
                raise PolicyError(f"{plugin} must keep install() until Issue 14 retires its Widgets UI")
            if evidence["disposition"] != "HEADLESS-REPLACE":
                raise PolicyError(f"{plugin} must map to HEADLESS-REPLACE in Phase 5 evidence")
            if not replacement:
                raise PolicyError(f"{plugin} must name a replacement workspace")
            continue

        if plugin in ADVANCED_INSTALLED:
            if product_row.get("disposition") != "ADVANCED":
                raise PolicyError(f"{plugin} must remain ADVANCED in product ledger")
            if product_row.get("artifact_scope") != "install":
                raise PolicyError(f"{plugin} must remain installed as an advanced workflow")
            if evidence["disposition"] != "RETAIN-NON-PRODUCT":
                raise PolicyError(f"{plugin} must map to RETAIN-NON-PRODUCT in Phase 5 evidence")
            if not _has_install_rule(plugin):
                raise PolicyError(f"{plugin} must keep install() as an explicit advanced boundary")
            continue

        if plugin == BLOCKED_PLUGIN:
            if product_row.get("disposition") != "OPEN":
                raise PolicyError(f"{plugin} must remain OPEN in product ledger")
            if evidence["disposition"] != "BLOCKED":
                raise PolicyError(f"{plugin} must remain BLOCKED in Phase 5 evidence")
            if product_row.get("follow_up_issue") != 66:
                raise PolicyError(f"{plugin} must keep follow-up issue #66")
            continue

        raise PolicyError(f"unclassified plugin policy group: {plugin}")

    blocked = [
        row["target"]
        for row in disposition_plugins.values()
        if row.get("disposition") == "BLOCKED"
    ]
    if blocked != [BLOCKED_PLUGIN]:
        raise PolicyError(f"expected only {BLOCKED_PLUGIN} to be BLOCKED, found {blocked}")


def main() -> int:
    try:
        validate_plugin_policies(ROOT)
    except PolicyError as exc:
        print(f"Plugin surface policy verification FAILED: {exc}", file=sys.stderr)
        return 1
    print(
        "Plugin surface policies verified: STOP-SHIPPING retired from install; "
        "ABSORB/ADVANCED groups explicit; RedactPlugin remains BLOCKED"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
