#!/usr/bin/env python3
"""Verify the Loop release profile never requires or ships Qt6::Widgets."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CMAKE = ROOT / "CMakeLists.txt"
FORBIDDEN_QT_NAME_PREFIXES = (
    "qt6widgets",
    "qt6quickwidgets",
    "qtwidgets",
    "qtquickwidgets",
    "libqt6widgets",
    "libqt6quickwidgets",
    "qt6printsupport",
    "qtprintsupport",
    "libqt6printsupport",
    "libqtprintsupport",
)
FORBIDDEN_CACHE_MARKERS = ()
REQUIRED_CACHE_MARKERS = (
    "LOOP_LOOP_DISTRIBUTION:BOOL=ON",
    "LOOP_CONFIGURE_REQUIRES_WIDGETS:INTERNAL=OFF",
)
FORBIDDEN_OPTION_MARKERS = (
    "LOOP_BUILD_CODE_GENERATOR:BOOL=ON",
    "LOOP_BUILD_JBIG2_VIEWER:BOOL=ON",
    "LOOP_BUILD_EXAMPLE_GENERATOR:BOOL=ON",
    "LOOP_BUILD_CANVAS_BENCHMARK:BOOL=ON",
)
REQUIRED_QT_CONFIGS = (
    "Qt6/Qt6Config.cmake",
    "Qt6Core/Qt6CoreConfig.cmake",
    "Qt6Gui/Qt6GuiConfig.cmake",
    "Qt6LinguistTools/Qt6LinguistToolsConfig.cmake",
    "Qt6Svg/Qt6SvgConfig.cmake",
    "Qt6TextToSpeech/Qt6TextToSpeechConfig.cmake",
    "Qt6Xml/Qt6XmlConfig.cmake",
    "Qt6Sql/Qt6SqlConfig.cmake",
    "Qt6Concurrent/Qt6ConcurrentConfig.cmake",
    "Qt6Qml/Qt6QmlConfig.cmake",
    "Qt6Quick/Qt6QuickConfig.cmake",
    "Qt6QuickControls2/Qt6QuickControls2Config.cmake",
    "Qt6QuickDialogs2/Qt6QuickDialogs2Config.cmake",
    "Qt6Test/Qt6TestConfig.cmake",
)


class ContractError(ValueError):
    pass


def _truncate_command_output(detail: str, limit: int = 800) -> str:
    detail = detail.strip()
    if len(detail) <= limit:
        return detail
    head = limit // 2
    tail = limit - head - len(" ... [truncated] ... ")
    return f"{detail[:head]} ... [truncated] ... {detail[-tail:]}"


def validate_cmake_release_profile() -> None:
    text = CMAKE.read_text(encoding="utf-8")
    if "set(_LOOP_REQUIRES_WIDGETS OFF)" not in text:
        raise ContractError("CMake must define _LOOP_REQUIRES_WIDGETS gating")
    if "elseif(_LOOP_REQUIRES_WIDGETS)" not in text:
        raise ContractError("CMake must gate find_package(Qt6 Widgets) behind _LOOP_REQUIRES_WIDGETS")
    if "REQUIRED COMPONENTS Core Gui Svg Xml Sql TextToSpeech Concurrent" not in text:
        raise ContractError("Widgets-free configure must not request Qt6::PrintSupport")
    release_regex = re.search(
        r"if\(LOOP_LOOP_DISTRIBUTION\)\s*"
        r"set\(_LOOP_QT_DLL_REGEX \"([^\"]+)\"",
        text,
    )
    if release_regex is None:
        raise ContractError("CMake must define a release-profile Qt install regex")
    if "Qt6Widgets" in release_regex.group(1) or "Qt6PrintSupport" in release_regex.group(1):
        raise ContractError("release-profile Qt install regex must omit Widgets-bound Qt modules")
    if "set(_LOOP_BUILD_CODE_GENERATOR_DEFAULT OFF)" not in text:
        raise ContractError("LOOP_LOOP_DISTRIBUTION must default developer Widgets tools OFF")


def _is_forbidden_qt_name(name: str) -> bool:
    return name.casefold().startswith(FORBIDDEN_QT_NAME_PREFIXES)


def validate_qt_prefix(qt_prefix: Path) -> None:
    qt_prefix = qt_prefix.resolve()
    if not qt_prefix.is_dir():
        raise ContractError(f"Widgets-free Qt prefix does not exist: {qt_prefix}")

    forbidden: list[str] = []
    for directory, dirnames, filenames in os.walk(qt_prefix):
        directory_path = Path(directory)
        for name in dirnames + filenames:
            if _is_forbidden_qt_name(name):
                forbidden.append(str((directory_path / name).relative_to(qt_prefix)))
    if forbidden:
        raise ContractError(f"Widgets-free Qt prefix contains forbidden paths: {sorted(forbidden)[:5]}")

    cmake_root = qt_prefix / "lib" / "cmake"
    missing = [relative for relative in REQUIRED_QT_CONFIGS if not (cmake_root / relative).is_file()]
    if missing:
        raise ContractError(f"Widgets-free Qt prefix is missing required Qt configs: {missing[:5]}")


def _cache_value(text: str, key: str) -> str | None:
    prefix = f"{key}:"
    for line in text.splitlines():
        if line.startswith(prefix):
            return line.split("=", 1)[1] if "=" in line else ""
    return None


def validate_cmake_cache(cache_path: Path, qt_prefix: Path | None = None) -> None:
    if not cache_path.is_file():
        raise ContractError(f"CMake cache does not exist: {cache_path}")

    text = cache_path.read_text(encoding="utf-8", errors="replace")
    hits = [marker for marker in FORBIDDEN_CACHE_MARKERS if marker in text]
    if hits:
        raise ContractError(f"release configure pulled Qt6::Widgets into cache: {hits[:3]}")

    missing = [marker for marker in REQUIRED_CACHE_MARKERS if marker not in text]
    if missing:
        raise ContractError(f"release configure cache missing required markers: {missing[:3]}")

    for option in FORBIDDEN_OPTION_MARKERS:
        if option in text:
            raise ContractError(f"Widgets-bound qualification target enabled in release probe: {option}")

    if qt_prefix is not None:
        expected_qt_dir = (qt_prefix.resolve() / "lib" / "cmake" / "Qt6").resolve()
        actual_qt_dir = _cache_value(text, "Qt6_DIR")
        if not actual_qt_dir:
            raise ContractError("release configure cache does not record Qt6_DIR")
        try:
            actual_qt_dir_path = Path(actual_qt_dir).resolve()
        except OSError as exc:
            raise ContractError(f"invalid Qt6_DIR in release configure cache: {actual_qt_dir}") from exc
        if os.path.normcase(str(actual_qt_dir_path)) != os.path.normcase(str(expected_qt_dir)):
            raise ContractError(
                "release configure did not use the filtered Qt prefix: "
                f"expected {expected_qt_dir}, got {actual_qt_dir_path}"
            )


def run_release_profile_configure(
    build_dir: Path,
    cmake_args: list[str],
    qt_prefix: Path | None = None,
    expect_failure: bool = False,
) -> None:
    build_dir = build_dir.resolve()
    build_dir.mkdir(parents=True, exist_ok=True)

    command = [
        "cmake",
        "-S",
        str(ROOT),
        "-B",
        str(build_dir),
        "-DLOOP_LOOP_DISTRIBUTION=ON",
        "-DLOOP_INSTALL_QT_DEPENDENCIES=0",
        "-DCMAKE_BUILD_TYPE=Release",
        *cmake_args,
    ]
    if qt_prefix is not None:
        qt_prefix = qt_prefix.resolve()
        command.extend(
            [
                f"-DCMAKE_PREFIX_PATH={qt_prefix}",
                f"-DQt6_DIR={qt_prefix / 'lib' / 'cmake' / 'Qt6'}",
                f"-DLOOP_QT_ROOT={qt_prefix}",
            ]
        )
    completed = subprocess.run(
        command,
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    detail = (completed.stdout + completed.stderr).strip()

    def diagnostic() -> str:
        # CMake/vcpkg often puts the actionable error at the end of a long
        # configure transcript.  Keep both ends so CI failures are useful
        # without requiring privileged access to the runner log archive.
        limit = 8000
        if len(detail) <= limit:
            return detail
        head = detail[:1500]
        tail = detail[-(limit - len(head) - 80) :]
        return f"{head}\n... [configure output truncated] ...\n{tail}"

    if expect_failure:
        if completed.returncode == 0:
            raise ContractError("Widgets-bound configure unexpectedly succeeded with Widgets unavailable")
        if "widgets" not in detail.casefold() and "qt6widgets" not in detail.casefold():
            raise ContractError(
                "Widgets-bound configure failed for an unrelated reason; expected a Widgets discovery error: "
                f"{_truncate_command_output(detail)}"
            )
        return

    if completed.returncode != 0:
        raise ContractError(f"release-profile configure failed: {_truncate_command_output(detail)}")

    validate_cmake_cache(build_dir / "CMakeCache.txt", qt_prefix)


def scan_install_tree(install_root: Path) -> list[str]:
    hits: list[str] = []
    if not install_root.is_dir():
        raise ContractError(f"install root does not exist: {install_root}")
    for path in install_root.rglob("*"):
        if not path.is_file():
            continue
        name = path.name
        if _is_forbidden_qt_name(name):
            hits.append(str(path))
    return hits


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--install-dir", type=Path, default=None, help="Optional installed tree to scan")
    parser.add_argument("--qt-prefix", type=Path, default=None, help="Widgets-free Qt prefix to validate and use")
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
        "--expect-configure-failure",
        action="store_true",
        help="Require --configure to fail specifically while discovering Qt Widgets",
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
        if args.qt_prefix is not None:
            validate_qt_prefix(args.qt_prefix)
        if args.cmake_cache is not None:
            validate_cmake_cache(args.cmake_cache.resolve(), args.qt_prefix)
        if args.configure:
            if args.expect_configure_failure and args.qt_prefix is None:
                raise ContractError("--expect-configure-failure requires --qt-prefix")
            if args.build_dir is not None:
                run_release_profile_configure(
                    args.build_dir.resolve(),
                    cmake_args,
                    args.qt_prefix,
                    args.expect_configure_failure,
                )
            else:
                with tempfile.TemporaryDirectory(prefix="loop-widgets-free-") as tmp:
                    run_release_profile_configure(
                        Path(tmp),
                        cmake_args,
                        args.qt_prefix,
                        args.expect_configure_failure,
                    )
        elif args.expect_configure_failure:
            raise ContractError("--expect-configure-failure requires --configure")
        if args.install_dir is not None:
            forbidden = scan_install_tree(args.install_dir.resolve())
            if forbidden:
                raise ContractError(f"forbidden Widgets-bound Qt artifacts: {forbidden[:5]}")
    except (ContractError, OSError) as exc:
        print(f"Widgets-free release profile FAILED: {exc}", file=sys.stderr)
        return 1

    messages = ["Widgets-free release profile verified: CMake omits Widgets-bound Qt modules for loop-release"]
    if args.cmake_cache is not None:
        messages.append(f"cache {args.cmake_cache} contains no Widgets requirement")
    if args.configure:
        target = args.build_dir if args.build_dir is not None else "<temp>"
        if args.expect_configure_failure:
            messages.append(f"Widgets-bound configure probe {target} failed closed as expected")
        else:
            messages.append(f"configure probe {target} succeeded without Widgets-bound Qt modules")
    if args.qt_prefix is not None:
        messages.append(f"filtered Qt prefix {args.qt_prefix} contains no Widgets-bound Qt paths")
    if args.install_dir is not None:
        messages.append(f"install tree {args.install_dir} contains no Widgets-bound Qt artifacts")
    print("; ".join(messages))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
