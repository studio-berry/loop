"""Regression checks for workflow paths and mandatory fast-gate tests."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


class WorkflowContractTests(unittest.TestCase):
    def test_agent_fast_runs_its_dedicated_policy_tests(self):
        workflow = (ROOT / ".github/workflows/reusable-linux.yml").read_text(encoding="utf-8")
        self.assertIn("name: Test agent policy checker", workflow)
        self.assertIn("if: inputs.fast", workflow)
        self.assertIn("python3 -m unittest scripts.agent.test_check_change -v", workflow)

    def test_windows_installer_verifies_from_its_checkout_root(self):
        workflow = (ROOT / ".github/workflows/WindowsInstall.yml").read_text(encoding="utf-8")
        self.assertIn("working-directory: loupe", workflow)
        self.assertIn(".\\scripts\\verify-loupe-surface.ps1", workflow)
        self.assertNotIn(".\\loupe\\scripts\\verify-loupe-surface.ps1", workflow)

    def test_ci_runs_phase5_widgets_evidence_and_contract(self):
        workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        self.assertIn("python3 scripts/generate_phase5_widgets_evidence.py --check", workflow)
        self.assertIn("python3 scripts/verify_phase5_widgets_contract.py", workflow)
        self.assertIn("python3 scripts/verify-plugin-form-accounting.py", workflow)
        self.assertIn("python3 scripts/verify-widgets-library-consumer-graph.py", workflow)

    def test_windows_validation_runs_phase5_widgets_twin(self):
        workflow = (ROOT / ".github/workflows/WindowsInstall.yml").read_text(encoding="utf-8")
        self.assertIn(".\\scripts\\verify-phase5-widgets-contract.ps1", workflow)

    def test_linux_release_gate_verifies_loupe_release_surface(self):
        workflow = (ROOT / ".github/workflows/reusable-linux.yml").read_text(encoding="utf-8")
        self.assertIn("LOUPE_LOUPE_DISTRIBUTION=ON", workflow)
        self.assertIn("-Profile loupe-release", workflow)
        self.assertNotIn("-Profile developer", workflow)


if __name__ == "__main__":
    unittest.main()
