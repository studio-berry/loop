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

#ifndef PDFLOGGER_H
#define PDFLOGGER_H

#include "pdfglobal.h"

#include <QString>
#include <QStringList>

namespace pdf
{

/// RAII wrapper that installs the rotating, privacy-scrubbing log sink shared
/// by PdfTool and the Editor. Mirrors the shape of PDFSentrySession so
/// main() wiring looks the same for both: construct one session near the top
/// of main() and let it live for the process lifetime.
///
/// Every message that passes through the installed handler is run through
/// PDFLogScrubber before it is written - scrubbing happens once, in the sink,
/// so no call site (existing or future) can leak a path or user name into the
/// log by forgetting to scrub it first.
class PDF4QTLIBCORESHARED_EXPORT PDFLogSession
{
public:
    enum Level
    {
        Off,
        Error,
        Warning,
        Info,
        Debug
    };

    /// Installs the message handler, chaining to whatever handler was
    /// previously installed (so existing stderr behavior, e.g. PdfTool's, is
    /// preserved). Resolves the initial level from LOUPE_LOG_LEVEL, else the
    /// diagnostics/logLevel QSettings key, else Level::Warning.
    /// \param applicationId Short id used as the log file base name and in
    ///        the "[applicationId]" field of every line (e.g. "editor", "pdftool")
    explicit PDFLogSession(const QString& applicationId);

    /// Restores the previously installed message handler and closes the log file.
    ~PDFLogSession();

    PDFLogSession(const PDFLogSession&) = delete;
    PDFLogSession& operator=(const PDFLogSession&) = delete;

    /// Sets the minimum level a message must have to be written to the log.
    /// Thread-safe; takes effect for the next message the handler processes.
    /// Messages below the configured level are still forwarded to whatever
    /// handler was previously installed - this only controls what is written
    /// to the log file.
    static void setLevel(Level level);

    /// Current persisted logging threshold. This is safe to expose in a
    /// diagnostics manifest because it contains no configuration secret.
    static Level level();

    /// False when the logger could not open, rotate, or flush its sink. The
    /// application continues running and the previous Qt handler is retained.
    static bool isHealthy();

    /// Directory the active session is writing into, resolved in this order:
    /// 1. LOUPE_LOG_DIR environment variable, if set;
    /// 2. "<settingsPath>/logs", when PDFSettings::getSettingsPath() is
    ///    non-empty (portable installs / --config);
    /// 3. QStandardPaths::AppLocalDataLocation + "/logs", falling back to
    ///    QDir::tempPath() when that location is empty.
    /// Returns an empty string when no session is active.
    static QString logDirectory();

    /// Rotated log files for the active session's application id, newest
    /// first: "<id>.log", "<id>.log.1", "<id>.log.2". Only files that
    /// currently exist are included. Returns an empty list when no session
    /// is active.
    static QStringList logFiles();

private:
    bool m_active = false;
};

}   // namespace pdf

#endif   // PDFLOGGER_H
