#!/usr/bin/env python3
"""Keep the one command catalog in parity with the shell it replaces.

docs/loop-shell-actions.json is the single registry for Editor commands.
scripts/verify-loop-shell-contract.ps1 already pins its ID set against the
<action> entries in LoopLibGui/pdfeditormainwindow.ui; this check owns the
`command` block that pdfinteraction::CommandCatalog consumes.

The interesting rule is shortcut parity. Shortcuts are not in the .ui — they are
assigned in C++ by PDFActionManager::initActions against enum names, which
LoopLibGui/pdfeditormainwindow.cpp maps to .ui action IDs. A catalog that
disagreed with those assignments would be a second command truth wearing the
first one's ID set, so both files are parsed and compared here.

See docs/LOOP_SHELL_CONTRACT.md and architecture invariant I22.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
POLICY_PATH = ROOT / "docs" / "loop-shell-actions.json"
MAIN_WINDOW_PATH = ROOT / "LoopLibGui" / "pdfeditormainwindow.cpp"
CONTROLLER_PATH = ROOT / "LoopLibGui" / "pdfprogramcontroller.cpp"

# Commands that have a handler in LoopLibInteraction (DocumentFacade for
# document lifecycle, ViewportCommandBridge for page/zoom/rotate). A command
# may only leave this set by being implemented somewhere else, never by being
# quietly downgraded.
IMPLEMENTED_COMMANDS = frozenset(
    {
        "actionOpen",
        "actionClose",
        "actionSave",
        "actionSave_As",
        "actionGoToNextPage",
        "actionGoToPreviousPage",
        "actionGoToDocumentStart",
        "actionGoToDocumentEnd",
        "actionZoom_In",
        "actionZoom_Out",
        "actionFitPage",
        "actionFitWidth",
        "actionFitHeight",
        "actionRotateLeft",
        "actionRotateRight",
    }
)

# Commands registered by the Quick shell host (EditorHost) rather than
# LoopLibInteraction. Promoting one to implemented still requires a real
# capability in loop-shell-actions.json.
SHELL_IMPLEMENTED_COMMANDS = frozenset(
    {
        "actionQuit",
    }
)

REQUIRED_COMMAND_KEYS = frozenset(
    {"label_key", "parameters", "capability", "cancellable", "availability"}
)
OPTIONAL_COMMAND_KEYS = frozenset({"shortcut"})

CAPABILITIES = frozenset(
    {
        "unclassified",
        "none",
        "document.read",
        "document.write",
        "document.modify",
        "application",
    }
)
AVAILABILITIES = frozenset({"implemented", "declared"})
PARAMETER_TYPES = frozenset({"string", "integer", "number", "boolean"})

SET_ACTION_RE = re.compile(
    r"setAction\s*\(\s*PDFActionManager::(?P<enum>\w+)\s*,\s*ui->(?P<id>\w+)\s*\)"
)
STANDARD_KEY_RE = re.compile(
    r"setShortcut\s*\(\s*(?P<enum>\w+)\s*,\s*QKeySequence::(?P<key>\w+)\s*\)"
)
SEQUENCE_RE = re.compile(
    r'setShortcut\s*\(\s*(?P<enum>\w+)\s*,\s*QKeySequence\s*\(\s*"(?P<sequence>[^"]+)"\s*\)\s*\)'
)
COMBINATION_RE = re.compile(
    r"setShortcut\s*\(\s*(?P<enum>\w+)\s*,\s*QKeyCombination\s*\(\s*Qt::(?P<modifier>\w+)\s*,"
    r"\s*Qt::Key_(?P<key>\w+)\s*\)\s*\)"
)

MODIFIER_NAMES = {"CTRL": "Ctrl", "SHIFT": "Shift", "ALT": "Alt", "META": "Meta"}


class ContractError(Exception):
    """The catalog is malformed, or it has drifted from the shell it mirrors."""


def load_policy(path: Path) -> dict:
    try:
        policy = json.loads(path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise ContractError(f"cannot read {path.name}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise ContractError(f"{path.name} is not valid JSON: {exc}") from exc

    if policy.get("schema_version") != 1:
        raise ContractError(
            f"{path.name} schema_version must be 1, got {policy.get('schema_version')!r}"
        )
    actions = policy.get("actions")
    if not isinstance(actions, list) or not actions:
        raise ContractError(f"{path.name} must declare a non-empty actions array")

    expected = policy.get("expected_action_count")
    if expected != len(actions):
        raise ContractError(
            f"{path.name} declares expected_action_count {expected!r} "
            f"but carries {len(actions)} actions"
        )
    return policy


def read_source(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        raise ContractError(f"cannot read {path.name}: {exc}") from exc


def action_ids_by_enum(text: str, source: Path = MAIN_WINDOW_PATH) -> dict[str, str]:
    mapping = {m.group("enum"): m.group("id") for m in SET_ACTION_RE.finditer(text)}
    if not mapping:
        raise ContractError(
            f"{source.name} exposes no setAction(PDFActionManager::X, ui->actionY) "
            "mapping; the shortcut parity check cannot run"
        )
    return mapping


def init_actions_body(text: str, source: Path = CONTROLLER_PATH) -> str:
    marker = "void PDFActionManager::initActions"
    start = text.find(marker)
    if start < 0:
        raise ContractError(f"{source.name} has no PDFActionManager::initActions")
    end = text.find("\n}\n", start)
    if end < 0:
        raise ContractError(
            f"{source.name}: cannot find the end of PDFActionManager::initActions"
        )
    return text[start:end]


def parse_shortcut_calls(chunk: str, source: Path = CONTROLLER_PATH) -> dict[str, dict]:
    found: dict[str, dict] = {}
    for match in STANDARD_KEY_RE.finditer(chunk):
        found[match.group("enum")] = {"standard_key": match.group("key")}
    for match in SEQUENCE_RE.finditer(chunk):
        found[match.group("enum")] = {"sequence": match.group("sequence")}
    for match in COMBINATION_RE.finditer(chunk):
        modifier = MODIFIER_NAMES.get(match.group("modifier").upper())
        if modifier is None:
            raise ContractError(
                f"{source.name}: unsupported key modifier Qt::{match.group('modifier')}"
            )
        found[match.group("enum")] = {"sequence": f"{modifier}+{match.group('key')}"}
    return found


def widget_shortcuts(text: str, source: Path = CONTROLLER_PATH) -> dict[str, dict]:
    """enum name -> expected catalog shortcut, honouring the Q_OS_WIN branch."""
    body = init_actions_body(text, source)

    windows_chunk = ""
    other_chunk = ""
    if "#ifdef Q_OS_WIN" in body:
        head, rest = body.split("#ifdef Q_OS_WIN", 1)
        if "#else" not in rest or "#endif" not in rest:
            raise ContractError(
                f"{source.name}: the Q_OS_WIN shortcut branch is not a "
                "plain #ifdef/#else/#endif; the parity check cannot read it"
            )
        windows_chunk, rest = rest.split("#else", 1)
        other_chunk, tail = rest.split("#endif", 1)
        common_chunk = head + tail
    else:
        common_chunk = body

    shortcuts = parse_shortcut_calls(common_chunk, source)
    for enum, shortcut in parse_shortcut_calls(other_chunk, source).items():
        shortcuts[enum] = dict(shortcut)
    for enum, shortcut in parse_shortcut_calls(windows_chunk, source).items():
        shortcuts.setdefault(enum, {})
        shortcuts[enum]["windows"] = shortcut
    return shortcuts


def check_command_block(action_id: str, command: object) -> list[str]:
    errors: list[str] = []
    if not isinstance(command, dict):
        return [f"{action_id}: command must be an object"]

    keys = set(command)
    missing = sorted(REQUIRED_COMMAND_KEYS - keys)
    if missing:
        errors.append(f"{action_id}: command is missing {', '.join(missing)}")
    unknown = sorted(keys - REQUIRED_COMMAND_KEYS - OPTIONAL_COMMAND_KEYS)
    if unknown:
        errors.append(f"{action_id}: command has unknown keys {', '.join(unknown)}")

    expected_label = f"command.{action_id}.label"
    if command.get("label_key") != expected_label:
        errors.append(
            f"{action_id}: label_key must be {expected_label!r}, "
            f"got {command.get('label_key')!r}"
        )

    availability = command.get("availability")
    if availability not in AVAILABILITIES:
        errors.append(f"{action_id}: availability {availability!r} is not a known state")

    capability = command.get("capability")
    if capability not in CAPABILITIES:
        errors.append(f"{action_id}: capability {capability!r} is not a known capability")
    elif capability == "unclassified" and availability == "implemented":
        errors.append(
            f"{action_id}: an implemented command needs a real capability, "
            "not 'unclassified'"
        )

    if not isinstance(command.get("cancellable"), bool):
        errors.append(f"{action_id}: cancellable must be a boolean")

    parameters = command.get("parameters")
    if not isinstance(parameters, list):
        errors.append(f"{action_id}: parameters must be an array")
    else:
        seen: set[str] = set()
        for index, parameter in enumerate(parameters):
            where = f"{action_id}: parameter {index}"
            if not isinstance(parameter, dict):
                errors.append(f"{where} must be an object")
                continue
            name = parameter.get("name")
            if not isinstance(name, str) or not name:
                errors.append(f"{where} needs a non-empty name")
            elif name in seen:
                errors.append(f"{where} repeats the name {name!r}")
            else:
                seen.add(name)
            if parameter.get("type") not in PARAMETER_TYPES:
                errors.append(f"{where} has unsupported type {parameter.get('type')!r}")
            if not isinstance(parameter.get("required"), bool):
                errors.append(f"{where} needs a boolean 'required'")
            unknown_parameter = sorted(set(parameter) - {"name", "type", "required"})
            if unknown_parameter:
                errors.append(f"{where} has unknown keys {', '.join(unknown_parameter)}")

        if availability == "declared" and parameters:
            errors.append(
                f"{action_id}: a declared command cannot promise parameters it never reads"
            )

    return errors


def check_shortcut_parity(
    policy: dict,
    enum_to_id: dict[str, str],
    shortcuts: dict[str, dict],
    main_window_path: Path = MAIN_WINDOW_PATH,
) -> list[str]:
    errors: list[str] = []
    commands = {action["id"]: action.get("command", {}) for action in policy["actions"]}

    expected_by_id: dict[str, dict] = {}
    for enum, shortcut in shortcuts.items():
        action_id = enum_to_id.get(enum)
        if action_id is None:
            errors.append(
                f"PDFActionManager::{enum} has a shortcut but no ui-> action mapping in "
                f"{main_window_path.name}"
            )
            continue
        expected_by_id[action_id] = shortcut

    for action_id, expected in sorted(expected_by_id.items()):
        command = commands.get(action_id)
        if command is None:
            errors.append(f"{action_id}: has a Widgets shortcut but no catalog entry")
            continue
        actual = command.get("shortcut")
        if actual is None:
            errors.append(
                f"{action_id}: the Widgets shell binds {expected!r} but the catalog "
                "declares no shortcut"
            )
        elif actual != expected:
            errors.append(
                f"{action_id}: catalog shortcut {actual!r} contradicts the Widgets "
                f"shell's {expected!r}"
            )

    for action_id, command in sorted(commands.items()):
        if "shortcut" in command and action_id not in expected_by_id:
            errors.append(
                f"{action_id}: the catalog invents a shortcut the Widgets shell does "
                "not bind; add it to PDFActionManager::initActions first"
            )

    return errors


def check_implemented_set(policy: dict) -> list[str]:
    errors: list[str] = []
    implemented = {
        action["id"]
        for action in policy["actions"]
        if action.get("command", {}).get("availability") == "implemented"
    }
    known_handlers = IMPLEMENTED_COMMANDS | SHELL_IMPLEMENTED_COMMANDS
    unexpected = sorted(implemented - known_handlers)
    if unexpected:
        errors.append(
            "commands marked implemented without a registered handler: "
            f"{', '.join(unexpected)}"
        )
    missing = sorted(IMPLEMENTED_COMMANDS - implemented)
    if missing:
        errors.append(
            "commands with a handler but no 'implemented' availability: "
            f"{', '.join(missing)}"
        )
    return errors


def validate_catalog(
    policy_path: Path = POLICY_PATH,
    main_window_path: Path = MAIN_WINDOW_PATH,
    controller_path: Path = CONTROLLER_PATH,
) -> list[str]:
    """Returns every rule violation. A malformed input raises instead."""
    policy = load_policy(policy_path)

    errors: list[str] = []
    seen_ids: set[str] = set()
    for action in policy["actions"]:
        if not isinstance(action, dict) or not isinstance(action.get("id"), str):
            errors.append("every action needs a string id")
            continue
        action_id = action["id"]
        if action_id in seen_ids:
            errors.append(f"{action_id}: duplicate action id")
        seen_ids.add(action_id)
        if "command" not in action:
            errors.append(f"{action_id}: has no command descriptor")
            continue
        errors.extend(check_command_block(action_id, action["command"]))

    errors.extend(check_implemented_set(policy))

    if main_window_path.is_file() and controller_path.is_file():
        enum_to_id = action_ids_by_enum(read_source(main_window_path), main_window_path)
        shortcuts = widget_shortcuts(read_source(controller_path), controller_path)
        errors.extend(check_shortcut_parity(policy, enum_to_id, shortcuts, main_window_path))
    elif not main_window_path.is_file() and not controller_path.is_file():
        pass
    else:
        errors.append(
            "Widgets shortcut parity requires both pdfeditormainwindow.cpp and "
            "pdfprogramcontroller.cpp, or neither after Issue 17"
        )
    return errors


def verify() -> str:
    errors = validate_catalog()
    if errors:
        raise ContractError("\n".join(f"  - {error}" for error in errors))

    policy = load_policy(POLICY_PATH)
    implemented = len(IMPLEMENTED_COMMANDS)
    if MAIN_WINDOW_PATH.is_file() and CONTROLLER_PATH.is_file():
        shortcuts = widget_shortcuts(read_source(CONTROLLER_PATH), CONTROLLER_PATH)
        shortcut_note = (
            f"{len(shortcuts)} shortcuts in parity with PDFActionManager::initActions."
        )
    else:
        shortcut_note = "Widgets shortcut parity skipped (Quick shell owns bindings after Issue 17)."
    return (
        f"Command catalog verified: {len(policy['actions'])} descriptors "
        f"({implemented} implemented, {len(policy['actions']) - implemented} declared); "
        f"{shortcut_note}"
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.parse_args(argv)

    try:
        print(verify())
    except ContractError as exc:
        print(f"{POLICY_PATH.name}: command catalog check failed:\n{exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
