# Fuzz regression corpus

Pinned inputs that reproduce previously-fixed fuzz findings. Each seed is owned
by exactly one libFuzzer harness under `Fuzz/corpus/<harness>/` and listed in
`manifest.json` with a checksum, origin, issue reference, and expected outcome.

Hosted CI (`.github/workflows/fuzz.yml`), `scripts/fuzz-bughunt.sh`, and
`scripts/docker-fuzz-build-inner.sh` run every harness's regression corpus
(`-runs=0`) before time-bounded mutation fuzzing.

Do not add third-party malware samples. All seeds must be synthetic or
fuzzer-generated against Loop and redistributable under `LICENSE`.

## Layout

| Path | Harness |
|------|---------|
| `Fuzz/corpus/fuzz_pdf_parser/` | `fuzz_pdf_parser` |
| `Fuzz/corpus/fuzz_stream_filters/` | `fuzz_stream_filters` |
| `Fuzz/corpus/fuzz_content_stream/` | `fuzz_content_stream` |
| `Fuzz/corpus/fuzz_images/` | `fuzz_images` |

`scripts/ci/check_fuzz_corpus.py` enforces that every tracked seed has a
manifest entry and that checksums, harness ownership, and paths stay in sync.

## MIC-326 / R-003 (JBIG2, `fuzz_images`)

| File | Finding | Root cause | Fix |
|------|---------|------------|-----|
| `jbig2-composition-timeout.bin` | libFuzzer timeout (>1200s), later re-confirmed at >60s locally | `PDFJBIG2Decoder::processSymbolDictionary`'s height-class loop (`pdfjbig2decoder.cpp`) makes zero progress when a height class's first delta-width decodes as out-of-band: `NSYMSDECODED` stays unchanged, the outer `while` condition is still true, and the loop spins forever on the arithmetic decoder without ever allocating a bitmap. Invisible to the pre-existing allocation budget, which only charges on allocation. | Added `PDFJBIG2Decoder::accountDecodeWork(items, pixels)`, charged once per symbol-dictionary height class (and per decoded bitmap, symbol instance, halftone grid cell) against a tight, dedicated item budget (`JBIG2_MAX_TOTAL_DECODE_WORK_ITEMS`), independent of pixel count. |
| `jbig2-symbol-dict-zero-progress-timeout.bin` | libFuzzer timeout (>30s), found by local fuzzing while verifying the fix above | `PDFJBIG2Decoder::readBitmap` decodes a legal-size generic-region bitmap (bounded by the allocation budget) but arithmetic-decoding it pixel-by-pixel under ASan/UBSan takes far longer than allocating it — allocation was bounded, wall-clock time was not. | Same `accountDecodeWork` call now also charges decoded/composited *pixels* against a second, independent budget (`JBIG2_MAX_TOTAL_DECODE_WORK_PIXELS`) sized generously (4x the allocation cap) so it never rejects a legitimately large scan, while the item budget above is what actually bounds degenerate, near-zero-pixel loops. |

Both seeds now execute in well under 100ms (was: 1200s+ and 30s+
respectively). See `docs/V1_RELEASE_READINESS.md` §R-003 for the full
investigation writeup, including the corrected diagnosis after the first
committed fix (`8f174230`, which charged only composited pixels with a
1-pixel floor) turned out not to reach either of these code paths.

## MIC-326 / R-003 (Huffman, `fuzz_images`)

`jbig2_huffman_signed_overflow.bin` — UBSan signed-integer overflow in
`PDFJBIG2HuffmanDecoder::readSignedInteger`, fixed by `8f174230`
(range-checked 64-bit arithmetic via `checkHuffmanRange`). The original CI
run only uploaded the timeout artifact; the seed was not recovered. Re-add it
here if the original artifact is found.

## Adding a new crash

1. Reproduce on the owning harness.
2. Minimize:
   `"$bin" "$crash" -minimize_crash=1 -exact_artifact_path=min.bin`
3. Place under `Fuzz/corpus/<harness>/` with a kebab-case id (for example
   `my-crash.bin`).
4. Compute the digest: `sha256sum Fuzz/corpus/<harness>/my-crash.bin`
5. Append a row to `manifest.json` with `origin: fuzz-finding`,
   `minimized: true`, `expected: terminates-without-crash`, and the GitHub
   issue number.
6. Land the seed with the fix. CI regression (`-runs=0`) is the proof the
   crash no longer fires.
