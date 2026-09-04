Category: internal
Audience: maintainers
Breaking-Change: no
Summary: Merge stable into dev so the integration branch carries the Session 07 package-proof pipeline (#527), replacing the superseded manual windeployqt/linuxdeployqt staging with the CMake-generated Qt deploy-script closure. Restore AppDir preparation before appimagetool, bundle only the QSQLITE SQL driver, widen packaged Qt search paths for Windows relocated smoke, and fix a pre-existing clang-format violation blocking agent-fast.
