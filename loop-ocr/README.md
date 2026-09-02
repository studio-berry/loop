# Loop OCR sidecar (EasyOCR)

> **Not a supported V1 feature.** V1 ships OCR as CLI-only, advanced / bring-your-own-sidecar
> (MIC-343): `PdfTool ocr` is present but inert until a sidecar is supplied via
> `LOOP_OCR_SIDECAR` or a dev launcher. Neither the Editor `OcrPlugin` UI nor this bundled
> `LoopOcrService` sidecar ship in a V1 release package — `LOOP_PLUGIN_OCR` and
> `LOOP_BUNDLE_OCR_SERVICE` are both off in release builds. OCR returns as a marketed,
> supported feature post-V1, after the AI Module. See [MIC-333](https://linear.app/mbx2/issue/MIC-333)
> for supply-chain guidance if you build and run this sidecar yourself.

Bundled Windows sidecar for read-only OCR. Invoked by `PdfTool ocr` via stdio JSON lines.

## Dev setup (Python 3.12 venv)

```powershell
cd loop-ocr
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
python tools\download_models.py
```

Run interactively:

```powershell
python service\main.py
```

For `PdfTool ocr` without `--sidecar`, resolution order is:

1. `LOOP_OCR_SIDECAR` environment variable
2. Bundled `LoopOcrService/LoopOcrService.exe` next to `PdfTool`
3. Dev launcher `loop-ocr/tools/dev_ocr_sidecar.cmd` (relative to a build-tree `PdfTool`)

```powershell
PdfTool ocr scan.pdf --console-format json --sidecar loop-ocr\tools\dev_ocr_sidecar.cmd
```

Send one JSON line per request on stdin, read one JSON line per response on stdout.

The sidecar protocol is strict. Requests require page >= 1, a non-empty image,
1 <= dpi <= 1200, a positive finite media_box, and rotation of 0, 90, 180, or
270. languages must be an array; values are trimmed, lower-cased, deduplicated,
and sorted. Successful responses require a boolean ok, the matching page,
string text, and an array of lines with finite confidence and non-negative
finite bounding-box dimensions. Invalid requests or OCR failures return
ok: false with an error string.

## Build PyInstaller bundle

```powershell
pip install pyinstaller
pyinstaller tools\pyinstaller.spec --distpath prebuilt --workpath build\pyinstaller
```

Output: `prebuilt/LoopOcrService/LoopOcrService.exe`

Enable CMake install with `-DLOOP_BUNDLE_OCR_SERVICE=ON` (copies `prebuilt/LoopOcrService` next to `PdfTool`).
The runtime sidecar uses a language-keyed EasyOCR reader cache and disables
model downloads. Run `tools/download_models.py` during the build or staging
step with network access to preload the selected models; production OCR then
fails explicitly if the requested model set is absent instead of downloading
at first use.

## Mock sidecar (tests / CI)

```powershell
python tools\mock_ocr_sidecar.py
```

Or `tools\mock_ocr_sidecar.cmd` on Windows.

## IPC request (stdin)

```json
{"image": "C:/temp/page-1.png", "page": 1, "dpi": 300, "languages": ["en"], "media_box": {"x": 0, "y": 0, "width": 612, "height": 792}, "rotation": 0}
```

## IPC response (stdout)

```json
{"page": 1, "ok": true, "text": "Hello", "lines": [{"text": "Hello", "confidence": 0.95, "bbox": {"x": 72, "y": 700, "width": 40, "height": 12}}]}

Run python -m unittest discover loop-ocr/tests for the dependency-free
request, language, coordinate, rotation, and finite-number contract tests.
```
