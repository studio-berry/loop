# Semantic Trust Engine — 0.1.1 exit audit (S18)

Product `LOUPE_VERSION` remains **0.1.0**. This audit does **not** bump
SemVer. Waves A–D land as stacked topic branches; they are not yet
merged to `origin/dev`. Re-run this checklist on `dev` after that merge
before tagging `0.1.1`.

## Checklist

| Box | Session | Status on this branch |
|-----|---------|------------------------|
| #234/#236/#237/#238/#239 acceptance | S01–S05 | Implemented on Wave A; not merged to `origin/dev` |
| Schema round-trip; unsupported majors fail closed | S03 | Wave A |
| Five-family Evidence Graph matches golden corpus | S06–S08 | Wave B |
| Async/cache results are revision-bound | S04–S05 | Wave A |
| Targeted revalidation matches full-run verdicts | S11 | Wave C |
| Standards claims pass an independent oracle | S12 | Wave C (mock oracle + fixture triad) |
| Resource exhaustion reports INCOMPLETE within bounds | S14–S15 | This branch |
| Huge-document behavior measured and bounded | S15 | This branch (generated representatives) |
| Model-based lifecycle catches stale/overwrite/rollback defects | S16 | This branch (injected defects; not a command-alphabet fuzzer) |
| Native plugin ABI/load policy tested | S17 | This branch |
| Benchmark identity and architecture invariants automated | S18 | This branch (`PDFRunIdentity`, `docs/architecture-invariants.json`, catalog `--check`) |
| No new GUI required | all | Held |

## Remaining before 0.1.1

- Merge Waves A–D to `dev` and re-run the mapped `UnitTests*` targets plus `scripts/generate-architecture-catalogs.py --check`.
- Confirm independent oracle coverage still matches the S12 fixture triad on `dev`.
- Do not bump `LOUPE_VERSION` until every row is green on `dev`.

## Identity fields

`PdfTool benchmark --console-format json` writes `data.identity` with
commit, compiler, build, OS, Qt, CPU, GPU, renderer, fixture digest,
profile/operation version, and product version. Workload envelopes use
the same `PDFRunIdentity` object.
