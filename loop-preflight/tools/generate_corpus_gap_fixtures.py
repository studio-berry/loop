#!/usr/bin/env python3
"""Generate golden-corpus fixtures that close the #668 corpus gaps.

Each fixture is minimal and isolates one check under a test-only profile.
PDFs are synthetic (no client/job files). Requires pikepdf.
"""

from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path

import pikepdf
from pikepdf import Dictionary, Name, Array, Stream


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT = ROOT / "testdata" / "fixtures"
FONT_EMBEDDED = DEFAULT_OUT / "font-embedded.pdf"
THIN_STROKES = DEFAULT_OUT / "thin-strokes-hairline.pdf"


def _save(pdf: pikepdf.Pdf, path: Path) -> None:
    if "/Metadata" in pdf.Root:
        del pdf.Root["/Metadata"]
    pdf.docinfo[Name("/Title")] = f"Loop corpus gap fixture - {path.stem}"
    pdf.docinfo[Name("/Producer")] = "Loop generate_corpus_gap_fixtures.py"
    pdf.save(path, deterministic_id=True, fix_metadata_version=True)
    print(f"wrote {path.name}")


def empty_page(out: Path, name: str) -> None:
    pdf = pikepdf.Pdf.new()
    pdf.add_blank_page(page_size=(200, 200))
    _save(pdf, out / f"{name}.pdf")


def off_page_content(out: Path) -> None:
    pdf = pikepdf.Pdf.new()
    page = pdf.add_blank_page(page_size=(200, 200))
    content = b"q 0 0 0 rg 300 300 20 20 re f Q\n"
    page.Contents = Stream(pdf, content)
    _save(pdf, out / "off-page-content.pdf")


def invisible_content(out: Path) -> None:
    pdf = pikepdf.Pdf.new()
    page = pdf.add_blank_page(page_size=(200, 200))
    # Zero fill alpha paints a mark the invisible-content check reports.
    content = b"q /GS0 gs 0 0 0 rg 40 40 40 40 re f Q\n"
    page.Resources = Dictionary(
        ExtGState=Dictionary(GS0=Dictionary(Type=Name("/ExtGState"), ca=0.0, CA=0.0))
    )
    page.Contents = Stream(pdf, content)
    _save(pdf, out / "invisible-content.pdf")


def obscured_content(out: Path) -> None:
    pdf = pikepdf.Pdf.new()
    page = pdf.add_blank_page(page_size=(200, 200))
    # Small opaque mark, then a larger opaque mark that fully covers it.
    content = (
        b"q 0 0 0 rg 50 50 20 20 re f "
        b"10 10 100 100 re f Q\n"
    )
    page.Contents = Stream(pdf, content)
    _save(pdf, out / "obscured-content.pdf")


def hidden_layers(out: Path) -> None:
    pdf = pikepdf.Pdf.new()
    page = pdf.add_blank_page(page_size=(200, 200))
    ocg = pdf.make_indirect(Dictionary(Type=Name("/OCG"), Name="HiddenMarks"))
    pdf.Root.OCProperties = Dictionary(
        OCGs=Array([ocg]),
        D=Dictionary(Order=Array([ocg]), OFF=Array([ocg]), BaseState=Name("/ON")),
    )
    page.Resources = Dictionary(Properties=Dictionary(Hidden=ocg))
    content = b"/OC /Hidden BDC q 0 0 0 rg 40 40 40 40 re f Q EMC\n"
    page.Contents = Stream(pdf, content)
    _save(pdf, out / "hidden-layers.pdf")


def font_integrity_corrupt(out: Path) -> None:
    if not FONT_EMBEDDED.exists():
        raise SystemExit(f"missing base fixture {FONT_EMBEDDED}")
    shutil.copyfile(FONT_EMBEDDED, out / "font-integrity-corrupt.pdf")
    with pikepdf.open(out / "font-integrity-corrupt.pdf", allow_overwriting_input=True) as pdf:
        for page in pdf.pages:
            fonts = page.Resources.get("/Font")
            if fonts is None:
                continue
            for key in fonts.keys():
                font = fonts[key]
                descriptor = font.get("/FontDescriptor")
                if descriptor is None:
                    continue
                for stream_key in ("/FontFile2", "/FontFile3", "/FontFile"):
                    if stream_key in descriptor:
                        # Truncate the embedded program so inspectPDFFontIntegrity
                        # reports TruncatedProgram rather than a clean parse.
                        program = descriptor[stream_key]
                        data = program.read_bytes()[:16]
                        descriptor[stream_key] = Stream(pdf, data)
                        _save(pdf, out / "font-integrity-corrupt.pdf")
                        return
        raise SystemExit("font-embedded.pdf has no embedded FontFile* stream to corrupt")


def thin_parts_clear(out: Path) -> None:
    """Hairline stroke clearly below the thin-parts threshold (not near-threshold)."""
    if not THIN_STROKES.exists():
        raise SystemExit(f"missing base fixture {THIN_STROKES}")
    shutil.copyfile(THIN_STROKES, out / "thin-parts-clear.pdf")
    print("wrote thin-parts-clear.pdf (from thin-strokes-hairline.pdf)")


def blank_page(out: Path) -> None:
    empty_page(out, "blank-page")


def conformance_pdfx5(out: Path) -> None:
    pdf = pikepdf.Pdf.new()
    pdf.add_blank_page(page_size=(200, 200))
    pdf.docinfo[Name("/GTS_PDFXVersion")] = "PDF/X-5n:2010"
    _save(pdf, out / "conformance-pdfx5n.pdf")


def main(argv: list[str]) -> int:
    out = Path(argv[1] if len(argv) > 1 else DEFAULT_OUT)
    out.mkdir(parents=True, exist_ok=True)
    off_page_content(out)
    invisible_content(out)
    obscured_content(out)
    hidden_layers(out)
    font_integrity_corrupt(out)
    thin_parts_clear(out)
    blank_page(out)
    conformance_pdfx5(out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
