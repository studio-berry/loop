from __future__ import annotations

import json
import shutil
import tempfile
import unittest
from pathlib import Path

from scripts.ci import check_operator_help as checker
from scripts.ci.generate_operator_samples import generated_files


class OperatorHelpContractTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for name in ("LoopEditor/app.qrc", "LoopEditor/operator", "docs/generated/preflight-check-catalog.json"):
            source = checker.ROOT / name
            destination = self.root / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            if source.is_dir():
                shutil.copytree(source, destination)
            else:
                shutil.copyfile(source, destination)
        self.expected = generated_files()

    def check(self) -> list[str]:
        return checker.check(self.root, self.expected)

    def test_current_corpus_and_help_binding_pass(self) -> None:
        self.assertEqual(self.check(), [])

    def test_unbundled_member_fails(self) -> None:
        qrc = self.root / "LoopEditor/app.qrc"
        qrc.write_text(qrc.read_text(encoding="utf-8").replace('alias="operator/samples/pass.pdf"', 'alias="lost.pdf"'), encoding="utf-8")
        self.assertTrue(any("not bundled" in error for error in self.check()))

    def test_help_cannot_bind_an_independent_catalog(self) -> None:
        qrc = self.root / "LoopEditor/app.qrc"
        qrc.write_text(qrc.read_text(encoding="utf-8").replace("../docs/generated/preflight-check-catalog.json", "operator/old-catalog.json"), encoding="utf-8")
        self.assertTrue(any("generated check catalog directly" in error for error in self.check()))

    def test_missing_limits_fail(self) -> None:
        path = self.root / "docs/generated/preflight-check-catalog.json"
        catalog = json.loads(path.read_text(encoding="utf-8"))
        catalog["checks"]["bleed"]["limitations"] = ""
        path.write_text(json.dumps(catalog), encoding="utf-8")
        self.assertTrue(any("bleed" in error for error in self.check()))

    def test_changed_sample_fails(self) -> None:
        (self.root / "LoopEditor/operator/samples/pass.pdf").write_bytes(b"changed")
        self.assertTrue(any("deterministic source: pass.pdf" in error for error in self.check()))

    def test_missing_license_fails(self) -> None:
        (self.root / "LoopEditor/operator/samples/LICENSE.txt").unlink()
        self.assertTrue(any("LICENSE.txt" in error for error in self.check()))

    def test_missing_workspace_fails(self) -> None:
        path = self.root / "LoopEditor/operator/guide.json"
        guide = json.loads(path.read_text(encoding="utf-8"))
        guide["workspaces"].pop()
        path.write_text(json.dumps(guide), encoding="utf-8")
        self.assertTrue(any("seven workspaces" in error for error in self.check()))


if __name__ == "__main__":
    unittest.main()
