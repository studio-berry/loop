from __future__ import annotations

import contextlib
import copy
import io
import json
import tempfile
import unittest
from pathlib import Path

from scripts.qualification import verify_operator_acceptance as checker


class OperatorPacketTest(unittest.TestCase):
    source_sha = "a" * 40

    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.packet = self.make_packet("linux")

    @staticmethod
    def write(path: Path, value: dict) -> None:
        path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")

    def make_packet(self, platform: str) -> Path:
        root = self.root / platform
        root.mkdir()
        package_name = "Loop.AppImage" if platform == "linux" else "Loop.msi"
        for name in (package_name, "source.pdf", "published.pdf", "journey.mp4", "journey.log"):
            (root / name).write_bytes(f"synthetic test bytes: {name}".encode())
        source = checker.digest(root / "source.pdf")
        output = checker.digest(root / "published.pdf")
        plan, profile, report = "b" * 64, "c" * 64, "d" * 64
        approval = {"plan_digest": plan, "source_sha256": source, "candidate_sha256": output}
        sign_off = {**approval, "published_sha256": output, "effective_profile_digest": profile,
                    "revalidation_report_sha256": report,
                    "approval": {"decision": "approve", "actorId": "Editor", "policyId": "desktop-postflight"}}
        self.write(root / "receipt.json", {"governed": {
            "approval": approval, "sign_off": sign_off,
            "revalidation": {"artifact_sha256": output, "report_sha256": report,
                             "effective_profile_digest": profile, "bytes_verified": True,
                             "sign_off_eligible": True, "verdict": {"state": "pass"}}}})
        self.write(root / "inspection.json", {
            "document_revision_digest": source, "effective_profile_digest": profile,
            "inspection_complete": True, "errors": [{"id": "bleed-1"}], "warnings": []})
        package_digest = checker.digest(root / package_name)
        executable = "LoopEditor.exe" if platform == "windows" else "LoopEditor"
        installed_root = "C:/test/install" if platform == "windows" else "/test/install"
        self.write(root / "boundary.json", {
            "kind": "loop-package-boundary-evidence", "schema_version": 1,
            "source_sha": self.source_sha, "platform": platform, "status": "passed",
            "forbidden_findings": [], "checks": {key: True for key in (
                "all_payload_files_hashed", "all_binary_files_inspected", "target_architecture_matches",
                "qt6widgets_absent", "qt6widgets_surface_absent", "unresolved_non_system_dependencies_absent")},
            "package": {"name": package_name, "format": "AppImage" if platform == "linux" else "MSI",
                        "sha256": package_digest, "size": (root / package_name).stat().st_size},
            "payload": {"files": [{"path": f"bin/{executable}", "sha256": "e" * 64}]}})
        self.write(root / "environment.json", {
            "platform": platform, "os": platform, "os_version": "test", "architecture": "x64",
            "host": "test-only", "recorded_utc": "2026-10-06T00:00:00Z", "clean_machine": True})
        self.write(root / "installation.json", {
            "command": "synthetic install", "installed_root": installed_root, "status": "passed",
            "source_sha": self.source_sha, "package_sha256": package_digest, "exit_code": 0})
        self.write(root / "launch.json", {
            "command": "synthetic launch", "installed_root": installed_root, "executable": f"{installed_root}/bin/{executable}",
            "surface": "LoopEditor", "scope": "installed-product", "status": "passed", "source_sha": self.source_sha,
            "package_sha256": package_digest, "payload_path": f"bin/{executable}", "executable_sha256": "e" * 64})
        packet = {
            "kind": "loop-operator-acceptance-packet", "version": 1, "platform": platform,
            "source_sha": self.source_sha, "status": "passed", "unavailable": [],
            "package": package_name, "package_evidence": "boundary.json", "environment": "environment.json",
            "installation": "installation.json", "launch": "launch.json", "source_pdf": "source.pdf",
            "published_pdf": "published.pdf", "inspection": "inspection.json", "receipt": "receipt.json",
            "video": "journey.mp4", "log": "journey.log",
            "steps": [{"action": action, "status": "passed", "implementation": "product-core",
                       "video_seconds": index, "observation": "synthetic test observation",
                       "finding_id": "bleed-1", "plan_digest": plan} for index, action in enumerate(checker.STEPS)]}
        self.write(root / "packet.json", packet)
        self.seal(root)
        return root

    def seal(self, root: Path) -> None:
        packet = checker.read_object(root / "packet.json")
        packet["members"] = {path.name: checker.digest(path) for path in root.iterdir()
                             if path.is_file() and path.name not in ("packet.json", "review.json")}
        self.write(root / "packet.json", packet)
        self.write(root / "review.json", {
            "kind": "human", "decision": "approve", "packet_sha256": checker.digest(root / "packet.json"),
            "reviewer": "synthetic test reviewer", "reviewed_utc": "2026-10-06T00:00:00Z",
            "rationale": "test-only fixture", "attestations": {key: True for key in checker.ATTESTATIONS}})

    def mutate(self, name: str, update, reseal: bool = True) -> None:
        path = self.packet / name
        value = checker.read_object(path)
        update(value)
        self.write(path, value)
        if reseal:
            self.seal(self.packet)

    def reject(self, reason: str) -> None:
        with self.assertRaisesRegex((ValueError, OSError), reason):
            checker.verify(self.packet, self.source_sha)

    def test_complete_synthetic_pair_exercises_verifier(self) -> None:
        windows = self.make_packet("windows")
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(checker.main(["--source-sha", self.source_sha, str(self.packet), str(windows)]), 0)

    def test_skipped_mocked_or_failed_step_is_rejected(self) -> None:
        original = checker.read_object(self.packet / "packet.json")
        for field, value in (("status", "skipped"), ("status", "failed"), ("implementation", "mock-core")):
            with self.subTest(field=field, value=value):
                modified = copy.deepcopy(original)
                modified["steps"][1][field] = value
                self.write(self.packet / "packet.json", modified)
                self.seal(self.packet)
                self.reject("skipped, failed or mocked")

    def test_step_order_is_required(self) -> None:
        self.mutate("packet.json", lambda value: value["steps"].reverse())
        self.reject("out of order")

    def test_malformed_step_is_rejected(self) -> None:
        self.mutate("packet.json", lambda value: value["steps"].append(None))
        self.reject("out of order")

    def test_missing_payload_inventory_is_rejected(self) -> None:
        self.mutate("boundary.json", lambda value: value.update(payload=None))
        self.reject("payload inventory")

    def test_missing_attestation_is_rejected(self) -> None:
        self.mutate("review.json", lambda value: value.update(attestations=None), False)
        self.reject("human review")

    def test_video_order_is_required(self) -> None:
        self.mutate("packet.json", lambda value: value["steps"][0].update(video_seconds=100))
        self.reject("video offsets")

    def test_missing_video_is_rejected(self) -> None:
        (self.packet / "journey.mp4").unlink()
        self.reject("missing")

    def test_empty_video_is_rejected(self) -> None:
        (self.packet / "journey.mp4").write_bytes(b"")
        self.seal(self.packet)
        self.reject("empty video")

    def test_changed_member_is_rejected(self) -> None:
        (self.packet / "journey.log").write_bytes(b"changed after review")
        self.reject("digest mismatch")

    def test_unbound_member_is_rejected(self) -> None:
        (self.packet / "unbound.txt").write_text("unbound", encoding="utf-8")
        self.reject("unbound")

    def test_unavailable_lane_is_rejected(self) -> None:
        self.mutate("packet.json", lambda value: value.update(unavailable=["Linux not run"]))
        self.reject("unavailable lanes")

    def test_different_source_sha_is_rejected(self) -> None:
        self.mutate("boundary.json", lambda value: value.update(source_sha="f" * 40))
        self.reject("different source SHA")

    def test_changed_package_is_rejected_even_after_resealing(self) -> None:
        (self.packet / "Loop.AppImage").write_bytes(b"another package")
        self.seal(self.packet)
        self.reject("byte count differs")

    def test_nonclean_host_is_rejected(self) -> None:
        self.mutate("environment.json", lambda value: value.update(clean_machine=False))
        self.reject("clean-machine")

    def test_build_tree_or_harness_is_rejected(self) -> None:
        self.mutate("launch.json", lambda value: value.update(surface="ProductQuickAccessibilitySmoke", scope="build-tree"))
        self.reject("installed product")

    def test_wrong_executable_digest_is_rejected(self) -> None:
        self.mutate("launch.json", lambda value: value.update(executable_sha256="f" * 64))
        self.reject("package payload")

    def test_launch_outside_installation_is_rejected(self) -> None:
        self.mutate("launch.json", lambda value: value.update(executable="/build/LoopEditor"))
        self.reject("installed LoopEditor")

    def test_harness_cannot_masquerade_as_product(self) -> None:
        self.mutate("launch.json", lambda value: value.update(executable="/test/install/ProductQuickAccessibilitySmoke"))
        self.reject("installed LoopEditor")

    def test_nonfinite_video_offset_is_rejected(self) -> None:
        self.mutate("packet.json", lambda value: value["steps"][1].update(video_seconds=float("nan")))
        self.reject("video offset")

    def test_different_installed_package_is_rejected(self) -> None:
        self.mutate("installation.json", lambda value: value.update(package_sha256="f" * 64))
        self.reject("inspected package")

    def test_located_finding_must_exist(self) -> None:
        self.mutate("packet.json", lambda value: value["steps"][2].update(finding_id="invented"))
        self.reject("located finding")

    def test_stale_plan_is_rejected(self) -> None:
        self.mutate("packet.json", lambda value: value["steps"][4].update(plan_digest="f" * 64))
        self.reject("same plan")

    def test_retained_pdf_must_match_receipt(self) -> None:
        (self.packet / "published.pdf").write_bytes(b"separately saved PDF")
        self.seal(self.packet)
        self.reject("retained source and published PDF")

    def test_incomplete_inspection_is_rejected(self) -> None:
        self.mutate("inspection.json", lambda value: value.update(inspection_complete=False))
        self.reject("incomplete")

    def test_failed_revalidation_is_rejected(self) -> None:
        self.mutate("receipt.json", lambda value: value["governed"]["revalidation"].update(bytes_verified=False))
        self.reject("bytes_verified")

    def test_human_review_cannot_be_pending_or_automated(self) -> None:
        for field, value in (("decision", "pending"), ("kind", "agent")):
            with self.subTest(field=field):
                self.seal(self.packet)
                self.mutate("review.json", lambda record: record.update({field: value}), False)
                self.reject("human review")

    def test_review_must_bind_packet(self) -> None:
        self.mutate("packet.json", lambda value: value["steps"][0].update(observation="changed"), False)
        self.reject("human review")

    def test_pair_requires_both_platforms(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(checker.main(["--source-sha", self.source_sha, str(self.packet), str(self.packet)]), 1)

    def test_pair_requires_matching_semantic_receipt(self) -> None:
        windows = self.make_packet("windows")
        receipt = checker.read_object(windows / "receipt.json")
        for key in ("approval", "sign_off"):
            receipt["governed"][key]["plan_digest"] = "f" * 64
        self.write(windows / "receipt.json", receipt)
        packet = checker.read_object(windows / "packet.json")
        for step in packet["steps"][3:]:
            step["plan_digest"] = "f" * 64
        self.write(windows / "packet.json", packet)
        self.seal(windows)
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(checker.main(["--source-sha", self.source_sha, str(self.packet), str(windows)]), 1)

    def test_member_cannot_escape_packet(self) -> None:
        with self.assertRaisesRegex(ValueError, "escapes"):
            checker.member(self.packet, "../outside.json")


if __name__ == "__main__":
    unittest.main()
