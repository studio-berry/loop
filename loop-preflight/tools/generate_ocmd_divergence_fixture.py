"""Generate the ocmd-print-divergence regression fixture (issue #115).

Standard library only. Two optional-content groups carry opposite View and Print
usage states and the default configuration's /AS array applies both events:

  A  visible on screen, hidden in print
  B  hidden on screen, visible in print

Five membership dictionaries exercise every /P policy and a /VE expression. Two
govern Form XObjects painted with Do (/OC in the XObject dictionary), three
govern marked content (BDC). None of them is a plain group, and none of them is
hidden by the print activity the shared hidden-content scan used to evaluate in
a way it reported, so the scan passed this file clean although screen and print
show different content.

  M1  AllOn  [A]          Form XObject   screen ON,  print OFF
  M2  AnyOn  [B]          BDC            screen OFF, print ON
  M3  VE [Not A]          BDC            screen OFF, print ON
  M4  AllOff [A]          Form XObject   screen OFF, print ON
  M5  AnyOff [A]          BDC            screen OFF, print ON
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT = ROOT / "testdata" / "fixtures"


def build() -> bytes:
    def stream(dictionary: bytes, payload: bytes) -> bytes:
        return b"<< " + dictionary + b" /Length %d >>\nstream\n" % len(payload) + payload + b"\nendstream"

    def square(x: int, rgb: str) -> bytes:
        return b"q " + rgb.encode() + b" rg " + b"%d 20 30 30 re f Q\n" % x

    form = b"/Type /XObject /Subtype /Form /BBox [ 0 0 200 200 ] "
    content = (
        b"/Fm1 Do\n/Fm4 Do\n"
        + b"/OC /M2 BDC " + square(10, "0 0.6 0") + b"EMC\n"
        + b"/OC /M3 BDC " + square(50, "0 0 0.8") + b"EMC\n"
        + b"/OC /M5 BDC " + square(90, "0.8 0 0") + b"EMC\n"
    )
    objects: dict[int, bytes] = {
        1: b"<< /Type /Catalog /Pages 2 0 R /OCProperties << /OCGs [ 5 0 R 6 0 R ] /D << /BaseState /ON "
        b"/ON [ 5 0 R 6 0 R ] /AS [ << /Event /View /Category [ /View ] /OCGs [ 5 0 R 6 0 R ] >> "
        b"<< /Event /Print /Category [ /Print ] /OCGs [ 5 0 R 6 0 R ] >> ] >> >> >>",
        2: b"<< /Type /Pages /Kids [ 3 0 R ] /Count 1 >>",
        3: b"<< /Type /Page /Parent 2 0 R /MediaBox [ 0 0 200 200 ] /Contents 4 0 R /Resources << "
        b"/Properties << /M2 8 0 R /M3 9 0 R /M5 11 0 R >> /XObject << /Fm1 12 0 R /Fm4 13 0 R >> >> >>",
        4: stream(b"", content),
        5: b"<< /Type /OCG /Name (ScreenOnly) /Usage << /View << /ViewState /ON >> /Print << /PrintState /OFF >> >> >>",
        6: b"<< /Type /OCG /Name (PrintOnly) /Usage << /View << /ViewState /OFF >> /Print << /PrintState /ON >> >> >>",
        7: b"<< /Type /OCMD /OCGs [ 5 0 R ] /P /AllOn >>",
        8: b"<< /Type /OCMD /OCGs [ 6 0 R ] /P /AnyOn >>",
        9: b"<< /Type /OCMD /VE [ /Not 5 0 R ] >>",
        10: b"<< /Type /OCMD /OCGs [ 5 0 R ] /P /AllOff >>",
        11: b"<< /Type /OCMD /OCGs [ 5 0 R ] /P /AnyOff >>",
        12: stream(form + b"/OC 7 0 R", square(130, "0 0 0")),
        13: stream(form + b"/OC 10 0 R", square(170, "0.5 0.5 0.5")),
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
    buffer += b"trailer\n<< /Size %d /Root 1 0 R /ID [ <6f636d64646976657267> <6f636d64646976657267> ] >>\n" % size
    buffer += b"startxref\n%d\n%%%%EOF\n" % xref
    return bytes(buffer)


def main(argv: list[str]) -> int:
    out = Path(argv[1] if len(argv) > 1 else DEFAULT_OUT)
    out.mkdir(parents=True, exist_ok=True)
    target = out / "ocmd-print-divergence.pdf"
    target.write_bytes(build())
    print(f"wrote {target.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
