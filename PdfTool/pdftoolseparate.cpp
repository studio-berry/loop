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

#include "pdftoolseparate.h"
#include "pdftoolcancel.h"
#include "pdfdocumentbuilder.h"
#include "pdfexception.h"
#include "pdfoptimizer.h"
#include "pdfdocumentwriter.h"

#include <QFileInfo>

namespace pdftool
{

static PDFToolSeparate s_toolSeparateApplication;

QString PDFToolSeparate::getStandardString(StandardString standardString) const
{
    switch (standardString)
    {
        case Command:
            return "separate";

        case Name:
            return PDFToolTranslationContext::tr("Extract pages");

        case Description:
            return PDFToolTranslationContext::tr("Separate document into single page documents.");

        default:
            Q_ASSERT(false);
            break;
    }

    return QString();
}

PDFToolExitCode PDFToolSeparate::execute(const PDFToolOptions& options)
{
    pdf::PDFDocument document;
    QByteArray sourceData;
    if (!readDocument(options, document, &sourceData, false))
    {
        return PDFToolExitCode::InputError;
    }

    if (!document.getStorage().getSecurityHandler()->isAllowed(pdf::PDFSecurityHandler::Permission::CopyContent))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("pdf.copy-not-permitted"), PDFToolTranslationContext::tr("Document doesn't allow to copy content."));
        return PDFToolExitCode::ProcessingFailure;
    }

    QString parseError;
    std::vector<pdf::PDFInteger> pageIndices = options.getPageRange(document.getCatalog()->getPageCount(), parseError, true);

    if (!parseError.isEmpty())
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("cli.invalid-arguments"), parseError);
        return PDFToolExitCode::InvalidInvocation;
    }

    if (options.separatePagePattern.isEmpty())
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("cli.invalid-arguments"), PDFToolTranslationContext::tr("File template is empty."));
        return PDFToolExitCode::InvalidInvocation;
    }

    if (!options.separatePagePattern.contains("%"))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("cli.invalid-arguments"), PDFToolTranslationContext::tr("File template must contain character '%' for page number."));
        return PDFToolExitCode::InvalidInvocation;
    }

    QStringList plannedOutputs;
    plannedOutputs.reserve(int(pageIndices.size()));
    for (pdf::PDFInteger pageIndex : pageIndices)
    {
        QString fileName = options.separatePagePattern;
        fileName.replace('%', QString::number(pageIndex + 1));
        plannedOutputs.append(fileName);
    }

    if (const PDFToolExitCode blocked = validateDestructiveOutputs(options, plannedOutputs))
    {
        return blocked;
    }

    size_t failedWrites = 0;
    int plannedOutputIndex = 0;

    for (pdf::PDFInteger pageIndex : pageIndices)
    {
        if (isCancelRequested())
        {
            return PDFToolExitCode::Cancelled;
        }

        const QString fileName = plannedOutputs.at(plannedOutputIndex++);

        try
        {
            pdf::PDFDocumentBuilder documentBuilder(&document);
            documentBuilder.flattenPageTree();
            std::vector<pdf::PDFObjectReference> pageReferences = documentBuilder.getPages();
            std::vector<pdf::PDFObjectReference> singlePageRef = { pageReferences[pageIndex] };
            documentBuilder.setPages(singlePageRef);
            documentBuilder.removeOutline();
            documentBuilder.removeThreads();
            documentBuilder.removeDocumentActions();
            documentBuilder.removeStructureTree();

            pdf::PDFDocument singlePageDocument = documentBuilder.build();

            // Optimize document - remove unused objects and shrink object storage
            pdf::PDFOptimizer optimizer(pdf::PDFOptimizer::RemoveUnusedObjects | pdf::PDFOptimizer::ShrinkObjectStorage, nullptr);
            optimizer.setDocument(&singlePageDocument);
            optimizer.optimize();
            singlePageDocument = optimizer.takeOptimizedDocument();

            if (options.destructiveReport)
            {
                if (options.outputStyle != PDFOutputFormatter::Style::Json)
                {
                    PDFConsole::writeText(PDFToolTranslationContext::tr("Would extract page %1 to '%2'.")
                                            .arg(pageIndex + 1)
                                            .arg(fileName),
                                        options.outputCodec);
                }
            }

            if (options.destructiveDryRun)
            {
                if (options.executionContext)
                {
                    options.executionContext->addOutput({QStringLiteral("file"), QStringLiteral("separate"), fileName, QStringLiteral("planned")});
                }
                continue;
            }

            pdf::PDFDocumentWriter writer(nullptr);
            pdf::PDFOperationResult result = writer.write(fileName, &singlePageDocument, true);
            if (!result)
            {
                ++failedWrites;
                reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("output.write-failed"), result.getErrorMessage(), QJsonObject{{QStringLiteral("path"), fileName}});
            }

            if (options.executionContext)
            {
                options.executionContext->addOutput({
                    QStringLiteral("file"),
                    QStringLiteral("separate"),
                    fileName,
                    result ? QStringLiteral("written") : QStringLiteral("partial")
                });
            }
        }
        catch (const pdf::PDFException &exception)
        {
            ++failedWrites;
            reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("operation.failed"), exception.getMessage());
            if (options.executionContext)
            {
                options.executionContext->addOutput({
                    QStringLiteral("file"),
                    QStringLiteral("separate"),
                    fileName,
                    QStringLiteral("partial")
                });
            }
        }
    }

    return failedWrites > 0 ? PDFToolExitCode::PartialOutput : PDFToolExitCode::Success;
}

PDFToolAbstractApplication::Options PDFToolSeparate::getOptionsFlags() const
{
    return ConsoleFormat | OpenDocument | PageSelector | Separate | DestructiveWrite;
}


}   // namespace pdftool
