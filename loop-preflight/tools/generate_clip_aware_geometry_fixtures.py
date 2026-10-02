"""Generate the clip-aware geometry regression fixtures (issue #118).

Standard library only. Both pages are 200 x 200 pt.

off-page-content-clipped.pdf
    A 400 x 400 black square is painted under a clip on 300..400, wholly outside
    the page. The square raw bounds cover the page, so a bounds-only test
    sees a mark on the page, yet every painted pixel is off the page.

obscured-content-clipped.pdf
    Two marks that later opaque paint covers although no covering object's
    bounds contain the mark's raw bounds:
      A  a 200 x 200 square clipped to 40..60, so only a 20 x 20 patch paints;
         one opaque 50 x 50 rectangle covers that patch.
      C  a 20 x 20 square covered only by the union of two adjacent opaque
         rectangles; neither rectangle covers it alone.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT = ROOT / "testdata" / "fixtures"


def build(content: bytes, ident: bytes) -> bytes:
    objects = {
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        2: b"<< /Type /Pages /Kids [ 3 0 R ] /Count 1 >>",
        3: b"<< /Type /Page /Parent 2 0 R /MediaBox [ 0 0 200 200 ] /Contents 4 0 R /Resources << >> >>",
        4: b"<< /Length %d >>\nstream\n" % len(content) + content + b"endstream",
    }
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


OFF_PAGE = b"q 300 300 100 100 re W n 0 g 0 0 400 400 re f Q\n"

OBSCURED = (
    b"q 40 40 20 20 re W n 0 g 0 0 200 200 re f Q\n"
    b"0.5 g 30 30 50 50 re f\n"
    b"0 g 100 100 20 20 re f\n"
    b"0.5 g 95 95 15 30 re f 110 95 15 30 re f\n"
)


def main(argv: list[str]) -> int:
    out = Path(argv[1] if len(argv) > 1 else DEFAULT_OUT)
    out.mkdir(parents=True, exist_ok=True)
    (out / "off-page-content-clipped.pdf").write_bytes(build(OFF_PAGE, b"6f6666636c6970"))
    (out / "obscured-content-clipped.pdf").write_bytes(build(OBSCURED, b"6f627363636c6970"))
    print("wrote off-page-content-clipped.pdf, obscured-content-clipped.pdf")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
