#!/usr/bin/env python3
"""Build the deterministic synthetic resource-envelope fixture bundle.

Hosted qualification cannot reach the external DIV2K corpus or a private
fixture bundle, so the office, image-heavy, and 10,000-page fixtures are
generated here. Every pixel derives from SHAKE-256 over a fixed label and the
PDF structure is fixed, so the same generator version produces byte-identical
fixtures on every platform. Image data is incompressible noise stored with
FlateDecode: file size tracks decoded size and every page pays a real decode.

    python scripts/resource_envelope/synthetic_workload.py \\
        --output-dir /tmp/loop-envelope --manifest /tmp/loop-envelope/fixtures.json
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Sequence

_ROOT = Path(__file__).resolve().parents[2]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

from scripts.resource_envelope.create_fixture_manifest import create_manifest
from scripts.resource_envelope.pathological_workload import build_pathological_pdf
from scripts.resource_envelope.run_matrix import FIXTURE_SPECS


GENERATOR_VERSION = 1
PAGE_WIDTH = 612
PAGE_HEIGHT = 792
IMAGE_HEAVY_WORKLOAD = "synthetic-image-heavy"
_WORDS = (
    "press", "proof", "bleed", "ink", "plate", "sheet", "trim", "spot", "cyan", "magenta",
    "yellow", "black", "overprint", "separation", "imposition", "signature", "gutter", "margin",
    "raster", "vector", "profile", "output", "intent", "coverage", "density", "register",
)


@dataclass(frozen=True)
class ImageSpec:
    width: int
    height: int

    @property
    def decoded_bytes(self) -> int:
        return self.width * self.height * 3


# Sized so each fixture lands inside run_matrix.FIXTURE_SPECS byte bounds.
OFFICE_PAGES = 24
OFFICE_IMAGE_EVERY = 4
OFFICE_IMAGE = ImageSpec(380, 280)
IMAGE_HEAVY_PAGES = 60
IMAGE_HEAVY_IMAGE = ImageSpec(2048, 1360)
TEN_THOUSAND_PAGES = 10000
TEN_THOUSAND_UNIQUE_IMAGES = 64
TEN_THOUSAND_IMAGE = ImageSpec(640, 480)


def _noise(label: str, size: int) -> bytes:
    return hashlib.shake_256(f"loop-resource-envelope/v{GENERATOR_VERSION}/{label}".encode("ascii")).digest(size)


def _stream(dictionary: bytes, payload: bytes) -> bytes:
    return dictionary[:-2] + b" /Length " + str(len(payload)).encode("ascii") + b" >>\nstream\n" + payload + b"\nendstream"


def _image_object(label: str, image: ImageSpec) -> bytes:
    dictionary = (
        f"<< /Type /XObject /Subtype /Image /Width {image.width} /Height {image.height}"
        " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /FlateDecode >>"
    ).encode("ascii")
    return _stream(dictionary, zlib.compress(_noise(label, image.decoded_bytes), level=1))


def _image_placement(image: ImageSpec, name: str) -> bytes:
    scale = min(PAGE_WIDTH / image.width, PAGE_HEIGHT / image.height)
    width = image.width * scale
    height = image.height * scale
    x = (PAGE_WIDTH - width) / 2
    y = (PAGE_HEIGHT - height) / 2
    return f"q {width:.4f} 0 0 {height:.4f} {x:.4f} {y:.4f} cm /{name} Do Q\n".encode("ascii")


def _page_object(parent: int, contents: int, resources: bytes) -> bytes:
    return (
        f"<< /Type /Page /Parent {parent} 0 R /MediaBox [0 0 {PAGE_WIDTH} {PAGE_HEIGHT}] /Resources ".encode("ascii")
        + resources
        + f" /Contents {contents} 0 R >>".encode("ascii")
    )


class _PdfWriter:
    """Writes numbered objects straight to disk so a 500 MB fixture never sits in memory."""

    def __init__(self, handle: BinaryIO, object_count: int) -> None:
        self._handle = handle
        self._offsets = [0] * (object_count + 1)
        self._written = 0
        self._write(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")

    def _write(self, data: bytes) -> None:
        self._handle.write(data)
        self._written += len(data)

    def add(self, number: int, body: bytes) -> None:
        if self._offsets[number]:
            raise ValueError(f"object {number} written twice")
        self._offsets[number] = self._written
        self._write(f"{number} 0 obj\n".encode("ascii") + body + b"\nendobj\n")

    def finish(self, file_id: str) -> None:
        missing = [number for number, offset in enumerate(self._offsets) if number and not offset]
        if missing:
            raise ValueError(f"objects never written: {missing[:5]}")
        xref = self._written
        self._write(f"xref\n0 {len(self._offsets)}\n0000000000 65535 f \n".encode("ascii"))
        for offset in self._offsets[1:]:
            self._write(f"{offset:010d} 00000 n \n".encode("ascii"))
        self._write(
            f"trailer\n<< /Size {len(self._offsets)} /Root 1 0 R /ID [<{file_id}> <{file_id}>] >>\n"
            f"startxref\n{xref}\n%%EOF\n".encode("ascii")
        )


def _file_id(fixture_id: str) -> str:
    return hashlib.sha256(f"{fixture_id}/v{GENERATOR_VERSION}".encode("ascii")).hexdigest()[:32]


def _office_text(page_index: int) -> bytes:
    selector = _noise(f"office/text/{page_index}", 40 * 12)
    lines = [b"BT", b"/F1 10 Tf", b"12 TL", f"54 {PAGE_HEIGHT - 60} Td".encode("ascii")]
    for line in range(40):
        words = " ".join(_WORDS[value % len(_WORDS)] for value in selector[line * 12 : line * 12 + 12])
        lines.append(f"({words}) '".encode("ascii"))
    lines.append(b"ET")
    for row in range(8):
        y = 80 + row * 18
        lines.append(f"0.2 w 54 {y} m {PAGE_WIDTH - 54} {y} l S".encode("ascii"))
    return b"\n".join(lines) + b"\n"


def build_office(output: Path, pages: int = OFFICE_PAGES, image: ImageSpec = OFFICE_IMAGE) -> None:
    image_pages = [index for index in range(pages) if index % OFFICE_IMAGE_EVERY == 0]
    font = 3
    first_image = 4
    first_page = first_image + len(image_pages)
    object_count = first_page + 2 * pages - 1
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("wb") as handle:
        writer = _PdfWriter(handle, object_count)
        writer.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
        kids = " ".join(f"{first_page + 2 * index} 0 R" for index in range(pages))
        writer.add(2, f"<< /Type /Pages /Kids [{kids}] /Count {pages} >>".encode("ascii"))
        writer.add(font, b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>")
        for slot in range(len(image_pages)):
            writer.add(first_image + slot, _image_object(f"office/image/{slot}", image))
        for index in range(pages):
            content = _office_text(index)
            xobjects = b""
            if index in image_pages:
                slot = image_pages.index(index)
                content += f"q {image.width / 2:.4f} 0 0 {image.height / 2:.4f} 54 {PAGE_HEIGHT - 620} cm /Im0 Do Q\n".encode("ascii")
                xobjects = f" /XObject << /Im0 {first_image + slot} 0 R >>".encode("ascii")
            resources = f"<< /Font << /F1 {font} 0 R >>".encode("ascii") + xobjects + b" >>"
            page_object = first_page + 2 * index
            writer.add(page_object, _page_object(2, page_object + 1, resources))
            writer.add(page_object + 1, _stream(b"<< >>", content))
        writer.finish(_file_id("office-2mb"))


def build_image_pages(output: Path, fixture_id: str, pages: int, unique_images: int, image: ImageSpec) -> None:
    if pages < 1 or unique_images < 1:
        raise ValueError("pages and unique_images must be positive")
    first_image = 3
    first_page = first_image + unique_images
    object_count = first_page + 2 * pages - 1
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("wb") as handle:
        writer = _PdfWriter(handle, object_count)
        writer.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
        kids = " ".join(f"{first_page + 2 * index} 0 R" for index in range(pages))
        writer.add(2, f"<< /Type /Pages /Kids [{kids}] /Count {pages} >>".encode("ascii"))
        for slot in range(unique_images):
            writer.add(first_image + slot, _image_object(f"{fixture_id}/image/{slot}", image))
        placement = _image_placement(image, "Im0")
        for index in range(pages):
            resources = f"<< /XObject << /Im0 {first_image + index % unique_images} 0 R >> >>".encode("ascii")
            page_object = first_page + 2 * index
            writer.add(page_object, _page_object(2, page_object + 1, resources))
            writer.add(page_object + 1, _stream(b"<< >>", placement))
        writer.finish(_file_id(fixture_id))


def _provenance(fixture_id: str) -> str:
    return f"scripts/resource_envelope/synthetic_workload.py generator v{GENERATOR_VERSION} ({fixture_id})"


def build_bundle(output_dir: Path) -> dict[str, object]:
    output_dir.mkdir(parents=True, exist_ok=True)
    paths = {fixture_id: output_dir / f"{fixture_id}.pdf" for fixture_id in FIXTURE_SPECS if fixture_id != "multi-gb"}
    build_office(paths["office-2mb"])
    build_image_pages(paths["image-heavy-500mb"], "image-heavy-500mb", IMAGE_HEAVY_PAGES, IMAGE_HEAVY_PAGES, IMAGE_HEAVY_IMAGE)
    build_image_pages(paths["ten-thousand-page"], "ten-thousand-page", TEN_THOUSAND_PAGES, TEN_THOUSAND_UNIQUE_IMAGES, TEN_THOUSAND_IMAGE)
    build_pathological_pdf(paths["pathological-vector"], 256, 512, "pathological-vector")
    build_pathological_pdf(paths["transparency-spots"], 256, 256, "transparency-spots")

    manifest = create_manifest(paths, "synthetic")
    for record in manifest["fixtures"]:
        fixture_id = str(record["fixture_id"])
        record["provenance"] = _provenance(fixture_id)
        record["path"] = paths[fixture_id].name
        if fixture_id == "ten-thousand-page":
            record["workload"] = IMAGE_HEAVY_WORKLOAD
        spec = FIXTURE_SPECS[fixture_id]
        size = int(record["size_bytes"])
        if (spec["min_bytes"] is not None and size < spec["min_bytes"]) or (spec["max_bytes"] is not None and size > spec["max_bytes"]):
            raise ValueError(f"{fixture_id} generated {size} bytes, outside its fixture bounds")
    manifest["generator"] = {"script": "scripts/resource_envelope/synthetic_workload.py", "version": GENERATOR_VERSION}
    return manifest


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True, help="fixture manifest to write; paths are relative to it")
    args = parser.parse_args(argv)
    try:
        if args.manifest.resolve().parent != args.output_dir.resolve():
            raise ValueError("--manifest must be written inside --output-dir so its relative paths resolve")
        manifest = build_bundle(args.output_dir)
        args.manifest.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    except (OSError, ValueError) as exc:
        print(f"synthetic workload error: {exc}", file=sys.stderr)
        return 2
    print(json.dumps({record["fixture_id"]: {"sha256": record["sha256"], "size_bytes": record["size_bytes"]} for record in manifest["fixtures"]}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
