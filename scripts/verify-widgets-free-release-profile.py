#!/usr/bin/env python3
"""Verify the Loupe release profile never requires or ships Qt6::Widgets."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CMAKE = ROOT / "CMakeLists.txt"
FORBIDDEN_WIDGETS_ARTIFACTS = (
    "Qt6Widgets.dll",
    "libQt6Widgets.so",
    "libQt6Widgets.so.6",
    "Qt6Widgets.dylib",
)
FORBIDDEN_CACHE_MARKERS = (
    "Qt6Widgets_DIR:",
    "Qt6Widgets_FOUND:",
    "Qt6Widgets_VERSION:",
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


def validate_cmake_cache(cache_path: Path) -> None:
    if not cache_path.is_file():
        raise ContractError(f"CMake cache does not exist: {cache_path}")

    text = cache_path.read_text(encoding="utf-8", errors="replace")
    hits = [marker for marker in FORBIDDEN_CACHE_MARKERS if marker in text]
    if hits:
        raise ContractError(f"release configure pulled Qt6::Widgets into cache: {hits[:3]}")

    distribution = re.search(r"^LOUPE_LOUPE_DISTRIBUTION:BOOL=(ON|OFF)$", text, re.MULTILINE)
    if distribution is None or distribution.group(1) != "ON":
        raise ContractError("CMake cache must be configured with LOUPE_LOUPE_DISTRIBUTION=ON")

    for option in (
        "LOUPE_BUILD_CODE_GENERATOR:BOOL=ON",
        "LOUPE_BUILD_JBIG2_VIEWER:BOOL=ON",
        "LOUPE_BUILD_EXAMPLE_GENERATOR:BOOL=ON",
        "LOUPE_BUILD_CANVAS_BENCHMARK:BOOL=ON",
    ):
        if option in text:
            raise ContractError(f"Widgets-bound qualification target enabled in release probe: {option}")


def run_release_profile_configure(build_dir: Path, cmake_args: list[str]) -> None:
    build_dir = build_dir.resolve()
    build_dir.mkdir(parents=True, exist_ok=True)

    command = [
        "cmake",
        "-S",
        str(ROOT),
        "-B",
        str(build_dir),
        "-DLOUPE_LOUPE_DISTRIBUTION=ON",
        "-DLOUPE_INSTALL_QT_DEPENDENCIES=0",
        "-DCMAKE_BUILD_TYPE=Release",
        *cmake_args,
    ]
    completed = subprocess.run(
        command,
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        detail = (completed.stderr or completed.stdout).strip()
        raise ContractError(f"release-profile configure failed: {detail[:800]}")

    validate_cmake_cache(build_dir / "CMakeCache.txt")


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
    parser.add_argument("--cmake-cache", type=Path, default=None, help="Validate an existing CMakeCache.txt")
    parser.add_argument(
        "--configure",
        action="store_true",
        help="Run cmake configure for the release profile and verify Qt6::Widgets is not required",
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=None,
        help="Build directory for --configure (default: temporary directory)",
    )
    parser.add_argument(
        "cmake_args",
        nargs=argparse.REMAINDER,
        help="Extra cmake arguments after -- (for example -DCMAKE_TOOLCHAIN_FILE=...)",
    )
    args = parser.parse_args()

    cmake_args = list(args.cmake_args)
    if cmake_args and cmake_args[0] == "--":
        cmake_args = cmake_args[1:]

    try:
        validate_cmake_release_profile()
        if args.cmake_cache is not None:
            validate_cmake_cache(args.cmake_cache.resolve())
        if args.configure:
            if args.build_dir is not None:
                run_release_profile_configure(args.build_dir.resolve(), cmake_args)
            else:
                with tempfile.TemporaryDirectory(prefix="loupe-widgets-free-") as tmp:
                    run_release_profile_configure(Path(tmp), cmake_args)
        if args.install_dir is not None:
            forbidden = scan_install_tree(args.install_dir.resolve())
            if forbidden:
                raise ContractError(f"forbidden Qt6Widgets artifacts: {forbidden[:5]}")
    except (ContractError, OSError) as exc:
        print(f"Widgets-free release profile FAILED: {exc}", file=sys.stderr)
        return 1

    messages = ["Widgets-free release profile verified: CMake gates Qt6::Widgets for loupe-release"]
    if args.cmake_cache is not None:
        messages.append(f"cache {args.cmake_cache} contains no Qt6Widgets requirement")
    if args.configure:
        target = args.build_dir if args.build_dir is not None else "<temp>"
        messages.append(f"configure probe {target} succeeded without Qt6Widgets")
    if args.install_dir is not None:
        messages.append(f"install tree {args.install_dir} contains no Qt6Widgets artifacts")
    print("; ".join(messages))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
