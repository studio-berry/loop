"""Synthetic adversarial PDF corpus for pdf::PDFProcessingBudget (gh-243).

Each fixture is a small, deterministic, hand-assembled PDF shaped to trip
exactly one `pdf::PDFBudgetKind` when read with the tightened limit recorded
for it in manifest.json -- not by being large, but by being the wrong shape
(a decompression bomb, a deeply nested object, thousands of tiny operators,
...). None of these files are third-party samples and none exceed a few
kilobytes: the point is that a hostile document does not need to be big to
be hostile, and Loop must fail closed (report the exact exceeded budget)
rather than hang, get OOM-killed, or silently return a clean result.

Two thirds of the manifest ("path": "session") trip their budget while
`pdf::PreflightEngine` walks an already-parsed document: the fixture is a
syntactically valid, fully readable PDF, and the corpus test tightens one
`pdf::PDFProcessingLimits` field (or, for the raster-probe fixture, one
preflight check parameter) before running a profile that touches page
content. The rest ("path": "reader") trip during `pdf::PDFDocumentReader`
itself, before a document exists at all -- those fixtures still parse as a
syntactically valid xref/trailer, but one object in the table is shaped to
blow the tightened limit while `pdf::PDFDocumentReader::readFromBuffer()`
walks every occupied entry.

Regenerate with:
    python3 -m scripts.resource_envelope.budget_exhaustion_corpus \
        --output-dir UnitTests/testdata/budget_exhaustion
"""

from __future__ import annotations

import argparse
import hashlib
import json
import zlib
from pathlib import Path

SCHEMA_VERSION = 1


def _object_body(number: int, body: bytes) -> bytes:
    return f"{number} 0 obj\n".encode("ascii") + body + b"\nendobj\n"


def stream_body(extra_dict_entries: bytes, data: bytes) -> bytes:
    """A stream object body: `<< <extra entries> /Length N >>\\nstream\\n<data>\\nendstream`."""

    prefix = b"<< " + extra_dict_entries
    if extra_dict_entries:
        prefix += b" "
    return prefix + f"/Length {len(data)} >>\nstream\n".encode("ascii") + data + b"\nendstream"


def assemble_pdf(bodies: dict[int, bytes]) -> bytes:
    """Assembles a minimal, syntactically valid PDF from object bodies.

    `bodies` maps an object number to its raw body (without the surrounding
    "N 0 obj" / "endobj" markers). Object numbers must be the contiguous
    range 1..max(bodies) so the cross-reference table is trivial to build,
    and every entry -- reachable from the catalog or not -- lands in the
    xref table, because pdf::PDFDocumentReader budgets every occupied entry
    it walks, not only the ones the page tree references.
    """

    max_object = max(bodies)
    if set(bodies) != set(range(1, max_object + 1)):
        raise ValueError("object numbers must be contiguous starting at 1")

    pdf = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets = [0] * (max_object + 1)
    for number in range(1, max_object + 1):
        offsets[number] = len(pdf)
        pdf.extend(_object_body(number, bodies[number]))

    xref_offset = len(pdf)
    pdf.extend(f"xref\n0 {max_object + 1}\n".encode("ascii"))
    pdf.extend(b"0000000000 65535 f \n")
    for number in range(1, max_object + 1):
        pdf.extend(f"{offsets[number]:010d} 00000 n \n".encode("ascii"))
    pdf.extend(
        f"trailer\n<< /Size {max_object + 1} /Root 1 0 R >>\nstartxref\n{xref_offset}\n%%EOF\n".encode("ascii")
    )
    return bytes(pdf)


def build_page_document(pages_content: list[bytes], media_box: tuple[int, int, int, int] = (0, 0, 612, 792)) -> dict[int, bytes]:
    """A minimal, fully readable N-page document; object 1 is the Catalog, object 2 the Pages node.

    Pages use only resource-free content operators (rg/re/f and similar), so
    no /Resources entries are required. Returns the object body map; caller
    assembles it (optionally after appending more objects).
    """

    bodies: dict[int, bytes] = {1: b"<< /Type /Catalog /Pages 2 0 R >>"}
    media = " ".join(str(value) for value in media_box)
    page_refs: list[int] = []
    next_object = 3
    for content in pages_content:
        page_object = next_object
        content_object = next_object + 1
        next_object += 2
        page_refs.append(page_object)
        bodies[page_object] = (
            f"<< /Type /Page /Parent 2 0 R /MediaBox [{media}]"
            f" /Resources << /ProcSet [/PDF] >> /Contents {content_object} 0 R >>"
        ).encode("ascii")
        bodies[content_object] = stream_body(b"", content)
    kids = " ".join(f"{reference} 0 R" for reference in page_refs)
    bodies[2] = f"<< /Type /Pages /Kids [{kids}] /Count {len(page_refs)} >>".encode("ascii")
    return bodies


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


# ---------------------------------------------------------------------------
# Fixture builders. Each returns (pdf_bytes, manifest_case_without_pdf_or_sha).
# ---------------------------------------------------------------------------


def build_decompression_bomb() -> tuple[bytes, dict]:
    """FlateDecode content stream with an extreme decoded/compressed ratio."""

    payload = b"A" * 200_000
    compressed = zlib.compress(payload, level=9)
    content_stream = stream_body(b"/Filter /FlateDecode", compressed)
    bodies = build_page_document([b"0 0 0 rg 0 0 1 1 re f\n"])
    # Splice a compressed content stream into the page built by build_page_document
    # (object 4 is the first page's content stream; see build_page_document).
    bodies[4] = content_stream
    pdf = assemble_pdf(bodies)
    case = {
        "id": "decompression-bomb",
        "description": (
            "A single page whose content stream is a FlateDecode decompression "
            "bomb: %d bytes of highly compressible payload compress to %d bytes "
            "(ratio ~%d:1)." % (len(payload), len(compressed), len(payload) // max(1, len(compressed)))
        ),
        "path": "session",
        "limits": {"maxDecompressionRatio": 40},
        "profile": {
            "name": "budget-corpus-decompression-bomb",
            "checks": [{"id": "color-inventory", "severity": "info"}],
        },
        "expected": {"kind": "decompression-ratio", "pool": "decoded-streams"},
    }
    return pdf, case


def build_cumulative_decoded_bytes() -> tuple[bytes, dict]:
    """Many pages, each with a small unfiltered content stream; the *sum* exceeds the cap."""

    page_count = 12
    single_page_content = (b"1 0 0 rg 0 0 1 1 re f\n" * 46)  # ~1012 bytes, decoded 1:1 (no filter)
    bodies = build_page_document([single_page_content] * page_count)
    pdf = assemble_pdf(bodies)
    case = {
        "id": "cumulative-decoded-bytes",
        "description": (
            "%d pages, each with an unfiltered ~%d byte content stream; no single "
            "stream is large, but the cumulative decoded total across the "
            "document exceeds a tightened cap." % (page_count, len(single_page_content))
        ),
        "path": "session",
        "limits": {"maxCumulativeDecodedBytes": 6000},
        "profile": {
            "name": "budget-corpus-cumulative-decoded-bytes",
            "checks": [{"id": "color-inventory", "severity": "info"}],
        },
        "expected": {"kind": "cumulative-decoded-bytes", "pool": "decoded-streams"},
    }
    return pdf, case


def build_deep_nested_content_streams() -> tuple[bytes, dict]:
    """A chain of Form XObjects, each invoking the next via the Do operator."""

    form_count = 12
    bodies: dict[int, bytes] = {1: b"<< /Type /Catalog /Pages 2 0 R >>"}

    # Object numbers: 3 = page, 4 = page content, 5.. = form dictionaries (one
    # object per form; each form's content stream is embedded via the form's
    # own /Length, so a form is itself the stream object).
    first_form_object = 5
    form_objects = [first_form_object + index for index in range(form_count)]

    page_object = 3
    page_content_object = 4
    bodies[page_object] = (
        f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200]"
        f" /Resources << /ProcSet [/PDF] /XObject << /Fm0 {form_objects[0]} 0 R >> >>"
        f" /Contents {page_content_object} 0 R >>"
    ).encode("ascii")
    bodies[page_content_object] = stream_body(b"", b"/Fm0 Do\n")

    for index, form_object in enumerate(form_objects):
        is_last = index == len(form_objects) - 1
        if is_last:
            resources = b"<< /ProcSet [/PDF] >>"
            content = b"0 0 0 rg 0 0 1 1 re f\n"
        else:
            next_object = form_objects[index + 1]
            resources = f"<< /ProcSet [/PDF] /XObject << /Fm{index + 1} {next_object} 0 R >> >>".encode("ascii")
            content = f"/Fm{index + 1} Do\n".encode("ascii")
        dict_entries = (
            b"/Type /XObject /Subtype /Form /BBox [0 0 200 200] /Resources " + resources
        )
        bodies[form_object] = stream_body(dict_entries, content)

    bodies[2] = f"<< /Type /Pages /Kids [{page_object} 0 R] /Count 1 >>".encode("ascii")
    pdf = assemble_pdf(bodies)
    case = {
        "id": "deep-nested-content-streams",
        "description": (
            "A page whose content stream invokes a chain of %d nested Form "
            "XObjects (each drawing the next via the Do operator)." % form_count
        ),
        "path": "session",
        "limits": {"maxRecursiveContentDepth": 4},
        "profile": {
            "name": "budget-corpus-deep-nested-content-streams",
            "checks": [{"id": "color-inventory", "severity": "info"}],
        },
        "expected": {"kind": "recursive-content-depth", "pool": "document-model"},
    }
    return pdf, case


def build_long_running_render_work() -> tuple[bytes, dict]:
    """A content stream with far more operator/operand tokens than the tightened cap."""

    content = b"0 0 1 1 re f\n" * 40  # 5 tokens per repeat = 200 tokens
    bodies = build_page_document([content])
    pdf = assemble_pdf(bodies)
    case = {
        "id": "long-running-render-work",
        "description": (
            "A page content stream with 200 operator/operand tokens -- a stand-in "
            "for a pathologically operation-heavy page that would otherwise take "
            "an unbounded amount of processing to finish rendering."
        ),
        "path": "session",
        "limits": {"maxRenderOperations": 40},
        "profile": {
            "name": "budget-corpus-long-running-render-work",
            "checks": [{"id": "color-inventory", "severity": "info"}],
        },
        "expected": {"kind": "render-operations", "pool": "raster-tile"},
    }
    return pdf, case


def build_raster_probe_pixel_budget() -> tuple[bytes, dict]:
    """A page whose declared extent forces an oversized raster probe."""

    media_box = (0, 0, 4000, 4000)
    content = b"1 0 0 rg 0 0 4000 4000 re f\n"
    bodies = build_page_document([content], media_box=media_box)
    pdf = assemble_pdf(bodies)
    case = {
        "id": "raster-probe-pixel-budget",
        "description": (
            "A page with a 4000x4000pt declared MediaBox and a single fill "
            "spanning it; the 'thin-parts' check's raster probe is configured "
            "with an unreachably small pixel budget."
        ),
        "path": "session",
        "limits": {},
        "profile": {
            "name": "budget-corpus-raster-probe-pixel-budget",
            "checks": [
                {
                    "id": "thin-parts",
                    "severity": "info",
                    "min_effective_width_pt": 0.25,
                    "classes": ["thin-fill"],
                    "probe_dpi": 150,
                    "max_raster_pixels": 4,
                }
            ],
        },
        "expected": {"kind": "render-pixels", "pool": "raster-tile"},
    }
    return pdf, case


def build_deep_recursive_object_graph() -> tuple[bytes, dict]:
    """A minimal document plus one object holding a deeply nested array literal."""

    bodies = build_page_document([b"0 0 0 rg 0 0 1 1 re f\n"])
    extra_object = max(bodies) + 1
    depth = 40
    nested_array = (b"[" * depth) + b"0" + (b"]" * depth)
    bodies[extra_object] = nested_array
    pdf = assemble_pdf(bodies)
    case = {
        "id": "deep-recursive-object-graph",
        "description": (
            "An otherwise-ordinary document with one extra indirect object whose "
            "value is a %d-level nested array literal, unreachable from the "
            "catalog -- every occupied xref entry is still parsed." % depth
        ),
        "path": "reader",
        "limits": {"maxObjectDepth": 20},
        "expected": {"kind": "object-depth", "pool": "document-model"},
    }
    return pdf, case


def build_pathological_object_count() -> tuple[bytes, dict]:
    """A minimal document plus hundreds of trivial extra indirect objects."""

    bodies = build_page_document([b"0 0 0 rg 0 0 1 1 re f\n"])
    next_object = max(bodies) + 1
    extra_object_count = 120
    for offset in range(extra_object_count):
        bodies[next_object + offset] = b"null"
    pdf = assemble_pdf(bodies)
    case = {
        "id": "pathological-object-count",
        "description": (
            "An otherwise-ordinary document plus %d trivial extra indirect "
            "objects (unreachable from the catalog) driving the document's "
            "total visited-object count past a tightened cap." % extra_object_count
        ),
        "path": "reader",
        "limits": {"maxObjectsVisited": 60},
        "expected": {"kind": "objects-visited", "pool": "document-model"},
    }
    return pdf, case


BUILDERS = (
    build_decompression_bomb,
    build_cumulative_decoded_bytes,
    build_deep_nested_content_streams,
    build_long_running_render_work,
    build_raster_probe_pixel_budget,
    build_deep_recursive_object_graph,
    build_pathological_object_count,
)


def generate_corpus(output_dir: Path) -> dict:
    output_dir.mkdir(parents=True, exist_ok=True)
    cases = []
    for builder in BUILDERS:
        pdf_bytes, case = builder()
        filename = case["id"].replace("-", "_") + ".pdf"
        (output_dir / filename).write_bytes(pdf_bytes)
        case = dict(case)
        case["pdf"] = filename
        case["sha256"] = _sha256(pdf_bytes)
        cases.append(case)

    manifest = {"schema_version": SCHEMA_VERSION, "cases": cases}
    manifest_path = output_dir / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=Path("UnitTests/testdata/budget_exhaustion"))
    args = parser.parse_args()
    manifest = generate_corpus(args.output_dir)
    print(json.dumps({"cases": [case["id"] for case in manifest["cases"]]}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
