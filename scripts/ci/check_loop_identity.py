#!/usr/bin/env python3
"""Fail closed when an active source or packaging contract reintroduces the old name."""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path
from typing import Iterable, Mapping


ROOT = Path(__file__).resolve().parents[2]
LEGACY_PRODUCT_TOKEN = "lo" + "upe"
LEGACY_PRODUCT_PATTERN = re.compile(re.escape(LEGACY_PRODUCT_TOKEN), re.IGNORECASE)
TEXT_SUFFIXES = {
    ".c",
    ".cc",
    ".cmake",
    ".cpp",
    ".h",
    ".hpp",
    ".json",
    ".md",
    ".ps1",
    ".py",
    ".sh",
    ".toml",
    ".txt",
    ".wxs",
    ".yml",
    ".yaml",
    ".qml",
    ".desktop",
    ".xml",
}

# These records are historical evidence or immutable issue references. They
# are deliberately not used by the product, packaging, or CI runtime.
LEGACY_TOKEN_ALLOWLIST = frozenset(
    {
        "LoopLibCore/sources/pdfsettings.cpp",
        "changes/cdx-session-03-secondary-executables.md",
        "changes/cdx-session-05-widgets-libraries.md",
        "changes/cursor-fix-pr356-surface-profile-a5d8.md",
        "changes/cursor-gh-358-harvest-checklist-1ed5.md",
        "changes/cdx-" + LEGACY_PRODUCT_TOKEN + "-to-loop-rebrand.md",
        "docs/SESSION_07_PACKAGE_BOUNDARY.md",
        "docs/evidence/phase5-widgets-parity-evidence.json",
    }
)

ENTRYPOINT_SURFACES = {
    "LoopEditor/main.cpp": "LoopEditor",
    "PdfTool/main.cpp": "PdfTool",
    "CodeGenerator/main.cpp": "CodeGenerator",
    "JBIG2_Viewer/main.cpp": "Jbig2Viewer",
    "PdfExampleGenerator/main.cpp": "PdfExampleGenerator",
    "loop-preflight/tools/generate_fixtures.cpp": "LoopPreflightFixtureGenerator",
    "QuickShellSmoke/main.cpp": "QuickShellSmoke",
    "ProductQuickAccessibilitySmoke/main.cpp": "ProductQuickAccessibilitySmoke",
    "CanvasBenchmark/main.cpp": "CanvasBenchmark",
}


def tracked_paths(root: Path = ROOT) -> list[Path]:
    completed = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z"],
        check=True,
        capture_output=True,
    )
    return [root / item for item in completed.stdout.decode("utf-8").split("\0") if item]


def relative_path(root: Path, path: Path) -> str:
    return path.relative_to(root).as_posix()


def text_for(path: Path) -> str | None:
    if path.suffix.lower() not in TEXT_SUFFIXES:
        return None
    try:
        data = path.read_bytes()
    except OSError:
        return None
    if b"\0" in data:
        return None
    return data.decode("utf-8", errors="replace")


def legacy_token_findings(
    files: Mapping[str, str],
    allowlist: Iterable[str] = LEGACY_TOKEN_ALLOWLIST,
) -> list[str]:
    allowed = set(allowlist)
    findings: list[str] = []
    for path, content in sorted(files.items()):
        if path in allowed:
            continue
        for line_number, line in enumerate(content.splitlines(), 1):
            if LEGACY_PRODUCT_PATTERN.search(line):
                findings.append(f"{path}:{line_number}: legacy product token")
    return findings


def find_legacy_token_findings(root: Path = ROOT) -> list[str]:
    files: dict[str, str] = {}
    for path in tracked_paths(root):
        content = text_for(path)
        if content is not None:
            files[relative_path(root, path)] = content
    return legacy_token_findings(files)


def contract_findings(root: Path = ROOT) -> list[str]:
    findings = find_legacy_token_findings(root)

    def require(path_name: str, snippet: str, description: str) -> None:
        path = root / path_name
        content = path.read_text(encoding="utf-8")
        if snippet not in content:
            findings.append(f"{path_name}: missing {description}")

    for snippet, description in (
        ('set(LOOP_PRODUCT_NAME "Loop")', "canonical product name"),
        ('set(LOOP_ORGANIZATION_NAME "Loop")', "canonical organization name"),
        ('set(LOOP_ORGANIZATION_DOMAIN "io.github.mberrys")', "canonical organization domain"),
        ('set(LOOP_PACKAGE_ID "io.github.mberrys.Loop-pdf")', "canonical package id"),
    ):
        require("CMakeLists.txt", snippet, description)

    require(
        "LoopLibCore/sources/pdfapplicationidentity.cpp",
        "SetCurrentProcessExplicitAppUserModelID",
        "Windows AppUserModelID initialization",
    )
    require(
        "LoopLibCore/sources/pdfsettings.cpp",
        "migrateLegacySettings",
        "legacy settings migration",
    )

    for path_name, surface in ENTRYPOINT_SURFACES.items():
        require(
            path_name,
            f"initializeApplicationIdentity(pdf::PDFApplicationSurface::{surface})",
            f"identity initialization for {surface}",
        )

    for path in tracked_paths(root):
        path_name = relative_path(root, path)
        if path_name == "LoopLibCore/sources/pdfapplicationidentity.cpp":
            continue
        content = text_for(path)
        if content is not None and re.search(r"QCoreApplication::set(?:Organization|Application)Name", content):
            findings.append(f"{path_name}: direct application identity mutation")

    return sorted(set(findings))


def main() -> int:
    findings = contract_findings()
    if findings:
        print("Loop identity contract FAILED:", file=sys.stderr)
        for finding in findings:
            print(f"- {finding}", file=sys.stderr)
        return 1
    print("Loop identity contract passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
