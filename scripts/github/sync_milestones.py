#!/usr/bin/env python3
"""Sync GitHub milestone descriptions from docs/github-milestones/."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MILESTONES_DIR = ROOT / "docs" / "github-milestones"
MANIFEST_PATH = MILESTONES_DIR / "manifest.json"
REPO = "studio-berry/loupe"


@dataclass(frozen=True)
class MilestoneSpec:
    title: str
    description: str
    state: str


@dataclass(frozen=True)
class RemoteMilestone:
    number: int
    title: str
    description: str
    state: str


def load_specs() -> list[MilestoneSpec]:
    manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
    specs: list[MilestoneSpec] = []
    for entry in manifest["milestones"]:
        description_path = MILESTONES_DIR / entry["description_file"]
        specs.append(
            MilestoneSpec(
                title=entry["title"],
                description=description_path.read_text(encoding="utf-8").strip(),
                state=entry.get("state", "open"),
            )
        )
    return specs


def gh_api(method: str, path: str, payload: dict | None = None) -> object:
    command = ["gh", "api", "--method", method, path]
    if payload is not None:
        command.extend(["--input", "-"])
    try:
        completed = subprocess.run(
            command,
            input=None if payload is None else json.dumps(payload),
            text=True,
            capture_output=True,
            check=True,
        )
    except subprocess.CalledProcessError as exc:
        raise RuntimeError(exc.stderr.strip() or exc.stdout.strip()) from exc
    if not completed.stdout.strip():
        return {}
    return json.loads(completed.stdout)


def list_remote_milestones() -> list[RemoteMilestone]:
    raw = gh_api("GET", f"repos/{REPO}/milestones?state=all&per_page=100")
    if not isinstance(raw, list):
        raise RuntimeError("unexpected milestone list response")
    return [
        RemoteMilestone(
            number=item["number"],
            title=item["title"],
            description=item.get("description") or "",
            state=item["state"],
        )
        for item in raw
    ]


def plan_updates(
    specs: list[MilestoneSpec], remote: list[RemoteMilestone]
) -> list[tuple[MilestoneSpec, RemoteMilestone]]:
    by_title = {item.title: item for item in remote}
    planned: list[tuple[MilestoneSpec, RemoteMilestone]] = []
    for spec in specs:
        current = by_title.get(spec.title)
        if current is None:
            raise RuntimeError(f"missing GitHub milestone titled {spec.title!r}")
        if (
            current.description.strip() == spec.description
            and current.state == spec.state
        ):
            continue
        planned.append((spec, current))
    return planned


def apply_update(spec: MilestoneSpec, current: RemoteMilestone) -> None:
    gh_api(
        "PATCH",
        f"repos/{REPO}/milestones/{current.number}",
        {
            "description": spec.description,
            "state": spec.state,
        },
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--apply",
        action="store_true",
        help="write updates to GitHub (default is dry run)",
    )
    args = parser.parse_args()

    specs = load_specs()
    remote = list_remote_milestones()
    updates = plan_updates(specs, remote)

    if not updates:
        print("All milestone descriptions already match manifest.")
        return 0

    for spec, current in updates:
        print(f"{spec.title}: milestone #{current.number}")
        if current.description.strip() != spec.description:
            print("  description: update")
        if current.state != spec.state:
            print(f"  state: {current.state} -> {spec.state}")

    if not args.apply:
        print(f"Dry run only. Re-run with --apply to update {len(updates)} milestone(s).")
        return 0

    for spec, current in updates:
        apply_update(spec, current)
        print(f"Updated {spec.title} (#{current.number}).")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc
