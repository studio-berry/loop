#!/usr/bin/env python3
# Policy gate for supply-chain pinning.
#
# The validator is intentionally usable both as a repository CLI and as a
# small pure-validation library. Keep the policy checks independent from the
# filesystem so regression fixtures can exercise allowed and rejected inputs
# without mutating the checkout or accessing the network.

import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable

ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = ROOT / ".github" / "workflows"
PINS = ROOT / ".github" / "pins" / "packaging-tools.json"
VCPKG_CONFIG = ROOT / "vcpkg-configuration.json"

ACTION = re.compile(r"^\s*uses:\s*(?!\./)([^@\s]+)@([^#\s]+)", re.MULTILINE)
URL = re.compile(r"https?://[^\s\"'<>`]+", re.IGNORECASE)
FULL_SHA = re.compile(r"^[0-9a-f]{40}$", re.IGNORECASE)
HEX_DIGEST = re.compile(r"^[0-9a-f]{64}$", re.IGNORECASE)
MUTABLE_DOWNLOAD_MARKERS = (
    "/releases/download/continuous",
    "/releases/download/latest",
    "/releases/latest",
)

SUPPLY_CHAIN_TEXT_ROOT_NAMES = (".github/workflows", "scripts/ci", "cmake", "Flatpak", "WixInstaller")
TEXT_SUFFIXES = {".cmake", ".json", ".ps1", ".py", ".sh", ".yml", ".yaml", ".in"}
EXCLUDED_SOURCE_NAMES = {"check_supply_chain_pins.py", "test_check_supply_chain_pins.py"}


@dataclass(frozen=True)
class PinViolation:
    path: Path
    message: str

    def format(self, root: Path = ROOT) -> str:
        try:
            display_path = self.path.relative_to(root)
        except ValueError:
            display_path = self.path
        return f"{display_path}: {self.message}"


def _mutable_url(url: str) -> bool:
    normalized = url.rstrip(".,);]}").lower()
    return any(marker in normalized for marker in MUTABLE_DOWNLOAD_MARKERS)


def validate_workflow_text(path: Path, text: str) -> list[PinViolation]:
    """Validate action refs and mutable download URLs in one workflow text."""
    violations: list[PinViolation] = []
    for action, ref in ACTION.findall(text):
        if not FULL_SHA.fullmatch(ref):
            violations.append(PinViolation(path, f"{action}@{ref} is not pinned to a full commit SHA"))

    violations.extend(validate_source_text(path, text))
    return violations


def validate_source_text(path: Path, text: str) -> list[PinViolation]:
    """Reject mutable release URLs anywhere in production supply-chain text."""
    violations: list[PinViolation] = []
    for candidate in URL.findall(text):
        if _mutable_url(candidate):
            violations.append(PinViolation(path, f"mutable release download URL: {candidate}"))
    return violations


def validate_vcpkg_data(path: Path, config: Any) -> list[PinViolation]:
    baseline = config.get("default-registry", {}).get("baseline") if isinstance(config, dict) else None
    if not isinstance(baseline, str) or not FULL_SHA.fullmatch(baseline):
        return [PinViolation(path, "default-registry baseline is not a full commit SHA")]
    return []


def _require_https_url(data: dict[str, Any], path: Path, key: str, errors: list[PinViolation]) -> None:
    value = data.get(key)
    if not isinstance(value, str) or not value.startswith("https://"):
        errors.append(PinViolation(path, f"{key} must be an HTTPS URL"))
    elif _mutable_url(value):
        errors.append(PinViolation(path, f"{key} resolves through a mutable release URL"))


def _validate_asset_pin(path: Path, name: str, pin: Any, errors: list[PinViolation]) -> None:
    if not isinstance(pin, dict):
        errors.append(PinViolation(path, f"{name}: missing pin"))
        return
    for key in ("upstream", "upstreamCommit", "assetId", "assetName", "sha256"):
        if not pin.get(key):
            errors.append(PinViolation(path, f"{name}.{key}: missing"))
    if not isinstance(pin.get("assetId"), int) or isinstance(pin.get("assetId"), bool) or pin["assetId"] <= 0:
        errors.append(PinViolation(path, f"{name}.assetId is not a positive integer"))
    if not FULL_SHA.fullmatch(str(pin.get("upstreamCommit", ""))):
        errors.append(PinViolation(path, f"{name}.upstreamCommit is not a full commit SHA"))
    if not HEX_DIGEST.fullmatch(str(pin.get("sha256", ""))):
        errors.append(PinViolation(path, f"{name}.sha256 is not a 64-char hex digest"))


def validate_packaging_pins_data(path: Path, pins: Any) -> list[PinViolation]:
    """Validate the authoritative packaging-tools.json object."""
    errors: list[PinViolation] = []
    if not isinstance(pins, dict):
        return [PinViolation(path, "packaging-tools.json must contain an object")]
    if pins.get("schemaVersion") != 1:
        errors.append(PinViolation(path, "schemaVersion must be 1"))

    for tool in ("appimagetool", "appimageRuntime", "linuxdeployqt", "sentryCli"):
        _validate_asset_pin(path, tool, pins.get(tool), errors)

    wix = pins.get("wix")
    if not isinstance(wix, dict) or not wix.get("version") or not wix.get("url"):
        errors.append(PinViolation(path, "wix: missing version or url"))
    else:
        _require_https_url(wix, path, "url", errors)
    if not isinstance(wix, dict) or not HEX_DIGEST.fullmatch(str(wix.get("sha256", ""))):
        errors.append(PinViolation(path, "wix.sha256 is not a 64-char hex digest"))

    for key in ("windowsSdk", "flatpak", "deb", "digicertKeylocker"):
        if not isinstance(pins.get(key), dict):
            errors.append(PinViolation(path, f"{key}: missing pin"))

    windows_sdk = pins.get("windowsSdk", {})
    if not windows_sdk.get("version"):
        errors.append(PinViolation(path, "windowsSdk.version: missing"))
    if not HEX_DIGEST.fullmatch(str(windows_sdk.get("makeAppxSha256", ""))):
        errors.append(PinViolation(path, "windowsSdk.makeAppxSha256 is not a 64-char hex digest"))

    flatpak = pins.get("flatpak", {})
    for key in ("runner", "flatpakVersion", "flatpakBuilderVersion"):
        if not flatpak.get(key):
            errors.append(PinViolation(path, f"flatpak.{key}: missing"))

    deb = pins.get("deb", {})
    for key in ("runner", "dpkgVersion"):
        if not deb.get(key):
            errors.append(PinViolation(path, f"deb.{key}: missing"))

    keylocker = pins.get("digicertKeylocker", {})
    keylocker_values = [keylocker.get("version"), keylocker.get("url"), keylocker.get("sha256")]
    if any(value is not None for value in keylocker_values):
        if not all(keylocker_values):
            errors.append(PinViolation(path, "digicertKeylocker.version, url, and sha256 must be set together"))
        else:
            _require_https_url(keylocker, path, "url", errors)
            if not HEX_DIGEST.fullmatch(str(keylocker.get("sha256", ""))):
                errors.append(PinViolation(path, "digicertKeylocker.sha256 is not a 64-char hex digest"))

    return errors


def _read_json(path: Path) -> tuple[Any | None, list[PinViolation]]:
    try:
        return json.loads(path.read_text(encoding="utf-8")), []
    except FileNotFoundError:
        return None, [PinViolation(path, "file is missing")]
    except json.JSONDecodeError as exc:
        return None, [PinViolation(path, f"invalid JSON: {exc}")]


def iter_supply_chain_files(root: Path = ROOT) -> Iterable[Path]:
    seen: set[Path] = set()
    for relative_root in SUPPLY_CHAIN_TEXT_ROOT_NAMES:
        source_root = root / relative_root
        if not source_root.exists():
            continue
        for path in sorted(source_root.rglob("*")):
            if path.is_file() and path.suffix.lower() in TEXT_SUFFIXES and path.name not in EXCLUDED_SOURCE_NAMES:
                resolved = path.resolve()
                if resolved not in seen:
                    seen.add(resolved)
                    yield path


def validate_repository(root: Path = ROOT) -> list[PinViolation]:
    """Run all repository checks without changing files or using the network."""
    violations: list[PinViolation] = []
    workflows = root / ".github" / "workflows"
    for path in sorted(list(workflows.glob("*.yml")) + list(workflows.glob("*.yaml"))):
        violations.extend(validate_workflow_text(path, path.read_text(encoding="utf-8")))

    for path in iter_supply_chain_files(root):
        if path.parent == workflows:
            continue
        violations.extend(validate_source_text(path, path.read_text(encoding="utf-8")))

    config, config_errors = _read_json(root / "vcpkg-configuration.json")
    violations.extend(config_errors)
    if config is not None:
        violations.extend(validate_vcpkg_data(root / "vcpkg-configuration.json", config))

    pins, pin_errors = _read_json(root / ".github" / "pins" / "packaging-tools.json")
    violations.extend(pin_errors)
    if pins is not None:
        violations.extend(validate_packaging_pins_data(root / ".github" / "pins" / "packaging-tools.json", pins))
    return violations


def check_workflows() -> list[str]:
    return [violation.format() for violation in validate_repository() if violation.path.parent == WORKFLOWS]


def check_vcpkg() -> list[str]:
    config, errors = _read_json(VCPKG_CONFIG)
    if config is not None:
        errors.extend(validate_vcpkg_data(VCPKG_CONFIG, config))
    return [error.format() for error in errors]


def check_packaging_pins() -> list[str]:
    pins, errors = _read_json(PINS)
    if pins is not None:
        errors.extend(validate_packaging_pins_data(PINS, pins))
    return [error.format() for error in errors]


def main() -> int:
    violations = validate_repository()
    if violations:
        for violation in violations:
            print(f"ERROR: {violation.format()}", file=sys.stderr)
        return 1
    print("Supply-chain pin policy passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
