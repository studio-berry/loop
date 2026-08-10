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

#include "pdftoolflattentransparency.h"

#include "pdfdocumentwriter.h"

#include <QJsonDocument>

namespace pdftool
{

static PDFToolFlattenTransparency s_toolFlattenTransparencyApplication;

QString PDFToolFlattenTransparency::getStandardString(StandardString standardString) const
{
    switch (standardString)
    {
        case Command:
            return QStringLiteral("flatten-transparency");
        case Name:
            return PDFToolTranslationContext::tr("Flatten Transparency");
        case Description:
            return PDFToolTranslationContext::tr("Rasterize PDF pages through the shared renderer and remove live transparency.");
        default:
            Q_ASSERT(false);
            return {};
    }
}

PDFToolExitCode PDFToolFlattenTransparency::execute(const PDFToolOptions& options)
{
    if (!options.destructiveDryRun && options.flattenTransparencyOutputDocument.isEmpty())
    {
        reportDiagnostic(options,
                         PDFToolDiagnosticSeverity::Error,
                         QStringLiteral("cli.invalid-arguments"),
                         PDFToolTranslationContext::tr("Output document file name is not set. Use -o/--output or --dry-run."));
        return PDFToolExitCode::InvalidInvocation;
    }

    pdf::PDFDocument document;
    if (!readDocument(options, document, nullptr, false))
    {
        return PDFToolExitCode::InputError;
    }

    pdf::PDFTransparencyFlattenSettings settings = options.flattenTransparencySettings;
    settings.analyzeOnly = options.destructiveDryRun;
    if (!options.pageSelectorSelection.isEmpty())
    {
        settings.pageRange = options.pageSelectorSelection;
    }
    else if (!options.pageSelectorFirstPage.isEmpty() || !options.pageSelectorLastPage.isEmpty())
    {
        const QString first = options.pageSelectorFirstPage.isEmpty() ? QStringLiteral("1") : options.pageSelectorFirstPage;
        const QString last = options.pageSelectorLastPage.isEmpty() ? QStringLiteral("-") : options.pageSelectorLastPage;
        settings.pageRange = QStringLiteral("%1-%2").arg(first, last);
    }

    pdf::PDFTransparencyFlattenReport report;
    const pdf::PDFOperationResult result = pdf::PDFTransparencyFlattener::apply(&document, settings, &report);
    if (!result)
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("operation.failed"), result.getErrorMessage());
        return PDFToolExitCode::ProcessingFailure;
    }

    if (options.outputStyle == PDFOutputFormatter::Style::Json || options.destructiveReport || options.destructiveDryRun)
    {
        if (options.executionContext)
        {
            options.executionContext->setData(report.toJson());
        }
        else
        {
            PDFConsole::writeText(QJsonDocument(report.toJson()).toJson(QJsonDocument::Indented), options.outputCodec);
        }
    }

    if (options.destructiveDryRun)
    {
        if (options.executionContext)
        {
            options.executionContext->addOutput({ QStringLiteral("file"), QStringLiteral("primary"), options.flattenTransparencyOutputDocument, QStringLiteral("planned") });
        }
        return PDFToolExitCode::Success;
    }

    if (const PDFToolExitCode blocked = validateDestructiveOutput(options, options.flattenTransparencyOutputDocument); blocked != PDFToolExitCode::Success)
    {
        return blocked;
    }

    pdf::PDFDocumentWriter writer(nullptr);
    const pdf::PDFOperationResult writeResult = writer.write(options.flattenTransparencyOutputDocument, &document, true);
    if (!writeResult)
    {
        reportDiagnostic(options,
                         PDFToolDiagnosticSeverity::Error,
                         QStringLiteral("output.write-failed"),
                         writeResult.getErrorMessage(),
                         QJsonObject{{ QStringLiteral("path"), options.flattenTransparencyOutputDocument }});
        return PDFToolExitCode::ProcessingFailure;
    }

    if (options.executionContext)
    {
        options.executionContext->addOutput({ QStringLiteral("file"), QStringLiteral("primary"), options.flattenTransparencyOutputDocument, QStringLiteral("written") });
    }
    return PDFToolExitCode::Success;
}

PDFToolAbstractApplication::Options PDFToolFlattenTransparency::getOptionsFlags() const
{
    return ConsoleFormat | OpenDocument | PageSelector | ColorManagementSystem | FlattenTransparency | DestructiveWrite;
}

} // namespace pdftool
