#!/usr/bin/env python3
"""Keep the host-neutral interaction layer free of Widgets and QML dependencies.

The compile-time boundary is structural: LoopLibInteraction links neither
Qt6::Widgets nor Qt6::Qml/Quick, so Qt's per-module include paths are absent and
a forbidden include fails to build. This check stops the link edge from being
re-added, which is the only way that structural guarantee can be lost.

See docs/interaction-boundary-policy.json and architecture invariant I21.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
POLICY_PATH = ROOT / "docs" / "interaction-boundary-policy.json"

SOURCE_SUFFIXES = (".h", ".hpp", ".cpp", ".cc")

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]')
LINK_LIBRARIES_RE = re.compile(
    r"target_link_libraries\s*\(\s*(?P<target>[A-Za-z0-9_:]+)(?P<body>[^)]*)\)",
    re.MULTILINE,
)
LINK_KEYWORDS = frozenset({"PRIVATE", "PUBLIC", "INTERFACE"})


class ContractError(Exception):
    """A boundary rule was violated, or the policy itself is malformed."""


def load_policy(path: Path) -> dict:
    try:
        policy = json.loads(path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise ContractError(f"cannot read {path.name}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise ContractError(f"{path.name} is not valid JSON: {exc}") from exc

    if policy.get("schema_version") != 1:
        raise ContractError(
            f"{path.name} schema_version must be 1, got {policy.get('schema_version')!r}"
        )
    if not isinstance(policy.get("targets"), list) or not policy["targets"]:
        raise ContractError(f"{path.name} must declare a non-empty targets array")
    if not isinstance(policy.get("forbidden_includes"), list):
        raise ContractError(f"{path.name} must declare forbidden_includes")

    for entry in policy["targets"]:
        missing = sorted(
            {"name", "cmake", "sources", "linkage", "allowed_link_targets"} - entry.keys()
        )
        if missing:
            raise ContractError(
                f"{path.name} target {entry.get('name', '?')} is missing: {', '.join(missing)}"
            )
    return policy


def compile_forbidden(patterns: list[str]) -> list[re.Pattern[str]]:
    compiled = []
    for pattern in patterns:
        try:
            compiled.append(re.compile(pattern))
        except re.error as exc:
            raise ContractError(f"forbidden_includes entry {pattern!r} is not a regex: {exc}") from exc
    return compiled


def strip_cmake_comments(text: str) -> str:
    """Drop `#` comments so prose about a rule is not mistaken for a violation."""
    stripped_lines = []
    for line in text.splitlines():
        in_quotes = False
        cut = len(line)
        index = 0
        while index < len(line):
            character = line[index]
            if character == "\\" and in_quotes:
                index += 2
                continue
            if character == '"':
                in_quotes = not in_quotes
            elif character == "#" and not in_quotes:
                cut = index
                break
            index += 1
        stripped_lines.append(line[:cut])
    return "\n".join(stripped_lines)


def parse_link_targets(cmake_text: str, target_name: str) -> list[str]:
    """Return every library named in this target's target_link_libraries calls."""
    linked: list[str] = []
    for match in LINK_LIBRARIES_RE.finditer(cmake_text):
        if match.group("target") != target_name:
            continue
        for token in match.group("body").split():
            if token in LINK_KEYWORDS or token.startswith("$"):
                continue
            linked.append(token)
    return linked


def check_cmake(root: Path, entry: dict) -> list[str]:
    errors: list[str] = []
    name = entry["name"]
    cmake_path = root / entry["cmake"]
    try:
        text = strip_cmake_comments(cmake_path.read_text(encoding="utf-8"))
    except OSError as exc:
        return [f"{entry['cmake']}: cannot read ({exc})"]

    linkage = entry["linkage"]
    if not re.search(rf"add_library\s*\(\s*{re.escape(name)}\s+{re.escape(linkage)}\b", text):
        errors.append(
            f"{entry['cmake']}: {name} must be declared add_library({name} {linkage} ...). "
            f"A SHARED interaction library with exported headers becomes an accidental public ABI."
        )

    allowed = set(entry["allowed_link_targets"])
    for linked in parse_link_targets(text, name):
        if linked not in allowed:
            errors.append(
                f"{entry['cmake']}: {name} links {linked}, which is not in allowed_link_targets. "
                f"Linking a presentation module restores its include path and dissolves the "
                f"compile-time boundary."
            )

    if entry.get("installed") is False:
        if re.search(rf"install\s*\(\s*TARGETS[^)]*\b{re.escape(name)}\b", text):
            errors.append(f"{entry['cmake']}: {name} is declared non-installed but has an install(TARGETS ...) rule")
        if "GenerateExportHeader" in text or "GENERATE_EXPORT_HEADER" in text:
            errors.append(
                f"{entry['cmake']}: {name} is a non-installed static library and must not "
                f"generate an export header"
            )
    return errors


def check_sources(root: Path, entry: dict, forbidden: list[re.Pattern[str]]) -> list[str]:
    errors: list[str] = []
    sources_dir = root / entry["sources"]
    if not sources_dir.is_dir():
        return [f"{entry['sources']}: not a directory"]

    for path in sorted(sources_dir.rglob("*")):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        relative = path.relative_to(root).as_posix()
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            match = INCLUDE_RE.match(line)
            if not match:
                continue
            included = match.group(1)
            for pattern in forbidden:
                if pattern.search(included):
                    errors.append(
                        f"{relative}:{number}: forbidden include {included!r} "
                        f"(matches {pattern.pattern!r})"
                    )
                    break
    return errors


def validate_repository(root: Path, policy_path: Path) -> list[str]:
    policy = load_policy(policy_path)
    forbidden = compile_forbidden(policy["forbidden_includes"])

    errors: list[str] = []
    for entry in policy["targets"]:
        errors.extend(check_cmake(root, entry))
        errors.extend(check_sources(root, entry, forbidden))
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--policy",
        type=Path,
        default=POLICY_PATH,
        help="path to the interaction boundary policy (default: docs/interaction-boundary-policy.json)",
    )
    args = parser.parse_args()

    try:
        errors = validate_repository(ROOT, args.policy)
    except ContractError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        return 1

    print("interaction boundary ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
