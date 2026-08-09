// MIT License
//
// Copyright (c) 2018-2025 Jakub Melka and Contributors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#ifndef PDFDIAGNOSTICS_H
#define PDFDIAGNOSTICS_H

#include "pdfglobal.h"
#include "pdfplugin.h"

#include <QCoreApplication>
#include <QString>
#include <QStringList>

namespace pdf
{

struct PDF4QTLIBCORESHARED_EXPORT PDFDiagnosticsOptions
{
    /// Stable surface id stored in the manifest (e.g. "editor" or "pdftool").
    /// Falls back to QCoreApplication::applicationName() when empty.
    QString applicationId;

    /// Directory the bundle directory is created under (e.g. a folder the user picked)
    QString outputDirectory;

    /// Optional exact final bundle directory. When set, it takes precedence
    /// over outputDirectory and an existing destination is rejected.
    QString destinationPath;

    /// Include the rotated log files (logs/*.log) in the bundle
    bool includeLogs = true;

    /// Optional; when non-empty, plugins.json is written with this list
    PDFPluginInfos plugins;
};

struct PDF4QTLIBCORESHARED_EXPORT PDFDiagnosticsResult
{
    bool success = false;
    QString bundleDirectory;
    QStringList files;
    QString errorMessage;
};

/// Builds a self-contained, privacy-scrubbed support bundle: a plain directory
/// with a manifest.json, system/dependency info, the rotated log files
/// (already scrubbed at write time by PDFLogSession, and scrubbed again here
/// defensively on copy), and a README explaining what is and is not included.
/// Never writes a PDF, document content, settings, recent-files list,
/// environment dump, or crash minidump - see README.txt for why crash
/// minidumps specifically stay a separate, opt-in mechanism with different
/// privacy properties (SECURITY.md, R-008 in docs/V1_RELEASE_READINESS.md).
///
/// Core-clean by design (no Qt Widgets): the caller in Gui/App code owns any
/// consent dialog, folder picker, or result message box around this call.
class PDF4QTLIBCORESHARED_EXPORT PDFDiagnosticsCollector
{
    Q_DECLARE_TR_FUNCTIONS(pdf::PDFDiagnosticsCollector)

public:
    PDFDiagnosticsCollector() = delete;

    /// Collects the bundle. On any failure, removes whatever partial bundle
    /// directory was created so a broken bundle is never left on disk.
    static PDFDiagnosticsResult collect(const PDFDiagnosticsOptions& options);
};

}   // namespace pdf

#endif   // PDFDIAGNOSTICS_H
