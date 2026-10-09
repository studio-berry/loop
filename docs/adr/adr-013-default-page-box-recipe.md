# ADR-013: Ship a default recipe for the page-box drag move

**Status:** accepted
**Implemented-at:** issue #205 (shipped recipe, precedence, fail-closed selection)
**Last-verified:** 2026-10-07 @ 7fb8109c304e9365fc58318ced3bc9fc613f1d84
**Superseded-by:** none
**Date:** 2026-10-07
**Deciders:** Loop owner (issue #205); interaction boundary owners
**Builds on:** ADR-012 (`actionMoveSelection`, `translate-page-box`)

## Context

A completed page-box drag proposes a `translate-page-box` move in the Fix
workspace. Before this decision no recipe shipped with Loop, so the proposal
could not execute until an operator imported a recipe that offered the
operation. The recipes directory is empty in every test and smoke environment,
so no existing sample could satisfy the drag.

## Decision

### Where the recipe ships

The recipe ships as a Qt resource embedded in `LoopLibInteraction`, beside the
command contract:

- Source: `LoopLibInteraction/builtin-recipe-translate-page-box.json`, embedded
  under `:/loop/builtin-recipe-translate-page-box.json`.
- An installed package carries it inside the binary. No file is placed in the
  install tree, and nothing has to be copied into the user's configuration.

Rejected alternatives:

- A profile entry: profiles are preflight-scoped and would need a second
  registry for action lists.
- A first-run onboarding step: a user who skips onboarding, or who opens a
  document on an existing install, would still get no recipe.

### Versioning and updates

- The shipped recipe changes only with a Loop release. Its `recipeHash` is
  computed from the recipe content, so any change to the shipped JSON changes
  the hash the Fix workspace and plan identity report.
- Operators who want to edit it export the recipe and import the copy. A copy
  is an operator recipe and is never overwritten by an application update.
- The shipped entry is read-only: `ActionListCatalog::saveRecipe` refuses it
  with an explicit message. `exportRecipe` works on it.

### Load-time validation

A recipe's placeholders (`${box}`, `${page_index}`, `${dx}`, `${dy}`) are
resolved from the drag at plan time. Catalog validation at load uses probe
bindings (`box: media`, `page_index: 0`, `dx: 0`, `dy: 0`) so that the
placeholders are type-checked against the operation's parameter schema. The
probe is never executed; it only decides whether the shipped entry is valid.

### Precedence

`EditorHost::selectActionListRecipeForOperation` resolves an operation in this
order:

1. A **valid operator recipe** that offers the operation is selected. Invalid
   operator recipes beside it do not block it.
2. An **invalid operator recipe** that offers the operation, with no valid
   operator recipe, stops the change. The message names the recipe and its
   diagnostic. The shipped recipe does not run in its place.
3. The **shipped recipe**, when it is valid, is selected. If it is invalid, the
   change stops and names its diagnostic.
4. Otherwise no recipe runs, and the message says to import one.

The shipped recipe is never the default selection. It is chosen only when an
operation needs it, so the Fix workspace does not open on it.

The shipped entry is listed in the recipe list with `builtIn: true`, so the
operator can see it beside their own recipes.

### Failure behaviour

A refused move stays in the Fix workspace with no recipe selected and no
document change. The typed `move/rejected` reason is reported by the command
handler, and the announcement names the recipe or diagnostic that stopped it.

## Consequences

- Recipe selection now distinguishes operator from shipped recipes. Callers that
  announced their own no-recipe message have been reduced to one selection call,
  and the announcement wording is unified to "then plan the change".
- Any test that runs in the shared test-mode configuration sees the shipped
  entry. Recipe tests therefore clear the operator recipes directory first.
- The shipped recipe is only as valid as the operation it names. A removed or
  renamed `translate-page-box` makes the shipped entry invalid, and the change
  fails closed with that diagnostic.

## Acceptance evidence

Recorded for issue #205:

| Acceptance item | Status |
| --- | --- |
| One recorded decision for where the recipe ships, installed-package location, versioning | This record |
| End-to-end default-install run: drag, proposal, plan, approval, execute, published-byte revalidation, artifact identity | Test `shippedPageBoxRecipeRunsDefaultMoveThroughPlanApprovalAndRevalidation` written; **not yet run**, see "Local verification limit" |
| User-imported recipe is supported and not silently shadowed; precedence recorded | Tests `operatorRecipeWinsOverShippedRecipe` and `invalidOperatorRecipeStopsMoveWithoutShippedFallback`; **not yet run** |
| Failure with recipe absent, stale, or offending the operation fails closed with `move/rejected` and names what is missing | Invalid-recipe path covered by `invalidOperatorRecipeStopsMoveWithoutShippedFallback`; the "absent" case cannot occur in a shipped build and is not separately tested |
| Installed-package run on Linux and Windows | **Not run** |

## Local verification limit

The authoring environment has no Qt installation and no configured build
directory, so the C++ changes in this record were not compiled or executed
there. The hosted `agent-fast / build` and quick-lane tests are the first
compile and run of these changes. Re-open the acceptance rows above from that
evidence and the installed-package run.
