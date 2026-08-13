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

#include "pdftoolrepairdiff.h"

#include "pdftoolcancel.h"
#include "pdfdocumentreader.h"

#include <QDir>
#include <QJsonDocument>

#include <algorithm>

namespace pdftool
{

namespace
{

class CancelControl final : public pdf::PDFOperationControl
{
public:
    bool isOperationCancelled() const override
    {
        return isCancelRequested();
    }
};

bool readDocumentFromPath(const QString& path,
                  const PDFToolOptions& options,
                  pdf::PDFDocument* document,
                  QString* error)
{
    pdf::PDFDocumentReader reader(nullptr,
                                  [&options](bool*) { return options.password; },
                                  options.permissiveReading,
                                  false);
    *document = reader.readFromFile(path);
    if (reader.getReadingResult() != pdf::PDFDocumentReader::Result::OK)
    {
        if (error)
        {
            *error = reader.getErrorMessage();
        }
        return false;
    }
    return true;
}

int unexpectedChangeCount(const pdf::PDFRepairDiffReport& report)
{
    int count = 0;
    for (const pdf::PDFRepairStructuralChange& change : report.structuralChanges)
    {
        count += change.classification == pdf::PDFRepairChangeClass::Unexpected;
    }
    for (const pdf::PDFRepairPageVisualDiff& page : report.pages)
    {
        count += page.unexpectedChangedPixelCount > 0;
    }
    return count;
}

} // namespace

static PDFToolRepairDiff s_toolRepairDiffApplication;

QString PDFToolRepairDiff::getStandardString(StandardString standardString) const
{
    switch (standardString)
    {
        case Command:
            return QStringLiteral("repair-diff");
        case Name:
            return PDFToolTranslationContext::tr("Repair Diff");
        case Description:
            return PDFToolTranslationContext::tr("Compare two serialized PDF states with deterministic visual and semantic repair evidence.");
    }
    return QString();
}

PDFToolExitCode PDFToolRepairDiff::execute(const PDFToolOptions& options)
{
    if (options.repairDiffFiles.size() != 2)
    {
        reportDiagnostic(options,
                         PDFToolDiagnosticSeverity::Error,
                         QStringLiteral("cli.invalid-arguments"),
                         PDFToolTranslationContext::tr("repair-diff requires exactly a before and an after PDF."));
        return PDFToolExitCode::InvalidInvocation;
    }

    pdf::PDFDocument before;
    pdf::PDFDocument after;
    QString error;
    if (!readDocumentFromPath(options.repairDiffFiles.at(0), options, &before, &error))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("pdf.before-unreadable"), error,
                         QJsonObject{{QStringLiteral("path"), options.repairDiffFiles.at(0)}});
        return PDFToolExitCode::InputError;
    }
    if (!readDocumentFromPath(options.repairDiffFiles.at(1), options, &after, &error))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("pdf.after-unreadable"), error,
                         QJsonObject{{QStringLiteral("path"), options.repairDiffFiles.at(1)}});
        return PDFToolExitCode::InputError;
    }

    pdf::PDFRepairDiffOptions diffOptions = options.repairDiffOptions;
    CancelControl cancelControl;
    diffOptions.operationControl = &cancelControl;
    if (!diffOptions.renderDirectory.isEmpty() && !QDir().mkpath(diffOptions.renderDirectory))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("output.artifact-directory-failed"),
                         PDFToolTranslationContext::tr("Unable to create repair-diff artifact directory."),
                         QJsonObject{{QStringLiteral("path"), diffOptions.renderDirectory}});
        return PDFToolExitCode::ProcessingFailure;
    }

    pdf::PDFRepairDiffReport report;
    const pdf::PDFOperationResult result = pdf::PDFRepairDiffEngine::compare(before, after, diffOptions, &report);
    if (!result)
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("repair-diff.failed"), result.getErrorMessage());
        return PDFToolExitCode::ProcessingFailure;
    }

    const QJsonObject reportJson = report.toJson();
    if (options.executionContext)
    {
        options.executionContext->setData(QJsonObject{
            { QStringLiteral("ok"), report.status != pdf::PDFRepairDiffStatus::Failed },
            { QStringLiteral("command"), QStringLiteral("repair-diff") },
            { QStringLiteral("report"), reportJson }
        });
        if (!diffOptions.renderDirectory.isEmpty())
        {
            options.executionContext->addOutput({ QStringLiteral("directory"), QStringLiteral("artifacts"), diffOptions.renderDirectory, QStringLiteral("written") });
        }
    }
    if (options.outputStyle != PDFOutputFormatter::Style::Json)
    {
        PDFConsole::writeText(QString::fromUtf8(QJsonDocument(reportJson).toJson(QJsonDocument::Indented)), options.outputCodec);
    }

    if (report.status == pdf::PDFRepairDiffStatus::Incomplete)
    {
        return PDFToolExitCode::PartialOutput;
    }
    if (unexpectedChangeCount(report) > 0)
    {
        return PDFToolExitCode::Findings;
    }
    return PDFToolExitCode::Success;
}

PDFToolAbstractApplication::Options PDFToolRepairDiff::getOptionsFlags() const
{
    return ConsoleFormat | RepairDiff;
}

} // namespace pdftool
