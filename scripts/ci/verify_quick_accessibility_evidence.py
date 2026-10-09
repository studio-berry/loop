#!/usr/bin/env python3
"""Fail-closed gate over the installed-tree Quick accessibility qualification.

The package workflows run the product Quick accessibility harness twice against
the installed tree -- once on the native (platform-default) graphics backend and
once with ``QT_QUICK_BACKEND=software`` -- and, on Windows, drive the native UI
Automation backend once. Each run writes a qualification record
(``scripts/run-product-quick-a11y-smoke.ps1`` /
``scripts/run-installed-quick-a11y-uia.ps1``). This gate reads the records and
fails when any lane is absent, was not captured against an installed tree, does
not carry its own claim, or reports a backend inconsistent with that claim.

The records are not interchangeable:

* ``native-backend-operator-path`` -- platform-default graphics backend.
* ``software-renderer-operator-path`` -- forced software rasterizer.
* ``native-accessibility-backend`` -- the real OS accessibility bridge
  (Windows UI Automation), required with ``--require-native-accessibility``.

A software record can never satisfy the native-accessibility claim: it carries a
different claim and no OS-accessibility observation, so
``--require-native-accessibility`` fails closed when that lane is missing,
skipped, failed, or replaced.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path


KIND = "loop-quick-accessibility-qualification"
NATIVE_CLAIM = "native-backend-operator-path"
SOFTWARE_CLAIM = "software-renderer-operator-path"
NATIVE_ACCESSIBILITY_CLAIM = "native-accessibility-backend"
INSTALLED_SCOPE = "installed-tree"
UNKNOWN_GRAPHICS_APIS = {"unknown", "null", "unrecognized", "absent", ""}


def _load(path: Path, label: str) -> tuple[dict | None, list[str]]:
    if not path.is_file():
        return None, [f"{label} accessibility evidence is missing: {path}"]
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as exc:
        return None, [f"{label} accessibility evidence is not readable JSON: {path}: {exc}"]
    if not isinstance(value, dict):
        return None, [f"{label} accessibility evidence must be a JSON object: {path}"]
    return value, []


def _normalized(path: str) -> str:
    return os.path.normcase(os.path.abspath(path))


def _check_common(record: dict, label: str, expected_sha: str, install_tree: str) -> list[str]:
    errors: list[str] = []
    if record.get("kind") != KIND:
        errors.append(f"{label} evidence kind is {record.get('kind')!r}, expected {KIND!r}")
    if record.get("status") != "pass":
        errors.append(f"{label} evidence did not record a passing run: {record.get('status')!r}")
    source = record.get("source_sha")
    if not isinstance(source, str) or not source:
        errors.append(f"{label} evidence does not record a source SHA")
    elif expected_sha and source != expected_sha.lower():
        errors.append(
            f"{label} evidence source SHA {source} does not match the qualification SHA {expected_sha.lower()}"
        )
    artifact = record.get("artifact")
    if not isinstance(artifact, dict):
        errors.append(f"{label} evidence has no artifact identity")
        artifact = {}
    if artifact.get("scope") != INSTALLED_SCOPE:
        errors.append(
            f"{label} evidence was captured against {artifact.get('scope')!r}; "
            f"the qualification must run against an {INSTALLED_SCOPE!r}"
        )
    recorded_tree = artifact.get("install_tree")
    if not isinstance(recorded_tree, str) or not recorded_tree:
        errors.append(f"{label} evidence does not record the installed tree path")
    elif install_tree and _normalized(recorded_tree) != _normalized(install_tree):
        errors.append(f"{label} evidence installed tree {recorded_tree} does not match {install_tree}")
    executable_digest = artifact.get("executable_sha256")
    if not isinstance(executable_digest, str) or not re.fullmatch(r"[0-9a-f]{64}", executable_digest):
        errors.append(f"{label} evidence does not record the harness executable digest")
    return errors


def _check_native_accessibility(record: dict, expected_sha: str, install_tree: str) -> list[str]:
    errors = _check_common(record, "native accessibility", expected_sha, install_tree)
    if record.get("backend") != "native" or record.get("claim") != NATIVE_ACCESSIBILITY_CLAIM:
        errors.append(
            "native accessibility evidence does not carry the "
            f"{NATIVE_ACCESSIBILITY_CLAIM!r} claim: a software-only run cannot satisfy the native lane"
        )
    artifact = record.get("artifact") if isinstance(record.get("artifact"), dict) else {}
    if not artifact.get("fixture_sha256"):
        errors.append("native accessibility evidence does not record the operator fixture digest")
    observed = record.get("observed")
    if not isinstance(observed, dict):
        errors.append("native accessibility evidence has no observed backend record")
        observed = {}
    platform = observed.get("platform")
    if platform not in {"Windows UI Automation", "Linux AT-SPI"}:
        errors.append(f"native accessibility evidence did not use an OS accessibility client: {platform!r}")
    if observed.get("native_accessibility_backend_active") is not True:
        errors.append("native accessibility evidence does not record the native accessibility backend active")
    observed_api = observed.get("graphics_api")
    if not isinstance(observed_api, str) or observed_api.casefold() in UNKNOWN_GRAPHICS_APIS | {"software"}:
        errors.append(f"native accessibility evidence ran on a non-native graphics backend: {observed_api!r}")
    if not isinstance(observed.get("observation_count"), int) or observed.get("observation_count", 0) < 1:
        errors.append("native accessibility evidence does not record any accessibility observations")
    return errors


def verify(
    native_path: Path,
    software_path: Path,
    expected_sha: str,
    install_tree: str,
    native_accessibility_path: Path | None = None,
    require_native_accessibility: bool = False,
    package_boundary_path: Path | None = None,
) -> list[str]:
    errors: list[str] = []

    native, native_load_errors = _load(native_path, "native backend")
    errors.extend(native_load_errors)
    software, software_load_errors = _load(software_path, "software backend")
    errors.extend(software_load_errors)

    if native is not None:
        if native.get("backend") != "native":
            errors.append(f"native backend evidence records backend {native.get('backend')!r}")
        if native.get("claim") != NATIVE_CLAIM:
            errors.append(
                f"native backend evidence does not carry the {NATIVE_CLAIM!r} claim: "
                "a software-only run cannot satisfy the native lane"
            )
        errors.extend(_check_common(native, "native backend", expected_sha, install_tree))
        observed = native.get("observed")
        if not isinstance(observed, dict):
            errors.append("native backend evidence has no observed backend record")
            observed = {}
        graphics_api = observed.get("graphics_api")
        if not isinstance(graphics_api, str) or graphics_api.casefold() in UNKNOWN_GRAPHICS_APIS:
            errors.append(f"native backend evidence selected an unknown graphics backend: {graphics_api!r}")
        if graphics_api == "software":
            errors.append("native backend evidence fell back to the software rasterizer")

    if software is not None:
        if software.get("backend") != "software":
            errors.append(f"software backend evidence records backend {software.get('backend')!r}")
        if software.get("claim") != SOFTWARE_CLAIM:
            errors.append(f"software backend evidence does not carry the {SOFTWARE_CLAIM!r} claim")
        errors.extend(_check_common(software, "software backend", expected_sha, install_tree))
        observed = software.get("observed")
        if not isinstance(observed, dict):
            errors.append("software backend evidence has no observed backend record")
            observed = {}
        if observed.get("graphics_api") != "software":
            errors.append(
                f"software backend evidence did not report the software rasterizer: {observed.get('graphics_api')!r}"
            )

    native_accessibility = None
    if native_accessibility_path is not None:
        native_accessibility, access_load_errors = _load(native_accessibility_path, "native accessibility")
        errors.extend(access_load_errors)
        if native_accessibility is not None:
            errors.extend(_check_native_accessibility(native_accessibility, expected_sha, install_tree))
    elif require_native_accessibility:
        errors.append(
            "native accessibility evidence is required but was not provided; "
            "the native accessibility backend lane is missing, skipped or failed"
        )

    if package_boundary_path is not None:
        boundary, boundary_errors = _load(package_boundary_path, "package boundary")
        errors.extend(boundary_errors)
        if boundary is not None:
            checks = boundary.get("checks", {})
            if (boundary.get("kind") != "loop-package-boundary-evidence"
                    or boundary.get("status") != "passed"
                    or boundary.get("forbidden_findings") != []
                    or not isinstance(checks, dict)
                    or any(checks.get(key) is not True for key in (
                        "all_payload_files_hashed", "all_binary_files_inspected", "target_architecture_matches",
                        "qt6widgets_absent", "qt6widgets_surface_absent", "unresolved_non_system_dependencies_absent"))):
                errors.append("package dependency inspection did not pass every Widgets-free graph check")
            if not re.fullmatch(r"[0-9a-fA-F]{40}", expected_sha) or boundary.get("source_sha") != expected_sha.lower():
                errors.append("package boundary source SHA does not match the qualification SHA")
            package = boundary.get("package", {})
            package_digest = package.get("sha256") if isinstance(package, dict) else None
            if not isinstance(package_digest, str) or not re.fullmatch(r"[0-9a-f]{64}", package_digest):
                errors.append("package boundary has no package SHA-256")
            records = [(native, "native backend"), (software, "software backend")]
            if native_accessibility_path:
                records.append((native_accessibility, "native accessibility"))
                expected_platform = {"linux": "Linux AT-SPI", "windows": "Windows UI Automation"}.get(boundary.get("platform"))
                if native_accessibility is not None:
                    observed = native_accessibility.get("observed")
                    actual_platform = observed.get("platform") if isinstance(observed, dict) else None
                    if not expected_platform or actual_platform != expected_platform:
                        errors.append("native accessibility client does not match the inspected package platform")
            else:
                errors.append("package qualification requires the native accessibility backend record")
            executable_digests = set()
            for record, label in records:
                if record is not None:
                    artifact = record.get("artifact", {})
                    if not isinstance(artifact, dict) or artifact.get("package_sha256") != package_digest:
                        errors.append(f"{label} evidence does not match the inspected package digest")
                    if isinstance(artifact, dict) and isinstance(artifact.get("executable_sha256"), str):
                        executable_digests.add(artifact["executable_sha256"])
            if len(executable_digests) != 1:
                errors.append("accessibility lanes did not use the same qualification executable")

    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True, help="native backend qualification record")
    parser.add_argument("--software", type=Path, required=True, help="software backend qualification record")
    parser.add_argument(
        "--native-accessibility",
        type=Path,
        default=None,
        help="native OS accessibility backend qualification record (Windows UI Automation)",
    )
    parser.add_argument(
        "--require-native-accessibility",
        action="store_true",
        help="fail when the native accessibility backend record is absent",
    )
    parser.add_argument("--source-sha", default="", help="exact source SHA expected in the records")
    parser.add_argument("--install-tree", default="", help="installed tree the records must name")
    parser.add_argument("--package-boundary", type=Path, help="final package dependency inspection record")
    args = parser.parse_args(argv)

    errors = verify(
        args.native,
        args.software,
        args.source_sha,
        args.install_tree,
        native_accessibility_path=args.native_accessibility,
        require_native_accessibility=args.require_native_accessibility,
        package_boundary_path=args.package_boundary,
    )
    if errors:
        print("Quick accessibility qualification FAILED:", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1

    print(
        "Quick accessibility qualification verified: installed-tree native and software records "
        "carry distinct claims"
        + (", and the native accessibility backend is active" if args.native_accessibility else "")
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
