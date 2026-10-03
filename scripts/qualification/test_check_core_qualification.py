from __future__ import annotations

import contextlib
import copy
import hashlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from scripts.qualification import check_core_qualification as checker


class CoreQualificationTest(unittest.TestCase):
    candidate_sha = "a" * 40

    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.run_path = self.root / "run.json"
        self.snapshot = {
            "databaseId": 100,
            "url": "https://github.com/studio-berry/loop/actions/runs/100",
            "headSha": self.candidate_sha,
            "status": "completed",
            "conclusion": "success",
            "jobs": [
                self.job("source_integrity", 1, ["Verify tracked source integrity"]),
                self.job("linux / build", 2, self.build_steps()),
                self.job("windows / build", 3, self.build_steps()),
            ],
        }
        self.records = {}
        for platform, name, package_format in (
            ("linux", "Loop.AppImage", "AppImage"),
            ("windows", "Loop.msi", "MSI"),
        ):
            package = self.root / name
            payload = f"test package for {platform}".encode()
            package.write_bytes(payload)
            self.records[platform] = {
                "schema_version": 1,
                "kind": "loop-package-boundary-evidence",
                "platform": platform,
                "status": "passed",
                "source_sha": self.candidate_sha,
                "forbidden_findings": [],
                "checks": {
                    "all_payload_files_hashed": True,
                    "all_binary_files_inspected": True,
                    "target_architecture_matches": True,
                    "qt6widgets_absent": True,
                    "qt6widgets_surface_absent": True,
                    "unresolved_non_system_dependencies_absent": True,
                },
                "package": {
                    "name": name,
                    "format": package_format,
                    "size": len(payload),
                    "sha256": hashlib.sha256(payload).hexdigest(),
                },
            }
        self.arguments = [
            "--candidate-sha", self.candidate_sha,
            "--ci-run", str(self.run_path),
            "--linux-evidence", str(self.root / "linux.json"),
            "--windows-evidence", str(self.root / "windows.json"),
            "--linux-package", str(self.root / "Loop.AppImage"),
            "--windows-package", str(self.root / "Loop.msi"),
        ]

    @staticmethod
    def build_steps() -> list[str]:
        return [
            "Build Widgets-absent release profile",
            "Test Widgets-absent release profile",
            "Build project",
            "Run unit tests",
            "Run preflight corpus gate",
        ]

    @staticmethod
    def job(name: str, identifier: int, steps: list[str]) -> dict:
        return {
            "name": name,
            "databaseId": identifier,
            "status": "completed",
            "conclusion": "success",
            "steps": [
                {"name": step, "status": "completed", "conclusion": "success"}
                for step in steps
            ],
        }

    def write_inputs(self) -> None:
        self.run_path.write_text(json.dumps(self.snapshot), encoding="utf-8")
        for platform, record in self.records.items():
            (self.root / f"{platform}.json").write_text(json.dumps(record), encoding="utf-8")

    def run_checker(self) -> tuple[int, str, str]:
        self.write_inputs()
        output, errors = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            code = checker.main(self.arguments)
        return code, output.getvalue(), errors.getvalue()

    def assert_rejected(self, reason: str) -> None:
        code, output, errors = self.run_checker()
        self.assertEqual(code, 1)
        self.assertEqual(output, "")
        self.assertIn(reason, errors)

    def test_valid_evidence_verifies_provenance_and_keeps_admission_pending(self) -> None:
        code, output, errors = self.run_checker()
        self.assertEqual(code, 0, errors)
        self.assertIn(self.candidate_sha, output)
        self.assertIn("Module admission: PENDING REVIEW", output)
        self.assertIn("/job/3", output)
        self.assertIn(self.records["linux"]["package"]["sha256"], output)

    def test_direct_script_invocation(self) -> None:
        self.write_inputs()
        result = subprocess.run(
            [sys.executable, str(Path(checker.__file__)), *self.arguments],
            cwd=self.root, capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("PENDING REVIEW", result.stdout)

    def test_candidate_must_be_a_full_sha(self) -> None:
        self.arguments[1] = "dev"
        self.assert_rejected("full 40-character")

    def test_mixed_ci_and_package_shas_are_rejected(self) -> None:
        self.snapshot["headSha"] = "b" * 40
        self.assert_rejected("CI headSha")
        self.snapshot["headSha"] = self.candidate_sha
        self.records["windows"]["source_sha"] = "b" * 40
        self.assert_rejected("source SHA mismatch")

    def test_aggregate_success_does_not_hide_bad_required_jobs(self) -> None:
        for conclusion in ("skipped", "cancelled", "failure", None):
            with self.subTest(conclusion=conclusion):
                self.snapshot["jobs"][2]["conclusion"] = conclusion
                self.assert_rejected("windows / build must be completed and successful")
        self.snapshot["jobs"][2]["conclusion"] = "success"
        self.snapshot["jobs"][2]["status"] = "in_progress"
        self.assert_rejected("windows / build must be completed and successful")

    def test_required_jobs_and_steps_cannot_be_missing_or_duplicated(self) -> None:
        original = copy.deepcopy(self.snapshot)
        for missing in (True, False):
            with self.subTest(job_missing=missing):
                self.snapshot = copy.deepcopy(original)
                if missing:
                    self.snapshot["jobs"].pop()
                else:
                    self.snapshot["jobs"].append(copy.deepcopy(self.snapshot["jobs"][2]))
                self.assert_rejected("expected exactly one 'windows / build'")
            with self.subTest(step_missing=missing):
                self.snapshot = copy.deepcopy(original)
                steps = self.snapshot["jobs"][1]["steps"]
                if missing:
                    steps.pop()
                else:
                    steps.append(copy.deepcopy(steps[-1]))
                self.assert_rejected("expected exactly one 'Run preflight corpus gate'")

    def test_skipped_required_step_is_rejected_but_unrelated_skip_is_allowed(self) -> None:
        self.snapshot["jobs"][1]["steps"][3]["conclusion"] = "skipped"
        self.assert_rejected("Run unit tests must be completed and successful")
        self.snapshot["jobs"][1]["steps"][3]["conclusion"] = "success"
        self.snapshot["jobs"][1]["steps"].append(
            {"name": "Prove change with agent-fast", "status": "completed", "conclusion": "skipped"}
        )
        self.snapshot["jobs"].append(
            {"name": "agent-fast / build", "status": "completed", "conclusion": "skipped"}
        )
        self.assertEqual(self.run_checker()[0], 0)

    def test_each_required_step_must_complete_successfully(self) -> None:
        for job in self.snapshot["jobs"]:
            for step in job["steps"]:
                with self.subTest(job=job["name"], step=step["name"]):
                    step["status"] = "in_progress"
                    self.assert_rejected(f"{step['name']} must be completed and successful")
                    step["status"] = "completed"

    def test_run_must_be_terminal_and_from_this_repository(self) -> None:
        self.snapshot["conclusion"] = "failure"
        self.assert_rejected("CI run must be completed and successful")
        self.snapshot["conclusion"] = "success"
        self.snapshot["url"] = "https://github.com/other/repo/actions/runs/100"
        self.assert_rejected("CI URL")

    def test_malformed_run_records_fail_explicitly(self) -> None:
        original = copy.deepcopy(self.snapshot)
        for malformed in ([], None, "jobs", [None]):
            with self.subTest(jobs=malformed):
                self.snapshot = copy.deepcopy(original)
                self.snapshot["jobs"] = malformed
                self.assert_rejected("CI jobs")
        self.snapshot = copy.deepcopy(original)
        self.snapshot["jobs"][1]["steps"] = [None]
        self.assert_rejected("steps must be an array of objects")
        self.snapshot = []
        self.assert_rejected("CI snapshot must be a JSON object")

    def test_incomplete_and_malformed_package_evidence_are_rejected(self) -> None:
        self.records["linux"]["checks"]["all_payload_files_hashed"] = False
        self.assert_rejected("checks are incomplete")
        self.records["linux"] = []
        self.assert_rejected("package evidence must be a JSON object")

    def test_package_identity_requires_name_size_and_actual_bytes(self) -> None:
        identity = self.records["linux"]["package"]
        original = copy.deepcopy(identity)
        for size in (None, True, -1, original["size"] + 1):
            with self.subTest(size=size):
                identity["size"] = size
                self.assert_rejected("byte count")
        identity.update(original)
        identity["name"] = "Other.AppImage"
        self.assert_rejected("name differs")
        identity.update(original)
        (self.root / "Loop.AppImage").write_bytes(b"x" * original["size"])
        self.assert_rejected("SHA-256 differs")

    def test_invalid_json_or_absent_package_is_rejected(self) -> None:
        self.write_inputs()
        self.run_path.write_text("{", encoding="utf-8")
        errors = io.StringIO()
        with contextlib.redirect_stderr(errors):
            self.assertEqual(checker.main(self.arguments), 1)
        self.assertIn("rejected", errors.getvalue())
        (self.root / "Loop.msi").unlink()
        self.assert_rejected("package must be a regular file")


if __name__ == "__main__":
    unittest.main()
