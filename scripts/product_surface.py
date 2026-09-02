#!/usr/bin/env python3
"""Shared validation for the checked-in Loop product-surface contract.

The product-surface manifest is intentionally small and dependency-free.  This
module keeps the schema-level checks and the source/install/package checks in one
place so the Python and PowerShell entry points cannot grow separate inventories.
"""

from __future__ import annotations

import fnmatch
import json
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any, Iterable


PROFILES = ("developer", "loop-release")
MANIFEST_RELATIVE = Path("docs/product-surface.json")
SCHEMA_RELATIVE = Path("docs/schemas/product-surface.schema.json")
ARTIFACT_EXTENSIONS = frozenset({".a", ".dll", ".dylib", ".exe", ".lib", ".so"})
SURFACE_KINDS = frozenset({"application", "developer-tool", "library", "plugin", "workspace", "packaging"})
DISPOSITIONS = frozenset(
    {"KEEP", "ADVANCED", "CLI-ONLY", "ABSORB", "HIDE", "OPEN", "STOP-SHIPPING", "REMOVE"}
)
SURFACE_KEYS = frozenset(
    {
        "id",
        "kind",
        "artifact",
        "artifact_scope",
        "artifact_patterns",
        "build_option",
        "source_status",
        "disposition",
        "owner",
        "rationale",
        "profiles",
        "replacement_surface",
        "dependencies",
        "validation_evidence",
        "follow_up_issue",
    }
)


class ContractError(ValueError):
    """Raised when the product-surface contract cannot be proven."""


def _path_text(path: Path, root: Path | None = None) -> str:
    if root is not None:
        try:
            return path.resolve().relative_to(root.resolve()).as_posix()
        except ValueError:
            pass
    return path.as_posix()


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ContractError(f"cannot read {_path_text(path)}: {exc}") from exc


def load_manifest(root: Path, manifest_path: Path | None = None) -> dict[str, Any]:
    path = manifest_path or root / MANIFEST_RELATIVE
    value = load_json(path)
    if not isinstance(value, dict):
        raise ContractError(f"{_path_text(path, root)} must contain an object")
    return value


def _is_nonempty_string(value: Any) -> bool:
    return isinstance(value, str) and bool(value.strip())


def _validate_profile_map(value: Any, label: str, errors: list[str], *, arrays: bool = False) -> None:
    if not isinstance(value, dict):
        errors.append(f"{label} must be an object")
        return
    if set(value) != set(PROFILES):
        errors.append(f"{label} must define exactly developer and loop-release")
        return
    for profile in PROFILES:
        item = value[profile]
        if arrays:
            if not isinstance(item, list) or any(not isinstance(entry, str) for entry in item):
                errors.append(f"{label}.{profile} must be an array of strings")
            elif len(item) != len(set(item)):
                errors.append(f"{label}.{profile} contains duplicate entries")
        elif item not in {"present", "absent"}:
            errors.append(f"{label}.{profile} must be present or absent")


def validate_manifest(manifest: dict[str, Any], root: Path | None = None) -> list[str]:
    """Validate the checked-in manifest without third-party JSON-schema packages."""

    errors: list[str] = []
    expected_top = {"schema_version", "adr", "profiles", "surfaces", "packaging", "cli", "ui", "shell_contract"}
    unexpected = sorted(set(manifest) - expected_top)
    missing = sorted(expected_top - set(manifest))
    if unexpected:
        errors.append(f"manifest has unexpected top-level fields: {', '.join(unexpected)}")
    if missing:
        errors.append(f"manifest is missing top-level fields: {', '.join(missing)}")

    if manifest.get("schema_version") != 1:
        errors.append("schema_version must be 1")
    if manifest.get("adr") != "docs/adr/adr-005-product-surface-pruning-classification.md":
        errors.append("adr must link to ADR-005")
    if manifest.get("profiles") != list(PROFILES):
        errors.append("profiles must be exactly [developer, loop-release]")
    if manifest.get("shell_contract") != "docs/loop-shell.json":
        errors.append("shell_contract must link to docs/loop-shell.json")

    surfaces = manifest.get("surfaces")
    surface_ids: list[str] = []
    if not isinstance(surfaces, list) or not surfaces:
        errors.append("surfaces must be a non-empty array")
        surfaces = []
    for index, row in enumerate(surfaces):
        label = f"surface[{index}]"
        if not isinstance(row, dict):
            errors.append(f"{label} must be an object")
            continue
        extra = sorted(set(row) - SURFACE_KEYS)
        if extra:
            errors.append(f"{label} has unexpected fields: {', '.join(extra)}")
        for field in ("id", "kind", "disposition", "owner", "rationale", "profiles", "source_status", "validation_evidence", "follow_up_issue"):
            if field not in row:
                errors.append(f"{label} missing {field}")
        surface_id = row.get("id")
        if not isinstance(surface_id, str) or not re.fullmatch(r"[a-z][a-z0-9-]*", surface_id):
            errors.append(f"{label}.id must be kebab-case")
        else:
            surface_ids.append(surface_id)
        if row.get("kind") not in SURFACE_KINDS:
            errors.append(f"{label}.kind is invalid: {row.get('kind')!r}")
        if row.get("disposition") not in DISPOSITIONS:
            errors.append(f"{label}.disposition is invalid: {row.get('disposition')!r}")
        if row.get("source_status") not in {"maintained", "deleted"}:
            errors.append(f"{label}.source_status must be maintained or deleted")
        if not isinstance(row.get("rationale"), str) or not row.get("rationale", "").strip():
            errors.append(f"{label}.rationale must be non-empty")
        if not isinstance(row.get("validation_evidence"), str) or not row.get("validation_evidence", "").strip():
            errors.append(f"{label}.validation_evidence must be non-empty")
        _validate_profile_map(row.get("profiles"), f"{label}.profiles", errors)
        scope = row.get("artifact_scope")
        if scope not in {"install", "build", "none"}:
            errors.append(f"{label}.artifact_scope is invalid: {scope!r}")
        artifact = row.get("artifact")
        if scope == "none" and artifact is not None:
            errors.append(f"{label} with artifact_scope=none must have artifact=null")
        if scope in {"install", "build"} and not _is_nonempty_string(artifact):
            errors.append(f"{label} with artifact_scope={scope} needs an artifact")
        patterns = row.get("artifact_patterns")
        if scope == "install":
            if not isinstance(patterns, list) or not patterns or any(not _is_nonempty_string(item) for item in patterns):
                errors.append(f"{label}.artifact_patterns must be a non-empty string array for installed artifacts")
        elif patterns is not None and (not isinstance(patterns, list) or any(not _is_nonempty_string(item) for item in patterns)):
            errors.append(f"{label}.artifact_patterns must be a string array")
        if row.get("build_option") is not None and not _is_nonempty_string(row.get("build_option")):
            errors.append(f"{label}.build_option must be a non-empty string")
        if row.get("source_status") == "deleted":
            profile_map = row.get("profiles") if isinstance(row.get("profiles"), dict) else {}
            if any(profile_map.get(profile) != "absent" for profile in PROFILES):
                errors.append(f"{label} is deleted but not absent from every profile")
        if row.get("disposition") == "OPEN":
            if not _is_nonempty_string(row.get("owner")):
                errors.append(f"{label} OPEN disposition needs a non-empty owner")
            if not isinstance(row.get("follow_up_issue"), int) or isinstance(row.get("follow_up_issue"), bool) or row.get("follow_up_issue", 0) < 1:
                errors.append(f"{label} OPEN disposition needs a positive follow_up_issue")
        elif row.get("owner") is not None and not isinstance(row.get("owner"), str):
            errors.append(f"{label}.owner must be a string or null")
        follow_up = row.get("follow_up_issue")
        if follow_up is not None and (not isinstance(follow_up, int) or isinstance(follow_up, bool) or follow_up < 1):
            errors.append(f"{label}.follow_up_issue must be a positive integer or null")
        if row.get("dependencies") is not None and (
            not isinstance(row.get("dependencies"), list) or any(not isinstance(item, str) for item in row["dependencies"])
        ):
            errors.append(f"{label}.dependencies must be a string array")

    if len(surface_ids) != len(set(surface_ids)):
        errors.append("surface IDs must be unique")

    packaging = manifest.get("packaging")
    if not isinstance(packaging, dict):
        errors.append("packaging must be an object")
        packaging = {}
    expected_packaging = {
        "desktop_entries",
        "appx_applications",
        "loop_launcher",
        "flatpak",
        "wix",
        "first_party_artifact_globs",
        "ignored_artifact_paths",
    }
    extra = sorted(set(packaging) - expected_packaging)
    missing = sorted(expected_packaging - set(packaging))
    if extra:
        errors.append(f"packaging has unexpected fields: {', '.join(extra)}")
    if missing:
        errors.append(f"packaging is missing fields: {', '.join(missing)}")
    _validate_profile_map(packaging.get("desktop_entries"), "packaging.desktop_entries", errors, arrays=True)
    _validate_profile_map(packaging.get("appx_applications"), "packaging.appx_applications", errors, arrays=True)

    launcher = packaging.get("loop_launcher")
    if not isinstance(launcher, dict):
        errors.append("packaging.loop_launcher must be an object")
    else:
        if set(launcher) != {"desktop_entry", "executable", "appx_id", "file_associations"}:
            errors.append("packaging.loop_launcher has an unexpected field set")
        for field in ("desktop_entry", "executable", "appx_id"):
            if not _is_nonempty_string(launcher.get(field)):
                errors.append(f"packaging.loop_launcher.{field} must be non-empty")
        if not isinstance(launcher.get("file_associations"), list) or any(not _is_nonempty_string(item) for item in launcher.get("file_associations", [])):
            errors.append("packaging.loop_launcher.file_associations must be a string array")

    flatpak = packaging.get("flatpak")
    if not isinstance(flatpak, dict):
        errors.append("packaging.flatpak must be an object")
    else:
        if set(flatpak) != {"manifest", "app_id", "command", "profiles"}:
            errors.append("packaging.flatpak has an unexpected field set")
        for field in ("manifest", "app_id", "command"):
            if not _is_nonempty_string(flatpak.get(field)):
                errors.append(f"packaging.flatpak.{field} must be non-empty")
        _validate_profile_map(flatpak.get("profiles"), "packaging.flatpak.profiles", errors)

    wix = packaging.get("wix")
    if not isinstance(wix, dict):
        errors.append("packaging.wix must be an object")
    else:
        if set(wix) != {"template", "profiles", "required_components", "forbidden_tokens"}:
            errors.append("packaging.wix has an unexpected field set")
        if not _is_nonempty_string(wix.get("template")):
            errors.append("packaging.wix.template must be non-empty")
        _validate_profile_map(wix.get("profiles"), "packaging.wix.profiles", errors)
        if not isinstance(wix.get("required_components"), list) or any(not _is_nonempty_string(item) for item in wix.get("required_components", [])):
            errors.append("packaging.wix.required_components must be a string array")
        if not isinstance(wix.get("forbidden_tokens"), list) or any(not _is_nonempty_string(item) for item in wix.get("forbidden_tokens", [])):
            errors.append("packaging.wix.forbidden_tokens must be a string array")

    for field in ("first_party_artifact_globs", "ignored_artifact_paths"):
        value = packaging.get(field)
        if not isinstance(value, list) or not value or any(not _is_nonempty_string(item) for item in value):
            errors.append(f"packaging.{field} must be a non-empty string array")

    cli = manifest.get("cli")
    if not isinstance(cli, dict):
        errors.append("cli must be an object")
    else:
        if set(cli) != {"discovery_command", "capability_field", "required_build_capabilities", "command_inventory"}:
            errors.append("cli has an unexpected field set")
        if cli.get("discovery_command") != "PdfTool capabilities --console-format json":
            errors.append("cli.discovery_command must use PdfTool capabilities")
        if cli.get("capability_field") != "build_capabilities":
            errors.append("cli.capability_field must be build_capabilities")
        caps = cli.get("required_build_capabilities")
        if not isinstance(caps, list) or not caps or any(not _is_nonempty_string(item) for item in caps):
            errors.append("cli.required_build_capabilities must be a non-empty string array")
        inventory = cli.get("command_inventory")
        if not isinstance(inventory, dict):
            errors.append("cli.command_inventory must be an object")
        else:
            if set(inventory) != {"source", "field", "mode", "required_commands"}:
                errors.append("cli.command_inventory has an unexpected field set")
            if inventory.get("source") != cli.get("discovery_command") or inventory.get("field") != "commands":
                errors.append("cli.command_inventory must derive from the PdfTool commands field")
            if inventory.get("mode") != "all-registered":
                errors.append("cli.command_inventory.mode must be all-registered")
            required_commands = inventory.get("required_commands")
            if not isinstance(required_commands, list) or any(not _is_nonempty_string(item) for item in required_commands):
                errors.append("cli.command_inventory.required_commands must be a string array")
            elif len(required_commands) != len(set(required_commands)):
                errors.append("cli.command_inventory.required_commands contains duplicates")

    ui = manifest.get("ui")
    if not isinstance(ui, dict):
        errors.append("ui must be an object")
    else:
        if set(ui) != {"contract", "entrypoint_surface", "legacy_forms"}:
            errors.append("ui has an unexpected field set")
        if ui.get("contract") != "docs/loop-shell.json":
            errors.append("ui.contract must link to docs/loop-shell.json")
        if ui.get("entrypoint_surface") not in surface_ids:
            errors.append("ui.entrypoint_surface must name a manifest surface")
        legacy_forms = ui.get("legacy_forms")
        if not isinstance(legacy_forms, list) or any(not _is_nonempty_string(item) for item in legacy_forms):
            errors.append("ui.legacy_forms must be a string array")
        elif root is not None:
            for relative in legacy_forms:
                if not (root / relative).is_file():
                    errors.append(f"ui legacy form does not exist: {relative}")

    if root is not None:
        for relative in (manifest.get("adr"), manifest.get("shell_contract")):
            if isinstance(relative, str) and not (root / relative).is_file():
                errors.append(f"manifest references missing file: {relative}")
    return errors


def require_valid_manifest(manifest: dict[str, Any], root: Path | None = None) -> None:
    errors = validate_manifest(manifest, root)
    if errors:
        raise ContractError("; ".join(errors))


def profile_rows(manifest: dict[str, Any], profile: str, *, scope: str | None = None) -> list[dict[str, Any]]:
    rows = []
    for row in manifest["surfaces"]:
        if row.get("profiles", {}).get(profile) != "present":
            continue
        if scope is not None and row.get("artifact_scope") != scope:
            continue
        rows.append(row)
    return rows


def _normalise_name(name: str) -> str:
    value = name.casefold()
    if value.startswith("lib"):
        value = value[3:]
    return value


def artifact_patterns(row: dict[str, Any]) -> list[str]:
    patterns = row.get("artifact_patterns")
    if isinstance(patterns, list) and patterns:
        return [str(pattern).casefold() for pattern in patterns]
    artifact = str(row.get("artifact", "")).casefold()
    return [artifact, f"{artifact}.*", f"lib{artifact}.*"]


def _matches_row(path: Path, row: dict[str, Any]) -> bool:
    name = path.name.casefold()
    normalised = _normalise_name(path.name)
    return any(
        fnmatch.fnmatchcase(name, pattern) or fnmatch.fnmatchcase(normalised, _normalise_name(pattern))
        for pattern in artifact_patterns(row)
    )


def _matches_path_glob(relative: str, pattern: str) -> bool:
    relative = relative.replace("\\", "/")
    pattern = pattern.replace("\\", "/")
    return fnmatch.fnmatchcase(relative, pattern) or Path(relative).match(pattern)


def _is_ignored_artifact(relative: str, patterns: Iterable[str]) -> bool:
    return any(_matches_path_glob(relative, pattern) for pattern in patterns)


def _first_party_file(path: Path, relative: str, packaging: dict[str, Any]) -> bool:
    if path.suffix and path.suffix.casefold() not in ARTIFACT_EXTENSIONS:
        return False
    if _is_ignored_artifact(relative, packaging["ignored_artifact_paths"]):
        return False
    name = path.name.casefold()
    normalised = _normalise_name(path.name)
    return any(
        fnmatch.fnmatchcase(name, pattern.casefold()) or fnmatch.fnmatchcase(normalised, _normalise_name(pattern))
        for pattern in packaging["first_party_artifact_globs"]
    )


def _target_inventory(root: Path) -> tuple[dict[str, dict[str, Any]], set[str]]:
    scripts = root / "scripts"
    if str(scripts) not in sys.path:
        sys.path.insert(0, str(scripts))
    try:
        from generate_phase5_widgets_evidence import _installed_targets, _target_declarations

        targets, _ = _target_declarations(root)
        return targets, _installed_targets(root)
    except (ImportError, OSError, ValueError) as exc:
        raise ContractError(f"cannot derive CMake target inventory: {exc}") from exc


def _read_cache_values(cache_path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    try:
        lines = cache_path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as exc:
        raise ContractError(f"cannot read CMake cache {cache_path}: {exc}") from exc
    for line in lines:
        if not line or line.startswith("//") or line.startswith("#") or ":" not in line or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.split(":", 1)[0]
        values[key] = value
    return values


def validate_source(root: Path, manifest: dict[str, Any], profile: str, build_dir: Path | None = None) -> list[str]:
    errors: list[str] = []
    targets, installed_targets = _target_inventory(root)
    rows = [row for row in manifest["surfaces"] if row.get("artifact_scope") in {"build", "install"} and row.get("artifact")]
    manifest_install_targets = {
        row["artifact"] for row in rows if row.get("source_status") == "maintained" and row.get("artifact_scope") == "install"
    }

    for row in rows:
        artifact = row["artifact"]
        status = row.get("source_status")
        if status == "maintained" and artifact not in targets:
            errors.append(f"maintained manifest artifact is not declared by CMake: {artifact}")
        if status == "deleted" and artifact in targets:
            errors.append(f"deleted manifest artifact is still declared by CMake: {artifact}")
    for target in sorted(installed_targets - manifest_install_targets):
        errors.append(f"CMake install target is unmanifested: {target}")
    for target in sorted(manifest_install_targets - installed_targets):
        errors.append(f"manifest install artifact has no CMake install rule: {target}")

    cmake_text = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    for row in rows:
        option = row.get("build_option")
        if not option or row.get("source_status") != "maintained":
            continue
        if not re.search(rf"option\s*\(\s*{re.escape(option)}\b", cmake_text):
            errors.append(f"manifest build option is not declared by CMake: {option}")
            continue
        expected_state = row.get("profiles", {}).get(profile)
        if expected_state not in {"present", "absent"}:
            continue
        if option == "LOOP_BUILD_QUICK_CANVAS":
            if not re.search(r"if\s*\(\s*LOOP_BUILD_QUICK_CANVAS\s*\)", cmake_text):
                errors.append("LOOP_BUILD_QUICK_CANVAS does not guard its maintained product targets")
            continue
        expected_default = "ON" if expected_state == "present" else "OFF"
        default_marker = f"_{option}_DEFAULT"
        if profile == "loop-release":
            default_pattern = rf"if\s*\(\s*LOOP_LOOP_DISTRIBUTION\s*\).*?set\(\s*{re.escape(default_marker)}\s+OFF\s*\)"
        else:
            default_pattern = rf"else\s*\(\s*\).*?set\(\s*{re.escape(default_marker)}\s+ON\s*\)"
        if not re.search(default_pattern, cmake_text, flags=re.IGNORECASE | re.DOTALL):
            errors.append(
                f"{row['id']} expects {option}={expected_default} for {profile}, "
                f"but CMake does not select that default for the profile"
            )

    if build_dir is not None:
        cache = build_dir / "CMakeCache.txt"
        if not cache.is_file():
            errors.append(f"build directory has no CMakeCache.txt: {build_dir}")
        else:
            values = _read_cache_values(cache)
            expected_distribution = "ON" if profile == "loop-release" else "OFF"
            if values.get("LOOP_LOOP_DISTRIBUTION") != expected_distribution:
                errors.append(
                    f"CMake cache profile mismatch: expected LOOP_LOOP_DISTRIBUTION={expected_distribution}, "
                    f"found {values.get('LOOP_LOOP_DISTRIBUTION')!r}"
                )
            for row in rows:
                option = row.get("build_option")
                if not option or row.get("source_status") != "maintained":
                    continue
                expected = "ON" if row.get("profiles", {}).get(profile) == "present" else "OFF"
                if values.get(option) not in {expected, "1" if expected == "ON" else "0"}:
                    errors.append(f"CMake cache {option} does not match {profile}: expected {expected}, found {values.get(option)!r}")

    return errors


def _install_manifest_entries(path: Path, install_dir: Path) -> set[str]:
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as exc:
        raise ContractError(f"cannot read install manifest {path}: {exc}") from exc
    # Do not resolve the final candidate path: CMake may list an installed
    # symlink, and resolving it changes the manifest's entry name to its target.
    # Normalize the lexical paths for containment while preserving that final
    # filesystem name.
    install_root = Path(os.path.abspath(os.fspath(install_dir)))
    entries: set[str] = set()
    for line in lines:
        raw = line.strip()
        if not raw:
            continue
        candidate = Path(raw)
        if not candidate.is_absolute():
            candidate = path.parent / candidate
        candidate = Path(os.path.abspath(os.fspath(candidate)))
        try:
            relative = candidate.relative_to(install_root).as_posix()
        except ValueError:
            continue
        entries.add(relative)
    return entries


def validate_install(
    install_dir: Path,
    manifest: dict[str, Any],
    profile: str,
    install_manifest_path: Path | None = None,
) -> list[str]:
    errors: list[str] = []
    if not install_dir.is_dir():
        return [f"install directory does not exist: {install_dir}"]
    files = [path for path in install_dir.rglob("*") if path.is_file()]
    installed_rows = [row for row in manifest["surfaces"] if row.get("artifact_scope") == "install" and row.get("artifact")]
    expected_rows = [row for row in installed_rows if row.get("profiles", {}).get(profile) == "present"]
    absent_rows = [row for row in installed_rows if row.get("profiles", {}).get(profile) == "absent"]

    for row in expected_rows:
        if not any(_matches_row(path, row) for path in files):
            errors.append(f"missing required installed artifact: {row['artifact']}")
    for row in absent_rows:
        matches = [path for path in files if _matches_row(path, row)]
        if matches:
            errors.append(f"forbidden installed artifact {row['artifact']}: {', '.join(str(path) for path in matches[:3])}")

    packaging = manifest["packaging"]
    expected_first_party = {
        row["artifact"]: row
        for row in expected_rows
        if row.get("source_status") == "maintained"
    }
    for path in files:
        relative = path.relative_to(install_dir).as_posix()
        if not _first_party_file(path, relative, packaging):
            continue
        if not any(_matches_row(path, row) for row in expected_first_party.values()):
            errors.append(f"unmanifested first-party artifact: {relative}")

    if install_manifest_path is not None:
        expected_files = _install_manifest_entries(install_manifest_path, install_dir)
        actual_files = {path.relative_to(install_dir).as_posix() for path in files}
        extra = sorted(actual_files - expected_files)
        missing = sorted(expected_files - actual_files)
        if extra:
            errors.append(f"install tree contains files absent from CMake install manifest: {', '.join(extra[:5])}")
        if missing:
            errors.append(f"CMake install manifest lists missing files: {', '.join(missing[:5])}")

    return errors


def _find_artifact(install_dir: Path, row: dict[str, Any]) -> Path | None:
    for path in sorted((item for item in install_dir.rglob("*") if item.is_file()), key=lambda item: item.as_posix()):
        if _matches_row(path, row):
            return path
    return None


def _load_discovery_document(path: Path) -> dict[str, Any]:
    value = load_json(path)
    if not isinstance(value, dict):
        raise ContractError("CLI discovery JSON must contain an object")
    return value


def _run_discovery(pdf_tool: Path) -> dict[str, Any]:
    env = os.environ.copy()
    env.setdefault("QT_QPA_PLATFORM", "offscreen")
    try:
        completed = subprocess.run(
            [str(pdf_tool), "capabilities", "--console-format", "json"],
            capture_output=True,
            text=True,
            timeout=90,
            env=env,
            check=False,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise ContractError(f"cannot run PdfTool discovery: {exc}") from exc
    if completed.returncode != 0:
        raise ContractError(f"PdfTool discovery failed with exit code {completed.returncode}: {completed.stderr[-500:]}")
    try:
        value = json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        raise ContractError(f"PdfTool discovery did not return JSON: {exc}") from exc
    if not isinstance(value, dict):
        raise ContractError("PdfTool discovery output must be an object")
    return value


def validate_cli(
    manifest: dict[str, Any],
    *,
    profile: str | None = None,
    install_dir: Path | None = None,
    pdf_tool: Path | None = None,
    discovery_json: Path | None = None,
) -> list[str]:
    errors: list[str] = []
    cli = manifest["cli"]
    document: dict[str, Any]
    try:
        if discovery_json is not None:
            document = _load_discovery_document(discovery_json)
        else:
            if pdf_tool is None and install_dir is not None:
                row = next((item for item in manifest["surfaces"] if item.get("artifact") == "PdfTool"), None)
                if row is not None:
                    pdf_tool = _find_artifact(install_dir, row)
            if pdf_tool is None:
                return ["CLI verification needs --pdf-tool, --discovery-json, or an install directory containing PdfTool"]
            document = _run_discovery(pdf_tool)
    except ContractError as exc:
        return [str(exc)]

    data = document.get("data")
    if not isinstance(data, dict):
        return ["CLI discovery output is missing the data envelope"]
    capabilities = data.get(cli["capability_field"])
    if not isinstance(capabilities, list) or any(not isinstance(item, str) for item in capabilities):
        errors.append(f"CLI discovery output has invalid {cli['capability_field']}")
    else:
        for required in cli["required_build_capabilities"]:
            if required not in capabilities:
                errors.append(f"CLI discovery output is missing required build capability: {required}")

    inventory = cli["command_inventory"]
    commands = data.get(inventory["field"])
    if not isinstance(commands, list) or any(not isinstance(item, dict) for item in commands):
        errors.append("CLI discovery output has an invalid commands inventory")
        return errors
    command_ids = [item.get("id") for item in commands]
    if any(not isinstance(item, str) or not item.strip() for item in command_ids):
        errors.append("CLI command inventory contains a command without a non-empty id")
    valid_command_ids = [item for item in command_ids if isinstance(item, str) and item.strip()]
    if len(valid_command_ids) != len(set(valid_command_ids)):
        errors.append("CLI command inventory contains duplicate ids")
    if valid_command_ids != sorted(valid_command_ids):
        errors.append("CLI command inventory is not deterministically sorted")
    declared_command_ids = set(inventory["required_commands"])
    discovered_command_ids = set(valid_command_ids)
    for required in sorted(declared_command_ids - discovered_command_ids):
        errors.append(f"manifest CLI command is absent from PdfTool capabilities: {required}")
    if profile != "developer":
        for unexpected in sorted(discovered_command_ids - declared_command_ids):
            errors.append(f"PdfTool capability command is absent from manifest CLI inventory: {unexpected}")
    return errors


def _xml_local_name(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def _appx_applications(path: Path) -> tuple[set[str], set[str]]:
    try:
        root = ET.fromstring(path.read_text(encoding="utf-8"))
    except (OSError, ET.ParseError) as exc:
        raise ContractError(f"cannot parse AppX manifest {path}: {exc}") from exc
    applications: set[str] = set()
    associations: set[str] = set()
    for element in root.iter():
        if _xml_local_name(element.tag) == "Application" and element.get("Id"):
            applications.add(element.get("Id", ""))
        if _xml_local_name(element.tag) == "FileType":
            text = (element.text or "").strip()
            if text:
                associations.add(text)
    return applications, associations


def _appx_association_tokens(associations: Iterable[str]) -> set[str]:
    mime_to_extension = {
        "application/pdf": ".pdf",
    }
    return {mime_to_extension.get(association, association) for association in associations}


def validate_packaging_sources(root: Path, manifest: dict[str, Any], profile: str) -> list[str]:
    errors: list[str] = []
    packaging = manifest["packaging"]
    launcher = packaging["loop_launcher"]

    flatpak = packaging["flatpak"]
    if flatpak["profiles"][profile] == "present":
        path = root / flatpak["manifest"]
        try:
            document = load_json(path)
            if document.get("app-id") != flatpak["app_id"]:
                errors.append(f"Flatpak app-id drift: expected {flatpak['app_id']}")
            if document.get("command") != flatpak["command"]:
                errors.append(f"Flatpak command drift: expected {flatpak['command']}")
            modules = document.get("modules", [])
            project_module = next(
                (item for item in modules if isinstance(item, dict) and item.get("name") == "loop-pdf"),
                None,
            )
            opts = project_module.get("config-opts", []) if isinstance(project_module, dict) else []
            if "-DLOOP_LOOP_DISTRIBUTION=ON" not in opts:
                errors.append("Flatpak project module is not configured for loop-release")
        except ContractError as exc:
            errors.append(str(exc))

    wix = packaging["wix"]
    if wix["profiles"][profile] == "present":
        paths = [root / wix["template"], root / "WixInstaller" / "CMakeLists.txt"]
        for path in paths:
            try:
                text = path.read_text(encoding="utf-8")
            except OSError as exc:
                errors.append(f"cannot read WiX source {path}: {exc}")
                continue
            for token in wix["required_components"]:
                if path == paths[0] and token not in text:
                    errors.append(f"WiX template is missing manifest component: {token}")
            for token in wix["forbidden_tokens"]:
                if token in text:
                    errors.append(f"WiX source contains forbidden retired surface: {token}")

    appx_path = root / "AppxManifest.xml.in"
    if appx_path.is_file():
        try:
            applications, associations = _appx_applications(appx_path)
        except ContractError as exc:
            errors.append(str(exc))
        else:
            expected_apps = set(packaging["appx_applications"][profile])
            if applications != expected_apps:
                errors.append(f"AppX application inventory drift: expected {sorted(expected_apps)}, found {sorted(applications)}")
            expected_associations = _appx_association_tokens(launcher["file_associations"])
            if not expected_associations.issubset(associations):
                errors.append("AppX file association inventory is missing a manifest association")

    expected_desktop = set(packaging["desktop_entries"][profile])
    cmake_text = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    for entry in expected_desktop:
        if f"Desktop/{entry}" not in cmake_text:
            errors.append(f"CMake does not install manifest desktop entry: {entry}")
    desktop_path = root / "Desktop" / launcher["desktop_entry"]
    if desktop_path.is_file():
        text = desktop_path.read_text(encoding="utf-8")
        if not re.search(rf"(?m)^Exec={re.escape(launcher['executable'])}(?:\.exe)?\s+%[fF]$", text):
            errors.append(f"desktop launcher does not execute {launcher['executable']}")
        for association in launcher["file_associations"]:
            if not re.search(rf"(?m)^MimeType=.*{re.escape(association)}", text):
                errors.append(f"desktop launcher is missing file association: {association}")
    else:
        errors.append(f"desktop launcher file is missing: {launcher['desktop_entry']}")
    return errors


def run_verification(
    root: Path,
    manifest: dict[str, Any],
    profile: str,
    *,
    build_dir: Path | None = None,
    install_dir: Path | None = None,
    install_manifest_path: Path | None = None,
    pdf_tool: Path | None = None,
    discovery_json: Path | None = None,
) -> list[str]:
    errors = validate_manifest(manifest, root)
    if errors:
        return errors
    errors.extend(validate_source(root, manifest, profile, build_dir))
    errors.extend(validate_packaging_sources(root, manifest, profile))
    if install_dir is not None:
        errors.extend(validate_install(install_dir, manifest, profile, install_manifest_path))
        errors.extend(
            validate_cli(
                manifest,
                profile=profile,
                install_dir=install_dir,
                pdf_tool=pdf_tool,
                discovery_json=discovery_json,
            )
        )
    elif pdf_tool is not None or discovery_json is not None:
        errors.extend(validate_cli(manifest, profile=profile, pdf_tool=pdf_tool, discovery_json=discovery_json))
    return errors
