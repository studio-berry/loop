# Quick shell localization

Issue: #126. The shipping language set retains the existing English, Czech,
German, Spanish, French, Korean, Russian, Turkish, Simplified Chinese and
Traditional Chinese catalogs. Missing translations use the English source.
The pseudo-locale is a verification artifact and is not a shipped language.

`LoopLibInteraction/sources/shellglossary.h` owns the presentation names for
verdicts, severities and registered finding types. The controller and Quick
shell use the same `LoopGlossary` context. Core verdict reduction, stored IDs,
report messages and receipts retain their authoritative values. Unknown finding
codes remain visible as their original IDs until the glossary is extended.

Menu membership and command IDs still come from the command manifest. MenuModel
has extractable literal labels, so translation tools no longer have to infer
operator text from the identifiers. Accessible lifecycle and preview descriptions
also use the glossary translation context; state keys stay stable.

The existing root `LOOP_lupdate` target collects C++ and QML source targets.
`LOOP_lrelease` compiles the declared catalogs. The existing install rules stage
these `.qm` files under the configured translations directory; LoopEditor loads
them through PDFApplicationTranslator at startup.

## Verification procedure

Run `LOOP_lupdate` in an approved configured build, then inspect the generated
`.lupdate/LOOP_lupdate_project.json` for LoopEditor C++ and all Quick QML sources.
Build affected targets and `LOOP_lrelease`. Confirm all ten `.qm` files resolve
through the runtime translations path. Packaging qualification requires its own
approved install/package lane.

For an extracted source catalog outside the repository, run:

```powershell
python scripts/generate-quick-pseudo-locale.py <extracted.ts> <external-artifacts>
lrelease <external-artifacts>/quick-pseudo.ts -qm <external-artifacts>/quick-pseudo.qm
ProductQuickAccessibilitySmoke --pseudo-locale <external-artifacts>/quick-pseudo.qm <external-artifacts>
```

The smoke probe records Preflight, Fix and Inspect screenshots and JSON text,
accessibility names and truncation observations. It requires the translated
application menu name and rejects observed text truncation. Review screenshots
and unmarked text for additional hard-coded presentation strings; IDs and report
evidence remain technical values. Offscreen/software proof does not establish
native accessibility or installed-package qualification.

Run UnitTestsLoopStateVisual for translated glossary behavior, English fallback
and unchanged state keys. Run mapped Quick/accessibility lanes and the complete
`check-change.py --base origin/dev --build-dir <approved-build>` gate.
