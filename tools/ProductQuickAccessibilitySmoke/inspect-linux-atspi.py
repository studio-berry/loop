"""Read the operator probe through Linux's native AT-SPI accessibility bus."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import time
from pathlib import Path

try:
    import gi

    gi.require_version("Atspi", "2.0")
    from gi.repository import Atspi
except (ImportError, ValueError):
    Atspi = None


def wait_for(predicate, process, description: str):
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        if process.poll() is not None:
            raise RuntimeError(f"operator probe exited while waiting for {description}")
        time.sleep(0.1)
    raise RuntimeError(f"native accessibility timeout: {description}")


def descendants(root):
    pending = [root]
    while pending:
        node = pending.pop()
        node.clear_cache()
        yield node
        pending.extend(node.get_child_at_index(index) for index in range(node.get_child_count()))


def observe(node, expected: dict, stage: int, process, api) -> dict:
    if node.get_process_id() != process.pid:
        raise RuntimeError("AT-SPI node belongs to another process")
    if node.get_description() != expected["description"]:
        raise RuntimeError(f"native description differs for {expected['name']} at stage {stage}")
    focused = False
    if expected.get("focusable"):
        if not node.get_state_set().contains(api.StateType.FOCUSABLE):
            raise RuntimeError(f"{expected['name']} has no native keyboard focus target")
        if not node.get_component_iface().grab_focus():
            raise RuntimeError(f"native focus refused for {expected['name']}")

        def has_focus():
            node.clear_cache()
            return node.get_state_set().contains(api.StateType.FOCUSED)

        focused = bool(wait_for(has_focus, process, expected["name"] + " focus"))
    elif node.get_state_set().contains(api.StateType.ENABLED) != expected["enabled"]:
        raise RuntimeError(f"native availability differs for {expected['name']}")
    return {
        "stage": stage,
        "name": node.get_name(),
        "description": node.get_description(),
        "control_type": node.get_role_name(),
        "focused": focused,
        "process_id": node.get_process_id(),
    }


def inspect(executable: Path, output: Path) -> None:
    if Atspi is None:
        raise RuntimeError("Linux qualification requires python3-gi and gir1.2-atspi-2.0")
    if not os.environ.get("DISPLAY") or not os.environ.get("DBUS_SESSION_BUS_ADDRESS"):
        raise RuntimeError("Linux qualification requires an X11 display and a session accessibility bus")
    output.mkdir(parents=True, exist_ok=False)
    environment = dict(os.environ, QT_QPA_PLATFORM="xcb", QT_ACCESSIBILITY="1", QT_LINUX_ACCESSIBILITY_ALWAYS_ON="1")
    environment.pop("QT_QUICK_BACKEND", None)
    observations = []
    with (output / "stdout.txt").open("w") as stdout, (output / "stderr.txt").open("w") as stderr:
        process = subprocess.Popen([str(executable), "--operator-native-probe", str(output)], env=environment,
                                   stdout=stdout, stderr=stderr)
        try:
            for stage in range(7):
                command = output / "stage.command.tmp"
                command.write_text(str(stage), encoding="utf-8")
                command.replace(output / "stage.command")
                snapshot_path = output / f"stage-{stage}.json"
                wait_for(snapshot_path.is_file, process, f"stage {stage}")
                expected = json.loads(snapshot_path.read_text(encoding="utf-8"))

                def find_application():
                    desktop = Atspi.get_desktop(0)
                    desktop.clear_cache()
                    for index in range(desktop.get_child_count()):
                        app = desktop.get_child_at_index(index)
                        if app.get_process_id() == process.pid:
                            return app
                    return None

                app = wait_for(find_application, process, "AT-SPI application")
                for target in expected["nodes"]:
                    node = wait_for(lambda: next((node for node in descendants(app)
                                                  if node.get_name() == target["name"]), None),
                                    process, target["name"])
                    observations.append(observe(node, target, stage, process, Atspi))
            (output / "stage.command").write_text("7", encoding="utf-8")
            if process.wait(timeout=10) != 0:
                raise RuntimeError("operator probe failed")
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
    final = json.loads((output / "stage-6.json").read_text(encoding="utf-8"))
    if final["native_accessibility_active"] is not True:
        raise RuntimeError("native accessibility backend is inactive")
    if final["graphics_api"].casefold() in {"software", "unknown", "null", "unrecognized", "absent"}:
        raise RuntimeError("native probe has no native scene graph")
    if "native_accessibility_active=1" not in (output / "stdout.txt").read_text(encoding="utf-8"):
        raise RuntimeError("terminal operator probe did not confirm native accessibility")
    report = {
        "status": "pass", "platform": "Linux AT-SPI", "graphics_api": final["graphics_api"],
        "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
        "fixture_sha256": hashlib.sha256((output / "operator-fixture.pdf").read_bytes()).hexdigest(),
        "observations": observations,
    }
    (output / "native-atspi.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Native AT-SPI passed: {len(observations)} observations across seven operator states")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--output-directory", type=Path, required=True)
    args = parser.parse_args()
    inspect(args.executable.resolve(strict=True), args.output_directory.resolve())


if __name__ == "__main__":
    main()
