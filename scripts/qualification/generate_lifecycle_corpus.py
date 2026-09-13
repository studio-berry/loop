#!/usr/bin/env python3
"""Generate deterministic lifecycle trace corpus files for UnitTestsLifecycle."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CORPUS_DIR = ROOT / "UnitTests" / "testdata" / "lifecycle"
MAX_COMMANDS = 64
SOURCE_DIGEST = hashlib.sha256(b"lifecycle-source-v1").hexdigest()
EXPECTED_INVARIANTS = [
    "source-immutable",
    "cancel-is-terminal",
    "stale-results-rejected",
    "history-append-only",
]
ACTIVE_KINDS = [
    "open",
    "render-preflight",
    "cancel",
    "replace-revision",
    "save-reopen",
    "rollback",
]
CORPUS_SEEDS = (
    0x20260821,
    0x20260901,
    0x20260906,
    0x20261201,
)
FAILURE_TRACES = (
    {
        "file": "failure-stale-result-minimized.json",
        "replay_profile": "inject-stale-acceptance",
        "expected_violation": "stale-result-accepted",
        "commands": (("open", 9073021129658994722),),
        "shrink_history": [64, 1],
    },
    {
        "file": "failure-source-overwritten-minimized.json",
        "replay_profile": "inject-source-overwrite",
        "expected_violation": "source-overwritten",
        "commands": (("open", 9073021129658994722),),
        "shrink_history": [64, 1],
    },
    {
        "file": "failure-rollback-history-minimized.json",
        "replay_profile": "inject-history-mutation",
        "expected_violation": "rollback-history-mutated",
        "commands": (
            ("open", 9073021129658994722),
            ("save-reopen", 5643642477061534660),
        ),
        "shrink_history": [64, 2],
    },
)


def next_trace_random(state: int) -> tuple[int, int]:
    state = (state + 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
    value = state
    value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF
    value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
    return value ^ (value >> 31), state


def generate_trace(seed: int, max_commands: int = MAX_COMMANDS) -> list[tuple[str, int]]:
    trace: list[tuple[str, int]] = []
    state = seed
    for kind in ACTIVE_KINDS:
        argument, state = next_trace_random(state)
        trace.append((kind, argument))
    while len(trace) < max_commands - 1:
        kind_index, state = next_trace_random(state)
        kind = ACTIVE_KINDS[kind_index % len(ACTIVE_KINDS)]
        argument, state = next_trace_random(state)
        trace.append((kind, argument))
    argument, state = next_trace_random(state)
    trace.append(("close", argument))
    return trace


def trace_to_json(
    seed: int,
    trace: list[tuple[str, int]],
    observed_result: str,
    shrink_history: list[int],
) -> dict:
    commands = [
        {"index": index, "kind": kind, "argument": str(argument)}
        for index, (kind, argument) in enumerate(trace)
    ]
    return {
        "schema_kind": "loop-lifecycle-trace",
        "schema_version": 1,
        "seed": seed,
        "initial_artifact_digest": SOURCE_DIGEST,
        "commands": commands,
        "expected_invariants": EXPECTED_INVARIANTS,
        "observed_result": observed_result,
        "shrink_history": shrink_history,
    }


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        payload = handle.read()
    digest.update(payload.replace(b"\r\n", b"\n"))
    return digest.hexdigest()


def main() -> int:
    CORPUS_DIR.mkdir(parents=True, exist_ok=True)
    passing_traces = []
    for seed in CORPUS_SEEDS:
        trace = generate_trace(seed)
        filename = f"seed-{seed:08x}.json"
        path = CORPUS_DIR / filename
        path.write_text(
            json.dumps(trace_to_json(seed, trace, "invariants-held", []), indent=2) + "\n",
            encoding="utf-8",
            newline="\n",
        )
        passing_traces.append(
            {
                "seed": seed,
                "trace_file": filename,
                "command_count": len(trace),
                "sha256": sha256_file(path),
                "observed_result": "invariants-held",
            }
        )

    failure_entries = []
    for entry in FAILURE_TRACES:
        payload = trace_to_json(
            0x20260821,
            list(entry["commands"]),
            entry["expected_violation"],
            list(entry["shrink_history"]),
        )
        path = CORPUS_DIR / entry["file"]
        path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8", newline="\n")
        failure_entries.append(
            {
                "trace_file": entry["file"],
                "replay_profile": entry["replay_profile"],
                "expected_violation": entry["expected_violation"],
                "command_count": len(entry["commands"]),
                "sha256": sha256_file(path),
            }
        )

    manifest = {
        "schema_kind": "loop-lifecycle-corpus",
        "schema_version": 1,
        "max_commands": MAX_COMMANDS,
        "initial_artifact_digest": SOURCE_DIGEST,
        "passing_traces": passing_traces,
        "failure_traces": failure_entries,
    }
    manifest_path = CORPUS_DIR / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"Wrote {len(passing_traces)} passing and {len(failure_entries)} failure traces to {CORPUS_DIR}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
