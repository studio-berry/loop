#!/usr/bin/env python3
# MIT License
#
# Copyright (c) 2018-2025 Jakub Melka and Contributors
#
# Regenerate the bizarre page-box corpus under loop-preflight/testdata/page-box-corpus/.
# Pure-Python minimal PDF writer (no third-party deps).
#
# Usage:
#     python3 tools/generate_page_box_corpus.py [output-dir]

import json
import os
import sys


def _pdf_real(value):
    text = f"{float(value):.4f}".rstrip("0").rstrip(".")
    return text if text else "0"


def _write_minimal_pdf(path, media, crop=None, bleed=None, trim=None, art=None, rotate=None, content=None):
    page_entries = [
        f"/Type /Page",
        f"/Parent 2 0 R",
        f"/MediaBox [{_pdf_real(media[0])} {_pdf_real(media[1])} {_pdf_real(media[2])} {_pdf_real(media[3])}]",
        "/Resources << >>",
    ]
    if content is not None:
        page_entries.append("/Contents 4 0 R")
    if crop is not None:
        page_entries.append(
            f"/CropBox [{_pdf_real(crop[0])} {_pdf_real(crop[1])} {_pdf_real(crop[2])} {_pdf_real(crop[3])}]"
        )
    if bleed is not None:
        page_entries.append(
            f"/BleedBox [{_pdf_real(bleed[0])} {_pdf_real(bleed[1])} {_pdf_real(bleed[2])} {_pdf_real(bleed[3])}]"
        )
    if trim is not None:
        page_entries.append(
            f"/TrimBox [{_pdf_real(trim[0])} {_pdf_real(trim[1])} {_pdf_real(trim[2])} {_pdf_real(trim[3])}]"
        )
    if art is not None:
        page_entries.append(
            f"/ArtBox [{_pdf_real(art[0])} {_pdf_real(art[1])} {_pdf_real(art[2])} {_pdf_real(art[3])}]"
        )
    if rotate is not None:
        page_entries.append(f"/Rotate {int(rotate)}")

    objects = [
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< " + " ".join(page_entries) + " >>",
    ]
    if content is not None:
        objects.append(f"<< /Length {len(content)} >>\nstream\n{content}\nendstream")

    body = "%PDF-1.4\n"
    offsets = [0]
    for index, object_body in enumerate(objects, start=1):
        offsets.append(len(body))
        body += f"{index} 0 obj\n{object_body}\nendobj\n"

    xref_offset = len(body)
    body += f"xref\n0 {len(objects) + 1}\n"
    body += "0000000000 65535 f \n"
    for offset in offsets[1:]:
        body += f"{offset:010d} 00000 n \n"
    body += (
        "trailer\n"
        f"<< /Size {len(objects) + 1} /Root 1 0 R >>\n"
        "startxref\n"
        f"{xref_offset}\n"
        "%%EOF\n"
    )

    with open(path, "wb") as stream:
        stream.write(body.encode("latin-1"))


def generate(output_dir):
    os.makedirs(output_dir, exist_ok=True)

    fixtures = [
        {
            "id": "inverted-media",
            "pdf": "pagebox-inverted-media.pdf",
            "media": [312, 312, 0, 0],
            "crop": [302, 302, 10, 10],
            "bleed": [0, 0, 312, 312],
            "trim": [12, 12, 300, 300],
            "reader": {
                "result": "ok",
                "media_box": [0, 0, 312, 312],
                "crop_box": [10, 10, 292, 292],
                "bleed_box": [0, 0, 312, 312],
                "trim_box": [12, 12, 288, 288],
            },
            "content": (
                "0 0 0 rg\n"
                "0 0 312 312 re f\n"
            ),
            "preflight": {
                "profile": "profiles/loop-default.json",
                "pass": True,
                "check_ids": ["bleed"],
            },
            "clip": {
                "without_clip_nonwhite_pixels": 16384,
                "with_clip_nonwhite_pixels_max": 12000,
            },
        },
        {
            "id": "zero-area",
            "pdf": "pagebox-zero-area.pdf",
            "media": [100, 100, 100, 100],
            "reader": {
                "result": "ok",
                "media_box": [100, 100, 0, 0],
            },
        },
        {
            "id": "negative-origin",
            "pdf": "pagebox-negative-origin.pdf",
            "media": [-50, -50, 262, 262],
            "crop": [-40, -40, 252, 252],
            "reader": {
                "result": "ok",
                "media_box": [-50, -50, 312, 312],
                "crop_box": [-40, -40, 292, 292],
            },
        },
        {
            "id": "rotated-90",
            "pdf": "pagebox-rotated-90.pdf",
            "media": [0, 0, 312, 312],
            "rotate": 90,
            "reader": {
                "result": "ok",
                "media_box": [0, 0, 312, 312],
                "rotation": "Rotate90",
            },
        },
        {
            "id": "art-outside-media",
            "pdf": "pagebox-art-outside-media.pdf",
            "media": [0, 0, 200, 200],
            "art": [-20, -20, 240, 240],
            "reader": {
                "result": "ok",
                "media_box": [0, 0, 200, 200],
                "art_box": [-20, -20, 260, 260],
            },
        },
        {
            "id": "invalid-rotation",
            "pdf": "pagebox-invalid-rotation.pdf",
            "media": [0, 0, 200, 200],
            "rotate": 45,
            "reader": {
                "result": "failed",
                "error_contains": "Invalid page rotation",
            },
        },
    ]

    manifest = []
    for fixture in fixtures:
        pdf_path = os.path.join(output_dir, fixture["pdf"])
        _write_minimal_pdf(
            pdf_path,
            fixture["media"],
            crop=fixture.get("crop"),
            bleed=fixture.get("bleed"),
            trim=fixture.get("trim"),
            art=fixture.get("art"),
            rotate=fixture.get("rotate"),
            content=fixture.get("content"),
        )
        entry = {
            "id": fixture["id"],
            "pdf": fixture["pdf"],
            "reader": fixture["reader"],
        }
        if "preflight" in fixture:
            entry["preflight"] = fixture["preflight"]
        manifest.append(entry)

    manifest_path = os.path.join(output_dir, "manifest.json")
    with open(manifest_path, "w", encoding="utf-8") as stream:
        json.dump(manifest, stream, indent=2)
        stream.write("\n")


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(__file__), "..", "testdata", "page-box-corpus"
    )
    generate(os.path.abspath(out))
