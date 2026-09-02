#!/usr/bin/env python3
"""Validate the interaction trace corpus and the CI trace report (issue #146).

Two jobs, deliberately in one script because they share the corpus:

  --corpus-only  validate every scenario against the scenario schema and the
                 manifest digests.  Needs no build, so a malformed scenario
                 fails in seconds rather than after a compile.
  (default)      validate a report emitted by the trace test binary: identity,
                 scenario coverage, and the rule that missing telemetry is
                 reported as unavailable rather than as zero.

The schema check is hand-rolled against the tracked JSON Schema documents
because the repository pins no jsonschema dependency; check_fuzz_corpus.py and
scripts/resource_envelope/validate_envelope.py take the same approach.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CORPUS_DIR = ROOT / "UnitTests" / "testdata" / "interaction-traces"
MANIFEST_PATH = CORPUS_DIR / "manifest.json"
SCENARIO_SCHEMA = ROOT / "docs" / "schemas" / "interaction-scenario.schema.json"
REPORT_SCHEMA = ROOT / "docs" / "schemas" / "interaction-trace-report.schema.json"

KEBAB_CASE = re.compile(r"^[a-z][a-z0-9-]*$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")

# Mirrors docs/schemas/interaction-trace-report.schema.json.  Kept as literals
# rather than parsed out of the schema so that a schema edit that drops a value
# fails a test instead of silently widening the checker.
CONTRACTS = (
    "input-acknowledged",
    "frame-balance",
    "telemetry-available",
    "p95-input-to-frame",
    "p95-frame-time",
    "slow-frame-budget",
    "dropped-frames",
    "stale-result-safety",
    "final-state",
)
PHASES = (
    "input",
    "hit-test",
    "page-cache",
    "overlay",
    "composition",
    "async-overlap",
    "unknown",
)
STATUSES = ("verified", "static-only", "infrastructure-blocked")
LANES = ("deterministic", "present")
IDENTITY_FIELDS = (
    "commit",
    "compiler",
    "os",
    "qt",
    "cpu",
    "renderer",
    "fixture_digest",
    "profile_or_operation_version",
    "corpus_digest",
)

REQUIRED_SCENARIO_FIELDS = frozenset(
    {
        "schema_kind",
        "schema_version",
        "scenario_id",
        "description",
        "fixture",
        "cost_model",
        "budgets",
        "expected",
    }
)
REQUIRED_FIXTURE_FIELDS = frozenset(
    {
        "page_count",
        "page_size_mm",
        "pixel_per_mm",
        "device_pixel_ratio",
        "initial_zoom",
        "viewport_size_px",
        "page_layout",
    }
)
REQUIRED_BUDGET_FIELDS = frozenset(
    {"refresh_rate_hz", "frame_p95_ms", "input_to_frame_p95_ms"}
)

Violation = tuple[str, str]


def sha256_file(path: Path) -> str:
    """SHA-256 of a scenario file, over line-ending-normalized bytes.

    .gitattributes checks this repository's text out as CRLF by default, and
    pins the corpus back to LF so the digests stay stable.  Normalizing here as
    well means a checkout that lost that pin -- a zip export, a contributor with
    a global setting -- reports a real corpus edit rather than a line-ending
    difference nobody made.  These files are JSON, so their bytes carry no
    meaning a newline conversion can destroy.
    """
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        payload = handle.read()
    digest.update(payload.replace(b"\r\n", b"\n"))
    return digest.hexdigest()


def corpus_digest(manifest: dict) -> str:
    """A digest over the manifest's scenario digests, in id order.

    This is the value a report's identity.corpus_digest must carry, so a report
    produced against a different corpus cannot be compared to this one.
    """
    joined = "\n".join(
        f"{entry.get('id')}:{entry.get('sha256')}"
        for entry in sorted(manifest.get("scenarios", []), key=lambda e: str(e.get("id")))
    )
    return hashlib.sha256(joined.encode("utf-8")).hexdigest()


def load_manifest(corpus_dir: Path = CORPUS_DIR) -> dict:
    """Load and return the corpus manifest."""
    with (corpus_dir / "manifest.json").open(encoding="utf-8") as handle:
        return json.load(handle)


def validate_scenario(scenario: dict, label: str) -> list[Violation]:
    """Return (subject, reason) for every scenario-document violation."""
    violations: list[Violation] = []

    if scenario.get("schema_kind") != "loupe-interaction-scenario":
        violations.append((label, "schema_kind must be loupe-interaction-scenario"))
    if scenario.get("schema_version") != 1:
        violations.append((label, "schema_version must be 1"))

    missing = REQUIRED_SCENARIO_FIELDS - scenario.keys()
    if missing:
        violations.append((label, f"missing required fields: {sorted(missing)}"))
        return violations

    scenario_id = scenario["scenario_id"]
    if not isinstance(scenario_id, str) or not KEBAB_CASE.match(scenario_id):
        violations.append((label, f"scenario_id must be kebab-case, got {scenario_id!r}"))

    if not str(scenario.get("description", "")).strip():
        violations.append((label, "description must not be empty"))

    has_trace = "trace" in scenario
    has_script = "input_script" in scenario
    if has_trace == has_script:
        violations.append(
            (label, "exactly one of 'trace' or 'input_script' is required")
        )

    if has_trace:
        trace = scenario["trace"]
        if not isinstance(trace, dict):
            violations.append((label, "trace must be an object"))
        else:
            if trace.get("schema_version") != 1:
                violations.append((label, "trace.schema_version must be 1"))
            if not isinstance(trace.get("inputs"), list) or not trace["inputs"]:
                violations.append((label, "trace.inputs must be a non-empty array"))

    if has_script:
        script = scenario["input_script"]
        if not isinstance(script, list) or not script:
            violations.append((label, "input_script must be a non-empty array"))

    fixture = scenario["fixture"]
    if not isinstance(fixture, dict):
        violations.append((label, "fixture must be an object"))
    else:
        fixture_missing = REQUIRED_FIXTURE_FIELDS - fixture.keys()
        if fixture_missing:
            violations.append(
                (label, f"fixture missing required fields: {sorted(fixture_missing)}")
            )
        if not isinstance(fixture.get("page_count"), int) or fixture.get("page_count", 0) < 1:
            violations.append((label, "fixture.page_count must be a positive integer"))
        for key in ("pixel_per_mm", "device_pixel_ratio", "initial_zoom"):
            value = fixture.get(key)
            if not isinstance(value, (int, float)) or value <= 0:
                violations.append((label, f"fixture.{key} must be greater than zero"))

        seen_target_ids: set[str] = set()
        for index, target in enumerate(fixture.get("hit_targets", []) or []):
            if not isinstance(target, dict):
                violations.append((label, f"hit_targets[{index}] must be an object"))
                continue
            target_id = target.get("id")
            if not isinstance(target_id, str) or not target_id:
                violations.append((label, f"hit_targets[{index}] needs a non-empty id"))
            elif target_id in seen_target_ids:
                violations.append((label, f"duplicate hit target id {target_id!r}"))
            else:
                seen_target_ids.add(target_id)

    cost_model = scenario["cost_model"]
    if not isinstance(cost_model, dict) or "base_frame_ns" not in cost_model:
        violations.append((label, "cost_model must be an object with base_frame_ns"))
    else:
        for key, value in cost_model.items():
            if not isinstance(value, int) or isinstance(value, bool) or value < 0:
                violations.append(
                    (label, f"cost_model.{key} must be a non-negative integer of nanoseconds")
                )

    budgets = scenario["budgets"]
    if not isinstance(budgets, dict):
        violations.append((label, "budgets must be an object"))
    else:
        budget_missing = REQUIRED_BUDGET_FIELDS - budgets.keys()
        if budget_missing:
            violations.append(
                (label, f"budgets missing required fields: {sorted(budget_missing)}")
            )
        for key in ("frame_p95_ms", "input_to_frame_p95_ms"):
            value = budgets.get(key)
            if not isinstance(value, (int, float)) or value <= 0:
                violations.append((label, f"budgets.{key} must be greater than zero"))
        band = budgets.get("variance_band_multiplier")
        if band is not None and (not isinstance(band, (int, float)) or band < 1):
            violations.append((label, "budgets.variance_band_multiplier must be at least 1"))

    if not isinstance(scenario["expected"], dict):
        violations.append((label, "expected must be an object"))

    return violations


def validate_corpus(corpus_dir: Path = CORPUS_DIR, root: Path = ROOT) -> list[Violation]:
    """Return (subject, reason) for every corpus violation."""
    try:
        manifest = load_manifest(corpus_dir)
    except (OSError, json.JSONDecodeError) as exc:
        return [("manifest.json", f"unable to load manifest: {exc}")]

    violations: list[Violation] = []

    if manifest.get("schema_kind") != "loupe-interaction-corpus":
        violations.append(("manifest.json", "schema_kind must be loupe-interaction-corpus"))
    if manifest.get("schema_version") != 1:
        violations.append(("manifest.json", "schema_version must be 1"))
        return violations

    entries = manifest.get("scenarios")
    if not isinstance(entries, list) or not entries:
        violations.append(("manifest.json", "scenarios must be a non-empty array"))
        return violations

    seen_ids: set[str] = set()
    manifest_paths: set[str] = set()

    for index, entry in enumerate(entries):
        label = f"scenarios[{index}]"
        if not isinstance(entry, dict):
            violations.append((label, "scenario entry must be an object"))
            continue

        missing = {"id", "path", "issue", "sha256"} - entry.keys()
        if missing:
            violations.append((label, f"missing required fields: {sorted(missing)}"))
            continue

        blocked_on = entry.get("blocked_on")
        if blocked_on is not None:
            # A scenario may be reviewed as data before the harness can run it.
            # Saying so explicitly is what keeps the coverage check strict for
            # everything else: without this the check would have to be relaxed
            # for the whole corpus.
            if not isinstance(blocked_on, str) or not blocked_on.strip():
                violations.append((label, "blocked_on must be a non-empty string when present"))
            if not str(entry.get("blocked_reason", "")).strip():
                violations.append((label, "a blocked scenario must carry a blocked_reason"))

        entry_id = entry["id"]
        if not isinstance(entry_id, str) or not KEBAB_CASE.match(entry_id):
            violations.append((label, f"id must be kebab-case, got {entry_id!r}"))
        elif entry_id in seen_ids:
            violations.append((label, f"duplicate id {entry_id!r}"))
        else:
            seen_ids.add(entry_id)

        rel_path = str(entry["path"]).replace("\\", "/")
        manifest_paths.add(rel_path)

        if not rel_path.startswith("UnitTests/testdata/interaction-traces/"):
            violations.append((rel_path, "path must live under the interaction-traces corpus"))
            continue

        absolute = root / rel_path
        if not absolute.is_file():
            violations.append((rel_path, "manifest path does not exist"))
            continue

        digest = entry["sha256"]
        if not isinstance(digest, str) or not SHA256.match(digest):
            violations.append((rel_path, "sha256 must be a 64-character lowercase hex digest"))
        else:
            actual = sha256_file(absolute)
            if actual != digest:
                violations.append(
                    (rel_path, f"sha256 mismatch (manifest {digest}, actual {actual})")
                )

        try:
            scenario = json.loads(absolute.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            violations.append((rel_path, f"unable to load scenario: {exc}"))
            continue

        if scenario.get("scenario_id") != entry_id:
            violations.append(
                (rel_path, f"scenario_id {scenario.get('scenario_id')!r} != manifest id {entry_id!r}")
            )

        violations.extend(validate_scenario(scenario, rel_path))

    for path in sorted(corpus_dir.glob("*.json")):
        if path.name == "manifest.json":
            continue
        rel_path = str(path.relative_to(root)).replace("\\", "/")
        if rel_path not in manifest_paths:
            violations.append((rel_path, "tracked scenario is missing from manifest.json"))

    return violations


def runnable_ids(manifest: dict) -> set[str]:
    """Corpus ids whose harness support has landed.

    A blocked scenario is tracked and validated as data, but nothing can run it
    yet, so demanding a run for it would report the corpus as broken rather
    than as ahead of the harness.
    """
    return {
        str(entry["id"])
        for entry in manifest.get("scenarios", [])
        if not entry.get("blocked_on")
    }


def blocked_ids(manifest: dict) -> dict[str, str]:
    """Blocked corpus ids mapped to what they are waiting on."""
    return {
        str(entry["id"]): str(entry.get("blocked_on"))
        for entry in manifest.get("scenarios", [])
        if entry.get("blocked_on")
    }


def _percentile_keys(block: dict) -> list[str]:
    return [key for key in block if key.startswith("p") and key[1:].split("_")[0].isdigit()]


def validate_percentiles(block: object, label: str) -> list[Violation]:
    """Enforce the no-zero-for-missing rule (#140 AC2, docs/RESOURCE_BUDGETS.md).

    An unavailable measurement must carry null percentiles; an available one
    must carry real numbers.  A zero standing in for a measurement that never
    happened is the failure this exists to prevent.
    """
    if not isinstance(block, dict):
        return [(label, "percentile block must be an object")]

    violations: list[Violation] = []

    if "available" not in block:
        return [(label, "percentile block must carry 'available'")]

    available = block["available"]
    if not isinstance(available, bool):
        return [(label, "'available' must be a boolean")]

    keys = _percentile_keys(block)
    if not keys:
        return [(label, "percentile block must carry p50/p95/p99 values")]

    for key in keys:
        value = block[key]
        if available:
            if not isinstance(value, (int, float)) or isinstance(value, bool):
                violations.append((label, f"{key} must be a number when available is true"))
        elif value is not None:
            violations.append(
                (label, f"{key} must be null when available is false, got {value!r}")
            )

    if not available and block.get("sample_count", 0) not in (0, None):
        violations.append((label, "sample_count must be 0 when available is false"))

    return violations


def validate_report(
    report: dict,
    corpus_ids: set[str] | None = None,
    expected_corpus_digest: str | None = None,
    known_ids: set[str] | None = None,
) -> list[Violation]:
    """Return (subject, reason) for every report violation."""
    violations: list[Violation] = []

    if report.get("schema_kind") != "loupe-interaction-trace-report":
        violations.append(("report", "schema_kind must be loupe-interaction-trace-report"))
    if report.get("schema_version") != 1:
        violations.append(("report", "schema_version must be 1"))
        return violations

    lane = report.get("lane")
    if lane not in LANES:
        violations.append(("report", f"lane must be one of {list(LANES)}, got {lane!r}"))

    identity = report.get("identity")
    if not isinstance(identity, dict):
        violations.append(("report.identity", "identity must be an object"))
    else:
        for field in IDENTITY_FIELDS:
            value = identity.get(field)
            if not isinstance(value, str) or not value.strip():
                violations.append(("report.identity", f"{field} must be a non-empty string"))
        digest = identity.get("corpus_digest")
        if (
            expected_corpus_digest
            and isinstance(digest, str)
            and digest != expected_corpus_digest
        ):
            violations.append(
                (
                    "report.identity",
                    f"corpus_digest {digest} does not match the tracked corpus {expected_corpus_digest}",
                )
            )

    runs = report.get("runs")
    if not isinstance(runs, list) or not runs:
        violations.append(("report.runs", "runs must be a non-empty array"))
        return violations

    seen: set[str] = set()

    for index, run in enumerate(runs):
        label = f"runs[{index}]"
        if not isinstance(run, dict):
            violations.append((label, "run must be an object"))
            continue

        scenario_id = run.get("scenario_id")
        if not isinstance(scenario_id, str) or not KEBAB_CASE.match(scenario_id or ""):
            violations.append((label, f"scenario_id must be kebab-case, got {scenario_id!r}"))
        elif scenario_id in seen:
            violations.append((label, f"duplicate scenario_id {scenario_id!r}"))
        else:
            seen.add(scenario_id)
            label = f"runs[{scenario_id}]"

        status = run.get("status")
        if status not in STATUSES:
            violations.append((label, f"status must be one of {list(STATUSES)}, got {status!r}"))

        passed = run.get("passed")
        if not isinstance(passed, bool):
            violations.append((label, "passed must be a boolean"))
            passed = None

        contract = run.get("first_violated_contract")
        phase = run.get("responsible_phase")
        excerpt = run.get("failure_excerpt")

        if passed is False:
            # AC7: a failure must name what broke and who is responsible.  A
            # red run with no attribution is the outcome this check exists for.
            if contract not in CONTRACTS:
                violations.append(
                    (label, f"failed run needs first_violated_contract in {list(CONTRACTS)}, got {contract!r}")
                )
            if phase not in PHASES:
                violations.append(
                    (label, f"failed run needs responsible_phase in {list(PHASES)}, got {phase!r}")
                )
            if not isinstance(excerpt, list) or not excerpt:
                violations.append((label, "failed run needs a non-empty failure_excerpt"))
        elif passed is True:
            if contract is not None:
                violations.append((label, "passing run must not name a violated contract"))
            if phase is not None:
                violations.append((label, "passing run must not name a responsible phase"))

        for key in ("input_to_frame_ms", "frame_time_ms"):
            if key in run:
                violations.extend(validate_percentiles(run[key], f"{label}.{key}"))

        for key, block in (run.get("stage_ms") or {}).items():
            violations.extend(validate_percentiles(block, f"{label}.stage_ms.{key}"))

        hit_test = run.get("hit_test") or {}
        for key, block in hit_test.items():
            violations.extend(validate_percentiles(block, f"{label}.hit_test.{key}"))

        present = run.get("present_timing")
        if isinstance(present, dict):
            if present.get("available") is False and not str(present.get("reason", "")).strip():
                violations.append(
                    (label, "present_timing must carry a reason when unavailable")
                )
            elif present.get("available") is True:
                violations.extend(validate_percentiles(present, f"{label}.present_timing"))

        # A verified status is a claim that the measurement happened.  It may
        # not be paired with telemetry that says it did not.
        if status == "verified":
            latency = run.get("input_to_frame_ms")
            if isinstance(latency, dict) and latency.get("available") is False:
                violations.append(
                    (label, "status is verified but input_to_frame_ms is unavailable")
                )

    if corpus_ids is not None:
        # corpus_ids are the scenarios that must run; known_ids additionally
        # covers blocked scenarios, which may run early but need not.
        for missing_id in sorted(corpus_ids - seen):
            violations.append(("report.runs", f"corpus scenario {missing_id!r} has no run"))
        for extra_id in sorted(seen - (known_ids if known_ids is not None else corpus_ids)):
            violations.append(("report.runs", f"run {extra_id!r} is not in the corpus"))

    return violations


def trend_rows(report: dict, baseline: dict | None) -> list[str]:
    """Per-scenario p50/p95/p99 lines, with deltas when a baseline is given."""
    base_runs = {}
    if isinstance(baseline, dict):
        base_runs = {
            run.get("scenario_id"): run
            for run in baseline.get("runs", [])
            if isinstance(run, dict)
        }

    rows = []
    for run in report.get("runs", []):
        if not isinstance(run, dict):
            continue
        scenario_id = run.get("scenario_id", "?")
        latency = run.get("input_to_frame_ms") or {}
        if not latency.get("available"):
            rows.append(f"{scenario_id}: input-to-frame unavailable ({run.get('status')})")
            continue

        line = (
            f"{scenario_id}: p50={latency.get('p50_ms')}ms "
            f"p95={latency.get('p95_ms')}ms p99={latency.get('p99_ms')}ms"
        )
        base_latency = (base_runs.get(scenario_id) or {}).get("input_to_frame_ms") or {}
        if base_latency.get("available"):
            try:
                delta = float(latency["p95_ms"]) - float(base_latency["p95_ms"])
                line += f" (p95 delta {delta:+.3f}ms)"
            except (TypeError, ValueError, KeyError):
                pass
        rows.append(line)

    return rows


def load_json(path: Path) -> dict:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", nargs="?", help="trace report emitted by the test binary")
    parser.add_argument(
        "--corpus-only",
        action="store_true",
        help="validate the scenario corpus and exit; requires no build",
    )
    parser.add_argument("--baseline", help="baseline report for --trend deltas")
    parser.add_argument(
        "--trend",
        action="store_true",
        help="print per-scenario percentiles and deltas; never fails the run",
    )
    args = parser.parse_args(argv)

    corpus_violations = validate_corpus()
    if corpus_violations:
        print("ERROR: interaction trace corpus failed validation:", file=sys.stderr)
        for subject, reason in corpus_violations:
            print(f"  {subject}: {reason}", file=sys.stderr)
        return 1

    if args.corpus_only:
        manifest = load_manifest()
        blocked = blocked_ids(manifest)
        print(
            f"Interaction trace corpus policy passed "
            f"({len(manifest['scenarios'])} scenarios, {len(blocked)} awaiting harness support)."
        )
        for scenario_id, waiting_on in sorted(blocked.items()):
            print(f"  blocked: {scenario_id} (waiting on {waiting_on})")
        return 0

    if not args.report:
        parser.error("a report path is required unless --corpus-only is given")

    report_path = Path(args.report)
    if not report_path.is_absolute():
        report_path = Path.cwd() / report_path

    try:
        report = load_json(report_path)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"ERROR: unable to load {report_path}: {exc}", file=sys.stderr)
        return 1

    manifest = load_manifest()
    required = runnable_ids(manifest)
    known = required | set(blocked_ids(manifest))
    violations = validate_report(report, required, corpus_digest(manifest), known)

    if violations:
        print("ERROR: interaction trace report failed validation:", file=sys.stderr)
        for subject, reason in violations:
            print(f"  {subject}: {reason}", file=sys.stderr)
        return 1

    failed = [run for run in report["runs"] if run.get("passed") is False]
    for run in failed:
        print(
            f"FAIL {run['scenario_id']}: {run['first_violated_contract']} "
            f"(phase {run['responsible_phase']})",
            file=sys.stderr,
        )
        for line in run.get("failure_excerpt", []):
            print(f"    {line}", file=sys.stderr)

    if args.trend:
        baseline = None
        if args.baseline and Path(args.baseline).is_file():
            baseline = load_json(Path(args.baseline))
        print("Interaction trace trend:")
        for row in trend_rows(report, baseline):
            print(f"  {row}")

    if failed:
        return 1

    print(f"Interaction trace report passed ({len(report['runs'])} scenarios, lane {report['lane']}).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
