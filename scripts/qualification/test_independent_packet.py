from __future__ import annotations

import json
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

from scripts.qualification import install_oracle_bundle as installer
from scripts.qualification import verify_independent_packets as packets
from scripts.qualification import run_independent_validators as validators


class PacketIntegrityTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.inventory = {"corpus_revision": "corpus", "fixtures": [], "derived_fixtures": []}
        self.lane = {"schema": "loop.independent-qualification-packet", "schema_version": 2,
                     "source_sha": "a" * 40, "corpus_revision": "corpus", "inventory_sha256": packets.digest(packets.INVENTORY),
                     "status": "passed", "platform": "Linux", "run_url": "https://example.test/run", "distribution_sha256": "b" * 64,
                     "tests": {name: {"exit_code": 0} for name in packets.REQUIRED_TESTS},
                     "binaries": {name: "c" * 64 for name in packets.REQUIRED_TESTS}, "outcomes": {}}
        (self.directory / "ghostscript").mkdir()
        packets.write_json(self.directory / "ghostscript/measurements.json", {"status": "passed", "fixtures": []})
        packets.seal(self.directory, self.lane)

    def test_packet_members_and_source_are_bound(self):
        packets.verify(self.directory, self.inventory, "a" * 40)
        with self.assertRaisesRegex(ValueError, "Source SHA"):
            packets.verify(self.directory, self.inventory, "b" * 40)
        (self.directory / "ghostscript/measurements.json").write_text("changed")
        with self.assertRaisesRegex(ValueError, "integrity"):
            packets.verify(self.directory, self.inventory, "a" * 40)

    def test_historical_missing_and_failed_lanes_cannot_qualify(self):
        for alteration in ({"schema_version": 1}, {"status": "incomplete"}, {"tests": {}}, {"binaries": {}}, {"outcomes": {"invented": {}}}):
            lane = {**self.lane, **alteration}
            packets.seal(self.directory, lane)
            with self.assertRaises(ValueError):
                packets.verify(self.directory, self.inventory, "a" * 40)

    def test_expected_rejection_never_changes_compliance(self):
        artifact = self.directory / "negative.pdf"
        artifact.write_bytes(b"negative")
        raw = self.directory / "negative.stdout"
        raw.write_bytes(('<report><jobs><job><item><name>{input_path}</name></item><validationReport profileName="PDF/A-2B validation profile" isCompliant="false"/></job></jobs></report>').format(input_path=artifact.as_posix()).encode())
        evidence = {"schema": validators.SCHEMA, "schema_version": 2, "platform": {"system": "Linux"}, "candidate_sha": "a" * 40, "status": "rejected", "input": {"path": str(artifact), "bytes": 8, "sha256": packets.digest(artifact)},
                    "validators": [{"claim": "standards", "target": "pdfa-2b", "scope": "exact-input-bytes", "limitations": [], "status": "rejected", "version": "veraPDF 1.28.2", "command": ["verapdf", "negative.pdf"], "report_sha256": packets.digest(raw)}]}
        packets.write_json(self.directory / "negative.json", evidence)
        self.lane["outcomes"] = {"negative": {"record": "negative.json", "artifact": "negative.pdf", "raw_reports": {"standards": "negative.stdout"}}}
        self.inventory["fixtures"] = [{"id": "negative", "claim": "standards", "expected": "rejected"}]
        packets.seal(self.directory, self.lane)
        packets.verify(self.directory, self.inventory, "a" * 40)
        evidence["status"] = "passed"
        packets.write_json(self.directory / "negative.json", evidence)
        packets.seal(self.directory, self.lane)
        with self.assertRaisesRegex(ValueError, "Expected rejection"):
            packets.verify(self.directory, self.inventory, "a" * 40)

    def test_rendering_artifact_identity_is_bound(self):
        fixture = {"id": "render", "claim": "rendering", "expected": "passed"}
        self.inventory["fixtures"] = [fixture]
        (self.directory / "core-renders").mkdir()
        (self.directory / "core-renders/render.pdf").write_bytes(b"changed PDF")
        packets.write_json(self.directory / "ghostscript/measurements.json", {
            "status": "passed", "fixtures": [{"fixture": "render", "status": "passed",
            "artifact_sha256": "0" * 64, "planes": [{"separation": "Cyan"}]}]})
        packets.seal(self.directory, self.lane)
        with self.assertRaisesRegex(ValueError, "Rendering artifact identity"):
            packets.verify(self.directory, self.inventory, "a" * 40)

    def test_unknown_render_channel_encoding_cannot_qualify(self):
        fixture = {"id": "render", "claim": "rendering", "expected": "passed", "width": 8, "height": 8}
        self.inventory["fixtures"] = [fixture]
        (self.directory / "core-renders").mkdir()
        artifact = self.directory / "core-renders/render.pdf"
        artifact.write_bytes(b"PDF")
        packets.write_json(self.directory / "core-renders/render.json", {
            "width": 8, "height": 8, "process_space": "DeviceCMYK", "paper": "opaque-white",
            "channel_encoding": "ink-without-opacity", "channels": [{"name": "Cyan"}]})
        packets.write_json(self.directory / "ghostscript/measurements.json", {
            "status": "passed", "fixtures": [{"fixture": "render", "status": "passed",
            "artifact_sha256": packets.digest(artifact), "planes": [{"separation": "Cyan"}]}]})
        packets.seal(self.directory, self.lane)
        with self.assertRaisesRegex(ValueError, "channel encoding"):
            packets.verify(self.directory, self.inventory, "a" * 40)

    def test_missing_platform_prevents_qualification(self):
        with mock.patch("sys.stderr"), mock.patch("sys.argv", ["verify", "--source-sha", "a" * 40, str(self.directory)]), mock.patch.object(packets, "verify", return_value={"platform": "Linux"}):
            with self.assertRaises(SystemExit):
                packets.main()

    def test_bundle_digest_and_traversal_are_rejected(self):
        archive = self.directory / "tools.zip"
        with zipfile.ZipFile(archive, "w") as bundle:
            bundle.writestr("../escape", "bad")
        with self.assertRaisesRegex(ValueError, "SHA-256"):
            installer.unpack(archive, "0" * 64, self.directory / "oracles")
        with self.assertRaisesRegex(ValueError, "Unsafe"):
            installer.unpack(archive, packets.digest(archive), self.directory / "oracles")
        self.assertFalse((self.directory.parent / "escape").exists())


if __name__ == "__main__":
    unittest.main()
