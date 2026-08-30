#!/usr/bin/env python3
"""Create a relocatable Qt prefix with Widgets unavailable.

The hosted release jobs install a complete Qt distribution because the same
runner also executes the legacy qualification probes.  Session 06 needs a
stronger test: the release profile must still configure and build when the
Widgets module cannot be discovered at all.  This script creates a filtered
copy for that qualification build and never modifies the installed Qt prefix.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
from pathlib import Path


FORBIDDEN_NAME_PREFIXES = (
    "qt6widgets",
    "qt6quickwidgets",
    "qtwidgets",
    "qtquickwidgets",
    "libqt6widgets",
    "libqt6quickwidgets",
    "qt6printsupport",
    "libqt6printsupport",
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


class QualificationError(ValueError):
    """Raised when a Qt prefix cannot be used for qualification."""


def is_forbidden_name(name: str) -> bool:
    folded = name.casefold()
    return folded.startswith(FORBIDDEN_NAME_PREFIXES)


def forbidden_paths(root: Path) -> list[Path]:
    """Return paths whose names identify a Widgets module or artifact."""
    hits: list[Path] = []
    for directory, dirnames, filenames in os.walk(root):
        directory_path = Path(directory)
        for name in dirnames + filenames:
            if is_forbidden_name(name):
                hits.append(directory_path / name)
    return sorted(hits)


def required_config_paths(root: Path) -> list[Path]:
    cmake_root = root / "lib" / "cmake"
    return [cmake_root / relative for relative in REQUIRED_QT_CONFIGS]


def validate_prefix(root: Path) -> None:
    if not root.is_dir():
        raise QualificationError(f"Qt prefix does not exist: {root}")

    forbidden = forbidden_paths(root)
    if forbidden:
        preview = [str(path.relative_to(root)) for path in forbidden[:5]]
        raise QualificationError(f"filtered Qt prefix still contains Widgets paths: {preview}")

    missing = [str(path.relative_to(root)) for path in required_config_paths(root) if not path.is_file()]
    if missing:
        raise QualificationError(f"filtered Qt prefix is missing required Qt configs: {missing[:5]}")


def _relative(root: Path, path: Path) -> str:
    return path.relative_to(root).as_posix()


def stage_prefix(source: Path, destination: Path) -> dict[str, object]:
    source = source.resolve()
    destination = destination.resolve()

    if not source.is_dir():
        raise QualificationError(f"Qt source prefix does not exist: {source}")
    if destination.exists():
        raise QualificationError(f"destination already exists; refusing to overwrite: {destination}")
    if destination == source or destination in source.parents or source in destination.parents:
        raise QualificationError("Qt source and destination prefixes must be disjoint")

    excluded: list[str] = []

    def ignore(directory: str, names: list[str]) -> set[str]:
        directory_path = Path(directory)
        ignored = {name for name in names if is_forbidden_name(name)}
        excluded.extend(_relative(source, directory_path / name) for name in sorted(ignored))
        return ignored

    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(source, destination, symlinks=True, ignore=ignore)
    validate_prefix(destination)

    file_count = 0
    byte_count = 0
    for directory, _, filenames in os.walk(destination):
        for name in filenames:
            path = Path(directory) / name
            try:
                byte_count += path.stat().st_size
            except OSError as exc:
                raise QualificationError(f"unable to inspect staged Qt file {path}: {exc}") from exc
            file_count += 1

    return {
        "schema_version": 1,
        "kind": "loupe-widgets-free-qt-prefix",
        "source_prefix": str(source),
        "destination_prefix": str(destination),
        "forbidden_name_prefixes": list(FORBIDDEN_NAME_PREFIXES),
        "excluded_paths": sorted(excluded),
        "required_qt_configs": list(REQUIRED_QT_CONFIGS),
        "retained_file_count": file_count,
        "retained_byte_count": byte_count,
    }


def write_manifest(path: Path, manifest: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True, help="Complete installed Qt prefix")
    parser.add_argument("--destination", type=Path, required=True, help="New filtered Qt prefix")
    parser.add_argument("--manifest", type=Path, required=True, help="JSON manifest output path")
    args = parser.parse_args()

    try:
        manifest = stage_prefix(args.source, args.destination)
        write_manifest(args.manifest, manifest)
    except (OSError, QualificationError) as exc:
        print(f"Widgets-free Qt preparation FAILED: {exc}", file=sys.stderr)
        return 1

    print(
        "Widgets-free Qt prefix prepared: "
        f"excluded={len(manifest['excluded_paths'])} "
        f"retained_files={manifest['retained_file_count']} "
        f"manifest={args.manifest}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
