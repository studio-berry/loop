Category: internal
Audience: maintainers
Breaking-Change: no
Summary: Merge stable into dev so the integration branch carries the Session 07 package-proof pipeline (#527), replacing the superseded manual windeployqt/linuxdeployqt staging with the CMake-generated Qt deploy-script closure. Restore AppDir preparation (desktop entry, icon, AppRun) before appimagetool now that linuxdeployqt no longer performs that step.
