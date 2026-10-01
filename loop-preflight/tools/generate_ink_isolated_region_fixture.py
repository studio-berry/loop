"""Generate the ink-coverage-isolated-region regression fixture (issue #116).

Standard library only. A single A3 page carries one 6 mm x 6 mm rich-black
square (C=M=Y=K=1, 400% total area coverage) on otherwise blank paper. The
square covers about 0.03% of the page, under the 0.05% page-percentage floor
the ink-coverage probe used to apply, so the check passed the page clean
although the element sits far above any total-area-coverage limit.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT = ROOT / "testdata" / "fixtures"

A3_WIDTH_PT = 841.89
A3_HEIGHT_PT = 1190.55
SQUARE_PT = 17.0  # about 6 mm


def build() -> bytes:
    content = (
        b"q 1 1 1 1 k %.2f %.2f %.2f %.2f re f Q\n"
        % (300.0, 500.0, SQUARE_PT, SQUARE_PT)
    )
    objects = {
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        2: b"<< /Type /Pages /Kids [ 3 0 R ] /Count 1 >>",
        3: b"<< /Type /Page /Parent 2 0 R /MediaBox [ 0 0 %.2f %.2f ] /Contents 4 0 R /Resources << >> >>"
        % (A3_WIDTH_PT, A3_HEIGHT_PT),
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
    buffer += b"trailer\n<< /Size %d /Root 1 0 R /ID [ <696e6b69736f6c61746564> <696e6b69736f6c61746564> ] >>\n" % size
    buffer += b"startxref\n%d\n%%%%EOF\n" % xref
    return bytes(buffer)


def main(argv: list[str]) -> int:
    out = Path(argv[1] if len(argv) > 1 else DEFAULT_OUT)
    out.mkdir(parents=True, exist_ok=True)
    target = out / "ink-coverage-isolated-region.pdf"
    target.write_bytes(build())
    print(f"wrote {target.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
