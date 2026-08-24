# Installed Editor becomes Qt Quick navigable shell

Category: changed
Audience: developers
Breaking-Change: yes
Summary: Replace the installed LoupeEditor product root with a packaged Loupe.Quick ApplicationWindow that opens, closes, reopens, and navigates a PDF through the existing DocumentFacade, CommandCatalog, ViewportController, PageSurfaceCoordinator, InteractionController, and LoupeCanvas stack. Add ViewportCommandBridge handlers for page, zoom, and rotate catalog commands; EditorHost as the C++ presentation owner; and LoupeEditorWidgetsOracle as a non-installed Widgets parity target. This is the P4-S7 navigable slice only—the operator loop, inspector, and Phase 4 exit gates remain open.
