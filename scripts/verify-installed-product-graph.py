#!/usr/bin/env python3
"""Verify installed LoupeEditor is the Quick product graph and sole interactive install."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EDITOR_CMAKE = ROOT / "LoupeEditor" / "CMakeLists.txt"
SECONDARY_EXECUTABLES = ("LoupeViewer", "LoupePageMaster", "LoupeDiff", "LoupeLaunchPad")
RETIRED_PLUGINS = ("AudioBookPlugin", "OcrPlugin")
BUILD_DEFAULTS = ("VIEWER", "PAGEMASTER", "DIFF", "LAUNCHPAD")

FORBIDDEN_EDITOR_LIBS = frozenset(
    {
        "LoupeLibWidgets",
        "LoupeLibGui",
        "Qt6::Widgets",
        "Qt6::QuickWidgets",
    }
)
REQUIRED_EDITOR_LIBS = frozenset(
    {
        "LoupeEditorQuick",
        "Qt6::Quick",
        "Qt6::QuickControls2",
    }
)


class ContractError(ValueError):
    pass


def extract_target_link_block(cmake_text: str, target: str) -> str:
    pattern = re.compile(
        rf"target_link_libraries\s*\(\s*{re.escape(target)}\s+(?P<body>.*?)\)",
        re.MULTILINE | re.DOTALL,
    )
    match = pattern.search(cmake_text)
    if not match:
        raise ContractError(f"missing target_link_libraries for {target}")
    return match.group("body")


def linked_libraries(link_block: str) -> set[str]:
    tokens = re.split(r"\s+", link_block.replace("\n", " "))
    return {token.strip() for token in tokens if token.strip() and token not in {"PRIVATE", "PUBLIC", "INTERFACE"}}


def validate_sole_interactive_product(root: Path) -> None:
    product = json.loads((root / "docs" / "product-surface.json").read_text(encoding="utf-8"))
    installed_apps = [
        row
        for row in product.get("surfaces", [])
        if row.get("kind") == "application" and row.get("artifact_scope") == "install"
    ]
    keep = [row.get("artifact") for row in installed_apps if row.get("disposition") == "KEEP"]
    if keep != ["LoupeEditor"]:
        raise ContractError(f"installed KEEP applications must be only LoupeEditor, found {keep}")
    cli = [row.get("artifact") for row in installed_apps if row.get("disposition") == "CLI-ONLY"]
    if cli != ["PdfTool"]:
        raise ContractError(f"installed CLI-ONLY applications must be only PdfTool, found {cli}")
    for name in SECONDARY_EXECUTABLES:
        cmake = (root / name / "CMakeLists.txt").read_text(encoding="utf-8")
        if re.search(rf"install\s*\(\s*TARGETS\s+{re.escape(name)}\b", cmake):
            raise ContractError(f"{name} still has an install() rule")
    plugin_rows = {
        row["artifact"]: row
        for row in product.get("surfaces", [])
        if row.get("kind") == "plugin" and row.get("artifact")
    }
    for name in RETIRED_PLUGINS:
        row = plugin_rows[name]
        if row.get("artifact_scope") != "build":
            raise ContractError(f"{name} must be build-only in the product ledger")
        if row.get("profiles", {}).get("loupe-release") != "absent":
            raise ContractError(f"{name} must be absent from the loupe-release profile")
        cmake = (root / "LoupeEditorPlugins" / name / "CMakeLists.txt").read_text(encoding="utf-8")
        if re.search(rf"install\s*\(\s*TARGETS\s+{re.escape(name)}\b", cmake):
            raise ContractError(f"{name} still has an install() rule")
    root_cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    for option in BUILD_DEFAULTS:
        needle = f"set(_LOUPE_BUILD_{option}_DEFAULT OFF)"
        if needle not in root_cmake:
            raise ContractError(f"missing {needle}")
    packaging = product["packaging"]
    for profile in ("developer", "loupe-release"):
        apps = list(packaging["appx_applications"][profile])
        if apps != ["LoupeEditor"]:
            raise ContractError(f"{profile} AppX applications must be only LoupeEditor, found {apps}")
        desktop = packaging["desktop_entries"][profile]
        if any(name in entry for entry in desktop for name in SECONDARY_EXECUTABLES):
            raise ContractError(f"{profile} desktop entries still name a secondary executable")


def main() -> int:
    try:
        cmake_text = EDITOR_CMAKE.read_text(encoding="utf-8")
        editor_links = linked_libraries(extract_target_link_block(cmake_text, "LoupeEditor"))

        forbidden = sorted(FORBIDDEN_EDITOR_LIBS.intersection(editor_links))
        if forbidden:
            raise ContractError(f"LoupeEditor links forbidden Widgets graph: {', '.join(forbidden)}")

        missing = sorted(REQUIRED_EDITOR_LIBS - editor_links)
        if missing:
            raise ContractError(f"LoupeEditor missing required Quick links: {', '.join(missing)}")

        if not re.search(r"install\s*\([^)]*\bLoupeEditor\b", cmake_text, re.DOTALL):
            raise ContractError("LoupeEditor must remain an installed product target")

        validate_sole_interactive_product(ROOT)

    except (ContractError, OSError, KeyError, json.JSONDecodeError) as exc:
        print(f"Installed product graph FAILED: {exc}", file=sys.stderr)
        return 1

    print(
        "Installed product graph verified: LoupeEditor=Quick-only sole interactive install; "
        "PdfTool remains the installed CLI"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
