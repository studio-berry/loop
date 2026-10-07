"""Generate the ICCBased color-mode and invisible-content regression fixtures (issues #113, #117).

Standard library only. The ICC profile is read from UnitTests/testdata/synthetic-cmyk.icc,
whose header declares a CMYK color space.

color-icc-cmyk-profile.pdf
    Fills a rectangle in an ICCBased space whose profile and /N are CMYK.

color-icc-alternate-conflict.pdf
    Fills a rectangle in an ICCBased space with the same CMYK profile but /N 3,
    so the derived /Alternate is DeviceRGB and disagrees with the profile header.

invisible-content-zero-font-size.pdf
    Shows text at font size 0.

invisible-content-empty-clip.pdf
    Paints a filled rectangle under a clipping path of zero height.

invisible-content-outside-clip.pdf
    Paints a filled rectangle entirely outside the active clipping path.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
DEFAULT_OUT = ROOT / "testdata" / "fixtures"
PROFILE = REPO / "UnitTests" / "testdata" / "synthetic-cmyk.icc"

PAGE = 200
FONT = b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"


def assemble(content: bytes, resources: bytes, extra: dict[int, bytes], ident: bytes) -> bytes:
    objects = {
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        2: b"<< /Type /Pages /Kids [ 3 0 R ] /Count 1 >>",
        3: b"<< /Type /Page /Parent 2 0 R /MediaBox [ 0 0 %d %d ] /Contents 4 0 R /Resources %s >>" % (PAGE, PAGE, resources),
        4: b"<< /Length %d >>\nstream\n" % len(content) + content + b"endstream",
    }
    objects.update(extra)
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


def build_icc(components: int, ident: bytes) -> bytes:
    profile = PROFILE.read_bytes()
    stream = b"<< /N %d /Length %d >>\nstream\n" % (components, len(profile)) + profile + b"\nendstream"
    resources = b"<< /ColorSpace << /CS0 [ /ICCBased 5 0 R ] >> >>"
    content = b"/CS0 cs 0.2 0.4 0.6 0.1 sc 20 20 160 160 re f\n" if components == 4 else b"/CS0 cs 0.2 0.4 0.6 sc 20 20 160 160 re f\n"
    return assemble(content, resources, {5: stream}, ident)


def build_text(operators: bytes, ident: bytes) -> bytes:
    content = b"BT /F1 %b ET\n" % operators
    return assemble(content, b"<< /Font << /F1 5 0 R >> >>", {5: FONT}, ident)


def build_empty_clip() -> bytes:
    content = b"20 20 160 0 re W n\n1 0 0 rg 20 20 160 160 re f\n"
    return assemble(content, b"<< >>", {}, b"656d7074792d636c")


def build_outside_clip() -> bytes:
    content = b"150 150 30 30 re W n\n1 0 0 rg 20 20 60 60 re f\n"
    return assemble(content, b"<< >>", {}, b"6f75742d636c6970")


def main(argv: list[str]) -> int:
    out = Path(argv[1] if len(argv) > 1 else DEFAULT_OUT)
    out.mkdir(parents=True, exist_ok=True)
    files = {
        "color-icc-cmyk-profile.pdf": build_icc(4, b"6963632d636d796b"),
        "color-icc-alternate-conflict.pdf": build_icc(3, b"6963632d636f6e66"),
        "invisible-content-zero-font-size.pdf": build_text(b"0 Tf 20 100 Td (Hidden) Tj", b"7a65726f2d73697a"),
        "invisible-content-empty-clip.pdf": build_empty_clip(),
        "invisible-content-outside-clip.pdf": build_outside_clip(),
    }
    for name, data in files.items():
        (out / name).write_bytes(data)
    print("wrote " + ", ".join(files))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
