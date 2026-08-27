#!/usr/bin/env python3
"""Fail-closed verification for the Phase 5 Widgets evidence join."""

from __future__ import annotations

import argparse
import copy
import json
import sys
from pathlib import Path

from generate_phase5_widgets_evidence import (
    DISPOSITION_PATH,
    INVENTORY_PATH,
    QUALIFIED_BASELINE_SHA,
    EvidenceError,
    generate,
)
from product_surface import ContractError as ProductSurfaceError
from product_surface import load_manifest, run_verification


ALLOWED_DISPOSITIONS = frozenset({"DELETE", "HEADLESS-REPLACE", "RETAIN-NON-PRODUCT", "BLOCKED"})
ALLOWED_CROSSWALK_STATUSES = frozenset(
    {"matched", "explained-policy-only", "explained-non-widgets-boundary", "explained-plugin-source-deleted"}
)
REQUIRED_ROW_FIELDS = ("id", "kind", "disposition", "consumer", "rationale", "testable_condition")


class ContractError(ValueError):
    pass


def _load(root: Path, relative: str) -> dict:
    try:
        value = json.loads((root / relative).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ContractError(f"cannot load {relative}: {exc}") from exc
    if not isinstance(value, dict):
        raise ContractError(f"{relative} must be an object")
    return value


def _unique(values: list[str], label: str, errors: list[str]) -> None:
    duplicates = sorted({value for value in values if values.count(value) > 1})
    if duplicates:
        errors.append(f"duplicate {label}: {', '.join(duplicates)}")


def _reject_default_fields(row: dict, errors: list[str]) -> None:
    for key in ("disposition", "consumer", "rationale", "testable_condition"):
        value = str(row.get(key, ""))
        if "*" in value or value.strip().lower() in {"default", "any", "all"}:
            errors.append(f"broad/default {key} in {row.get('id')}")


def validate_inventory(inventory: dict, root: Path) -> list[str]:
    errors: list[str] = []
    if inventory.get("schema_version") != 1 or inventory.get("evidence_kind") != "phase5-widgets-qualified-graph":
        errors.append("unsupported inventory schema")
    if inventory.get("qualified_baseline_sha") != QUALIFIED_BASELINE_SHA:
        errors.append("inventory is not tied to the qualified baseline SHA")
    targets = inventory.get("targets")
    surfaces = inventory.get("surfaces")
    ui_forms = inventory.get("ui_forms")
    plugin_ui = inventory.get("plugin_ui")
    if not isinstance(targets, list) or not isinstance(surfaces, list) or not isinstance(ui_forms, list) or not isinstance(plugin_ui, list):
        return ["inventory targets, surfaces, ui_forms, and plugin_ui must be arrays"]
    target_ids = [row.get("id", "") for row in targets]
    _unique(target_ids, "target identity", errors)
    target_map = {row.get("id"): row for row in targets}
    for target in targets:
        cmake = root / str(target.get("cmake", ""))
        if not cmake.is_file():
            errors.append(f"target {target.get('id')} points to missing CMake file")
        linkage = target.get("widgets_linkage")
        paths = target.get("widgets_paths", [])
        if linkage == "direct" and not any(path[-1:] == ["Qt6::Widgets"] for path in paths):
            errors.append(f"target {target.get('id')} claims direct Widgets linkage without a path")
        if linkage == "transitive" and not paths:
            errors.append(f"target {target.get('id')} claims transitive Widgets linkage without a path")
        if linkage == "none" and paths:
            errors.append(f"target {target.get('id')} has Widgets paths but claims no linkage")
    surface_ids = [row.get("id", "") for row in surfaces]
    _unique(surface_ids, "surface identity", errors)
    for surface in surfaces:
        if surface.get("kind") == "ui-form":
            path = str(surface.get("path", ""))
            if not (root / path).is_file():
                errors.append(f"UI surface points to missing file: {path}")
            if surface.get("target") not in target_map:
                errors.append(f"UI surface {path} has no owning target")
        else:
            target = target_map.get(surface.get("target"))
            if not target:
                errors.append(f"surface {surface.get('id')} has no target row")
            elif target.get("widgets_linkage") == "none":
                errors.append(f"non-Widgets target exposed as a Widgets surface: {surface.get('target')}")
    ui_ids = [row.get("id", "") for row in ui_forms]
    _unique(ui_ids, "UI form identity", errors)
    if any(row.get("id") not in surface_ids for row in ui_forms):
        errors.append("every UI form must also be a surface row")
    plugin_ids = [row.get("plugin", "") for row in plugin_ui]
    _unique(plugin_ids, "plugin UI identity", errors)
    for group in plugin_ui:
        if group.get("forms"):
            target = target_map.get(group.get("target"))
            if not target or not str(target.get("cmake", "")).startswith("LoopEditorPlugins/"):
                errors.append(f"plugin UI group has no plugin target: {group.get('plugin')}")
            for path in group.get("forms", []):
                if path not in {row.get("path") for row in ui_forms}:
                    errors.append(f"plugin UI group references unknown form: {path}")
    return errors


def validate_disposition(inventory: dict, disposition: dict, root: Path) -> list[str]:
    errors: list[str] = []
    if disposition.get("schema_version") != 1 or disposition.get("evidence_kind") != "phase5-widgets-disposition":
        errors.append("unsupported disposition schema")
    if disposition.get("qualified_baseline_sha") != inventory.get("qualified_baseline_sha"):
        errors.append("disposition and inventory baseline SHA differ")
    inventory_surfaces = {row.get("id"): row for row in inventory.get("surfaces", [])}
    rows = disposition.get("rows")
    if not isinstance(rows, list):
        return ["disposition rows must be an array"]
    row_ids = [row.get("id", "") for row in rows]
    _unique(row_ids, "disposition identity", errors)
    if set(row_ids) != set(inventory_surfaces):
        missing = sorted(set(inventory_surfaces) - set(row_ids))
        unknown = sorted(set(row_ids) - set(inventory_surfaces))
        errors.append(f"inventory/disposition identity mismatch missing={missing} unknown={unknown}")
    for row in rows:
        for field in REQUIRED_ROW_FIELDS:
            if not str(row.get(field, "")).strip():
                errors.append(f"disposition row {row.get('id')} missing {field}")
        if row.get("disposition") not in ALLOWED_DISPOSITIONS:
            errors.append(f"invalid disposition for {row.get('id')}: {row.get('disposition')}")
        _reject_default_fields(row, errors)
        source = inventory_surfaces.get(row.get("id"))
        if source and row.get("kind") != source.get("kind"):
            errors.append(f"disposition kind drift for {row.get('id')}")
        if row.get("disposition") == "BLOCKED" and not any(char.isdigit() for char in str(row.get("testable_condition"))):
            errors.append(f"blocked row {row.get('id')} lacks a traceable decision/issue condition")
    crosswalk = disposition.get("crosswalk")
    if not isinstance(crosswalk, dict):
        errors.append("disposition crosswalk is missing")
    else:
        for name in ("product_surface", "plugin_action_policy", "legacy_surface_disposition"):
            entries = crosswalk.get(name)
            if not isinstance(entries, list):
                errors.append(f"crosswalk {name} is missing")
                continue
            for entry in entries:
                if entry.get("status") not in ALLOWED_CROSSWALK_STATUSES:
                    errors.append(f"invalid {name} crosswalk status for {entry.get('id', entry.get('plugin', entry.get('path')))}")
                if entry.get("status", "").startswith("explained") and not str(entry.get("explanation", "")).strip():
                    errors.append(f"explained {name} crosswalk entry has no explanation")
            if name == "plugin_action_policy" and len(entries) != 12:
                errors.append(f"plugin crosswalk must contain 12 rows, found {len(entries)}")
            if name == "legacy_surface_disposition" and len(entries) != 2:
                errors.append(f"legacy crosswalk must contain 2 rows, found {len(entries)}")
    return errors


def validate_contract(
    root: Path,
    inventory: dict | None = None,
    disposition: dict | None = None,
    *,
    build_dir: Path | None = None,
    install_dir: Path | None = None,
    install_manifest_path: Path | None = None,
    pdf_tool: Path | None = None,
    discovery_json: Path | None = None,
) -> list[str]:
    inventory = inventory or _load(root, INVENTORY_PATH.as_posix())
    disposition = disposition or _load(root, DISPOSITION_PATH.as_posix())
    errors = validate_inventory(inventory, root)
    errors.extend(validate_disposition(inventory, disposition, root))
    try:
        expected_inventory, expected_disposition = generate(root)
    except EvidenceError as exc:
        errors.append(str(exc))
    else:
        if inventory != expected_inventory:
            errors.append("phase5-widgets-inventory.json is stale or was hand-edited")
        if disposition != expected_disposition:
            errors.append("phase5-widgets-disposition.json is stale or was hand-edited")
        second_inventory, second_disposition = generate(root)
        if (expected_inventory, expected_disposition) != (second_inventory, second_disposition):
            errors.append("Phase 5 evidence generation is not deterministic")
    try:
        manifest = load_manifest(root)
        errors.extend(
            run_verification(
                root,
                manifest,
                "loupe-release",
                build_dir=build_dir,
                install_dir=install_dir,
                install_manifest_path=install_manifest_path,
                pdf_tool=pdf_tool,
                discovery_json=discovery_json,
            )
        )
    except ProductSurfaceError as exc:
        errors.append(f"product-surface verification failed: {exc}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--build-dir", type=Path, help="Use a configured CMake cache as release-profile evidence")
    parser.add_argument("--install-dir", type=Path, help="Use an installed tree as release-profile evidence")
    parser.add_argument("--install-manifest", type=Path, help="Compare the installed tree with CMake's install manifest")
    parser.add_argument("--pdf-tool", type=Path, help="PdfTool binary used for command inventory verification")
    parser.add_argument("--discovery-json", type=Path, help="Captured PdfTool capabilities JSON")
    args = parser.parse_args()
    root = args.root.resolve()
    resolve = lambda path: None if path is None else (path if path.is_absolute() else root / path)
    errors = validate_contract(
        root,
        build_dir=resolve(args.build_dir),
        install_dir=resolve(args.install_dir),
        install_manifest_path=resolve(args.install_manifest),
        pdf_tool=resolve(args.pdf_tool),
        discovery_json=resolve(args.discovery_json),
    )
    if errors:
        for error in errors:
            print(f"Phase 5 Widgets contract FAILED: {error}", file=sys.stderr)
        return 1
    inventory = _load(root, INVENTORY_PATH.as_posix())
    disposition = _load(root, DISPOSITION_PATH.as_posix())
    print(
        f"Phase 5 Widgets contract verified: {inventory['counts']['widgets_surfaces']} Widgets targets, "
        f"{inventory['counts']['ui_forms']} UI forms, {len(disposition['rows'])} explicit dispositions, "
        f"baseline {inventory['qualified_baseline_sha']}."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
