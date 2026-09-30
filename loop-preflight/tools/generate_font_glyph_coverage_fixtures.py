"""Generate the font-glyph-coverage regression fixture (issue #114).

Standard library only. The base is font-embedded.pdf, whose embedded TrueType
subset parses cleanly but has a (1, 0) cmap that stops at code 127. The page
content is rewritten to also show code 0xE9 from that font, which the program
has no glyph for. Every table still parses, so font-integrity passed it clean
before glyph coverage was audited.

The fixture carries the same embedded program in three shown-text locations: page
content, a Form XObject, and a Widget annotation appearance stream.
"""

from __future__ import annotations

import re
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT = ROOT / "testdata" / "fixtures"
BASE = DEFAULT_OUT / "font-embedded.pdf"

MISSING_CODE = b"\351"  # octal 0xE9, outside the subset's 0..127 cmap


def _objects(data: bytes) -> dict[int, bytes]:
    return {
        int(m.group(1)): m.group(2)
        for m in re.finditer(rb"(?:^|\n)(\d+) 0 obj\n(.*?)\nendobj", data, re.S)
    }


def _stream(dictionary: bytes, payload: bytes) -> bytes:
    body = zlib.compress(payload, 9)
    return b"<< " + dictionary + b" /Length %d /Filter /FlateDecode >>\nstream\n" % len(body) + body + b"\nendstream"


def build() -> bytes:
    objects = _objects(BASE.read_bytes())
    font_obj = objects[6]
    if b"/F2+0 7 0 R" not in font_obj:
        raise SystemExit("unexpected font-embedded.pdf layout")

    page_content = (
        b"1 0 0 1 0 0 cm 0 g\n"
        b"BT 1 0 0 1 40 150 Tm /F2+0 18 Tf (Frisket fixture: embedded font ) Tj (" + MISSING_CODE + b") Tj ET\n"
        b"/Fm0 Do\n"
    )
    form_content = b"BT 1 0 0 1 40 120 Tm /F2+0 18 Tf (" + MISSING_CODE + b") Tj ET\n"
    appearance = b"BT 1 0 0 1 2 2 Tm /F2+0 12 Tf (" + MISSING_CODE + b") Tj ET\n"
    resources = b"/Resources << /Font 6 0 R >>"

    page = objects[4].replace(
        b"/Resources << /Font 6 0 R /ProcSet",
        b"/Annots [ 12 0 R ] /Resources << /Font 6 0 R /XObject << /Fm0 11 0 R >> /ProcSet",
    )
    if page == objects[4]:
        raise SystemExit("could not attach form and annotation to page")

    out: dict[int, bytes] = dict(objects)
    out[4] = page
    out[5] = _stream(b"", page_content)
    out[11] = _stream(b"/Type /XObject /Subtype /Form /BBox [ 0 0 312 312 ] " + resources, form_content)
    out[12] = (
        b"<< /Type /Annot /Subtype /Widget /FT /Tx /T (glyph) /Rect [ 40 60 100 80 ] /F 4 "
        b"/AP << /N 13 0 R >> >>"
    )
    out[13] = _stream(b"/Type /XObject /Subtype /Form /BBox [ 0 0 60 20 ] " + resources, appearance)

    buffer = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets: dict[int, int] = {}
    for number in sorted(out):
        offsets[number] = len(buffer)
        buffer += b"%d 0 obj\n" % number + out[number] + b"\nendobj\n"
    xref = len(buffer)
    size = max(out) + 1
    buffer += b"xref\n0 %d\n0000000000 65535 f \n" % size
    for number in range(1, size):
        buffer += b"%010d 00000 n \n" % offsets[number]
    buffer += b"trailer\n<< /Size %d /Root 1 0 R /Info 2 0 R /ID [ <66676c7970686d6973736e67> <66676c7970686d6973736e67> ] >>\n" % size
    buffer += b"startxref\n%d\n%%%%EOF\n" % xref
    return bytes(buffer)


def main(argv: list[str]) -> int:
    out = Path(argv[1] if len(argv) > 1 else DEFAULT_OUT)
    out.mkdir(parents=True, exist_ok=True)
    target = out / "font-glyph-missing.pdf"
    target.write_bytes(build())
    print(f"wrote {target.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
