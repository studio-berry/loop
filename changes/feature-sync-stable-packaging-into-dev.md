Category: internal
Audience: maintainers
Breaking-Change: no
Summary: Merge stable into dev so the integration branch carries the Session 07 package-proof pipeline (#527), replacing the superseded manual windeployqt/linuxdeployqt staging with the CMake-generated Qt deploy-script closure. Restore AppDir preparation before appimagetool, bundle only the QSQLITE SQL driver, ship the Loop.Canvas QML plugin in the install tree, attach a console for Windows --quick-smoke so relocated smoke can emit Qt plugin diagnostics, defer normal-shell QML engine setup until after the smoke path, and fix the clang-format violation blocking agent-fast.
