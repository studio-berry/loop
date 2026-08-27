#!/usr/bin/env python3
"""Verify installed LoupeEditor links the Quick product graph only (W-01)."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EDITOR_CMAKE = ROOT / "LoupeEditor" / "CMakeLists.txt"

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

    except (ContractError, OSError) as exc:
        print(f"Installed product graph FAILED: {exc}", file=sys.stderr)
        return 1

    print(
        "Installed product graph verified: LoupeEditor=Quick-only; "
        "no legacy Widgets comparison target"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
