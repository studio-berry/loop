#!/usr/bin/env python3
"""Static isolation checks for loop-pdf-worker (#618).

Fails if the worker target sources initialize Sentry/crashpad, or if the IPC
allowlist drifts away from open/preflight/cancel/ping.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

WORKER_SOURCES = [
    ROOT / "PdfTool/loop-pdf-worker-main.cpp",
    ROOT / "PdfTool/pdfworkerruntime.cpp",
    ROOT / "PdfTool/pdfworkerruntime.h",
    ROOT / "PdfTool/pdfworkersandbox.cpp",
    ROOT / "PdfTool/pdfworkersandbox.h",
    ROOT / "PdfTool/pdfworkerprotocol.h",
]

FORBIDDEN = (
    re.compile(r"pdfsentry\.h"),
    re.compile(r"PDFSentrySession"),
    re.compile(r"sentry_init"),
    re.compile(r"crashpad", re.IGNORECASE),
    re.compile(r"loop_deploy_sentry_crashpad"),
)

PROTOCOL = ROOT / "PdfTool/pdfworkerprotocol.h"
CMAKE = ROOT / "PdfTool/CMakeLists.txt"
REQUIRED_OPS = {"ping", "open", "preflight", "cancel"}


def main() -> int:
    errors: list[str] = []
    for path in WORKER_SOURCES:
        if not path.is_file():
            errors.append(f"missing worker source {path.relative_to(ROOT)}")
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for pattern in FORBIDDEN:
            if pattern.search(text):
                errors.append(f"{path.relative_to(ROOT)} matches forbidden pattern {pattern.pattern}")

    cmake = CMAKE.read_text(encoding="utf-8", errors="replace")
    if "add_executable(loop-pdf-worker" not in cmake:
        errors.append("PdfTool/CMakeLists.txt does not define loop-pdf-worker")
    if re.search(r"loop_deploy_sentry_crashpad\(\s*loop-pdf-worker", cmake):
        errors.append("loop-pdf-worker must not deploy Sentry/crashpad")

    protocol = PROTOCOL.read_text(encoding="utf-8", errors="replace")
    ops = set(re.findall(r'QStringLiteral\("([a-z-]+)"\)', protocol))
    missing = REQUIRED_OPS - ops
    if missing:
        errors.append(f"worker allowlist missing ops: {sorted(missing)}")
    extras = ops - REQUIRED_OPS - {"unknown"}
    # PROTOCOL_VERSION and other literals may appear; only flag known expansions.
    banned_extras = extras & {"render-page", "transform", "sanitize", "extract-attachments"}
    if banned_extras:
        errors.append(f"worker allowlist includes non-prove-slice ops: {sorted(banned_extras)}")

    if errors:
        print("pdf worker isolation checks failed:", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1
    print("pdf worker isolation checks ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
