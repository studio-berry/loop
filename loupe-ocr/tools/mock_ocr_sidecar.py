"""Mock OCR sidecar for unit tests and CI (no EasyOCR / PyTorch)."""

from __future__ import annotations

import json
import os
import sys
import time


def handle_request(request: dict) -> dict:
    page = int(request.get("page", 0))
    image_path = request.get("image", "")
    if not image_path:
        return {"page": page, "ok": False, "error": "missing image path"}

    media_box = request.get("media_box") or {"x": 0.0, "y": 0.0, "width": 612.0, "height": 792.0}
    x = float(media_box.get("x", 72.0))
    y = float(media_box.get("y", 700.0))
    return {
        "page": page,
        "ok": True,
        "text": "MOCK OCR TEXT",
        "lines": [
            {
                "text": "MOCK OCR TEXT",
                "confidence": 0.99,
                "bbox": {"x": x, "y": y, "width": 120.0, "height": 14.0},
            }
        ],
    }


def write_response(response: dict) -> None:
    sys.stdout.write(json.dumps(response, separators=(",", ":"), allow_nan=False) + "\n")
    sys.stdout.flush()


def main() -> int:
    mode = os.environ.get("LOUPE_OCR_MOCK_MODE", "normal")
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            request = json.loads(line)
        except json.JSONDecodeError as exc:
            response = {"ok": False, "error": f"invalid json: {exc}"}
        elif not isinstance(request, dict):
            response = {"ok": False, "error": "request must be a JSON object"}
        else:
            if mode == "stderr-noise":
                sys.stderr.write("mock sidecar diagnostic\n")
                sys.stderr.flush()
                response = handle_request(request)
            elif mode == "malformed-json":
                sys.stdout.write("{not valid json}\n")
                sys.stdout.flush()
                continue
            elif mode == "wrong-page":
                response = handle_request(request)
                response["page"] = response.get("page", 0) + 1
            elif mode == "missing-lines":
                response = handle_request(request)
                response.pop("lines", None)
            elif mode == "explicit-error":
                page = request.get("page", 0)
                response = {"page": page, "ok": False, "error": "mock OCR failure"}
            elif mode == "hang":
                time.sleep(180)
                continue
            elif mode == "crash":
                return 7
            else:
                response = handle_request(request)
        write_response(response)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
