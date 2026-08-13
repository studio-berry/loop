#!/usr/bin/env python3
import json
import os
import subprocess
import sys
import unittest
from pathlib import Path

from scripts.ci.reduce_release_gate import REQUIRED_JOBS, reduce_needs


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "ci" / "reduce_release_gate.py"


def _needs(**results: str) -> dict[str, dict[str, str]]:
    return {name: {"result": result} for name, result in results.items()}


def _all_success() -> dict[str, dict[str, str]]:
    return _needs(**{name: "success" for name in REQUIRED_JOBS})


class ReduceReleaseGateTests(unittest.TestCase):
    def test_all_success_is_clean(self):
        self.assertEqual(reduce_needs(_all_success()), [])

    def test_failed_dependency_is_explicit(self):
        needs = _all_success()
        needs["linux"]["result"] = "failure"
        self.assertEqual(reduce_needs(needs), ["linux: failure"])

    def test_cancelled_dependency_is_explicit(self):
        needs = _all_success()
        needs["windows"]["result"] = "cancelled"
        self.assertEqual(reduce_needs(needs), ["windows: cancelled"])

    def test_skipped_dependency_fails(self):
        needs = _all_success()
        needs["fuzz_regression"]["result"] = "skipped"
        self.assertEqual(reduce_needs(needs), ["fuzz_regression: skipped"])

    def test_missing_dependency_fails(self):
        needs = _all_success()
        del needs["documentation"]
        self.assertEqual(reduce_needs(needs), ["documentation: missing (did not report)"])

    def test_missing_result_field_fails(self):
        needs = _all_success()
        needs["supply_chain"] = {}
        self.assertEqual(reduce_needs(needs), ["supply_chain: missing result"])

    def test_multiple_violations_are_all_reported(self):
        needs = _all_success()
        needs["linux"]["result"] = "failure"
        del needs["package_contract"]
        self.assertEqual(
            reduce_needs(needs),
            ["linux: failure", "package_contract: missing (did not report)"],
        )

    def test_invalid_needs_object_fails(self):
        self.assertEqual(reduce_needs(None), ["needs context is missing"])
        self.assertTrue(any("object" in item for item in reduce_needs(["linux"])))

    def test_script_reads_env_json_and_exits_nonzero_on_skip(self):
        env = os.environ.copy()
        env["PYTHONPATH"] = str(ROOT)
        env["RELEASE_GATE_NEEDS"] = json.dumps(_needs(linux="skipped"))
        completed = subprocess.run(
            [sys.executable, str(SCRIPT)],
            cwd=ROOT,
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(completed.returncode, 1)
        self.assertIn("linux: skipped", completed.stderr)

    def test_script_reads_stdin_when_env_absent(self):
        env = os.environ.copy()
        env["PYTHONPATH"] = str(ROOT)
        env.pop("RELEASE_GATE_NEEDS", None)
        completed = subprocess.run(
            [sys.executable, str(SCRIPT)],
            cwd=ROOT,
            env=env,
            input=json.dumps(_all_success()),
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(completed.returncode, 0)
        self.assertIn("Release gate passed", completed.stdout)


if __name__ == "__main__":
    unittest.main()
