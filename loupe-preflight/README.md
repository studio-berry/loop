# loupe-preflight

Standalone preflight **engine** for Loupe (sidecar CLI). Lives outside the PDF4QT / Loupe Editor process. The Qt plugin (Phase 2) shells out to this tool and only consumes the normalized report JSON defined here.

This directory currently locks the **contract** (MIC-131). The CLI binary itself is MIC-133.

## Layout

| Path | Purpose |
|------|---------|
| `schemas/profile.schema.json` | JSON Schema for declarative profiles (YAML or JSON) |
| `schemas/report.schema.json` | JSON Schema for stdout report JSON (`scope` + conditional `page`/`bbox` in v2) |
| `profiles/loupe-default.yaml` | Bundled **Loupe Default** profile (Venue Poster / Handbill / Signage) |
| `examples/report.example.json` | Example failing report matching the report schema |

## Profile (YAML/JSON)

Required:

- `name` — human-readable; copied into `report.profile`
- `checks[]` — each entry has `id`, optional `severity`, and check-specific params

Optional:

- `schema_version` — currently `1`
- `job_types[]` — Loupe job categories
- `description`
- `fixups[]` — each has `id`, `confirm` (default true), and params

Check params used by Phase 1 plans (open-ended via `additionalProperties`):

| Param | Used by |
|-------|---------|
| `min_dpi` | `image-resolution` |
| `amount_pt` | `bleed`, `add-bleed` |
| `required` | `bleed` |
| `allowed` | `color-mode` |
| `expected_width_pt` / `expected_height_pt` / `tolerance_pt` | `page-size`, `trim` |
| `raster_confirm` | `content-bleed` |
| `probe_dpi` | `content-bleed` |
| `probe_threshold` | `content-bleed` |
| `raster_white_threshold` | `content-bleed` |

The `transparency-risk` check has no additional parameters; it observes
transparency groups, blend modes, and blend-space crossings.

## Report JSON

Required top-level fields: `pass`, `profile`, `errors`, `warnings`, `fixups_available`.

`schema_version` is currently **2**. Version 1 required `page` and `bbox` on every finding; the plugin still accepts v1 reports for backward compatibility.

Every finding in `errors[]` / `warnings[]` **must** include:

| Field | Notes |
|-------|-------|
| `scope` | `document` \| `page` \| `object` (v2) |
| `type` | kebab-case machine id |
| `severity` | `error` \| `warning` \| `info` |
| `message` | human text for the dock panel |
| `page` | 1-based; required for `page` and `object` scope, absent for `document` |
| `bbox` | optional `[x0, y0, x1, y1]` in PDF user space (points), media-box lower-left origin; include only when a meaningful region exists |

`bbox` coordinates use PDF user space (points) with the page MediaBox lower-left as origin. Page-level issues without a specific region omit `bbox`. Regional/object issues include `bbox` when known.

`object_id` is optional (string or null) and never substitutes for `scope`. `fixups_available[]` entries need `id`, `safe`, `description`.

## Two-tier bleed checking

Loupe supports two tiers of bleed validation:

| Tier | Check ID | What it checks | Cost |
|------|----------|---------------|------|
| 1 | `bleed` | Box dimensions (BleedBox extends beyond TrimBox) | Free (page dictionary read) |
| 2 | `content-bleed` | Actual artwork extends into the bleed margin | Rasterization (opt-in, profile-driven) |
| 3 | `ink-coverage` | Per-pixel total ink coverage exceeds a press limit | Full-page rasterization (opt-in, profile-driven) |

Tier-2 runs only when Tier-1 passes (boxes are adequate) and the profile includes a
`content-bleed` check. It uses a fast vector-content bounds pass first. When
`raster_confirm: true`, pages flagged by the fast pass undergo a focused strip-raster
confirmation — no full-page render.

The default `loupe-default` profile does **not** enable Tier-2. Use a separate
profile to opt in (see `examples/profile-tiered-bleed.json`).

When a document fails either bleed tier, the engine dynamically surfaces the
`add-bleed` fixup in `fixups_available` and emits a `needs-auto-bleed` advisory
finding per affected page.

With `raster_confirm: true`, Tier-2 emits `bleed-margin-empty` (one finding per
empty edge) instead of a single aggregate `content-bleed` finding.

## Total ink coverage checking

The opt-in `ink-coverage` check rasterizes each page through the same transparency
renderer used by Output Preview, then reports one object-scope finding for each
connected region whose total ink coverage exceeds `max_ink_pct`. `max_ink_pct`
is expressed as a percentage of summed colorant values, so `300` means 300% TAC.
Optional parameters are `probe_dpi` (default 150), `min_region_area_pct`
(default 0.05% of the page), and `max_regions_per_page` (default 20). Spot inks
are included in TAC because the probe activates the ink mapper's spot colors.

This check is deliberately not enabled by `loupe-default.json`: it requires a
full-page rasterization and is intended for profiles that explicitly opt in.
Pages exceeding the raster pixel budget emit an informational page-scope finding
instead of silently passing.

## Transparency risk checking

The `transparency-risk` check observes actual page-content processing and emits
page-scoped `transparency-blend-mode` and `transparency-blend-space` findings.
Ordinary `Normal` transparency is legitimate and does not trigger a finding.
The check flags risky non-separable or unsupported blend modes at transparency
group boundaries and detects RGB/CMYK, process/Spot, and Gray-family crossings
using the group's effective blend space. Nested groups are evaluated at their
real compositing boundaries, including Forms, tiling, shadings, images, text,
and annotation appearance streams. These are appearance/production-risk
advisories, not claims that the PDF is invalid.

## Hairline and thin-stroke checking

The opt-in `thin-strokes` check observes actual stroking operations after the
graphics-state CTM has been applied. A declared width at or below
`zero_width_epsilon_pt` (default `0.000001`) emits a `hairline-stroke` finding;
other strokes emit `thin-stroke` when their minimum effective painted width is
below `min_effective_width_pt`. Effective width accounts for non-uniform scale,
rotation, shear, nested Forms, and page `/UserUnit`. `hairline_severity` and
`thin_stroke_severity` override the check's normal severity and fall back to it
when omitted. The default profile does not enable this check because the
production threshold is job-specific.

## Intended CLI (MIC-133)

```bash
# Default profile (Tier-1 bleed only)
PdfTool preflight document.pdf --profile loupe-preflight/profiles/loupe-default.json

# Tiered bleed profile (Tier-1 + Tier-2 content-bleed, fast bounds only)
PdfTool preflight document.pdf --profile loupe-preflight/examples/profile-tiered-bleed.json

# Tiered bleed with strip raster confirmation (bleed-margin-empty findings)
PdfTool preflight document.pdf --profile loupe-preflight/examples/profile-tiered-bleed-raster.json
```

- Exit `0` when `pass` is true; exit `1` when `errors[]` is non-empty.
- stdout: single JSON document validating against `schemas/report.schema.json`.
- Profiles: **JSON** at runtime today (`loupe-default.json` mirrors the YAML). YAML authoring is fine; convert or add a loader later.
- Implemented checks in `PreflightEngine`: **bleed**, **trim**, **page-size** (page boxes), **content-bleed** (tiered artwork bleed, optional `raster_confirm`), **ink-coverage** (opt-in TAC raster probe), **transparency-risk** (transparency blend-mode and blend-space risk), **thin-strokes** (opt-in hairline and effective-width detection), **color-mode**, **color-inventory**, **image-resolution**, **embedded-fonts**, **white-overprint**, and **output-intent**. `trim` and `page-size` are **job-spec dependent** — each is skipped unless its profile check entry supplies both `expected_width_pt` and `expected_height_pt` (compared strictly, orientation-sensitive, within `tolerance_pt`). The generic `loupe-default.json` leaves them unset, so those two checks are no-ops there until a job-specific profile sets a size. `output-intent` inspects catalog-level `/OutputIntents`; page-level output intents are not currently covered.

Other PdfTool commands accept `--console-format json` via `PDFOutputFormatter` (tree JSON, not the preflight report schema).

## ICC-managed RGB-to-CMYK fixup

The shared Core fixup is available to applications as `PDFRgbToCmykFixup` and
to headless workflows as:

```bash
PdfTool rgb-to-cmyk input.pdf \
  --target-profile GRACoL2013_CRPC6.icc \
  --intent relative \
  --dry-run \
  --report \
  --console-format json
```

The target profile is mandatory and must be a valid CMYK ICC profile. DeviceRGB
vector paints are transformed through the existing LittleCMS implementation;
unsupported RGB images or extended color constructs fail closed and are listed
in the report. A successful non-dry-run embeds the selected profile as the
document OutputIntent and performs a candidate postflight before replacing the
source document.

## Versioning

Bump `schema_version` only together with engine + plugin releases so the JSON contract stays in sync (see [docs/PACKAGING_LICENSING.md](../docs/PACKAGING_LICENSING.md) and the hybrid sidecar plan in Linear).

## Golden corpus & CI (MIC-132)

A golden corpus of fixture PDFs is run against `PdfTool preflight` in CI via a QtTest
executable, `UnitTestsPreflightCorpus` (`UnitTests/tst_preflightcorpus.cpp`), registered
with `ctest` in `UnitTests/CMakeLists.txt`. `.github/workflows/ci.yml` runs
`ctest --output-on-failure` after the build, so a regression in any check's pass/fail
outcome or report output fails the build.

### Layout

| Path | Purpose |
|------|---------|
| `testdata/manifest.schema.json` | JSON Schema for corpus manifest entries |
| `testdata/fixtures/manifest.json` | Corpus manifest: fixture → profile → expected outcome |
| `testdata/fixtures/*.pdf` | Fixture PDFs (hand-built or generated; **not** client/job PDFs) |
| `testdata/snapshots/<id>.json` | Golden report JSON per fixture, normalized (`engine_version`/`pdf` stripped) |
| `testdata/profiles/*.json` | Test-only profiles referenced by fixtures (e.g. `test-trim-pagesize.json`) |
| `tools/generate_fixtures.cpp` | Deterministically (re)generates the `bleed-*` fixtures via `pdf::PDFDocumentBuilder` |
| `tools/generate_fixtures.py` | Deterministically (re)generates the MIC-145 custom-check fixtures via reportlab/Pillow/pikepdf |

### Manifest entries

```json
{
  "id": "bleed-adequate",
  "pdf": "bleed-adequate.pdf",
  "profile": "profiles/loupe-default.json",
  "expect": { "pass": true, "check_ids": [] },
  "source": "generated",
  "notes": "short description of what the fixture exercises"
}
```

`expect.check_ids` lists the `check_id`s that must appear in `errors[]`/`warnings[]`; leave
empty for a clean pass. `UnitTestsPreflightCorpus` reads this manifest at test time — no
code changes are needed to add a fixture, only a new PDF + manifest entry (+ an initial
snapshot, below).

An optional `"pending": true` marks a fixture whose target check the engine does not implement
yet: the runner skips it and `expect{}` records the intended future outcome (see "Hand-built
custom-check fixtures" below).

### Adding a fixture

1. Get a PDF: either extend `tools/generate_fixtures.cpp` (preferred for parametric cases
   like bleed/trim/DPI amounts) or hand-build one (e.g. for font-embedding or color-mode
   cases that need real content). Put it in `testdata/fixtures/`. No client/job PDFs.
2. Add a `manifest.json` entry with the expected `pass` and `check_ids`.
3. Create its snapshot once locally: `LOUPE_UPDATE_SNAPSHOTS=1 ctest -R UnitTestsPreflightCorpus`,
   then review and commit the new `testdata/snapshots/<id>.json`.

Until a fixture's PDF exists on disk, `UnitTestsPreflightCorpus` skips (not fails) its rows,
so an in-progress manifest entry doesn't redden CI.

### Regenerating the seeded fixtures

```bash
cmake --build build --target LoupeGenerateFixtures
./build/usr/bin/LoupeGenerateFixtures loupe-preflight/testdata/fixtures
LOUPE_UPDATE_SNAPSHOTS=1 ctest --test-dir build -R UnitTestsPreflightCorpus
```

Review the resulting diff before committing — a snapshot change should always be traceable
to an intentional check-behavior change, not an accident.

### Snapshot vs. manifest checks

The manifest check (`preflightMatchesManifest`) is the correctness gate: does this fixture
still pass/fail the way it's supposed to. The snapshot check (`preflightMatchesSnapshot`) is
the regression gate: did anything about the report's *content* (message text, bbox, severity,
finding order) change, even if pass/fail didn't. Both run for every corpus entry.

### Hand-built custom-check fixtures (MIC-145)

Public corpora (veraPDF, Isartor, GWG — MIC-146) cover standards-backed checks but not
Loupe Default's custom rules. The custom-check fixtures live in this same corpus. Each is
minimal and **isolates a single check**: it is built so every *other* check in its profile
passes, and only the target check is exercised.

| Fixture | Profile | Expect | Check(s) |
|---------|---------|--------|----------|
| `color-rgb.pdf` | loupe-default | fail | `color-mode` (DeviceRGB image) |
| `color-cmyk.pdf` | loupe-default | pass | `color-mode` (DeviceCMYK image) |
| `image-dpi-low.pdf` | loupe-default | warning | `image-resolution` (~25 DPI) |
| `image-dpi-ok.pdf` | loupe-default | pass | `image-resolution` (~310 DPI) |
| `font-not-embedded.pdf` | loupe-default | fail | `embedded-fonts` (Helvetica, no FontFile) |
| `font-embedded.pdf` | loupe-default | pass | `embedded-fonts` (`/FontFile2` subset) |
| `trim-pagesize-mismatch.pdf` | test-trim-pagesize | fail | `trim`, `page-size` (540×720 vs 612×792) |
| `trim-pagesize-ok.pdf` | test-trim-pagesize | pass | `trim`, `page-size` (612×792) |
| `content-bleed-adequate.pdf` | tiered-bleed | pass | `content-bleed` (artwork extends into bleed) |
| `content-bleed-missing.pdf` | tiered-bleed | fail (warnings) | `content-bleed`, `needs-auto-bleed` (artwork stops at trim) |
| `content-bleed-raster-confirm.pdf` | tiered-bleed-raster | fail (warnings) | `bleed-margin-empty`, `needs-auto-bleed` (raster-confirmed empty margins) |
| `content-bleed-three-of-four.pdf` | tiered-bleed | fail (warnings) | `content-bleed` (one empty edge only) |
| `ink-coverage-over.pdf` | test-ink-coverage | warning | `ink-coverage` (over-limit TAC region) |
| `ink-coverage-ok.pdf` | test-ink-coverage | pass | clean TAC below the threshold |
| `transparency-normal-cmyk.pdf` | test-transparency-risk | pass | matching CMYK group/content is clean |
| `transparency-hue.pdf` | test-transparency-risk | warning | `transparency-blend-mode` |
| `transparency-rgb-group-cmyk.pdf` | test-transparency-risk | warning | `transparency-blend-space` |
| `transparency-cmyk-group-rgb.pdf` | test-transparency-risk | warning | `transparency-blend-space` |
| `transparency-cmyk-group-spot.pdf` | test-transparency-risk | warning | `transparency-blend-space` |
| `transparency-annotation-appearance.pdf` | test-transparency-risk | warning | annotation `/AP` `transparency-blend-space` |

The `bleed-*` pair above covers the `bleed` check, so every Loupe Default custom check has
at least one known-pass and one known-fail (or warning) case.

**How they were built.** `tools/generate_fixtures.py` builds them deterministically with
[`reportlab`](https://pypi.org/project/reportlab/) (pages, text, embedded fonts, raster
images), [`Pillow`](https://pypi.org/project/pillow/) (DeviceRGB / DeviceCMYK / DeviceGray
images at chosen pixel sizes) and [`pikepdf`](https://pypi.org/project/pikepdf/) (precise
`/MediaBox` / `/TrimBox` / `/BleedBox`, stable image names, stripped volatile metadata). They
are synthetic — **no client or job PDFs**. The trim/page-size pair is checked against a
test-only profile, `testdata/profiles/test-trim-pagesize.json`, that pins an expected US
Letter size at error severity (the shipped `loupe-default` profile intentionally leaves the
expected size unset).

**`pending` and CI.** MIC-134 shipped `bleed`, `trim`, and `page-size` (PR #10); the
`trim-pagesize-*` pair above is promoted and snapshotted (PR #13). Custom checks
`color-mode`, `image-resolution`, and `embedded-fonts` are implemented in
`PreflightEngine` and covered by the fixtures above. Tier-2 bleed findings
(`content-bleed`, `bleed-margin-empty`, `needs-auto-bleed`) are covered by the
`content-bleed-*` fixtures and `examples/profile-tiered-bleed*.json` profiles.

**To regenerate the PDFs** (only when a fixture's geometry/content must change):

```bash
pip install reportlab pillow pikepdf
python3 loupe-preflight/tools/generate_fixtures.py   # writes into testdata/fixtures/
```

The output is deterministic, so a re-run with no code change produces no diff. Review
`git status` / `git diff` before committing to confirm no real client file slipped in.

### Public PDF/A corpora (MIC-146)

A pinned subset of public PDF/A corpora (veraPDF-corpus, Isartor, BFO) is tracked in MIC-146.
Like the fixtures above, it adds entries to this same manifest and drops files into
`testdata/fixtures/` without needing to change the runner.
