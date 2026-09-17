Category: added
Audience: developers
Breaking-Change: no
Summary: Add Core-owned deterministic object selectors with logical composition, Action List schema v2 `select` support, preview scope digests, and stale-revision fail-closed execution fencing. PageMaster, PdfTool, and Editor Action List runs share `makeActionListExecutionOptions()` so selector resolution uses the same revision-bound Core resolver on every surface. Ada coverage closes per-predicate fixtures, NOT/unknown-set composition, adversarial parse cases, and Action List select validate/plan/execute fencing tests.
