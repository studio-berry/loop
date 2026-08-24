#!/usr/bin/env python3
"""Verify that optional Qt Quick qualification targets remain package-gated."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MANIFEST_PATH = ROOT / "docs" / "quick-runtime-manifest.json"


class ContractError(ValueError):
    """Raised when the Quick runtime contract is malformed or drifts."""


def load_json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ContractError(f"cannot read {path.relative_to(ROOT)}: {exc}") from exc
    if not isinstance(value, dict):
        raise ContractError(f"{path.relative_to(ROOT)} must contain an object")
    return value


def require_string(mapping: dict, key: str, label: str) -> str:
    value = mapping.get(key)
    if not isinstance(value, str) or not value:
        raise ContractError(f"{label}.{key} must be a non-empty string")
    return value


def target_block(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        raise ContractError(f"cannot read {path.relative_to(ROOT)}: {exc}") from exc


def validate_qualification_target(target: dict, root_cmake: str) -> None:
    name = require_string(target, "name", "target")
    option = require_string(target, "cmake_option", name)
    uri = require_string(target, "qml_uri", name)
    qml_file = ROOT / require_string(target, "qml_file", name)
    cmake_file = ROOT / require_string(target, "cmake_file", name)
    if target.get("install") is not False:
        raise ContractError(f"{name} must be marked install=false")
    modules = target.get("qt_modules")
    if not isinstance(modules, list) or not modules or not all(isinstance(item, str) for item in modules):
        raise ContractError(f"{name}.qt_modules must be a non-empty string list")

    option_pattern = re.compile(
        rf"option\(\s*{re.escape(option)}\b.*?\sOFF\s*\)", re.IGNORECASE | re.DOTALL
    )
    if not option_pattern.search(root_cmake):
        raise ContractError(f"{option} must default OFF in CMakeLists.txt")

    block = target_block(cmake_file)
    if not re.search(rf"qt_add_executable\(\s*{re.escape(name)}\b", block):
        raise ContractError(f"{name} is not declared by {cmake_file.relative_to(ROOT)}")
    if not re.search(rf"qt_add_qml_module\(\s*{re.escape(name)}\b", block):
        raise ContractError(f"{name} has no qt_add_qml_module declaration")
    if not re.search(rf"\bURI\s+{re.escape(uri)}\b", block):
        raise ContractError(f"{name} QML URI drifted from {uri}")
    if not qml_file.is_file():
        raise ContractError(f"{name} QML file is missing: {qml_file.relative_to(ROOT)}")

    for module in modules:
        if module not in block:
            raise ContractError(f"{name} no longer links declared module {module}")

    install_pattern = re.compile(rf"install\s*\([^)]*\b{re.escape(name)}\b", re.IGNORECASE | re.DOTALL)
    if install_pattern.search(root_cmake) or install_pattern.search(block):
        raise ContractError(f"qualification target {name} must not be installed")


def validate_product_target(product: dict, root_cmake: str) -> None:
    name = require_string(product, "name", "product_target")
    uri = require_string(product, "qml_uri", "product_target")
    qml_file = ROOT / require_string(product, "qml_file", "product_target")
    cmake_file = ROOT / require_string(product, "cmake_file", "product_target")
    if product.get("install") is not True:
        raise ContractError("product_target must be marked install=true")
    modules = product.get("qt_modules")
    if not isinstance(modules, list) or not modules or not all(isinstance(item, str) for item in modules):
        raise ContractError("product_target.qt_modules must be a non-empty string list")

    block = target_block(cmake_file)
    if not re.search(rf"qt_add_executable\(\s*{re.escape(name)}\b", block):
        raise ContractError(f"{name} is not declared by {cmake_file.relative_to(ROOT)}")
    if not re.search(rf"\bURI\s+{re.escape(uri)}\b", block):
        raise ContractError(f"product QML URI drifted from {uri}")
    if not qml_file.is_file():
        raise ContractError(f"product QML file is missing: {qml_file.relative_to(ROOT)}")

    for module in modules:
        if module not in block:
            raise ContractError(f"product target no longer links declared module {module}")

    install_pattern = re.compile(rf"install\s*\([^)]*\b{re.escape(name)}\b", re.IGNORECASE | re.DOTALL)
    if not install_pattern.search(block):
        raise ContractError(f"product target {name} must be installed")


def validate_manifest(manifest: dict) -> tuple[list[dict], dict | None]:
    if manifest.get("schema_version") != 1:
        raise ContractError("quick-runtime-manifest schema_version must be 1")

    status = manifest.get("status")
    product_shipped = manifest.get("product_qml_shipped")
    if product_shipped is True:
        if status not in {"product-navigable", "product-qml-shipped"}:
            raise ContractError("product_qml_shipped requires a product-navigable manifest status")
    elif product_shipped is not False:
        raise ContractError("product_qml_shipped must be a boolean")

    targets = manifest.get("qualification_targets")
    if not isinstance(targets, list) or not targets:
        raise ContractError("qualification_targets must be a non-empty list")

    product_target = manifest.get("product_target")
    if product_shipped is True:
        if not isinstance(product_target, dict):
            raise ContractError("product_qml_shipped requires product_target metadata")
    elif product_target is not None:
        raise ContractError("product_target must be absent until product_qml_shipped is true")

    names: set[str] = set()
    root_cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    for target in targets:
        if not isinstance(target, dict):
            raise ContractError("every qualification target must be an object")
        name = require_string(target, "name", "target")
        if name in names:
            raise ContractError(f"duplicate qualification target: {name}")
        names.add(name)
        validate_qualification_target(target, root_cmake)

    if names != {"QuickShellSmoke", "CanvasBenchmark"}:
        raise ContractError(f"unexpected qualification target set: {sorted(names)}")

    if product_shipped is True and isinstance(product_target, dict):
        validate_product_target(product_target, root_cmake)

    return targets, product_target if isinstance(product_target, dict) else None


def validate_licensing(manifest: dict) -> None:
    license_data = manifest.get("qt_license")
    if not isinstance(license_data, dict):
        raise ContractError("qt_license must be an object")
    if license_data.get("route") != "LGPL-3.0":
        raise ContractError("Qt Quick qualification must use the documented LGPL-3.0 route")
    for key in ("policy", "notice_generator", "relink_evidence", "corresponding_source_or_written_offer"):
        require_string(license_data, key, "qt_license")
    if license_data["relink_evidence"] == "complete" or license_data["corresponding_source_or_written_offer"] == "complete":
        raise ContractError("do not claim Qt relink evidence before the final-artifact gate is proven")

    gates = manifest.get("release_gates")
    if not isinstance(gates, dict):
        raise ContractError("release_gates must be an object")
    for key in ("final_artifact_sbom", "third_party_notices", "clean_machine_package_smoke", "qt_relink_test"):
        value = gates.get(key)
        if value not in {"open", "partial", "complete"}:
            raise ContractError(f"release_gates.{key} must be open, partial, or complete")
        if value == "complete" and key in {"final_artifact_sbom", "clean_machine_package_smoke", "qt_relink_test"}:
            raise ContractError(f"do not claim release gate {key} is complete for this session")


def validate_install_dir(install_dir: Path, targets: list[dict]) -> None:
    if not install_dir.is_dir():
        raise ContractError(f"install directory does not exist: {install_dir}")
    names = {target["name"] for target in targets}
    forbidden = []
    for path in install_dir.rglob("*"):
        if not path.is_file():
            continue
        if path.stem in names or path.name in names or any(name in path.name for name in names):
            forbidden.append(path.relative_to(install_dir).as_posix())
    if forbidden:
        raise ContractError(
            "qualification-only Quick target found in install tree: " + ", ".join(sorted(forbidden))
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--install-dir", type=Path, help="optional staged install tree to scan")
    args = parser.parse_args()

    try:
        manifest = load_json(MANIFEST_PATH)
        targets, product_target = validate_manifest(manifest)
        validate_licensing(manifest)
        if args.install_dir:
            validate_install_dir(args.install_dir.resolve(), targets)
    except (ContractError, OSError) as exc:
        print(f"Quick runtime contract FAILED: {exc}", file=sys.stderr)
        return 1

    product_flag = "true" if manifest.get("product_qml_shipped") else "false"
    product_name = product_target["name"] if product_target else "none"
    print(
        "Quick runtime contract verified: "
        f"qualification_targets={','.join(target['name'] for target in targets)}; "
        f"product_qml_shipped={product_flag}; product_target={product_name}; package-gates=open"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
