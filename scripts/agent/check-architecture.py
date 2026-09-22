#!/usr/bin/env python3
"""Check Loop architecture contracts, proof lanes, fixtures, and quality budgets."""

from __future__ import annotations

import argparse
import fnmatch
import json
import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from yaml_subset import load_yaml

ROOT = Path(__file__).resolve().parents[2]
ARCH = ROOT / "architecture"

LANE_KINDS = (
    "unit",
    "integration",
    "fixture",
    "differential",
    "security",
    "architecture",
    "packaging",
    "sealed-eval",
)
SOURCE_SUFFIXES = {".h", ".hh", ".hpp", ".cpp", ".cc", ".cxx"}
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]')
LINK_RE = re.compile(
    r"target_link_libraries\s*\(\s*(?P<target>[A-Za-z0-9_:]+)(?P<body>[^)]*)\)",
    re.MULTILINE,
)
LINK_KEYWORDS = frozenset({"PRIVATE", "PUBLIC", "INTERFACE"})
EXECUTABLE_RE = re.compile(
    r"add_executable\(\s*(UnitTests[A-Za-z0-9_]*)\b(?P<body>.*?)\)",
    re.DOTALL,
)
BARE_TEST_RE = re.compile(r"(?<![\w./])(tst_[A-Za-z0-9_]+\.cpp)")
CMAKE_TEST_RE = re.compile(r"\$\{CMAKE_SOURCE_DIR\}/(UnitTests/[A-Za-z0-9_./+-]+\.cpp)")
SKIP_DIRS = {"build", "vcpkg", ".git"}


def load_yaml_text(text: str):
    return load_yaml(text)


def read_yaml(path: Path):
    return load_yaml_text(path.read_text(encoding="utf-8"))


def load_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


def match_any(path: str, patterns: list[str]) -> bool:
    return any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns)


def strip_cmake_comments(text: str) -> str:
    kept: list[str] = []
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
        kept.append(line[:cut])
    return "\n".join(kept)


def link_tokens(cmake_text: str, target: str) -> list[str]:
    tokens: list[str] = []
    for match in LINK_RE.finditer(strip_cmake_comments(cmake_text)):
        if match.group("target") != target:
            continue
        for token in match.group("body").split():
            if token in LINK_KEYWORDS or token.startswith("$"):
                continue
            tokens.append(token)
    return tokens


def code_text(text: str) -> str:
    lines: list[str] = []
    for line in text.splitlines():
        marker = line.find("//")
        if marker >= 0:
            line = line[:marker]
        lines.append(line)
    return "\n".join(lines)


def iter_sources(root: Path, relative: str):
    path = root / relative
    if path.is_file():
        yield path
        return
    if not path.is_dir():
        return
    for child in path.rglob("*"):
        if child.suffix.lower() in SOURCE_SUFFIXES and SKIP_DIRS.isdisjoint(child.parts):
            yield child


def include_hits(text: str, patterns: list[re.Pattern[str]]) -> list[str]:
    hits: list[str] = []
    for line in text.splitlines():
        match = INCLUDE_RE.match(line)
        if not match:
            continue
        header = match.group(1)
        for pattern in patterns:
            if pattern.search(header):
                hits.append(header)
    return hits


def compile_patterns(patterns: list[str], label: str, errors: list[str]) -> list[re.Pattern[str]]:
    compiled: list[re.Pattern[str]] = []
    for pattern in patterns:
        try:
            compiled.append(re.compile(pattern))
        except re.error as exc:
            errors.append(f"{label}: invalid include pattern {pattern!r}: {exc}")
    return compiled


def subsystem_patterns(spec: dict, policy: dict) -> list[str]:
    patterns = list(spec.get("paths") or [])
    ref = spec.get("paths_from")
    if ref:
        module = str(ref).split(":", 1)[1]
        patterns.extend(policy["module_boundaries"][module]["paths"])
    return patterns


def parse_executables(root: Path, sources: list[str]) -> dict[str, list[str]]:
    text = "\n".join(strip_cmake_comments((root / relative).read_text(encoding="utf-8")) for relative in sources)
    found: dict[str, list[str]] = {}
    for match in EXECUTABLE_RE.finditer(text):
        body = match.group("body")
        tests = {f"UnitTests/{name}" for name in BARE_TEST_RE.findall(body)}
        tests.update(CMAKE_TEST_RE.findall(body))
        found[match.group(1)] = sorted(tests)
    return found


def branch_slug(branch: str) -> str:
    slug = re.sub(r"[^A-Za-z0-9._-]+", "-", branch.strip()).strip("-.")
    return slug or "change"


def current_branch(override: str | None) -> str:
    if override:
        return override
    for key in ("GITHUB_HEAD_REF", "CI_HEAD_BRANCH"):
        if os.environ.get(key):
            return os.environ[key]
    completed = subprocess.run(
        ["git", "-C", str(ROOT), "symbolic-ref", "--short", "HEAD"],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode == 0:
        return completed.stdout.strip()
    return "detached"


def integration_branches(policy: dict) -> set[str]:
    branches = policy.get("branches", {})
    names = set(branches.get("protected") or [])
    for key in ("integration", "qualification", "release", "default"):
        value = branches.get(key)
        if isinstance(value, str) and value:
            names.add(value)
    return names


def skip_evidence_reason(branch: str, policy: dict, skip: bool) -> str | None:
    if skip:
        return "non-PR event"
    event = os.environ.get("GITHUB_EVENT_NAME")
    if event and event != "pull_request":
        return "non-PR event"
    if branch in integration_branches(policy):
        return "integration branch"
    return None


def git_output(args: list[str]) -> str:
    completed = subprocess.run(
        ["git", "-C", str(ROOT), *args],
        check=True,
        capture_output=True,
        text=True,
    )
    return completed.stdout


def changed_paths(base: str, head: str) -> list[str]:
    base_sha = git_output(["rev-parse", "--verify", f"{base}^{{commit}}"]).strip()
    head_sha = git_output(["rev-parse", "--verify", f"{head}^{{commit}}"]).strip()
    merge_base = git_output(["merge-base", base_sha, head_sha]).strip()
    raw = subprocess.run(
        ["git", "-C", str(ROOT), "diff", "--name-only", "-z", f"{merge_base}..{head_sha}"],
        check=True,
        capture_output=True,
    )
    return [path for path in raw.stdout.decode("utf-8", errors="surrogateescape").split("\0") if path]


def check_boundaries(root: Path, document: dict) -> list[str]:
    errors: list[str] = []
    allow = set(document.get("widgets_link_allow") or [])
    tokens = set(document.get("widgets_link_tokens") or [])
    for path in root.rglob("*"):
        if not path.is_file() or path.suffix not in {".txt", ".cmake"}:
            continue
        if SKIP_DIRS.intersection(path.parts):
            continue
        if path.name not in {"CMakeLists.txt"} and path.suffix != ".cmake":
            continue
        relative = path.relative_to(root).as_posix()
        text = strip_cmake_comments(path.read_text(encoding="utf-8"))
        present = []
        for match in LINK_RE.finditer(text):
            for token in match.group("body").split():
                if token in tokens:
                    present.append(token)
        if present and relative not in allow:
            errors.append(
                f"boundaries: {relative} links {sorted(set(present))}, which is outside widgets_link_allow"
            )

    for layer in document.get("layers") or []:
        label = f"boundaries:{layer.get('id')}"
        cmake_path = root / layer["cmake"]
        if not cmake_path.is_file():
            errors.append(f"{label}: missing {layer['cmake']}")
            continue
        linked = link_tokens(cmake_path.read_text(encoding="utf-8"), layer["target"])
        if not linked:
            errors.append(f"{label}: no link edges for {layer['target']}")
        allowed = set(layer.get("may_link") or [])
        unexpected = sorted(set(linked) - allowed)
        if unexpected:
            errors.append(f"{label}: unexpected link {', '.join(unexpected)}")
        forbidden_widgets = sorted(set(linked) & tokens)
        if forbidden_widgets:
            errors.append(f"{label}: forbidden widget link {', '.join(forbidden_widgets)}")
        patterns = compile_patterns(layer.get("forbidden_includes") or [], label, errors)
        for source in iter_sources(root, layer["sources"]):
            hits = include_hits(source.read_text(encoding="utf-8", errors="replace"), patterns)
            for hit in hits:
                errors.append(f"{label}: {source.relative_to(root).as_posix()} includes {hit}")

    for separation in document.get("separations") or []:
        label = f"boundaries:{separation.get('id')}"
        patterns = compile_patterns(separation.get("forbidden_includes") or [], label, errors)
        forbidden_tokens = separation.get("forbidden_tokens") or []
        for relative in separation.get("paths") or []:
            sources = list(iter_sources(root, relative))
            if not sources and not (root / relative).exists():
                errors.append(f"{label}: missing {relative}")
            for source in sources:
                text = source.read_text(encoding="utf-8", errors="replace")
                for hit in include_hits(text, patterns):
                    errors.append(
                        f"{label}: {source.relative_to(root).as_posix()} includes {hit}"
                    )
                body = code_text(text)
                for token in forbidden_tokens:
                    if token in body:
                        errors.append(
                            f"{label}: {source.relative_to(root).as_posix()} uses {token}"
                        )
    return errors


def check_ownership(root: Path, document: dict) -> list[str]:
    errors: list[str] = []
    for surface in document.get("surfaces") or []:
        label = f"ownership:{surface.get('id')}"
        basenames = []
        for relative in surface.get("paths") or []:
            if not (root / relative).exists():
                errors.append(f"{label}: missing {relative}")
            basenames.append(Path(relative).name)
        absent = surface.get("absent_tree")
        if absent and (root / absent).exists():
            errors.append(f"{label}: {absent} exists; interactive plugins are not a core or tool tree")
        for relative in surface.get("forbid_basenames_under") or []:
            candidate = root / relative
            targets = [candidate] if candidate.is_file() else list(candidate.rglob("*")) if candidate.is_dir() else []
            for path in targets:
                if path.is_file() and path.name in basenames:
                    errors.append(
                        f"{label}: duplicate {path.relative_to(root).as_posix()}"
                    )
    return errors


def check_invariants(document: dict) -> list[str]:
    errors: list[str] = []
    implemented = {
        "forbidden-dependencies",
        "dependency-direction",
        "storage-boundaries",
        "deterministic-core",
        "model-network-isolation",
        "public-api-ownership",
    }
    checks = document.get("checks") or []
    seen = {item.get("id") for item in checks}
    missing = sorted(implemented - seen)
    if missing:
        errors.append(f"invariants: missing enforced checks: {', '.join(missing)}")
    for item in checks:
        if item.get("enforcement") == "enforced" and item.get("id") not in implemented:
            errors.append(f"invariants: {item.get('id')} is enforced but has no checker")
        if item.get("enforcement") not in {"enforced", "declared"}:
            errors.append(f"invariants: {item.get('id')} has invalid enforcement")
    return errors


def check_proof_lanes(root: Path, document: dict, policy: dict, catalog_targets: set[str]) -> tuple[list[str], dict[str, dict]]:
    errors: list[str] = []
    if list(document.get("lane_kinds") or []) != list(LANE_KINDS):
        errors.append("proof-lanes: lane_kinds must be the required vocabulary")
    subsystems = document.get("subsystems") or []
    by_id = {}
    for spec in subsystems:
        identifier = spec.get("id")
        if not identifier or identifier in by_id:
            errors.append(f"proof-lanes: duplicate or empty subsystem id {identifier!r}")
            continue
        by_id[identifier] = spec
        if not spec.get("paths") and not spec.get("paths_from"):
            errors.append(f"proof-lanes: {identifier} has no paths")
        required = spec.get("required") or []
        if not required:
            errors.append(f"proof-lanes: {identifier} has no proof lanes")
        for lane in required:
            kind = lane.get("kind")
            if kind not in LANE_KINDS:
                errors.append(f"proof-lanes: {identifier} has invalid lane kind {kind!r}")
            if lane.get("executes") not in {"binding", "stub"}:
                errors.append(f"proof-lanes: {identifier} lane {lane.get('id')} must execute binding or stub")
            bind = lane.get("bind")
            lane_id = lane.get("id")
            if bind == "ctest":
                if lane_id not in catalog_targets:
                    errors.append(f"proof-lanes: {identifier} ctest {lane_id} is not in the architecture catalog")
                module = lane.get("policy_module")
                tests = policy["module_boundaries"].get(module, {}).get("tests", [])
                if lane_id not in tests:
                    errors.append(
                        f"proof-lanes: {lane_id} ({identifier}) is not registered in agent-policy module {module} tests"
                    )
            elif bind == "policy":
                module = str(lane_id).split(":", 1)[-1]
                if module not in policy["module_boundaries"]:
                    errors.append(f"proof-lanes: unknown policy module {module}")
            elif bind in {"script", "catalog", "fixture", "workflow"}:
                relative = lane.get("ref") or lane_id
                if not (root / str(relative)).exists():
                    errors.append(f"proof-lanes: {identifier} missing {relative}")
                if bind == "workflow" and lane.get("contains"):
                    text = (root / str(relative)).read_text(encoding="utf-8")
                    if lane["contains"] not in text:
                        errors.append(f"proof-lanes: {relative} does not contain {lane['contains']!r}")
            else:
                errors.append(f"proof-lanes: {identifier} lane {lane_id} has invalid bind {bind!r}")

    for module, definition in policy["module_boundaries"].items():
        if module not in by_id:
            errors.append(f"proof-lanes: agent-policy module {module} has no subsystem")
        needs_policy = any(str(test).startswith("UnitTests") for test in definition.get("tests") or [])
        if needs_policy and not any(
            lane.get("bind") == "policy" and lane.get("id") == f"agent-policy:{module}"
            for spec in subsystems
            for lane in spec.get("required") or []
        ):
            errors.append(f"proof-lanes: module {module} has no policy proof lane")
        for test in definition.get("tests") or []:
            if str(test).startswith("UnitTests") and test not in catalog_targets:
                errors.append(f"proof-lanes: agent-policy {module} lists unknown target {test}")

    executables = parse_executables(root, document.get("cmake_sources") or [])
    if set(executables) != catalog_targets:
        missing = sorted(catalog_targets - set(executables))
        extra = sorted(set(executables) - catalog_targets)
        errors.append(
            "proof-lanes: cmake test targets drifted from the architecture catalog"
            + (f"; missing {', '.join(missing)}" if missing else "")
            + (f"; extra {', '.join(extra)}" if extra else "")
        )
    claimed: set[str] = set()
    for spec in subsystems:
        for lane in spec.get("required") or []:
            if lane.get("bind") == "ctest" and lane.get("id") in catalog_targets:
                claimed.add(lane["id"])
            elif lane.get("bind") == "policy":
                module = str(lane.get("id")).split(":", 1)[-1]
                for test in policy["module_boundaries"].get(module, {}).get("tests") or []:
                    if test in catalog_targets:
                        claimed.add(test)
    for spec in subsystems:
        if not spec.get("owns_targets"):
            continue
        patterns = subsystem_patterns(spec, policy)
        owned = {
            lane.get("id")
            for lane in spec.get("required") or []
            if lane.get("bind") == "ctest"
        }
        for target, sources in executables.items():
            matched = [source for source in sources if match_any(source, patterns)]
            if matched and target not in owned:
                errors.append(
                    f"proof-lanes: subsystem {spec['id']} owns {target} ({matched[0]}) but has no ctest proof lane for it"
                )
    deferred = set(document.get("migration", {}).get("deferred_targets") or [])
    missing_targets = sorted(catalog_targets - claimed - deferred)
    if missing_targets:
        errors.append(
            "proof-lanes: catalog targets have no proof lane and are not deferred: "
            + ", ".join(missing_targets)
        )
    both = sorted(deferred & claimed)
    if both:
        errors.append("proof-lanes: targets are both claimed and deferred: " + ", ".join(both))
    unknown = sorted(deferred - catalog_targets)
    if unknown:
        errors.append("proof-lanes: deferred targets are not in the catalog: " + ", ".join(unknown))
    return errors, by_id


def load_fixture_records(root: Path, governed_root: str) -> list[tuple[Path, dict]]:
    records: list[tuple[Path, dict]] = []
    base = root / governed_root
    if not base.is_dir():
        return records
    for path in sorted(base.rglob("*.yaml")):
        records.append((path, read_yaml(path)))
    return records


def check_fixtures(root: Path, document: dict, budgets: dict) -> tuple[list[str], dict[str, dict]]:
    errors: list[str] = []
    classes = set(document.get("classes") or [])
    if classes != {"development", "regression", "sealed-eval"}:
        errors.append("fixtures: classes must be development, regression, and sealed-eval")
    budget_ids = {metric.get("id") for metric in budgets.get("metrics") or []}
    records: dict[str, dict] = {}
    governed = document.get("governed_root")
    if not governed or not (root / governed).is_dir():
        errors.append(f"fixtures: missing governed root {governed}")
        return errors, records
    required = document.get("required_fields") or []
    for path, record in load_fixture_records(root, governed):
        relative = path.relative_to(root).as_posix()
        folder = path.parent.name
        identifier = record.get("id")
        if identifier in records:
            errors.append(f"fixtures: duplicate id {identifier}")
        records[str(identifier)] = record
        for field in required:
            if field not in record or record[field] in ("", None, []):
                errors.append(f"fixtures: {relative} missing {field}")
        if record.get("class") != folder:
            errors.append(f"fixtures: {relative} class {record.get('class')} is not in {folder}")
        if record.get("class") not in classes:
            errors.append(f"fixtures: {relative} has invalid class")
        if record.get("difficulty") not in set(document.get("difficulties") or []):
            errors.append(f"fixtures: {relative} has invalid difficulty")
        expected_approval = {
            "development": False,
            "regression": True,
            "sealed-eval": True,
        }.get(record.get("class"))
        if record.get("approval_required") is not expected_approval:
            errors.append(
                f"fixtures: {relative} approval_required must be {expected_approval}"
            )
        for metric in record.get("metrics") or []:
            if metric not in budget_ids:
                errors.append(f"fixtures: {relative} cites unknown budget {metric}")
    for class_name in ("development", "regression", "sealed-eval"):
        if not list((root / governed / class_name).glob("*.yaml")):
            errors.append(f"fixtures: seed class {class_name} has no records")
    return errors, records


def check_budgets(root: Path, document: dict, subsystem_ids: set[str]) -> list[str]:
    errors: list[str] = []
    metrics = document.get("metrics") or []
    seen: set[str] = set()
    by_subsystem: dict[str, int] = {}
    for metric in metrics:
        identifier = metric.get("id")
        if not identifier or identifier in seen:
            errors.append(f"quality-budgets: duplicate or empty metric {identifier!r}")
            continue
        seen.add(identifier)
        subsystem = metric.get("subsystem")
        by_subsystem[subsystem] = by_subsystem.get(subsystem, 0) + 1
        if subsystem not in subsystem_ids:
            errors.append(f"quality-budgets: {identifier} subsystem {subsystem} is unknown")
        if metric.get("lane") not in LANE_KINDS:
            errors.append(f"quality-budgets: {identifier} lane is not a proof-lane kind")
        if metric.get("fixture_class") not in {"development", "regression", "sealed-eval"}:
            errors.append(f"quality-budgets: {identifier} fixture_class is invalid")
        comparison = metric.get("comparison")
        if comparison == "minimum" and not isinstance(metric.get("minimum"), (int, float)):
            errors.append(f"quality-budgets: {identifier} needs a numeric minimum")
        elif comparison == "maximum" and not isinstance(metric.get("maximum"), (int, float)):
            errors.append(f"quality-budgets: {identifier} needs a numeric maximum")
        elif comparison == "maximum_regression_percent" and not isinstance(
            metric.get("maximum_regression_percent"), (int, float)
        ):
            errors.append(f"quality-budgets: {identifier} needs maximum_regression_percent")
        elif comparison not in {"minimum", "maximum", "maximum_regression_percent"}:
            errors.append(f"quality-budgets: {identifier} has invalid comparison")
        enforcement = metric.get("enforcement")
        if enforcement == "declared":
            if metric.get("harness") != "pending" or not metric.get("todo"):
                errors.append(f"quality-budgets: {identifier} is declared but has no pending harness TODO")
        elif enforcement == "enforced":
            errors.extend(measure_budget(root, metric))
        else:
            errors.append(f"quality-budgets: {identifier} enforcement must be enforced or declared")
    for subsystem in document.get("require_budget") or []:
        if by_subsystem.get(subsystem, 0) < 1:
            errors.append(f"quality-budgets: subsystem {subsystem} has no quality budget")
    return errors


def measure_budget(root: Path, metric: dict) -> list[str]:
    identifier = metric["id"]
    kind = metric.get("baseline_kind")
    baseline = root / str(metric.get("baseline"))
    if kind == "pdf-count":
        if not baseline.is_dir():
            return [f"quality-budgets: {identifier} baseline directory is missing"]
        count = len(list(baseline.glob("*.pdf")))
        if count < metric["minimum"]:
            return [f"quality-budgets: {identifier} pdf count {count} is below {metric['minimum']}"]
        return []
    if kind == "canvas-parity-field":
        if not baseline.is_file():
            return [f"quality-budgets: {identifier} baseline file is missing"]
        document = load_json(baseline)
        field = metric.get("baseline_field")
        seen = False
        errors: list[str] = []
        for case, entry in (document.get("cases") or {}).items():
            if field not in entry:
                continue
            seen = True
            if entry[field] > metric["maximum"]:
                errors.append(
                    f"quality-budgets: {identifier} case {case} {field}={entry[field]} exceeds {metric['maximum']}"
                )
        if not seen:
            errors.append(f"quality-budgets: {identifier} baseline has no {field}")
        return errors
    return [f"quality-budgets: {identifier} has unknown baseline_kind {kind!r}"]


def parse_evidence_token(token: str) -> tuple[str, str] | None:
    if not isinstance(token, str) or ":" not in token:
        return None
    kind, ref = token.split(":", 1)
    return kind, ref


def token_covers(token: str, lane: dict) -> bool:
    parsed = parse_evidence_token(token)
    if not parsed or parsed[0] != lane.get("kind"):
        return False
    kind, ref = parsed
    if kind == "fixture":
        if ":" not in ref:
            return False
        fixture_class, fixture_id = ref.split(":", 1)
        return fixture_id == lane.get("id") and fixture_class == lane.get("fixture_class")
    return ref == lane.get("id") or ref == lane.get("ref")


def check_evidence(
    root: Path,
    manifest: dict | None,
    manifest_path: str,
    touched: list[str],
    subsystems: dict[str, dict],
    fixtures: dict[str, dict],
    budgets: dict,
    require: bool,
) -> list[str]:
    if not require:
        return []
    errors: list[str] = []
    if manifest is None:
        return [f"evidence: missing {manifest_path} for subsystems {', '.join(touched)}"]
    claims = manifest.get("claims")
    if not isinstance(claims, list) or not claims:
        errors.append("evidence: claims must be a non-empty list")
        claims = []
    if "unresolved" not in manifest or not isinstance(manifest.get("unresolved"), list):
        errors.append("evidence: unresolved must be an explicit list")
    unresolved = set(manifest.get("unresolved") or [])
    budget_by_id = {metric["id"]: metric for metric in budgets.get("metrics") or []}
    tokens: list[tuple[dict, str]] = []
    for claim in claims:
        if not claim.get("id"):
            errors.append("evidence: claim is missing id")
        for token in claim.get("evidence") or []:
            parsed = parse_evidence_token(token)
            if not parsed:
                errors.append(f"evidence: {claim.get('id')} has unreadable evidence {token!r}")
                continue
            kind, ref = parsed
            tokens.append((claim, token))
            if kind == "budget":
                if ref not in budget_by_id:
                    errors.append(f"evidence: unknown budget {ref}")
                elif budget_by_id[ref].get("fixture_class") == "sealed-eval" and claim.get("lane") != "sealed-eval":
                    errors.append(f"evidence: sealed budget {ref} is cited outside a sealed-eval claim")
                continue
            if kind not in LANE_KINDS:
                errors.append(f"evidence: {token} is not a proof-lane kind or budget")
                continue
            if kind == "fixture":
                if ":" not in ref:
                    errors.append(f"evidence: fixture ref {token} must be fixture:<class>:<id>")
                    continue
                fixture_class, fixture_id = ref.split(":", 1)
                record = fixtures.get(fixture_id)
                if record is None or record.get("class") != fixture_class:
                    errors.append(f"evidence: fixture {fixture_class}:{fixture_id} does not resolve")
                elif fixture_class == "sealed-eval" and claim.get("lane") != "sealed-eval":
                    errors.append(f"evidence: sealed fixture {fixture_id} cannot satisfy a non-sealed claim")
                elif fixture_class == "development" and claim.get("lane") == "sealed-eval":
                    errors.append(f"evidence: development fixture {fixture_id} cannot satisfy sealed-eval")
                continue
            if not evidence_ref_resolves(root, kind, ref, subsystems):
                errors.append(f"evidence: {token} does not resolve")
            if kind in {"unit", "integration"} and ref in fixtures and fixtures[ref].get("class") == "sealed-eval":
                errors.append(f"evidence: {token} cites a sealed fixture as agent unit proof")
    for subsystem in touched:
        if subsystem == "documentation":
            continue
        spec = subsystems.get(subsystem)
        if spec is None:
            errors.append(f"evidence: changed subsystem {subsystem} has no proof-lane mapping")
            continue
        for lane in spec.get("required") or []:
            covered = any(token_covers(token, lane) for _claim, token in tokens)
            marker = f"{subsystem}:{lane.get('id')}"
            if lane.get("executes") == "stub":
                if not covered and marker not in unresolved and lane.get("id") not in unresolved:
                    errors.append(f"evidence: stub lane {marker} must be cited or listed in unresolved")
            elif not covered:
                errors.append(
                    f"evidence: touched subsystem {subsystem} is missing proof lane {lane.get('kind')}:{lane.get('id')}"
                )
    return errors


def evidence_ref_resolves(root: Path, kind: str, ref: str, subsystems: dict[str, dict]) -> bool:
    if (root / ref).exists():
        return True
    for spec in subsystems.values():
        for lane in spec.get("required") or []:
            if lane.get("kind") == kind and (lane.get("id") == ref or lane.get("ref") == ref):
                return True
    return False


def matching_subsystems(path: str, subsystems: dict[str, dict], policy: dict) -> list[str]:
    hits = []
    for identifier, spec in subsystems.items():
        if match_any(path, subsystem_patterns(spec, policy)):
            hits.append(identifier)
    return hits


def sealed_change_error(manifest: dict | None, fixtures_doc: dict) -> str | None:
    marker = fixtures_doc["rules"]["sealed_diff_marker"]
    value = fixtures_doc["rules"]["sealed_diff_value"]
    if not manifest or manifest.get(marker) != value:
        return "fixtures: sealed-eval output changed without sealed_output_approval: human"
    return None


def check_changes(
    root: Path,
    paths: list[str],
    policy: dict,
    proof: dict,
    subsystems: dict[str, dict],
    fixtures_doc: dict,
    budgets: dict,
    fixtures: dict[str, dict],
    branch: str,
    skip_evidence: bool,
) -> list[str]:
    errors: list[str] = []
    significant = proof.get("significant_paths") or []
    deferred = proof.get("migration", {}).get("deferred_paths") or []
    touched: set[str] = set()
    for path in paths:
        hits = matching_subsystems(path, subsystems, policy)
        if hits:
            touched.update(hits)
            continue
        if match_any(path, deferred):
            continue
        if match_any(path, significant):
            errors.append(
                f"proof-lanes: changed path {path} has no proof-lane subsystem. "
                "Add a subsystem or list the path under migration.deferred_paths."
            )
    sealed_root = f"{fixtures_doc.get('governed_root')}/sealed-eval/"
    sealed_changed = any(path.startswith(sealed_root) for path in paths)
    non_doc = sorted(name for name in touched if name != "documentation")
    reason = skip_evidence_reason(branch, policy, skip_evidence)
    manifest_path = f"changes/{branch_slug(branch)}.evidence.yaml"
    manifest = None
    manifest_file = root / manifest_path
    if manifest_file.is_file():
        manifest = read_yaml(manifest_file)
    if non_doc and reason is None:
        if manifest_path not in paths:
            errors.append(f"evidence: expected {manifest_path} in the change")
        errors.extend(
            check_evidence(
                root,
                manifest,
                manifest_path,
                sorted(touched),
                subsystems,
                fixtures,
                budgets,
                True,
            )
        )
    if sealed_changed and reason is None:
        approval = sealed_change_error(manifest, fixtures_doc)
        if approval:
            errors.append(approval)
    return errors


def compare_budget_documents(previous: dict | None, current: dict, manifest: dict | None) -> list[str]:
    if not previous:
        return []
    errors: list[str] = []
    old = {metric["id"]: metric for metric in previous.get("metrics") or []}
    unresolved = set((manifest or {}).get("unresolved") or [])
    approved = (manifest or {}).get("sealed_output_approval") == "human"
    for metric in current.get("metrics") or []:
        identifier = metric.get("id")
        prior = old.get(identifier)
        if not prior:
            continue
        looser = False
        for field, direction in (
            ("maximum", 1),
            ("maximum_regression_percent", 1),
            ("minimum", -1),
        ):
            if field in metric and field in prior and isinstance(metric[field], (int, float)):
                if direction == 1 and metric[field] > prior[field]:
                    looser = True
                if direction == -1 and metric[field] < prior[field]:
                    looser = True
        if looser and f"budget-loosened:{identifier}" not in unresolved:
            errors.append(
                f"quality-budgets: {identifier} moved past its previous cap without unresolved budget-loosened:{identifier}"
            )
        if metric.get("agent_tunable") is False and metric.get("fixture_class") == "sealed-eval":
            changed = any(metric.get(field) != prior.get(field) for field in ("maximum", "minimum", "maximum_regression_percent"))
            if changed and (f"budget-retune:{identifier}" not in unresolved or not approved):
                errors.append(
                    f"quality-budgets: sealed budget {identifier} is not agent-tunable"
                )
    return errors


def git_yaml_at(revision: str, relative: str) -> dict | None:
    completed = subprocess.run(
        ["git", "-C", str(ROOT), "show", f"{revision}:{relative}"],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        return None
    return load_yaml_text(completed.stdout)


def static_errors(root: Path = ROOT) -> list[str]:
    errors: list[str] = []
    try:
        boundaries = read_yaml(root / "architecture/boundaries.yaml")
        invariants = read_yaml(root / "architecture/invariants.yaml")
        proof = read_yaml(root / "architecture/proof-lanes.yaml")
        ownership = read_yaml(root / "architecture/ownership.yaml")
        fixtures_doc = read_yaml(root / "architecture/fixtures.yaml")
        budgets = read_yaml(root / "architecture/quality-budgets.yaml")
        policy = load_json(root / proof["policy"])
        catalog = load_json(root / proof["catalog"])
        schema = load_json(root / "architecture/schema.json")
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        return [f"architecture: cannot load contracts: {exc}"]
    if "proof-lanes" not in json.dumps(schema):
        errors.append("architecture: schema.json does not describe proof-lanes")
    errors.extend(check_boundaries(root, boundaries))
    errors.extend(check_ownership(root, ownership))
    errors.extend(check_invariants(invariants))
    # deterministic-core and model isolation are the include rules in boundaries.
    lane_errors, _subsystems = check_proof_lanes(root, proof, policy, set(catalog["test_targets"]))
    errors.extend(lane_errors)
    fixture_errors, _fixtures = check_fixtures(root, fixtures_doc, budgets)
    errors.extend(fixture_errors)
    errors.extend(check_budgets(root, budgets, {spec["id"] for spec in proof["subsystems"]}))
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", help="base commit or ref")
    parser.add_argument("--head", default="HEAD", help="head commit or ref")
    parser.add_argument("--head-branch", help="branch name used for the evidence manifest")
    parser.add_argument("--skip-evidence", action="store_true")
    args = parser.parse_args()
    errors = static_errors(ROOT)
    if args.base:
        try:
            proof = read_yaml(ARCH / "proof-lanes.yaml")
            policy = load_json(ROOT / proof["policy"])
            fixtures_doc = read_yaml(ARCH / "fixtures.yaml")
            budgets = read_yaml(ARCH / "quality-budgets.yaml")
            _lane_errors, subsystems = check_proof_lanes(
                ROOT,
                proof,
                policy,
                set(load_json(ROOT / proof["catalog"])["test_targets"]),
            )
            fixture_errors, fixtures = check_fixtures(ROOT, fixtures_doc, budgets)
            del fixture_errors
            paths = changed_paths(args.base, args.head)
            errors.extend(
                check_changes(
                    ROOT,
                    paths,
                    policy,
                    proof,
                    subsystems,
                    fixtures_doc,
                    budgets,
                    fixtures,
                    current_branch(args.head_branch),
                    args.skip_evidence,
                )
            )
            manifest_path = ROOT / f"changes/{branch_slug(current_branch(args.head_branch))}.evidence.yaml"
            manifest = read_yaml(manifest_path) if manifest_path.is_file() else None
            base_sha = git_output(["merge-base", args.base, args.head]).strip()
            previous = git_yaml_at(base_sha, "architecture/quality-budgets.yaml")
            errors.extend(compare_budget_documents(previous, budgets, manifest))
        except (OSError, subprocess.CalledProcessError, ValueError, KeyError) as exc:
            errors.append(f"architecture: cannot compare the change: {exc}")
    if errors:
        print("architecture contracts failed:", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1
    print("architecture contracts ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
