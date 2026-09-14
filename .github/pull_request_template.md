<!-- Berry Studio BSP-002 §4.2, adopted for this repository. Loop's own gates live in AGENTS.md. -->

## What changed

<!-- One paragraph: the problem, the change, and the issue. `dev` is not the default branch, so close the issue explicitly with `gh issue close`. -->

## Proof

- [ ] `python scripts/agent/check-change.py --base origin/dev --build-dir build-local` reports `pass`, or the report and the failing check are quoted here
- [ ] One `changes/<sanitized-head-branch>.md` fragment added (Category, Audience, Breaking-Change, Summary)
- [ ] Changed behaviour has a test that fails without the change
- [ ] Protected-path or contract change named above, with the reason it is required

## Internal logic (touched behavior-bearing code)

- [ ] Guard clauses handle invalid, stale, cancelled, absent, unauthorized, and terminal cases before the happy path
- [ ] Untrusted input is parsed once at the boundary into trusted typed or domain state, with no repeated checks downstream
- [ ] Invalid state stops before partial mutation or publication and returns a descriptive error or result
- [ ] Names carry the domain intent, and comments explain rationale rather than restating the code

## Anti-slop pass

- [ ] Redundant or explanatory comments that do not match the file's style removed
- [ ] Abnormal defensive checks and broad try/catch blocks removed where a trusted upstream boundary already guarantees the invariant, with real boundary and safety checks kept
- [ ] No `any` or equivalent cast added only to suppress a type error
- [ ] Python imports stay at file scope unless a local import is required
- [ ] Generated boilerplate, needless wrappers, and local-style drift removed
- [ ] Validation, security, cancellation, provenance, and failure handling preserved

Anti-slop summary (1-3 sentences):

<!-- What was removed, and what was kept on purpose. -->

## Security and rollback

- [ ] Untrusted input validated at the trust boundary; no new unsafe construct without an inline justification
- [ ] Rollback: <!-- how this change is reverted if it fails -->

## Docs

- [ ] Docs updated in this PR, or "none needed" with the reason

## Self-review (BSP-002 §4.3)

- [ ] Reviewed in the diff view, not the editor, at least 30 minutes after the final commit; overnight if the change touches security-sensitive code, data handling, or public API surface
