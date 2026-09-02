#!/usr/bin/env python3
"""Fail closed when deleted Phase 5 product surfaces re-enter maintained paths."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


DEFAULT_ROOT = Path(__file__).resolve().parents[2]
MAINTAINED_PREFIXES = (
    ".github/",
    "CMakeLists.txt",
    "WixInstaller/",
    "Desktop/",
    "README.md",
    "AGENTS.md",
    ".claude/",
    ".cursor/",
    "LoopEditor/",
    "LoopLibCore/",
    "LoopLibInteraction/",
    "LoopLibQuick/",
    "PdfTool/",
    "loop-ocr/",
    "scripts/",
    "UnitTests/",
)
CURRENT_DOCS = {
    "docs/ACCESSIBILITY_BASELINE.md",
    "docs/EDITOR_RECOVERY.md",
    "docs/JOB_SCHEDULER.md",
    "docs/LOOP_SHELL_CONTRACT.md",
    "docs/LOOP_WORKSPACES.md",
    "docs/PLATFORM_SUPPORT.md",
    "docs/REPO_MAP.md",
}
FORBIDDEN = (
    re.compile(r"\bLoopLibWidgets(?:/|\\|\b)"),
    re.compile(r"\bLoopLibGui(?:/|\\|\b)"),
    re.compile(r"\bLoop(?:Viewer|PageMaster|Diff|LaunchPad)(?:\.exe)?\b"),
)


def normalize(path: str) -> str:
    return path.replace("\\", "/")


def is_maintained(path: str) -> bool:
    normalized = normalize(path)
    # Contract fixtures and evidence generators must mention retired names in
    # order to prove their absence. They are validation authorities, not
    # product/build instructions.
    if normalized.startswith("scripts/ci/test_") or normalized.startswith("scripts/verify-"):
        return False
    if normalized in {
        "scripts/generate_phase5_widgets_evidence.py",
        "scripts/generate_widgets_library_consumer_graph.py",
    }:
        return False
    return normalized in CURRENT_DOCS or any(normalized.startswith(prefix) for prefix in MAINTAINED_PREFIXES)


def tracked_paths(root: Path) -> list[str]:
    import subprocess

    result = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z"],
        check=True,
        capture_output=True,
    )
    return [path for path in result.stdout.decode().split("\0") if path]


def violations(root: Path) -> list[tuple[str, int, str]]:
    findings: list[tuple[str, int, str]] = []
    for raw_path in tracked_paths(root):
        path = normalize(raw_path)
        if not is_maintained(path):
            continue
        file_path = root / raw_path
        try:
            data = file_path.read_bytes()
        except OSError:
            continue
        if b"\x00" in data:
            # Binary payload, not a source or build instruction. Nothing to scan.
            continue
        # Decode leniently: a maintained file saved in a non-UTF-8 encoding must
        # still be scanned rather than silently skipped, or the gate fails open.
        lines = data.decode("utf-8", errors="replace").splitlines()
        for line_number, line in enumerate(lines, 1):
            for pattern in FORBIDDEN:
                if pattern.search(line):
                    findings.append((path, line_number, line.strip()))
                    break
    return findings


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        findings = violations(root)
    except (OSError, ValueError) as exc:
        print(f"Phase 5 residue check FAILED: {exc}", file=sys.stderr)
        return 1
    if findings:
        print("Phase 5 residue check FAILED:", file=sys.stderr)
        for path, line_number, line in findings:
            print(f"  {path}:{line_number}: {line}", file=sys.stderr)
        return 1
    print("Phase 5 residue check passed: deleted product surfaces are absent from maintained paths.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
