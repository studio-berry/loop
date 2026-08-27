#!/usr/bin/env python3
"""Generate Session 05 Issue 16/17 Widgets library consumer graph evidence.

Re-dumps the loupe-release dependency graph around LoupeLibWidgets and
Widgets-bound LoupeLibGui, classifies every consumer and neutral relocation
owner, and records installed-product blockers for consumer-first deletion.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

from generate_phase5_widgets_evidence import (
    QUALIFIED_BASELINE_SHA,
    EvidenceError,
    _load_json,
    _surface_kind,
    generate,
)


GRAPH_PATH = Path("docs/generated/widgets-library-consumer-graph.json")
LIBRARIES = ("LoupeLibWidgets", "LoupeLibGui")
SOURCE_SUFFIXES = (".cpp", ".h", ".hpp")
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]')
WIDGETS_HEADER = re.compile(
    r"^(?:QtWidgets/|Q(?:Widget|Dialog|MainWindow|Application|PushButton|ToolBar|Menu|Action|Layout|GridLayout|VBoxLayout|HBoxLayout|FormLayout|ScrollArea|TabWidget|DockWidget|Frame|GroupBox|Label|LineEdit|ComboBox|CheckBox|RadioButton|SpinBox|Slider|ProgressBar|TableView|TreeView|ListView|ItemDelegate|StyledItemDelegate|AbstractItemView|HeaderView|ToolButton|MessageBox|FileDialog|ColorDialog|FontDialog|InputDialog|Wizard|WizardPage|GraphicsView|GraphicsScene|GraphicsItem|OpenGLWidget|TextEdit|PlainTextEdit|DateEdit|TimeEdit|DateTimeEdit|CalendarWidget|LCDNumber|Dial|KeySequenceEdit|Shortcut|SystemTrayIcon|UndoView|Completer|SortFilterProxyModel|StandardItemModel|ButtonGroup|StackedWidget|Splitter|SizeGrip|RubberBand|DesktopWidget|Screen|Window|GuiApplication))"
)
WIDGETS_TYPE = re.compile(r"\bQ(?:Widget|Dialog|MainWindow|DockWidget|Frame|PushButton|ToolBar|Menu)\b")


def _linkage(library: str, target_row: dict) -> str:
    if library in target_row.get("direct_links", []):
        return "direct"
    if library in target_row.get("transitive_targets", []):
        return "transitive"
    return "unknown"


def _consumer_class(target: str, target_row: dict, library: str) -> str:
    cmake = target_row["cmake"].replace("\\", "/")
    if cmake.startswith(("UnitTests/", "Fuzz/")):
        return "test-consumer"
    if target == "LoupeLibGui" and library == "LoupeLibWidgets":
        return "widgets-library"
    kind = _surface_kind(target, cmake)
    if kind == "plugin":
        return "installed-product-plugin" if target_row["installed_in_profile"] else "build-only-plugin"
    if kind == "application":
        if target_row["installed_in_profile"]:
            return "installed-product-application"
        return "build-only-retired-executable" if target_row["profile_enabled"] else "profile-disabled-executable"
    if kind == "library":
        return "widgets-library"
    if kind == "developer-tool":
        return "developer-tool-consumer"
    return "unclassified"


def _replacement_owner(
    target: str,
    consumer_class: str,
    disposition_row: dict | None,
    product_row: dict | None,
) -> str:
    if disposition_row and disposition_row.get("consumer"):
        return str(disposition_row["consumer"])
    if product_row and product_row.get("replacement_surface"):
        return str(product_row["replacement_surface"])
    if consumer_class == "test-consumer":
        return "UnitTests (profile-disabled in loupe-release)"
    if consumer_class == "widgets-library":
        return "LoupeEditor Quick shell and PdfTool"
    if consumer_class == "developer-tool-consumer":
        return "developer-only qualification boundary"
    if consumer_class == "build-only-retired-executable":
        return "loupe-editor or loupe-cli per product-surface.json"
    if consumer_class == "build-only-plugin":
        return "LoupeEditor (sole installed interactive product)"
    return "unassigned"


def _scan_neutral_code(root: Path) -> list[dict]:
    rows: list[dict] = []
    for directory in ("LoupeLibWidgets/sources", "LoupeLibGui"):
        base = root / directory
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
                continue
            relative = path.relative_to(root).as_posix()
            current_owner = "LoupeLibWidgets" if relative.startswith("LoupeLibWidgets/") else "LoupeLibGui"
            text = path.read_text(encoding="utf-8")
            ui_sibling = path.with_suffix(".ui")
            widgets_bound = bool(
                any(
                    WIDGETS_HEADER.match(include)
                    for line in text.splitlines()
                    if (match := INCLUDE_RE.match(line))
                    for include in [match.group(1)]
                )
                or WIDGETS_TYPE.search(text)
                or ui_sibling.is_file()
            )
            if widgets_bound:
                rows.append(
                    {
                        "path": relative,
                        "current_owner": current_owner,
                        "binding": "widgets-bound",
                        "correct_owner": "delete-with-widgets-library",
                        "rationale": "Widgets UI or presentation code deletes with its library after Quick/headless replacements are proven.",
                    }
                )
            else:
                rows.append(
                    {
                        "path": relative,
                        "current_owner": current_owner,
                        "binding": "neutral-relocatable",
                        "correct_owner": "LoupeLibCore",
                        "rationale": "No Qt Widgets include or QWidget type usage; relocate to Core before deleting the Widgets library.",
                    }
                )
    return rows


def build_graph(root: Path) -> dict:
    inventory, disposition = generate(root)
    product = _load_json(root, "docs/product-surface.json")
    product_rows = {
        row["artifact"]: row
        for row in product.get("surfaces", [])
        if row.get("artifact")
    }
    disposition_by_target = {
        row["target"]: row for row in disposition.get("rows", []) if row.get("target")
    }
    target_map = {row["id"]: row for row in inventory["targets"]}

    consumers: list[dict] = []
    for library in LIBRARIES:
        if library not in target_map:
            continue
        library_row = target_map[library]
        for consumer in sorted(library_row.get("consumers", [])):
            target_row = target_map[consumer]
            consumer_class = _consumer_class(consumer, target_row, library)
            disposition_row = disposition_by_target.get(consumer)
            product_row = product_rows.get(consumer)
            consumers.append(
                {
                    "id": f"{library}->{consumer}",
                    "library": library,
                    "consumer": consumer,
                    "linkage": _linkage(library, target_row),
                    "profile_enabled": target_row["profile_enabled"],
                    "installed_in_profile": target_row["installed_in_profile"],
                    "consumer_class": consumer_class,
                    "product_disposition": product_row.get("disposition") if product_row else None,
                    "phase5_disposition": disposition_row.get("disposition") if disposition_row else None,
                    "replacement_owner": _replacement_owner(
                        consumer, consumer_class, disposition_row, product_row
                    ),
                    "cmake": target_row["cmake"],
                }
            )

    neutral_code = _scan_neutral_code(root)
    unknown_product_consumers = sorted(
        row["consumer"]
        for row in consumers
        if row["consumer_class"] == "unclassified"
        or (
            row["installed_in_profile"]
            and row["consumer_class"]
            not in {
                "installed-product-plugin",
                "installed-product-application",
                "widgets-library",
            }
        )
    )
    unclassified_neutral_code = sorted(
        row["path"] for row in neutral_code if row["correct_owner"] == "unassigned"
    )
    installed_product_blockers = sorted(
        {
            row["consumer"]
            for row in consumers
            if row["installed_in_profile"]
            and row["consumer_class"] in {"installed-product-plugin", "installed-product-application"}
        }
    )

    library_rows = []
    for library in LIBRARIES:
        if library in target_map:
            row = target_map[library]
            library_rows.append(
                {
                    "target": library,
                    "status": "present-in-profile",
                    "installed_in_profile": row["installed_in_profile"],
                    "direct_consumers": sorted(
                        consumer["consumer"]
                        for consumer in consumers
                        if consumer["library"] == library and consumer["linkage"] == "direct"
                    ),
                    "transitive_consumers": sorted(
                        consumer["consumer"]
                        for consumer in consumers
                        if consumer["library"] == library and consumer["linkage"] == "transitive"
                    ),
                    "deletion_blocked": bool(installed_product_blockers),
                }
            )
        else:
            library_rows.append(
                {
                    "target": library,
                    "status": "deleted-not-in-profile",
                    "installed_in_profile": False,
                    "direct_consumers": [],
                    "transitive_consumers": [],
                    "deletion_blocked": False,
                }
            )

    deletion_safe = not installed_product_blockers and all(
        row.get("status") == "deleted-not-in-profile" for row in library_rows
    )

    return {
        "schema_version": 1,
        "evidence_kind": "widgets-library-consumer-graph",
        "issue": 17,
        "qualified_baseline_sha": QUALIFIED_BASELINE_SHA,
        "profile": inventory["profile"],
        "inputs": {
            "inventory": "docs/generated/phase5-widgets-inventory.json",
            "disposition": "docs/generated/phase5-widgets-disposition.json",
            "product_ledger": "docs/product-surface.json",
        },
        "libraries": library_rows,
        "consumers": consumers,
        "neutral_code": neutral_code,
        "acceptance": {
            "unknown_product_consumers": unknown_product_consumers,
            "unclassified_neutral_code": unclassified_neutral_code,
            "installed_product_blockers": installed_product_blockers,
            "deletion_safe": deletion_safe,
            "issue_17_prerequisite": (
                "Issue 17 deleted LoupeLibWidgets and Widgets-bound LoupeLibGui from the maintained loupe-release graph."
                if deletion_safe
                else "Issue 17 must not delete LoupeLibWidgets or Widgets-bound LoupeLibGui while installed_product_blockers is non-empty."
            ),
        },
        "counts": {
            "libraries": len(library_rows),
            "consumers": len(consumers),
            "neutral_code": len(neutral_code),
            "neutral_relocatable": sum(row["binding"] == "neutral-relocatable" for row in neutral_code),
            "widgets_bound": sum(row["binding"] == "widgets-bound" for row in neutral_code),
            "installed_product_blockers": len(installed_product_blockers),
        },
    }


def _serialized(value: dict) -> str:
    return json.dumps(value, indent=2, ensure_ascii=False, sort_keys=False) + "\n"


def _artifact_matches(path: Path, content: str) -> bool:
    if not path.is_file():
        return False
    on_disk = path.read_text(encoding="utf-8")
    if on_disk == content:
        return True
    try:
        return json.loads(on_disk) == json.loads(content)
    except json.JSONDecodeError:
        return False


def _check_or_write(root: Path, write: bool) -> list[str]:
    graph = build_graph(root)
    content = _serialized(graph)
    path = root / GRAPH_PATH
    if write:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8", newline="\n")
        return []
    if not _artifact_matches(path, content):
        return [GRAPH_PATH.as_posix()]
    return []


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--write", action="store_true")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    if args.write and args.check:
        parser.error("--write and --check are mutually exclusive")
    if not args.write and not args.check:
        args.check = True
    try:
        mismatches = _check_or_write(root, args.write)
    except EvidenceError as exc:
        print(f"Widgets library consumer graph FAILED: {exc}", file=sys.stderr)
        return 1
    if mismatches:
        print("Widgets library consumer graph is stale: " + ", ".join(mismatches))
        print("Run scripts/generate_widgets_library_consumer_graph.py --write to refresh it.")
        return 1
    graph = build_graph(root)
    mode = "written" if args.write else "verified"
    print(
        f"Widgets library consumer graph {mode}: {graph['counts']['consumers']} consumers, "
        f"{graph['counts']['neutral_code']} neutral-code rows, "
        f"{graph['counts']['installed_product_blockers']} installed blockers."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
