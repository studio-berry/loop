#!/usr/bin/env python3
"""Guard the single product async-submission boundary.

The scheduler owns product-facing long-lived work.  This check deliberately
keeps the small set of pre-existing QtConcurrent call sites visible while the
#238 migration is in progress, but fails as soon as a new unmanaged launch is
added or an existing call site multiplies.  Scheduler internals, the bounded
PDFExecutionPolicy primitive, and tests are explicit allowlisted exceptions.
"""

from __future__ import annotations

import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE_SUFFIXES = {".cpp", ".h", ".hpp", ".cc", ".cxx"}
LAUNCH_PATTERNS = {
    "QtConcurrent::run": re.compile(r"\bQtConcurrent::run\s*\("),
    "QThread::create": re.compile(r"\bQThread::create\s*\("),
    "QThreadPool::start": re.compile(r"\bpool\s*->\s*start\s*\("),
    "std::thread": re.compile(r"\bstd::thread\b"),
}

# These are known product debts, not a permission to add more work to the
# files.  The count check makes the migration observable in every PR.
KNOWN_QTCONCURRENT_COUNTS = {
    "LoopLibCore/sources/pdfdiff.cpp": 1,
}

SCHEDULER_INTERNALS = {
    "LoopLibCore/sources/pdfjobscheduler.cpp",
    "LoopLibCore/sources/pdfjobscheduler.h",
}
BOUNDED_LOW_LEVEL = {
    "LoopLibCore/sources/pdfexecutionpolicy.cpp",
    "LoopLibCore/sources/pdfexecutionpolicy.h",
}


@dataclass(frozen=True)
class AsyncLaunch:
    path: str
    line: int
    kind: str
    text: str


def normalize(path: str) -> str:
    return path.replace("\\", "/")


def tracked_paths(root: Path = ROOT) -> list[str]:
    result = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z"],
        check=True,
        capture_output=True,
    )
    return [path for path in result.stdout.decode().split("\0") if path]


def scan_source_text(path: str, text: str) -> list[AsyncLaunch]:
    normalized = normalize(path)
    if Path(normalized).suffix.lower() not in SOURCE_SUFFIXES:
        return []

    launches: list[AsyncLaunch] = []
    for line_number, line in enumerate(text.splitlines(), start=1):
        for kind, pattern in LAUNCH_PATTERNS.items():
            if pattern.search(line):
                launches.append(AsyncLaunch(normalized, line_number, kind, line.strip()))
    return launches


def inspect_repository(root: Path = ROOT) -> tuple[list[AsyncLaunch], list[AsyncLaunch]]:
    """Return (new violations, known legacy launches)."""
    violations: list[AsyncLaunch] = []
    known_legacy: list[AsyncLaunch] = []
    observed_legacy: dict[str, int] = {}

    for raw_path in tracked_paths(root):
        path = normalize(raw_path)
        absolute = root / raw_path
        if not absolute.is_file():
            continue
        try:
            text = absolute.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue

        for launch in scan_source_text(path, text):
            if path.startswith("UnitTests/"):
                continue
            if path in SCHEDULER_INTERNALS or path in BOUNDED_LOW_LEVEL:
                continue
            if launch.kind == "QtConcurrent::run" and path in KNOWN_QTCONCURRENT_COUNTS:
                known_legacy.append(launch)
                observed_legacy[path] = observed_legacy.get(path, 0) + 1
                continue
            violations.append(launch)

    for path, expected in KNOWN_QTCONCURRENT_COUNTS.items():
        observed = observed_legacy.get(path, 0)
        if observed != expected:
            violations.append(AsyncLaunch(path, 0, "QtConcurrent::run", f"expected {expected}, found {observed}"))

    return violations, known_legacy


def main() -> int:
    try:
        violations, known_legacy = inspect_repository()
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"ERROR: unable to inspect async launch sites: {exc}", file=sys.stderr)
        return 1

    print(f"Known legacy unmanaged launches: {len(known_legacy)}")
    for launch in known_legacy:
        print(f"  {launch.path}:{launch.line}: {launch.text}")
    if violations:
        print("ERROR: new unmanaged async launch detected:", file=sys.stderr)
        for launch in violations:
            print(f"  {launch.path}:{launch.line}: {launch.kind}: {launch.text}", file=sys.stderr)
        return 1

    print("Unmanaged async source audit passed; #238 legacy migration debt remains explicit.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
