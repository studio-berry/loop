#!/usr/bin/env python3
"""Verify installed LoopEditor is the Quick product graph and sole interactive install."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EDITOR_CMAKE = ROOT / "LoopEditor" / "CMakeLists.txt"
RETIRED_PLUGINS = frozenset(
    {
        "ActionListPlugin",
        "AudioBookPlugin",
        "DimensionsPlugin",
        "EditorPlugin",
        "LoopPreflightPlugin",
        "ObjectInspectorPlugin",
        "OcrPlugin",
        "OutputPreviewPlugin",
        "RedactPlugin",
        "ScannerPlugin",
        "SignaturePlugin",
        "SoftProofingPlugin",
    }
)

FORBIDDEN_EDITOR_LIBS = frozenset(
    {
        "LoopLibWidgets",
        "LoopLibGui",
        "Qt6::Widgets",
        "Qt6::QuickWidgets",
    }
)
REQUIRED_EDITOR_LIBS = frozenset(
    {
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
    if keep != ["LoopEditor"]:
        raise ContractError(f"installed KEEP applications must be only LoopEditor, found {keep}")
    cli = [row.get("artifact") for row in installed_apps if row.get("disposition") == "CLI-ONLY"]
    if cli != ["PdfTool"]:
        raise ContractError(f"installed CLI-ONLY applications must be only PdfTool, found {cli}")

    plugin_rows = {
        row["artifact"]: row
        for row in product.get("surfaces", [])
        if row.get("kind") == "plugin" and row.get("artifact")
    }
    for name in RETIRED_PLUGINS:
        row = plugin_rows[name]
        if row.get("artifact_scope") != "build":
            raise ContractError(f"{name} must be build-only in the product ledger")
        if row.get("profiles", {}).get("loop-release") != "absent":
            raise ContractError(f"{name} must be absent from the loop-release profile")
        cmake = root / "LoopEditorPlugins" / name / "CMakeLists.txt"
        if cmake.is_file() and re.search(
            rf"install\s*\(\s*TARGETS\s+{re.escape(name)}\b",
            cmake.read_text(encoding="utf-8"),
        ):
            raise ContractError(f"{name} still has an install() rule")

    packaging = product["packaging"]
    for profile in ("developer", "loop-release"):
        apps = list(packaging["appx_applications"][profile])
        if apps != ["LoopEditor"]:
            raise ContractError(f"{profile} AppX applications must be only LoopEditor, found {apps}")


def main() -> int:
    try:
        cmake_text = EDITOR_CMAKE.read_text(encoding="utf-8")
        editor_links = linked_libraries(extract_target_link_block(cmake_text, "LoopEditor"))

        forbidden = sorted(FORBIDDEN_EDITOR_LIBS.intersection(editor_links))
        if forbidden:
            raise ContractError(f"LoopEditor links forbidden Widgets graph: {', '.join(forbidden)}")

        missing = sorted(REQUIRED_EDITOR_LIBS - editor_links)
        if missing:
            raise ContractError(f"LoopEditor missing required Quick links: {', '.join(missing)}")

        if not re.search(r"install\s*\([^)]*\bLoopEditor\b", cmake_text, re.DOTALL):
            raise ContractError("LoopEditor must remain an installed product target")

        if not re.search(r"qt_add_qml_module\s*\(\s*LoopEditor\b", cmake_text):
            raise ContractError("LoopEditor must directly own the Loop.Quick QML module")
        if re.search(r"\bLoopEditorQuick\b", cmake_text):
            raise ContractError("retired LoopEditorQuick target remains in the product graph")
        if not re.search(r"qt_generate_deploy_qml_app_script\s*\(\s*TARGET\s+LoopEditor\b", cmake_text, re.DOTALL):
            raise ContractError("LoopEditor must generate its Qt QML deployment script")
        if not re.search(r"install\s*\(\s*SCRIPT\s+\$\{loop_editor_deploy_script\}", cmake_text):
            raise ContractError("LoopEditor deployment script must run during installation")

        validate_sole_interactive_product(ROOT)

    except (ContractError, OSError, KeyError, json.JSONDecodeError) as exc:
        print(f"Installed product graph FAILED: {exc}", file=sys.stderr)
        return 1

    print(
        "Installed product graph verified: LoopEditor=Quick-only sole interactive install; "
        "PdfTool remains the installed CLI"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
