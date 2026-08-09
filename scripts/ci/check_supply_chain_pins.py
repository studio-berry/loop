#!/usr/bin/env python3
# Policy gate for supply-chain pinning.
#
# Enforces, for every workflow under `.github/workflows/`:
#   * all `uses:` action references are immutable full commit SHAs;
#   * no mutable `continuous` / `latest` release download URLs remain;
#
# and verifies the authoritative vcpkg baseline in `vcpkg-configuration.json`
# is a full commit SHA. Run this near the beginning of CI; it fails the build
# when a pin regresses.

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = ROOT / ".github" / "workflows"
PINS = ROOT / ".github" / "pins" / "packaging-tools.json"

ACTION = re.compile(
    r"^\s*uses:\s*(?!\./)([^@\s]+)@([^#\s]+)",
    re.MULTILINE,
)

FULL_SHA = re.compile(r"^[0-9a-f]{40}$", re.IGNORECASE)
HEX_DIGEST = re.compile(r"^[0-9a-f]{64}$", re.IGNORECASE)

MUTABLE_DOWNLOADS = (
    "/releases/download/continuous/",
    "/releases/latest/",
    "/releases/latest/download/",
)

def check_workflows():
    """All external action refs must be full SHAs; no mutable download URLs."""
    errors = []
    for path in sorted(list(WORKFLOWS.glob("*.yml")) + list(WORKFLOWS.glob("*.yaml"))):
        text = path.read_text(encoding="utf-8")

        for action, ref in ACTION.findall(text):
            if not FULL_SHA.fullmatch(ref):
                errors.append(
                    f"{path.relative_to(ROOT)}: "
                    f"{action}@{ref} is not pinned to a full commit SHA"
                )

        for marker in MUTABLE_DOWNLOADS:
            if marker in text:
                errors.append(
                    f"{path.relative_to(ROOT)}: "
                    f"mutable release download URL: {marker}"
                )
    return errors


def check_vcpkg():
    """The default-registry baseline must be the sole, full-SHA vcpkg pin."""
    config_path = ROOT / "vcpkg-configuration.json"
    if not config_path.exists():
        return [f"missing {config_path.relative_to(ROOT)}"]
    config = json.loads(config_path.read_text(encoding="utf-8"))
    baseline = config.get("default-registry", {}).get("baseline")
    if not isinstance(baseline, str) or not FULL_SHA.fullmatch(baseline):
        return [
            "vcpkg-configuration.json default-registry baseline "
            "is not a full commit SHA"
        ]
    return []


def check_packaging_pins():
    """Packaging-tool pins must carry verifiable identities and digests."""
    if not PINS.exists():
        return [f"missing {PINS.relative_to(ROOT)}"]
    pins = json.loads(PINS.read_text(encoding="utf-8"))
    errors = []

    if pins.get("schemaVersion") != 1:
        errors.append("packaging-tools.json schemaVersion must be 1")

    for tool in ("appimagetool", "appimageRuntime", "linuxdeployqt"):
        pin = pins.get(tool)
        if not pin:
            errors.append(f"{tool}: missing pin")
            continue
        for key in ("upstream", "upstreamCommit", "assetId", "assetName", "sha256"):
            if not pin.get(key):
                errors.append(f"{tool}.{key}: missing")
        if (
            not isinstance(pin.get("assetId"), int)
            or isinstance(pin.get("assetId"), bool)
            or pin["assetId"] <= 0
        ):
            errors.append(f"{tool}.assetId is not a positive integer")
        if not FULL_SHA.fullmatch(str(pin.get("upstreamCommit", ""))):
            errors.append(f"{tool}.upstreamCommit is not a full commit SHA")
        if not HEX_DIGEST.fullmatch(str(pin.get("sha256", ""))):
            errors.append(f"{tool}.sha256 is not a 64-char hex digest")

    wix = pins.get("wix", {})
    if not wix.get("version") or not wix.get("url"):
        errors.append("wix: missing version or url")
    if not isinstance(wix.get("url"), str) or not wix["url"].startswith("https://"):
        errors.append("wix.url must be an HTTPS URL")
    if not HEX_DIGEST.fullmatch(str(wix.get("sha256", ""))):
        errors.append("wix.sha256 is not a 64-char hex digest")
    if any(m in str(wix.get("url", "")) for m in MUTABLE_DOWNLOADS):
        errors.append("wix.url resolves through a mutable release URL")

    for key in ("windowsSdk", "flatpak", "deb", "digicertKeylocker"):
        if not pins.get(key):
            errors.append(f"{key}: missing pin")

    windows_sdk = pins.get("windowsSdk", {})
    if not windows_sdk.get("version"):
        errors.append("windowsSdk.version: missing")
    if not HEX_DIGEST.fullmatch(str(windows_sdk.get("makeAppxSha256", ""))):
        errors.append("windowsSdk.makeAppxSha256 is not a 64-char hex digest")

    flatpak = pins.get("flatpak", {})
    for key in ("runner", "flatpakVersion", "flatpakBuilderVersion"):
        if not flatpak.get(key):
            errors.append(f"flatpak.{key}: missing")

    deb = pins.get("deb", {})
    for key in ("runner", "dpkgVersion"):
        if not deb.get(key):
            errors.append(f"deb.{key}: missing")

    keylocker = pins.get("digicertKeylocker", {})
    keylocker_values = [
        keylocker.get("version"),
        keylocker.get("url"),
        keylocker.get("sha256"),
    ]
    if any(value is not None for value in keylocker_values):
        if not all(keylocker_values):
            errors.append("digicertKeylocker.version, url, and sha256 must be set together")
        if not isinstance(keylocker.get("url"), str) or not keylocker["url"].startswith("https://"):
            errors.append("digicertKeylocker.url must be an HTTPS URL")
        if any(marker in str(keylocker.get("url", "")) for marker in MUTABLE_DOWNLOADS):
            errors.append("digicertKeylocker.url resolves through a mutable release URL")
        if not HEX_DIGEST.fullmatch(str(keylocker.get("sha256", ""))):
            errors.append("digicertKeylocker.sha256 is not a 64-char hex digest")

    return errors


def main():
    errors = []
    errors += check_workflows()
    errors += check_vcpkg()
    errors += check_packaging_pins()

    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)

    print("Supply-chain pin policy passed.")


if __name__ == "__main__":
    main()
