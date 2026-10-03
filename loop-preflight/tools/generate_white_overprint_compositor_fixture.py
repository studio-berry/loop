"""Generate the compositor-authority regression fixtures (issue #119).

Standard library only.

white-overprint-image.pdf
    A cyan backdrop is painted, then a 4 x 4 DeviceCMYK image of pure paper
    white (0 0 0 0) is painted with fill overprint on and overprint mode 1.
    The page-view scan only inspects path paints, so it never sees the image;
    the overprint-accurate compositor does: under OPM 1 a zero colorant selects
    the backdrop, so the white image vanishes instead of knocking out the cyan.

transparency-overprint-knockout.pdf
    A knockout transparency group (CMYK blend space) paints black with overprint
    on, OPM 1 and fill alpha 0.5 over a cyan backdrop. It uses only the Normal
    blend mode in a CMYK group, so the blend-mode and blend-space scan has
    nothing to report, yet how overprint, alpha and knockout combine is decided
    by the compositor.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT = ROOT / "testdata" / "fixtures"


def assemble(objects: dict[int, bytes], ident: bytes) -> bytes:
    buffer = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets: dict[int, int] = {}
    for number in sorted(objects):
        offsets[number] = len(buffer)
        buffer += b"%d 0 obj\n" % number + objects[number] + b"\nendobj\n"
    xref = len(buffer)
    size = max(objects) + 1
    buffer += b"xref\n0 %d\n0000000000 65535 f \n" % size
    for number in range(1, size):
        buffer += b"%010d 00000 n \n" % offsets[number]
    buffer += b"trailer\n<< /Size %d /Root 1 0 R /ID [ <%s> <%s> ] >>\n" % (size, ident, ident)
    buffer += b"startxref\n%d\n%%%%EOF\n" % xref
    return bytes(buffer)


def build_image() -> bytes:
    pixels = b"00" * (4 * 4 * 4)  # 4 x 4 x CMYK, all zero, as ASCIIHex
    content = (
        b"1 0 0 0 k 10 10 80 80 re f\n"
        b"/GS0 gs\n"
        b"q 40 0 0 40 30 30 cm /Im0 Do Q\n"
    )
    image = (
        b"<< /Type /XObject /Subtype /Image /Width 4 /Height 4 /ColorSpace /DeviceCMYK "
        b"/BitsPerComponent 8 /Filter /ASCIIHexDecode /Length %d >>\nstream\n" % (len(pixels) + 1)
        + pixels + b">\nendstream"
    )
    return assemble(
        {
            1: b"<< /Type /Catalog /Pages 2 0 R >>",
            2: b"<< /Type /Pages /Kids [ 3 0 R ] /Count 1 >>",
            3: b"<< /Type /Page /Parent 2 0 R /MediaBox [ 0 0 100 100 ] /Contents 4 0 R "
            b"/Resources << /XObject << /Im0 5 0 R >> "
            b"/ExtGState << /GS0 << /op true /OP true /OPM 1 >> >> >> >>",
            4: b"<< /Length %d >>\nstream\n" % len(content) + content + b"endstream",
            5: image,
        },
        b"77686974656f7076",
    )


def build_knockout() -> bytes:
    form_content = b"/GS1 gs 0 0 0 1 k 30 30 40 40 re f\n"
    page_content = b"1 0 0 0 k 10 10 80 80 re f\n/Fm0 Do\n"
    form = (
        b"<< /Type /XObject /Subtype /Form /BBox [ 0 0 100 100 ] "
        b"/Group << /S /Transparency /CS /DeviceCMYK /K true >> "
        b"/Resources << /ExtGState << /GS1 << /op true /OP true /OPM 1 /ca 0.5 /CA 0.5 >> >> >> "
        b"/Length %d >>\nstream\n" % len(form_content) + form_content + b"endstream"
    )
    return assemble(
        {
            1: b"<< /Type /Catalog /Pages 2 0 R >>",
            2: b"<< /Type /Pages /Kids [ 3 0 R ] /Count 1 >>",
            3: b"<< /Type /Page /Parent 2 0 R /MediaBox [ 0 0 100 100 ] /Contents 4 0 R "
            b"/Resources << /XObject << /Fm0 5 0 R >> >> >>",
            4: b"<< /Length %d >>\nstream\n" % len(page_content) + page_content + b"endstream",
            5: form,
        },
        b"6b6e6f636b6f7574",
    )


def main(argv: list[str]) -> int:
    out = Path(argv[1] if len(argv) > 1 else DEFAULT_OUT)
    out.mkdir(parents=True, exist_ok=True)
    (out / "white-overprint-image.pdf").write_bytes(build_image())
    (out / "transparency-overprint-knockout.pdf").write_bytes(build_knockout())
    print("wrote white-overprint-image.pdf, transparency-overprint-knockout.pdf")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
