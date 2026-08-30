#!/usr/bin/env python3
"""Generate deterministic Phase 5 Widgets graph and disposition evidence.

The inventory is observed from repository CMake and filesystem inputs.  The
disposition artifact is a derived Phase 5 view over the existing shell and
product ledgers; neither artifact is allowed to invent a second shell policy.
"""

from __future__ import annotations

import argparse
import json
import re
import shlex
import subprocess
from collections import defaultdict, deque
from pathlib import Path
from typing import Iterable


QUALIFIED_BASELINE_SHA = "d7f39224ad22f26c5f67dcd000383d6239acfff4"
INVENTORY_PATH = Path("docs/generated/phase5-widgets-inventory.json")
DISPOSITION_PATH = Path("docs/generated/phase5-widgets-disposition.json")

PROFILE_OPTIONS = {
    "LOUPE_LOUPE_DISTRIBUTION": True,
    "LOUPE_BUILD_ONLY_CORE_LIBRARY": False,
    "LOUPE_BUILD_QUICK_CANVAS": True,
    "LOUPE_BUILD_TESTS": False,
    "LOUPE_BUILD_FUZZERS": False,
    "LOUPE_BUILD_QUICK_SHELL_SMOKE": False,
    "LOUPE_BUILD_CANVAS_BENCHMARK": False,
    "LOUPE_BUILD_PRODUCT_QUICK_A11Y_SMOKE": False,
    "LOUPE_BUILD_CODE_GENERATOR": False,
    "LOUPE_BUILD_JBIG2_VIEWER": False,
    "LOUPE_BUILD_EXAMPLE_GENERATOR": False,
    "LOUPE_BUILD_VIEWER": False,
    "LOUPE_BUILD_PAGEMASTER": False,
    "LOUPE_BUILD_DIFF": False,
    "LOUPE_BUILD_LAUNCHPAD": False,
    "LOUPE_PLUGIN_AUDIOBOOK": False,
    "LOUPE_PLUGIN_OCR": False,
    "LOUPE_PLUGIN_SCANNER": False,
}

TARGET_COMMANDS = {"add_library", "add_executable", "qt_add_library", "qt_add_executable"}
VISIBILITY = {"PRIVATE", "PUBLIC", "INTERFACE"}
INSTALL_STOP = {
    "RUNTIME",
    "LIBRARY",
    "ARCHIVE",
    "OBJECTS",
    "PUBLIC_HEADER",
    "PRIVATE_HEADER",
    "BUNDLE",
    "INCLUDES",
    "EXPORT",
    "FILE_SET",
    "NAMELINK_COMPONENT",
    "COMPONENT",
    "DESTINATION",
    "PERMISSIONS",
    "CONFIGURATIONS",
    "OPTIONAL",
    "NAMELINK_ONLY",
    "NAMELINK_SKIP",
}


class EvidenceError(ValueError):
    """Raised when repository evidence cannot be represented safely."""


def _normalize_newlines(text: str) -> str:
    if text.startswith("\ufeff"):
        text = text[1:]
    return text.replace("\r\n", "\n").replace("\r", "\n")


def _tracked_files(root: Path, *pathspecs: str) -> list[str]:
    try:
        output = subprocess.check_output(
            ["git", "ls-files", "--", *pathspecs],
            cwd=root,
            stderr=subprocess.DEVNULL,
            text=True,
        )
    except (subprocess.CalledProcessError, FileNotFoundError, OSError):
        if pathspecs == ("CMakeLists.txt", "**/CMakeLists.txt"):
            return sorted(path.relative_to(root).as_posix() for path in root.rglob("CMakeLists.txt"))
        if pathspecs == ("*.ui", "**/*.ui"):
            return sorted(path.relative_to(root).as_posix() for path in root.rglob("*.ui"))
        raise EvidenceError(f"cannot enumerate tracked files for {pathspecs!r}") from None
    return sorted(line for line in output.splitlines() if line)


def _read_tracked_text(root: Path, relative: str) -> str:
    path_posix = Path(relative).as_posix()
    work_path = root / relative
    working = (
        _normalize_newlines(work_path.read_text(encoding="utf-8")) if work_path.is_file() else None
    )
    try:
        data = subprocess.check_output(
            ["git", "show", f"HEAD:{path_posix}"],
            cwd=root,
            stderr=subprocess.DEVNULL,
        )
        indexed = _normalize_newlines(data.decode("utf-8"))
    except (subprocess.CalledProcessError, FileNotFoundError, OSError):
        if working is None:
            raise EvidenceError(f"missing tracked file: {path_posix}")
        return working
    if working is not None and working != indexed:
        return working
    return indexed


def _strip_comments(text: str) -> str:
    """Remove ordinary CMake line comments without changing line structure."""

    return re.sub(r"(?m)#.*$", "", text)


def _calls(text: str, names: Iterable[str]) -> list[tuple[str, str]]:
    """Return balanced CMake command bodies in source order."""

    names_pattern = "|".join(sorted((re.escape(name) for name in names), key=len, reverse=True))
    clean = _strip_comments(text)
    result: list[tuple[str, str]] = []
    pattern = re.compile(rf"(?im)(?<![A-Za-z0-9_])({names_pattern})\s*\(")
    for match in pattern.finditer(clean):
        open_index = clean.find("(", match.start(), match.end())
        depth = 0
        quote: str | None = None
        escaped = False
        for index in range(open_index, len(clean)):
            char = clean[index]
            if quote:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == quote:
                    quote = None
                continue
            if char in {"\"", "'"}:
                quote = char
            elif char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
                if depth == 0:
                    result.append((match.group(1), clean[open_index + 1 : index]))
                    break
        else:
            raise EvidenceError(f"unbalanced CMake command: {match.group(1)}")
    return result


def _tokens(body: str) -> list[str]:
    """Tokenize the simple CMake command forms used by the project."""

    try:
        return shlex.split(body, posix=True, comments=False)
    except ValueError as exc:
        raise EvidenceError(f"cannot tokenize CMake command: {exc}") from exc


def _target_declarations(root: Path) -> tuple[dict[str, dict], dict[Path, list[str]]]:
    targets: dict[str, dict] = {}
    by_cmake: dict[Path, list[str]] = defaultdict(list)
    for relative in _tracked_files(root, "CMakeLists.txt", "**/CMakeLists.txt"):
        cmake = root / relative
        for command, body in _calls(_read_tracked_text(root, relative), TARGET_COMMANDS):
            args = _tokens(body)
            if not args or args[0].startswith("${"):
                continue
            name = args[0]
            if name in targets:
                raise EvidenceError(f"duplicate CMake target declaration: {name}")
            kind = "library" if "library" in command else "executable"
            targets[name] = {
                "id": name,
                "kind": kind,
                "cmake": relative,
            }
            by_cmake[cmake].append(name)
    return targets, by_cmake


def _target_links(root: Path) -> dict[str, list[str]]:
    links: dict[str, list[str]] = defaultdict(list)
    for relative in _tracked_files(root, "CMakeLists.txt", "**/CMakeLists.txt"):
        for _command, body in _calls(_read_tracked_text(root, relative), {"target_link_libraries"}):
            args = _tokens(body)
            if not args or args[0].startswith("${"):
                continue
            target = args[0]
            for link in args[1:]:
                if link in VISIBILITY or link in {"LINK_PUBLIC", "LINK_PRIVATE", "LINK_INTERFACE_LIBRARIES"}:
                    continue
                if link not in links[target]:
                    links[target].append(link)
    return links


def _installed_targets(root: Path) -> set[str]:
    installed: set[str] = set()
    for relative in _tracked_files(root, "CMakeLists.txt", "**/CMakeLists.txt"):
        for _command, body in _calls(_read_tracked_text(root, relative), {"install"}):
            args = _tokens(body)
            if not args or args[0] != "TARGETS":
                continue
            for token in args[1:]:
                if token in INSTALL_STOP:
                    break
                if not token.startswith("${"):
                    installed.add(token)
    return installed


def _profile_for_target(name: str, cmake: str) -> tuple[bool, str]:
    normalized = cmake.replace("\\", "/")
    if normalized.startswith("UnitTests/"):
        return False, "LOUPE_BUILD_TESTS=OFF in the loupe-release profile"
    if normalized.startswith("Fuzz/"):
        return False, "LOUPE_BUILD_FUZZERS=OFF in the loupe-release profile"
    if normalized.startswith("QuickShellSmoke/"):
        return False, "LOUPE_BUILD_QUICK_SHELL_SMOKE=OFF in the loupe-release profile"
    if normalized.startswith("CanvasBenchmark/"):
        return False, "LOUPE_BUILD_CANVAS_BENCHMARK=OFF in the loupe-release profile"
    if normalized.startswith("ProductQuickAccessibilitySmoke/"):
        return False, "LOUPE_BUILD_PRODUCT_QUICK_A11Y_SMOKE=OFF in the loupe-release profile"
    plugin_options = {
        "AudioBookPlugin": "LOUPE_PLUGIN_AUDIOBOOK",
        "OcrPlugin": "LOUPE_PLUGIN_OCR",
        "ScannerPlugin": "LOUPE_PLUGIN_SCANNER",
    }
    for plugin, option in plugin_options.items():
        if f"LoupeEditorPlugins/{plugin}/" in f"{normalized}/":
            return PROFILE_OPTIONS[option], f"{option}={'ON' if PROFILE_OPTIONS[option] else 'OFF'} in the loupe-release profile"
    option_by_directory = {
        "CodeGenerator/": "LOUPE_BUILD_CODE_GENERATOR",
        "JBIG2_Viewer/": "LOUPE_BUILD_JBIG2_VIEWER",
        "PdfExampleGenerator/": "LOUPE_BUILD_EXAMPLE_GENERATOR",
    }
    for directory, option in option_by_directory.items():
        if normalized.startswith(directory):
            return PROFILE_OPTIONS[option], f"{option}={'ON' if PROFILE_OPTIONS[option] else 'OFF'} in the loupe-release profile"
    if normalized.startswith("LoupeEditor/"):
        return True, "LOUPE_BUILD_QUICK_CANVAS=ON and not LOUPE_BUILD_ONLY_CORE_LIBRARY"
    if normalized.startswith("LoupeLibQuick/"):
        return True, "LOUPE_BUILD_QUICK_CANVAS=ON"
    if normalized.startswith(("LoupeLibInteraction/", "PdfTool/", "loupe-preflight/", "loupe-ocr/")):
        return True, "not LOUPE_BUILD_ONLY_CORE_LIBRARY"
    return True, "always reachable from the loupe-release GUI build"


def _closure(name: str, links: dict[str, list[str]], known: set[str]) -> set[str]:
    seen: set[str] = set()
    queue = deque(link for link in links.get(name, []) if link in known)
    while queue:
        current = queue.popleft()
        if current in seen:
            continue
        seen.add(current)
        queue.extend(link for link in links.get(current, []) if link in known and link not in seen)
    return seen


def _widgets_paths(name: str, links: dict[str, list[str]], known: set[str]) -> list[list[str]]:
    paths: list[list[str]] = []
    queue: deque[list[str]] = deque([[name]])
    while queue:
        path = queue.popleft()
        current = path[-1]
        for link in links.get(current, []):
            if link == "Qt6::Widgets":
                paths.append(path + [link])
            elif link in known and link not in path:
                queue.append(path + [link])
    return sorted(paths)


def _ui_owner(root: Path, ui: Path, by_cmake: dict[Path, list[str]]) -> str | None:
    current = ui.parent
    while current >= root:
        cmake = current / "CMakeLists.txt"
        if cmake in by_cmake and by_cmake[cmake]:
            return sorted(by_cmake[cmake])[0]
        if current == root:
            break
        current = current.parent
    return None


def _surface_kind(name: str, cmake: str) -> str:
    normalized = cmake.replace("\\", "/")
    if normalized.startswith(("UnitTests/", "Fuzz/")):
        return "test"
    if normalized.startswith("LoupeEditorPlugins/"):
        return "plugin"
    if name.endswith("Plugin"):
        return "plugin"
    if name in {"LoupeLibWidgets", "LoupeLibGui"}:
        return "library"
    if name in {"CodeGenerator", "JBIG2_VIEWER", "PdfExampleGenerator", "CanvasBenchmark"}:
        return "developer-tool"
    return "application"


def _is_widgets_surface(target: dict) -> bool:
    normalized = target["cmake"].replace("\\", "/")
    if normalized.startswith(("UnitTests/", "Fuzz/")):
        return False
    return target["widgets_linkage"] != "none"


def build_inventory(root: Path) -> dict:
    target_map, by_cmake = _target_declarations(root)
    links = _target_links(root)
    installed = _installed_targets(root)
    known = set(target_map)
    for name, target in target_map.items():
        profile_enabled, profile_condition = _profile_for_target(name, target["cmake"])
        direct_links = sorted(links.get(name, []))
        direct_qt = sorted(link.removeprefix("Qt6::") for link in direct_links if link.startswith("Qt6::"))
        transitive = _closure(name, links, known)
        transitive_qt = sorted(
            module
            for dependency in transitive
            for module in target_map[dependency].get("direct_qt_modules", [])
            if module not in direct_qt
        )
        widgets_paths = _widgets_paths(name, links, known)
        target.update(
            {
                "profile_enabled": profile_enabled,
                "profile_condition": profile_condition,
                "install_rule": name in installed,
                "installed_in_profile": name in installed and profile_enabled,
                "build_only_in_profile": profile_enabled and name not in installed,
                "direct_links": direct_links,
                "direct_qt_modules": direct_qt,
                "transitive_targets": sorted(transitive),
                "transitive_qt_modules": transitive_qt,
                "qt_modules": sorted(set(direct_qt) | set(transitive_qt)),
                "widgets_linkage": (
                    "direct"
                    if "Widgets" in direct_qt
                    else "transitive"
                    if widgets_paths
                    else "none"
                ),
                "widgets_paths": widgets_paths,
            }
        )
    reverse_consumers: dict[str, set[str]] = defaultdict(set)
    for name, target in target_map.items():
        for dependency in target["transitive_targets"]:
            reverse_consumers[dependency].add(name)

    ui_forms: list[dict] = []
    for relative in _tracked_files(root, "*.ui", "**/*.ui"):
        ui = root / relative
        owner = _ui_owner(root, ui, by_cmake)
        plugin = None
        parts = relative.split("/")
        if "LoupeEditorPlugins" in parts:
            index = parts.index("LoupeEditorPlugins")
            if len(parts) > index + 1:
                plugin = parts[index + 1]
        ui_forms.append(
            {
                "id": f"ui:{relative}",
                "path": relative,
                "owner_target": owner,
                "plugin": plugin,
                "widgets_related": bool(owner and target_map.get(owner, {}).get("widgets_linkage") != "none"),
            }
        )

    target_rows = []
    for name in sorted(target_map):
        target = target_map[name]
        target_rows.append(
            {
                **target,
                "consumers": sorted(reverse_consumers.get(name, set())),
            }
        )
    plugin_targets = sorted(
        target["id"]
        for target in target_rows
        if _surface_kind(target["id"], target["cmake"]) == "plugin"
    )
    plugin_ui = [
        {
            "plugin": plugin,
            "target": plugin,
            "forms": [form["path"] for form in ui_forms if form["plugin"] == plugin],
        }
        for plugin in plugin_targets
    ]
    surface_targets = [
        {
            "id": f"target:{target['id']}",
            "kind": _surface_kind(target["id"], target["cmake"]),
            "target": target["id"],
            "cmake": target["cmake"],
            "profile_enabled": target["profile_enabled"],
            "installed_in_profile": target["installed_in_profile"],
            "widgets_linkage": target["widgets_linkage"],
            "consumers": target["consumers"],
        }
        for target in target_rows
        if _is_widgets_surface(target)
    ]
    surfaces = surface_targets + [
        {
            "id": form["id"],
            "kind": "ui-form",
            "path": form["path"],
            "target": form["owner_target"],
            "plugin": form["plugin"],
            "profile_enabled": bool(form["owner_target"] and target_map.get(form["owner_target"], {}).get("profile_enabled")),
            "installed_in_profile": bool(form["owner_target"] and target_map.get(form["owner_target"], {}).get("installed_in_profile")),
            "widgets_linkage": "owned-by-widgets-surface" if form["widgets_related"] else "unresolved",
            "consumers": [form["owner_target"]] if form["owner_target"] else [],
        }
        for form in ui_forms
    ]
    legacy_executables = [
        {
            "target": target["id"],
            "kind": target["kind"],
            "profile_enabled": target["profile_enabled"],
            "installed_in_profile": target["installed_in_profile"],
            "widgets_linkage": target["widgets_linkage"],
            "cmake": target["cmake"],
        }
        for target in target_rows
        if target["kind"] == "executable" and target["widgets_linkage"] != "none"
        and not target["cmake"].replace("\\", "/").startswith(("UnitTests/", "Fuzz/"))
    ]

    return {
        "schema_version": 1,
        "evidence_kind": "phase5-widgets-qualified-graph",
        "qualified_baseline_sha": QUALIFIED_BASELINE_SHA,
        "profile": {
            "id": "loupe-release",
            "options": PROFILE_OPTIONS,
            "configuration_mode": "source-static",
            "configured_build_dir": None,
            "configuration_note": "Static CMake/file evidence is used when a qualified build directory is unavailable; no build qualification is implied.",
        },
        "inputs": {
            "cmake_files": sorted(target["cmake"] for target in target_rows),
            "shell_ledger": "docs/loupe-shell.json",
            "product_ledger": "docs/product-surface.json",
            "ui_glob": "**/*.ui",
        },
        "targets": target_rows,
        "legacy_executables": legacy_executables,
        "plugin_ui": plugin_ui,
        "ui_forms": ui_forms,
        "surfaces": sorted(surfaces, key=lambda row: row["id"]),
        "counts": {
            "targets": len(target_rows),
            "installed_in_profile": sum(row["installed_in_profile"] for row in target_rows),
            "build_only_in_profile": sum(row["build_only_in_profile"] for row in target_rows),
            "widgets_surfaces": len(surface_targets),
            "legacy_executables": len(legacy_executables),
            "plugin_ui_groups": len(plugin_ui),
            "ui_forms": len(ui_forms),
        },
    }


PRODUCT_TO_PHASE5 = {
    "CLI-ONLY": "HEADLESS-REPLACE",
    "ABSORB": "HEADLESS-REPLACE",
    "OPEN": "BLOCKED",
    "HIDE": "DELETE",
    "STOP-SHIPPING": "DELETE",
    "ADVANCED": "RETAIN-NON-PRODUCT",
}
LEGACY_TO_PHASE5 = {
    "MIGRATE": "HEADLESS-REPLACE",
    "CONSOLIDATE": "HEADLESS-REPLACE",
    "HEADLESS": "RETAIN-NON-PRODUCT",
    "RETIRE": "DELETE",
}
SPECIAL_TARGETS = {
    "CanvasBenchmark": {
        "disposition": "RETAIN-NON-PRODUCT",
        "consumer": "Qualification-only benchmark harness",
        "rationale": "CanvasBenchmark is explicitly non-installed qualification infrastructure.",
        "testable_condition": "Retain only outside the supported installed interactive product and keep its qualification-only build option explicit.",
    },
}


def _load_json(root: Path, relative: str) -> dict:
    try:
        value = json.loads(_read_tracked_text(root, relative))
    except (OSError, json.JSONDecodeError) as exc:
        raise EvidenceError(f"cannot load {relative}: {exc}") from exc
    if not isinstance(value, dict):
        raise EvidenceError(f"{relative} must contain an object")
    return value


def _product_target_map(product: dict) -> dict[str, dict]:
    return {
        row["artifact"]: row
        for row in product.get("surfaces", [])
        if row.get("artifact")
    }


def _proven_owner_ids(product: dict) -> frozenset[str]:
    return frozenset(
        row["id"]
        for row in product.get("surfaces", [])
        if row.get("kind") == "application"
        and row.get("artifact_scope") == "install"
        and row.get("profiles", {}).get("loupe-release") == "present"
        and not row.get("replacement_surface")
    )


def _product_testable_condition(row: dict, disposition: str, replacement, proven_owners: frozenset[str]) -> str:
    if row.get("kind") in {"application", "plugin"} and replacement in proven_owners:
        if disposition == "DELETE":
            return (
                "The product/package graph may drop this executable; "
                f"replacement {replacement} is already the installed owner."
            )
        if disposition == "HEADLESS-REPLACE":
            return (
                f"Remove the interactive Widgets surface only after {replacement} is proven "
                "to carry the required capability."
            )
    if row.get("kind") == "plugin" and disposition == "DELETE" and replacement is None:
        return (
            "The product/package graph may drop this plugin; "
            "LoupeEditor is the sole supported installed interactive product."
        )
    return {
        "DELETE": "Delete only after the product/package graph contains no maintained reference to this surface and its replacement evidence is green.",
        "HEADLESS-REPLACE": f"Remove the interactive Widgets surface only after {replacement or 'the named headless boundary'} is proven to carry the required capability.",
        "RETAIN-NON-PRODUCT": "Retain only outside the supported installed interactive product; keep the developer/compatibility boundary explicit.",
        "BLOCKED": f"Resolve the linked product decision before deletion or replacement; current follow-up issue is {row.get('follow_up_issue') or 'not assigned'}.",
    }[disposition]


def _product_phase5(row: dict, proven_owners: frozenset[str]) -> dict:
    source = row.get("disposition")
    if source not in PRODUCT_TO_PHASE5:
        raise EvidenceError(f"Widgets surface {row.get('id')} has unsupported product disposition {source!r}")
    disposition = PRODUCT_TO_PHASE5[source]
    replacement = row.get("replacement_surface")
    consumer = replacement or ("developer-only surface" if disposition == "RETAIN-NON-PRODUCT" else row.get("artifact"))
    return {
        "disposition": disposition,
        "consumer": str(consumer),
        "rationale": str(row.get("rationale", "")),
        "testable_condition": _product_testable_condition(row, disposition, replacement, proven_owners),
        "source_disposition": source,
        "source": "docs/product-surface.json",
        "replacement_target": replacement,
        "follow_up_issue": row.get("follow_up_issue"),
    }


def build_disposition(root: Path, inventory: dict) -> dict:
    shell = _load_json(root, "docs/loupe-shell.json")
    product = _load_json(root, "docs/product-surface.json")
    product_targets = _product_target_map(product)
    proven_owners = _proven_owner_ids(product)
    shell_plugins = {row["plugin"]: row for row in shell.get("plugin_action_policy", [])}
    shell_legacy = {row["path"]: row for row in shell.get("legacy_surface_disposition", [])}
    target_rows = {row["target"]: row for row in inventory["surfaces"] if row["kind"] != "ui-form"}
    rows: list[dict] = []
    for surface in inventory["surfaces"]:
        if surface["kind"] == "ui-form":
            source = shell_legacy.get(surface["path"])
            if not source:
                raise EvidenceError(f"inventory UI form is absent from shell ledger: {surface['path']}")
            source_disposition = source.get("disposition")
            if source_disposition not in LEGACY_TO_PHASE5:
                raise EvidenceError(f"unsupported legacy disposition for {surface['path']}: {source_disposition!r}")
            disposition = LEGACY_TO_PHASE5[source_disposition]
            rows.append(
                {
                    "id": surface["id"],
                    "kind": "ui-form",
                    "path": surface["path"],
                    "owner_target": surface["target"],
                    "consumer": str(source.get("replacement_target") or "developer/CLI compatibility boundary"),
                    "rationale": source["rationale"],
                    "disposition": disposition,
                    "testable_condition": source["deletion_condition"],
                    "source_disposition": source_disposition,
                    "source": "docs/loupe-shell.json:legacy_surface_disposition",
                    "replacement_target": source.get("replacement_target"),
                }
            )
            continue
        target = surface["target"]
        if target in SPECIAL_TARGETS:
            policy = SPECIAL_TARGETS[target]
            rows.append({"id": surface["id"], "kind": surface["kind"], "target": target, **policy, "source": "phase5-special-target-policy"})
            continue
        source = product_targets.get(target)
        if not source:
            raise EvidenceError(f"Widgets inventory target has no product/special disposition: {target}")
        policy = _product_phase5(source, proven_owners)
        if target in shell_plugins:
            policy = {
                **policy,
                "shell_disposition": shell_plugins[target].get("disposition"),
                "shell_target": shell_plugins[target].get("target"),
            }
        rows.append({"id": surface["id"], "kind": surface["kind"], "target": target, **policy})

    product_crosswalk = []
    for source in product.get("surfaces", []):
        artifact = source.get("artifact")
        if source.get("kind") == "workspace":
            product_crosswalk.append(
                {
                    "id": source["id"],
                    "status": "explained-policy-only",
                    "explanation": "Workspace policy is not a maintained Widgets target; it names the surviving product boundary.",
                }
            )
        elif artifact in target_rows:
            product_crosswalk.append({"id": source["id"], "artifact": artifact, "status": "matched"})
        else:
            product_crosswalk.append(
                {
                    "id": source["id"],
                    "artifact": artifact,
                    "status": "explained-non-widgets-boundary",
                    "explanation": "The ledger row is a maintained non-Widgets product boundary and is represented in inventory.targets with widgets_linkage=none.",
                }
            )
    plugin_crosswalk = [
        {
            "plugin": plugin,
            "surface_id": f"target:{plugin}",
            "status": "matched" if plugin in target_rows else "explained-plugin-source-deleted",
            "shell_disposition": row.get("disposition"),
            "explanation": (
                "Plugin sources deleted in Phase 5 Issue 17; product ledger records build-only loupe-release absence."
                if plugin not in target_rows
                else None
            ),
        }
        for plugin, row in sorted(shell_plugins.items())
    ]
    legacy_crosswalk = [
        {
            "path": path,
            "surface_id": f"ui:{path}",
            "status": "matched" if f"ui:{path}" in {row["id"] for row in rows} else "missing",
            "shell_disposition": row.get("disposition"),
        }
        for path, row in sorted(shell_legacy.items())
    ]
    return {
        "schema_version": 1,
        "evidence_kind": "phase5-widgets-disposition",
        "qualified_baseline_sha": inventory["qualified_baseline_sha"],
        "inventory": "docs/generated/phase5-widgets-inventory.json",
        "policy_inputs": ["docs/loupe-shell.json", "docs/product-surface.json"],
        "allowed_dispositions": ["DELETE", "HEADLESS-REPLACE", "RETAIN-NON-PRODUCT", "BLOCKED"],
        "rows": sorted(rows, key=lambda row: row["id"]),
        "crosswalk": {
            "product_surface": product_crosswalk,
            "plugin_action_policy": plugin_crosswalk,
            "legacy_surface_disposition": legacy_crosswalk,
        },
        "counts": {
            "rows": len(rows),
            "delete": sum(row["disposition"] == "DELETE" for row in rows),
            "headless_replace": sum(row["disposition"] == "HEADLESS-REPLACE" for row in rows),
            "retain_non_product": sum(row["disposition"] == "RETAIN-NON-PRODUCT" for row in rows),
            "blocked": sum(row["disposition"] == "BLOCKED" for row in rows),
        },
    }


def generate(root: Path) -> tuple[dict, dict]:
    inventory = build_inventory(root)
    disposition = build_disposition(root, inventory)
    return inventory, disposition


def _serialized(value: dict) -> str:
    return json.dumps(value, indent=2, ensure_ascii=False, sort_keys=False) + "\n"


def _artifact_matches(path: Path, content: str) -> bool:
    if not path.is_file():
        return False
    on_disk = _normalize_newlines(path.read_text(encoding="utf-8"))
    if on_disk == content:
        return True
    try:
        return json.loads(on_disk) == json.loads(content)
    except json.JSONDecodeError:
        return False


def _check_or_write(root: Path, write: bool) -> list[str]:
    inventory, disposition = generate(root)
    expected = {
        root / INVENTORY_PATH: _serialized(inventory),
        root / DISPOSITION_PATH: _serialized(disposition),
    }
    mismatches: list[str] = []
    for path, content in expected.items():
        if write:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8", newline="\n")
        elif not _artifact_matches(path, content):
            mismatches.append(path.relative_to(root).as_posix())
    return mismatches


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--write", action="store_true", help="write both generated evidence artifacts")
    parser.add_argument("--check", action="store_true", help="fail if generated artifacts are stale")
    args = parser.parse_args()
    root = args.root.resolve()
    if args.write and args.check:
        parser.error("--write and --check are mutually exclusive")
    if not args.write and not args.check:
        args.check = True
    mismatches = _check_or_write(root, args.write)
    if mismatches:
        print("Phase 5 evidence is stale: " + ", ".join(mismatches))
        print("Run scripts/generate_phase5_widgets_evidence.py --write to refresh it.")
        return 1
    inventory, disposition = generate(root)
    mode = "written" if args.write else "verified"
    print(
        f"Phase 5 Widgets evidence {mode}: {inventory['counts']['targets']} targets, "
        f"{inventory['counts']['ui_forms']} UI forms, {disposition['counts']['rows']} disposition rows."
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except EvidenceError as exc:
        print(f"Phase 5 Widgets evidence FAILED: {exc}")
        raise SystemExit(1)
