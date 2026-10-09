"""Exercise packaging identity guards before candidate code or cache access."""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = ROOT / ".github" / "workflows"


def step_run(workflow: str, name: str) -> str:
    steps = (WORKFLOWS / workflow).read_text(encoding="utf-8").split("      - name: ")
    step = next(step for step in steps if step.startswith(name + "\n"))
    lines = []
    for line in step.split("        run: |\n", 1)[1].splitlines():
        if line.strip() and not line.startswith("          "):
            break
        lines.append(line)
    return textwrap.dedent("\n".join(lines))


class PackageSourceIdentityTests(unittest.TestCase):
    def test_checkout_is_bound_to_workflow_commit_before_repository_code(self):
        for workflow, verifier in (
            ("LinuxInstall.yml", "scripts/ci/check_supply_chain_pins.py"),
            ("WindowsInstall.yml", "scripts\\ci\\check_supply_chain_pins.py"),
        ):
            with self.subTest(workflow=workflow):
                text = (WORKFLOWS / workflow).read_text(encoding="utf-8")
                checkouts = [step for step in text.split("      - name: ")
                             if "uses: actions/checkout@" in step]
                self.assertTrue(checkouts)
                for checkout in checkouts:
                    self.assertIn("ref: ${{ github.sha }}", checkout)
                    self.assertNotIn("inputs.source_sha", checkout)
                self.assertIn("WORKFLOW_SOURCE_SHA: ${{ github.sha }}", text)
                guard = text.index("- name: Verify exact source SHA")
                self.assertLess(guard, text.index(verifier))
                self.assertLess(guard, text.index("uses: actions/cache@"))

    def exercise_guard(self, workflow: str, interpreter: list[str]):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            repository = directory / "source"
            subprocess.run(["git", "init", "--quiet", str(repository)], check=True)
            subprocess.run(["git", "-C", str(repository), "-c", "user.name=Workflow test",
                            "-c", "user.email=workflow-test@example.invalid", "commit",
                            "--quiet", "--allow-empty", "-m", "Candidate"], check=True)
            sha = subprocess.check_output(["git", "-C", str(repository), "rev-parse", "HEAD"],
                                          text=True).strip()
            evidence = directory / "github-env"
            script = step_run(workflow, "Verify exact source SHA")
            cases = [
                (sha, sha, True),
                (sha.upper(), sha, True),
                ("0" * 40, sha, False),
                (sha, "0" * 40, False),
                ("dev", sha, False),
                (sha[:12], sha, False),
                (sha + "\n", sha, False),
                ("$(exit 0)", sha, False),
            ]
            for expected, workflow_sha, admitted in cases:
                with self.subTest(workflow=workflow, expected=expected, workflow_sha=workflow_sha):
                    evidence.unlink(missing_ok=True)
                    environment = dict(os.environ, EXPECTED_SOURCE_SHA=expected,
                                       WORKFLOW_SOURCE_SHA=workflow_sha, GITHUB_ENV=str(evidence))
                    result = subprocess.run([*interpreter, script], cwd=repository,
                                            env=environment, capture_output=True, text=True)
                    if admitted:
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                        self.assertEqual(evidence.read_text(encoding="utf-8-sig").strip(),
                                         "LOOP_SOURCE_SHA=" + sha)
                    else:
                        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                        self.assertFalse(evidence.exists(), "Rejected identities must not reach evidence publication")

    @unittest.skipIf(os.name == "nt", "Bash guard runs in the Linux CI lane")
    def test_linux_guard_admits_only_the_exact_workflow_source(self):
        self.exercise_guard("LinuxInstall.yml", ["bash", "-eu", "-c"])

    @unittest.skipUnless(os.name == "nt" or shutil.which("pwsh"), "PowerShell guard runs in the Windows CI lane")
    def test_windows_guard_admits_only_the_exact_workflow_source(self):
        self.assertIsNotNone(shutil.which("pwsh"))
        self.exercise_guard("WindowsInstall.yml", ["pwsh", "-NoProfile", "-NonInteractive", "-Command"])

    @unittest.skipIf(os.name == "nt", "Repository-dispatch proxy runs on Linux")
    def test_proxy_dispatches_the_selected_workflow_ref_without_executing_it(self):
        text = (WORKFLOWS / "dispatch-packaging.yml").read_text(encoding="utf-8")
        self.assertIn("SOURCE_REF: ${{ github.event.client_payload.source_ref || 'stable' }}", text)
        script = step_run("dispatch-packaging.yml", "Dispatch Linux and Windows package workflows")
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            trace = directory / "dispatch.jsonl"
            fake_gh = directory / "gh"
            fake_gh.write_text("#!" + sys.executable + "\n" +
                               "import json, os, sys\n" +
                               "with open(os.environ['DISPATCH_TRACE'], 'a') as stream:\n" +
                               "    stream.write(json.dumps(sys.argv[1:]) + '\\n')\n",
                               encoding="utf-8")
            fake_gh.chmod(0o755)
            sha = "a" * 40
            for ref in ("stable", "qualification/candidate", "candidate; touch injected"):
                with self.subTest(ref=ref):
                    trace.unlink(missing_ok=True)
                    environment = dict(os.environ, SOURCE_SHA=sha, SOURCE_REF=ref,
                                       DISPATCH_TRACE=str(trace), PATH=str(directory) + os.pathsep + os.environ["PATH"])
                    result = subprocess.run(["bash", "-eu", "-c", script], cwd=directory,
                                            env=environment, capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    calls = [json.loads(line) for line in trace.read_text().splitlines()]
                    self.assertEqual(calls, [
                        ["workflow", "run", workflow, "--repo", "studio-berry/loop",
                         "--ref", ref, "-f", "source_sha=" + sha]
                        for workflow in ("LinuxInstall.yml", "WindowsInstall.yml")
                    ])
                    self.assertFalse((directory / "injected").exists())


if __name__ == "__main__":
    unittest.main()
