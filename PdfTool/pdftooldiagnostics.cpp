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

#include "pdftooldiagnostics.h"

#include "pdfdiagnostics.h"

#include <QJsonArray>
#include <QJsonObject>

namespace pdftool
{

static PDFToolDiagnostics s_diagnosticsApplication;

QString PDFToolDiagnostics::getStandardString(StandardString standardString) const
{
    switch (standardString)
    {
        case Command:
            return QStringLiteral("diagnostics");

        case Name:
            return PDFToolTranslationContext::tr("Diagnostics");

        case Description:
            return PDFToolTranslationContext::tr("Collect a privacy-scrubbed diagnostics/support bundle.");

        default:
            Q_ASSERT(false);
            break;
    }

    return QString();
}

PDFToolExitCode PDFToolDiagnostics::execute(const PDFToolOptions& options)
{
    pdf::PDFDiagnosticsOptions collectorOptions;
    collectorOptions.outputDirectory = options.diagnosticsOutputDirectory;
    collectorOptions.includeLogs = options.diagnosticsIncludeLogs;
    collectorOptions.includeSettings = options.diagnosticsIncludeSettings;

    const pdf::PDFDiagnosticsResult result = pdf::PDFDiagnosticsCollector::collect(collectorOptions);

    if (!result.success)
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("diagnostics.collect-failed"), result.errorMessage);
        return PDFToolExitCode::ProcessingFailure;
    }

    if (options.outputStyle == PDFOutputFormatter::Style::Json && options.executionContext)
    {
        QJsonArray files;
        for (const QString& file : result.files)
        {
            files.append(file);
            options.executionContext->addOutput({ QStringLiteral("file"),
                                                  QStringLiteral("diagnostics"),
                                                  file,
                                                  QStringLiteral("written") });
        }

        options.executionContext->setData(QJsonObject{
            { QStringLiteral("bundleDirectory"), result.bundleDirectory },
            { QStringLiteral("files"), files } });
    }
    else
    {
        PDFConsole::writeText(result.bundleDirectory, options.outputCodec);
    }

    return PDFToolExitCode::Success;
}

PDFToolAbstractApplication::Options PDFToolDiagnostics::getOptionsFlags() const
{
    return ConsoleFormat | Diagnostics;
}

}   // namespace pdftool
