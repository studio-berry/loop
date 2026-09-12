#!/usr/bin/env python3
"""Guard that the GUI computes no preflight pass or severity logic.

Issue #195, "No pass/severity logic is computed in GUI code". Core owns the
truth: `pdf::reducePreflightVerdict()` yields the verdict state,
`pdfinteraction::PreflightController` owns the state machine, and
`pdfquick::tokens::resolvePreflightStateVisual()` maps Core's own state name onto
the canonical #194 treatment. The GUI renders that answer; it never derives one.

This check therefore fails when the QML or the Editor presentation layer:

  * reads Core's completeness flag (`inspectionComplete`),
  * aggregates findings (`errors.length`, `errors.isEmpty()`, `warnings.length`,
    `warnings.isEmpty()`) to choose copy, colour or state,
  * writes a pass/fail verdict literal instead of rendering the canonical state
    visual (uppercase `PASS`/`FAIL` copy; lowercase Core state names such as
    `"running"` are data, not verdicts, and are not flagged).

Scope: `ProductQuickAccessibilitySmoke/qml/**`, `ProductQuickAccessibilitySmoke/
main.cpp`, `LoopEditor/qml/**` and `LoopEditor/editorhost.cpp`. The whole of
editorhost.cpp is in scope on purpose: it is the QML-facing host, and it holds
no Core reducer call (`grep -n reducePreflightVerdict LoopEditor/editorhost.cpp`
is empty), so the presentation half and the file are the same thing today.

The allowed surfaces are named below with the reason each is legitimate. The
list is deliberately not a line-level suppression: nothing on it can match a
forbidden pattern, and the unit test asserts exactly that.
"""

from __future__ import annotations

import fnmatch
import re
import sys
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]

# The GUI layers. `*` also covers a nested directory under either qml root.
SCOPE_PATTERNS = (
    "ProductQuickAccessibilitySmoke/qml/*.qml",
    "ProductQuickAccessibilitySmoke/main.cpp",
    "LoopEditor/qml/*.qml",
    "LoopEditor/editorhost.cpp",
)
SCOPE_DIRECTORIES = (
    "ProductQuickAccessibilitySmoke/qml",
    "LoopEditor/qml",
)
SCOPE_FILES = (
    "ProductQuickAccessibilitySmoke/main.cpp",
    "LoopEditor/editorhost.cpp",
)


@dataclass(frozen=True)
class Rule:
    id: str
    pattern: re.Pattern[str]
    detail: str


@dataclass(frozen=True)
class Violation:
    path: str
    line: int
    rule: str
    detail: str

    def format(self) -> str:
        return f"{self.path}:{self.line}: {self.rule}: {self.detail}"


RULES = (
    Rule(
        "gui-derives-completeness",
        re.compile(r"\binspectionComplete\b"),
        "reads Core's completeness flag; only pdf::reducePreflightVerdict() may judge an inspection complete",
    ),
    Rule(
        "gui-aggregates-findings",
        re.compile(r"\b(?:errors|warnings)\s*\.\s*(?:length|count|isEmpty|size)\b"),
        "aggregates finding counts into a verdict; severity is PreflightController's to report",
    ),
    Rule(
        "gui-derives-pass-fail-copy",
        re.compile(r"""["']\s*(?:PASS|FAIL|PASSED|FAILED)\s*["']"""),
        "writes a pass/fail verdict literal; render preflightStateVisual / preflightOperatorSummary instead",
    ),
)

# Every surface a GUI file is allowed to read, with the reason it is legitimate:
# each one is Core's own answer, already resolved by LoopLibCore / LoopLibQuick /
# LoopLibInteraction. Reading them is rendering; interpreting them is not, and
# the rules above catch the interpretation.
ALLOWED_SURFACES = {
    "preflightOperatorSummary": "Core's own operator wording (PreflightController::operatorSummary)",
    "preflightStateName": "Core's own state name (PreflightController::State rendered by preflightStateToString)",
    "preflightStateVisual": "the canonical #194 treatment resolved by pdfquick::tokens::resolvePreflightStateVisual",
    "preflightStateColor": "the same visual's colour, looked up by role in LoopLibQuick's tokens",
    "preflightProfiles": "the profile enumeration Core resolved, with its own validity diagnostics",
    "preflightVariables": "the selected profile's variable bindings as Core resolved them",
    "selectedPreflightProfileId": "the controller's current profile selection",
    "hasPreflightReport": "the controller's report-presence flag",
    "runPreflight": "a preflight Q_INVOKABLE command; it asks Core, it does not decide",
    "cancelPreflight": "a preflight Q_INVOKABLE command",
    "selectPreflightProfile": "a preflight Q_INVOKABLE command",
    "setPreflightVariable": "a preflight Q_INVOKABLE command",
    "requestPreflightReportExport": "a preflight Q_INVOKABLE command",
    "exportPreflightReportFileUrl": "a preflight Q_INVOKABLE command; it writes Core's serialized report",
}


def relative(path: Path) -> str:
    return path.relative_to(ROOT).as_posix()


def in_scope(path: str) -> bool:
    return any(fnmatch.fnmatchcase(path, pattern) for pattern in SCOPE_PATTERNS)


def gui_sources() -> list[str]:
    paths: list[str] = []
    for directory in SCOPE_DIRECTORIES:
        root = ROOT / directory
        if root.is_dir():
            paths.extend(sorted(relative(path) for path in root.rglob("*.qml") if path.is_file()))
    for name in SCOPE_FILES:
        if (ROOT / name).is_file():
            paths.append(name)
    return sorted(path for path in paths if in_scope(path))


def scan_text(path: str, text: str) -> list[Violation]:
    """Report every forbidden pattern in `text`, as `path:line: rule`."""
    violations: list[Violation] = []
    for number, line in enumerate(text.splitlines(), start=1):
        for rule in RULES:
            if rule.pattern.search(line):
                violations.append(Violation(path, number, rule.id, rule.detail))
    return violations


def scan_tree() -> list[Violation]:
    violations: list[Violation] = []
    for name in gui_sources():
        violations.extend(scan_text(name, (ROOT / name).read_text(encoding="utf-8", errors="replace")))
    return violations


def main() -> int:
    sources = gui_sources()
    if not sources:
        print("ERROR: GUI preflight-truth guard scanned no files", file=sys.stderr)
        return 1

    violations = scan_tree()
    if violations:
        print("ERROR: GUI preflight-truth guard failed:", file=sys.stderr)
        for violation in violations:
            print(violation.format(), file=sys.stderr)
        return 1

    print(
        f"GUI preflight-truth guard passed: {len(sources)} GUI file(s) render Core's preflight verdict "
        "and derive none of it."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
