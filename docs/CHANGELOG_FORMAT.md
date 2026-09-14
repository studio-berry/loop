# Loop release changelog format

The canonical shape for every Loop changelog read outside the diff: promotion PR bodies
(`dev` → `unstable` → `stable`), GitHub release notes and release drafts, and milestone or
version closeouts. It does not replace the per-PR fragment
`changes/<sanitized-head-branch>.md` (Category, Audience, Breaking-Change, Summary), which
every PR still adds exactly once.

## Skeleton

```markdown
# [<version>] <milestone canonical name>

<one paragraph: what the release is, which milestone it closes, what it rules out>

Scope: `<base tag>` (`<base sha>`) → `<tip sha>` — <n> non-merge commits.
Canonical milestone text: `docs/github-milestones/<version>.md`.

## Added

## Changed

## Fixed

## Security

## Internal

## Closes

Closes #<n>

## Verification

## Breaking changes
```

## Rules

1. **Title** is `[<version>] <milestone canonical name>`, copied verbatim from
   `docs/github-milestones/<version>.md`. Work outside a milestone names its train instead of
   inventing a title.
2. **The scope line is measured, not estimated.** Take the count from
   `git log --oneline --no-merges <base>..<tip> | wc -l` and the base from `git describe --tags`
   or the release tag, and name the range by tag **and** SHA.
3. **Sections use the fragment vocabulary in this order:** Added, Changed, Fixed, Security,
   Internal. Drop what is empty; never introduce a section the vocabulary does not have.
4. **Every item names its issue and its mechanism** — `(#<n>)` plus what the code now does, or
   the file or API that carries it. Adjectives do not replace behaviour, and a catch-all
   "various fixes" is not an item.
   - Good: `#520 — searchDocumentText() allocates an operation-scoped PDFProcessingBudget, so an idle session's elapsed timer no longer throws through Quick search.`
   - Bad: `Improved search reliability.`
5. **Group by outcome, not by commit.** A twelve-commit hardening train is one item naming the
   class of input and the invariant it now enforces.
6. **`Closes` is a claim about acceptance criteria, not about commits.** An issue is closed only
   when the shipped diff meets the criteria written in its body. Partial work goes to `Refs`
   with the outstanding criterion named. A closing keyword is never added to make a milestone
   read as finished.
7. **One keyword per line.** `Closes #10` and `Closes #11` are two lines; a comma list is not
   this format.
8. **Record who closes them.** Closing keywords act only when the PR merges into the default
   branch (`stable`). Work that landed on `dev` or `unstable` leaves its issue open by design,
   and the block says so rather than leaving the reader to work it out.
9. **Verification cites observable state** — workflow, SHA or tag, and result. "Verified
   locally" without the command and its output is not verification.
10. **Breaking changes are stated, never implied.** Either `None.` with the reason it is none
    (additive API, no schema or persistence-format bump), or the migration and its ordering
    constraint.
11. **No claim a `git` or `gh` command cannot back.** Counts, PR numbers, SHAs, tags, check
    conclusions and issue states are copied from command output. When a fact could not be
    established, the changelog says that instead of asserting it.

## Placement

- **Promotion PRs** carry the whole format in the PR body. The PR body is the changelog.
- **Releases** reuse that body for the release notes or the generated release draft; a second
  copy is not maintained by hand.
- **Milestones** may copy the merged promotion body into their evidence directory. The PR body
  stays authoritative until the copy is made.

## Worked example

`[0.2.1] Operator Completion, Product Surface & Trust Leftovers`, the promotion
`unstable` → `stable` for milestone 17: a scope line over `0.2.0.1-alpha` (`86ee1b5b`) →
`d2e9ce9f`; items under Added / Changed / Fixed / Security / Internal, each naming its issue and
mechanism; sixteen `Closes` lines with `#236` marked as already shipped in `0.2.0.1-alpha`;
`Refs #241` with the outstanding criterion named (the renderer-differential lane and documented
corpus licensing); a verification line naming four green workflows at a SHA; and
`Breaking changes: None.` with the reason.
