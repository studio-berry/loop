#!/usr/bin/env python3
"""Verify the Loupe release profile never requires or ships Qt6::Widgets."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CMAKE = ROOT / "CMakeLists.txt"
FORBIDDEN_WIDGETS_ARTIFACTS = (
    "Qt6Widgets.dll",
    "libQt6Widgets.so",
    "libQt6Widgets.so.6",
    "Qt6Widgets.dylib",
)


class ContractError(ValueError):
    pass


def validate_cmake_release_profile() -> None:
    text = CMAKE.read_text(encoding="utf-8")
    if "set(_LOUPE_REQUIRES_WIDGETS OFF)" not in text:
        raise ContractError("CMake must define _LOUPE_REQUIRES_WIDGETS gating")
    if "elseif(_LOUPE_REQUIRES_WIDGETS)" not in text:
        raise ContractError("CMake must gate find_package(Qt6 Widgets) behind _LOUPE_REQUIRES_WIDGETS")
    if "LOUPE_LOUPE_DISTRIBUTION" not in text or "Qt6Widgets" not in text:
        raise ContractError("CMake must omit Qt6Widgets from release-profile Qt install regex")
    if "set(_LOUPE_BUILD_CODE_GENERATOR_DEFAULT OFF)" not in text:
        raise ContractError("LOUPE_LOUPE_DISTRIBUTION must default developer Widgets tools OFF")


def scan_install_tree(install_root: Path) -> list[str]:
    hits: list[str] = []
    if not install_root.is_dir():
        raise ContractError(f"install root does not exist: {install_root}")
    for path in install_root.rglob("*"):
        if not path.is_file():
            continue
        name = path.name
        if name in FORBIDDEN_WIDGETS_ARTIFACTS or name.startswith("Qt6Widgets."):
            hits.append(str(path))
    return hits


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--install-dir", type=Path, default=None, help="Optional installed tree to scan")
    args = parser.parse_args()

    try:
        validate_cmake_release_profile()
        if args.install_dir is not None:
            forbidden = scan_install_tree(args.install_dir.resolve())
            if forbidden:
                raise ContractError(f"forbidden Qt6Widgets artifacts: {forbidden[:5]}")
    except (ContractError, OSError) as exc:
        print(f"Widgets-free release profile FAILED: {exc}", file=sys.stderr)
        return 1

    if args.install_dir is not None:
        print(
            f"Widgets-free release profile verified: CMake gates Qt6::Widgets; "
            f"install tree {args.install_dir} contains no Qt6Widgets artifacts"
        )
    else:
        print("Widgets-free release profile verified: CMake gates Qt6::Widgets for loupe-release")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
