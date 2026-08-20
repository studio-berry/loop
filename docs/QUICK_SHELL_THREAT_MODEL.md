# Qt Quick shell threat model

This threat model is the security boundary for the staged Qt Quick shell. The
S22 `QuickShellSmoke` target is a qualification harness, not a trusted product
surface. The model becomes a release gate when product QML is introduced.

## Assets and trust boundaries

The protected assets are document bytes, document and preflight revision
identity, operation history, approval state, plugin capabilities, user
credentials, and the integrity of the packaged Qt runtime.

The boundary has three zones:

| Zone | Owner | Allowed responsibility |
| --- | --- | --- |
| C++ core/session | C++ | document identity, revision fences, PDF operations, history, plugins, persistence, network policy |
| Quick presentation | QML/Quick | visual composition, focus, transient control state, accessible labels and status presentation |
| External inputs | OS, drag/drop, files, plugins, package runtime | untrusted input; validated before entering C++ command paths |

QML is not a document store, command executor, plugin loader, network client,
or filesystem authority. The canonical machine-readable contract is
[`quick-shell-policy.json`](quick-shell-policy.json).

## Threats and controls

| Threat | Control | Evidence |
| --- | --- | --- |
| Remote or filesystem QML import executes outside the package | qrc-only first-party imports; remote and filesystem imports denied | static policy check |
| QML creates a dynamic component with an unexpected source | `Qt.createComponent`, `Qt.createQmlObject`, and relative imports forbidden | static policy check |
| QML reaches document bytes or a second history | C++ typed, revision-fenced command boundary; no `PDFDocument` or persistence authority in QML | policy plus bridge tests |
| Network/process escape from presentation | network, process, shell, and URL-launch paths denied | static policy plus packaging review |
| Plugin impersonation or unsafe instance creation | packaged native plugins are inspected and admitted before instance creation | plugin policy and runtime tests |
| Sensitive payload leaks through diagnostics | status messages carry state and identifiers only; PDF payload logging is forbidden | code review and log assertions |
| Unbounded customer-content cache | no persistent QML content cache; transient visual caches are bounded and invalidated by revision | runtime/resource tests |
| Keyboard trap or inaccessible state | shared name/role/state/focus/contrast/DPI/reduced-motion contract with Widgets baseline | accessibility and focus-bridge tests |
| Renderer/backend failure hidden by headless startup | smoke must observe `scene_graph_initialized` and a non-`Unknown` `GraphicsApi` | native/software CI logs |
| Qt runtime or license omission | actual linked QML/Quick modules appear in SBOM/notices and clean-machine package smoke | release artifact dossier |

## Session 6 qualification surfaces

The opt-in [`quick-runtime-manifest.json`](quick-runtime-manifest.json) names
the two current Quick targets and keeps both explicitly qualification-only.
[`verify-quick-runtime-contract.py`](../scripts/verify-quick-runtime-contract.py)
fails if either target defaults on, is installed, loses its declared Qt module
links, or is treated as proof of final-artifact SBOM, notices, clean-machine,
or LGPL relink completion.

The S22 bridge probe is run with
[`run-quick-focus-bridge.ps1`](../scripts/run-quick-focus-bridge.ps1). It
exercises keyboard focus from QWidget to Quick and back in both directions and
checks the Quick control's name, description, and role. The probe records
whether a native accessibility backend is active, but does not claim
screen-reader runtime evidence when the platform accessibility backend is not
enabled. Product Quick accessibility and installed-runtime evidence remain
release gates.

## Admission rules

Product Quick work may proceed from the qualification harness only when:

1. the S21 canvas candidate and benchmark outcome are accepted;
2. static policy and token checks are green;
3. native and `QT_QUICK_BACKEND=software` scene-graph smoke runs are green on
   Windows and Linux;
4. the QWidget-to-Quick focus and accessibility bridge has executable tests;
5. package inspection identifies every linked Qt/QML runtime module and its
   licensing notices; and
6. security review confirms that no QML path owns document bytes, revision
   identity, mutation history, network, filesystem, or process execution.

Any failure is a NO-GO for the corresponding surface; it is not converted into
a warning by running under `QT_QPA_PLATFORM=offscreen`.
