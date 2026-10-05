#!/usr/bin/env python3
"""Measure fixed Core separation planes against Ghostscript tiffsep output."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import shutil
from pathlib import Path

from PIL import Image, ImageChops


def measure(core: Path, oracle: Path, fixture: dict, difference: Path) -> dict:
    with Image.open(core) as first, Image.open(oracle) as second:
        a, b = first.convert("L"), second.convert("L")
    expected = (fixture["width"], fixture["height"])
    if a.size != expected or b.size != expected:
        raise ValueError("Render geometry mismatch")
    delta = ImageChops.difference(a, b)
    delta.save(difference)
    measurements = []
    for region in fixture["regions"]:
        x, y, width, height = region
        if min(x, y) < 0 or min(width, height) <= 0 or x + width > a.width or y + height > a.height:
            raise ValueError("Invalid fixture measurement region")
        histogram = delta.crop((x, y, x + width, y + height)).histogram()
        count = sum(histogram[fixture["max_channel_delta"] + 1:])
        measurements.append({"region": region, "differing_pixels": count,
                             "maximum_delta": max(index for index, count in enumerate(histogram) if count),
                             "passed": count <= fixture["differing_pixel_budget"]})
    return {"status": "passed" if all(item["passed"] for item in measurements) else "rejected", "regions": measurements}


def compare(root: Path, manifest: dict, core_directory: Path, output: Path, distribution: Path) -> dict:
    lock = json.loads((distribution / "oracles.json").read_text(encoding="utf-8"))
    program = (distribution / lock["tools"]["ghostscript"]["path"]).resolve()
    version = subprocess.run([str(program), "--version"], capture_output=True, check=True).stdout.decode().strip()
    if not re.fullmatch(r"10\.[0-9]+\.[0-9]+", version):
        raise ValueError("Unqualified Ghostscript version")
    output.mkdir(parents=True, exist_ok=True)
    records = []
    fixtures = [item for item in manifest["fixtures"] if item["claim"] == "rendering"]
    fixtures += [item for item in manifest["derived_fixtures"] if item["claim"] == "rendering"]
    for fixture in fixtures:
        name = fixture["id"]
        source = core_directory / fixture["artifact"] if "artifact" in fixture else root / fixture["path"]
        retained = core_directory / (name + ".pdf")
        if source.resolve() != retained.resolve():
            shutil.copy2(source, retained)
        source = retained
        metadata = json.loads((core_directory / (name + ".json")).read_text(encoding="utf-8"))
        if metadata["process_space"] != "DeviceCMYK" or metadata["renderer"] != "PDFTransparencyRenderer" or metadata.get("paper") != "opaque-white" or metadata.get("channel_encoding") != "255*(1-ink*opacity)":
            raise ValueError("Unqualified Core renderer settings")
        prefix = output / name
        command = [str(program), "-dSAFER", "-dBATCH", "-dNOPAUSE", "-sDEVICE=tiffsep", "-r72",
                   f"-g{fixture['width']}x{fixture['height']}", "-dFIXEDMEDIA", "-dPDFFitPage",
                   "-dTextAlphaBits=1", "-dGraphicsAlphaBits=1", "-dUseFastColor=true", "-dSimulateOverprint=true",
                   *[f"-sDefault{space.upper()}Profile={(distribution / lock['profiles'][space]['path']).resolve()}" for space in ("rgb", "cmyk", "gray")],
                   f"-sOutputFile={prefix}.tif", str(source)]
        (output / (name + ".command.json")).write_text(json.dumps({"command": command, "version": version,
            "artifact_sha256": hashlib.sha256(source.read_bytes()).hexdigest()}, indent=2) + "\n", encoding="utf-8")
        completed = subprocess.run(command, capture_output=True, timeout=120, check=False)
        (output / (name + ".stdout")).write_bytes(completed.stdout)
        (output / (name + ".stderr")).write_bytes(completed.stderr)
        planes = []
        if completed.returncode == 0:
            expected_names = {plane["name"] for plane in metadata["channels"]}
            oracle_names = {path.stem.split("(", 1)[1][:-1] for path in output.glob(name + "(*).tif")}
            if expected_names != oracle_names:
                records.append({"fixture": name, "artifact_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                                "command": command, "exit_code": completed.returncode, "status": "incomplete", "planes": [],
                                "limitations": [fixture["limit"], f"Separation coverage mismatch: {expected_names} vs {oracle_names}"]})
                continue
            for index, plane in enumerate(metadata["channels"]):
                if Path(plane["file"]).name != plane["file"]:
                    raise ValueError("Invalid Core plane path")
                oracle = output / (name + "(" + plane["name"] + ").tif")
                measurement = measure(core_directory / plane["file"], oracle, fixture, output / f"{name}.delta-{index}.png")
                planes.append({"separation": plane["name"], **measurement})
        records.append({"fixture": name, "artifact_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                        "command": command, "exit_code": completed.returncode,
                        "status": "passed" if planes and all(item["status"] == "passed" for item in planes) else "rejected",
                        "planes": planes, "limitations": [fixture["limit"]]})
    return {"schema": "loop.independent-render-measurements", "schema_version": 1,
            "oracle": "Ghostscript", "version": version, "settings": "DeviceCMYK; 72dpi fit to 128x128; no font substitution permitted by the text-free corpus",
            "status": "passed" if records and all(item["status"] == "passed" for item in records) else "rejected", "fixtures": records}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core-directory", type=Path, required=True)
    parser.add_argument("--distribution", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    result = compare(root, json.loads((root / "docs/independent-claims.json").read_text(encoding="utf-8")), args.core_directory, args.output, args.distribution)
    (args.output / "measurements.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return 0 if result["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
