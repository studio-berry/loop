#!/usr/bin/env python3
"""Fail-closed plugin and legacy `.ui` form accounting for Phase 5 Session 04."""

from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RETIRED_PLUGINS = frozenset(
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
UI_REFERENCE = re.compile(r"[\w./\\-]+\.ui")


class AccountingError(ValueError):
    pass


def _run(script: str) -> None:
    completed = subprocess.run(
        [sys.executable, str(ROOT / "scripts" / script)],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        detail = (completed.stderr or completed.stdout).strip()
        raise AccountingError(f"{script} failed: {detail}")


def _ledger_ui_paths(shell: dict) -> set[str]:
    return {entry["path"].replace("\\", "/") for entry in shell.get("legacy_surface_disposition", [])}


def _resolve_ui_path(cmake: Path, root: Path, token: str) -> str:
    normalized = token.replace("\\", "/")
    direct = root / normalized
    if direct.is_file():
        return normalized
    return cmake.parent.relative_to(root).joinpath(normalized).as_posix().replace("\\", "/")


def _tracked_ui_references(root: Path) -> list[tuple[str, str]]:
    references: list[tuple[str, str]] = []
    for cmake in root.rglob("CMakeLists.txt"):
        if ".git" in cmake.parts:
            continue
        relative = cmake.relative_to(root).as_posix()
        for line in cmake.read_text(encoding="utf-8").splitlines():
            code = line.split("#", 1)[0]
            if ".ui" not in code:
                continue
            for token in UI_REFERENCE.findall(code):
                references.append((relative, _resolve_ui_path(cmake, root, token)))
    return references


def validate_accounting(root: Path) -> None:
    _run("verify-loupe-shell-contract.py")
    _run("verify-plugin-surface-policies.py")

    shell = json.loads((root / "docs/loupe-shell.json").read_text(encoding="utf-8"))
    ledger = _ledger_ui_paths(shell)
    expected_ledger_count = 2
    if len(ledger) != expected_ledger_count:
        raise AccountingError(f"expected {expected_ledger_count} ledgered .ui forms, found {len(ledger)}")

    repo_ui = {
        path.relative_to(root).as_posix()
        for path in root.rglob("*.ui")
        if path.is_file() and ".git" not in path.parts
    }
    ledger_only = sorted(ledger - repo_ui)
    repo_only = sorted(repo_ui - ledger)
    if ledger_only or repo_only:
        raise AccountingError(
            f"legacy .ui inventory drift ledger_only={ledger_only} repo_only={repo_only}"
        )

    for cmake, ui_path in _tracked_ui_references(root):
        normalized = ui_path.replace("\\", "/")
        if normalized not in repo_ui:
            raise AccountingError(f"{cmake} references missing .ui file: {normalized}")
        if normalized not in ledger:
            raise AccountingError(f"{cmake} references unledgered .ui file: {normalized}")

    product = json.loads((root / "docs/product-surface.json").read_text(encoding="utf-8"))
    plugin_rows = {
        row["artifact"]: row
        for row in product.get("surfaces", [])
        if row.get("kind") == "plugin" and row.get("artifact")
    }
    for name in RETIRED_PLUGINS:
        row = plugin_rows[name]
        if row.get("artifact_scope") != "build":
            raise AccountingError(f"{name} must be build-only in product ledger")
        if row.get("profiles", {}).get("loupe-release") != "absent":
            raise AccountingError(f"{name} must be absent from loupe-release in product ledger")
        cmake = root / "LoupeEditorPlugins" / name / "CMakeLists.txt"
        if cmake.is_file() and re.search(rf"install\s*\(\s*TARGETS\s+{re.escape(name)}\b", cmake.read_text(encoding="utf-8")):
            raise AccountingError(f"{name} still has install() rule")


def main() -> int:
    try:
        validate_accounting(ROOT)
    except AccountingError as exc:
        print(f"Plugin/form accounting FAILED: {exc}", file=sys.stderr)
        return 1
    print(
        "Plugin/form accounting verified: shell ledger, plugin policies, CMake .ui references, "
        "and retired install boundaries are fail-closed"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
