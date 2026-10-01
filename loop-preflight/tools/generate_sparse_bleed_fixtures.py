"""Generate the sparse-mark bleed regression fixtures (issue #120).

Standard library only.

Both pages are 216 x 216 pt with a 180 x 180 pt TrimBox at 18 pt and a BleedBox
9 pt outside it, painted with cyan artwork over the trim area only. The 9 pt
bleed margin holds nothing but sparse marks.

content-bleed-sparse-marks.pdf
    One black 1 x 1 pt dot in the middle of each bleed strip. The union of the
    artwork bounds touches every strip, so a bounds-only probe reports all four
    edges as covered although the margin is effectively empty.

content-bleed-hairline-margin.pdf
    A 0.1 pt black diagonal hairline crosses each bleed strip corner to corner.
    Its bounding box fills the strip, so even a coverage estimate taken from
    the bounds sees a populated margin, but it inks well under a tenth of the
    strip's pixels at 150 dpi.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT = ROOT / "testdata" / "fixtures"

MEDIA = 216
TRIM_LO, TRIM_HI = 18, 198
BLEED_LO, BLEED_HI = 9, 207
BOXES = (
    b"/MediaBox [ 0 0 %d %d ] /TrimBox [ %d %d %d %d ] /BleedBox [ %d %d %d %d ]"
    % (MEDIA, MEDIA, TRIM_LO, TRIM_LO, TRIM_HI, TRIM_HI, BLEED_LO, BLEED_LO, BLEED_HI, BLEED_HI)
)


def assemble(content: bytes, ident: bytes) -> bytes:
    objects = {
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        2: b"<< /Type /Pages /Kids [ 3 0 R ] /Count 1 >>",
        3: b"<< /Type /Page /Parent 2 0 R " + BOXES + b" /Contents 4 0 R /Resources << >> >>",
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


ARTWORK = b"1 0 0 0 k %d %d %d %d re f\n" % (TRIM_LO, TRIM_LO, TRIM_HI - TRIM_LO, TRIM_HI - TRIM_LO)


def build_sparse_marks() -> bytes:
    mid = MEDIA // 2
    dots = b"0 0 0 1 k\n"
    for x, y in ((12, mid), (203, mid), (mid, 12), (mid, 203)):
        dots += b"%d %d 1 1 re f\n" % (x, y)
    return assemble(ARTWORK + dots, b"7370617273656d6b")


def build_hairline_margin() -> bytes:
    lines = b"0 0 0 1 K 0.1 w\n"
    lines += b"%d %d m %d %d l S\n" % (BLEED_LO, TRIM_LO, TRIM_LO, TRIM_HI)
    lines += b"%d %d m %d %d l S\n" % (TRIM_HI, TRIM_LO, BLEED_HI, TRIM_HI)
    lines += b"%d %d m %d %d l S\n" % (TRIM_LO, BLEED_LO, TRIM_HI, TRIM_LO)
    lines += b"%d %d m %d %d l S\n" % (TRIM_LO, TRIM_HI, TRIM_HI, BLEED_HI)
    return assemble(ARTWORK + lines, b"68616972206c696e")


def main(argv: list[str]) -> int:
    out = Path(argv[1] if len(argv) > 1 else DEFAULT_OUT)
    out.mkdir(parents=True, exist_ok=True)
    (out / "content-bleed-sparse-marks.pdf").write_bytes(build_sparse_marks())
    (out / "content-bleed-hairline-margin.pdf").write_bytes(build_hairline_margin())
    print("wrote content-bleed-sparse-marks.pdf, content-bleed-hairline-margin.pdf")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
