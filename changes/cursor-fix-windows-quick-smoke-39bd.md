Category: fixed
Audience: maintainers
Breaking-Change: no
Summary: Stop overriding windeployqt qt.conf plugin paths on Windows during LoopEditor --quick-smoke, skip Sentry for the smoke fast path, and emit staged stderr markers plus install-tree diagnostics when relocated Windows packaging smoke fails. Copy qoffscreen beside LoopEditor.exe, append Plugins=plugins to qt.conf, remove attachConsole, and match PdfTool library-path probing on Windows.
