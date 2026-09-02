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

#include "pdftooladdbleed.h"

#include "pdfbleedfixup.h"
#include "pdfartifactstore.h"
#include "pdfdocumentwriter.h"
#include "pdfoperationhistorystore.h"
#include "pdfoutputformatter.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

namespace pdftool
{

static PDFToolAddBleed s_toolAddBleedApplication;

QString PDFToolAddBleed::getStandardString(PDFToolAbstractApplication::StandardString standardString) const
{
    switch (standardString)
    {
        case Command:
            return "add-bleed";
        case Name:
            return PDFToolTranslationContext::tr("Add Bleed");
        case Description:
            return PDFToolTranslationContext::tr("Expand page boxes and fill bleed margins by mirroring, repeating, or stretching edge pixels.");
        default:
            Q_ASSERT(false);
            break;
    }
    return QString();
}

static QString modeName(pdf::PDFBleedFixupMode mode)
{
    switch (mode)
    {
        case pdf::PDFBleedFixupMode::Mirror:
            return QStringLiteral("mirror");
        case pdf::PDFBleedFixupMode::PixelRepeat:
            return QStringLiteral("pixel-repeat");
        case pdf::PDFBleedFixupMode::Stretch:
            return QStringLiteral("stretch");
    }
    return QStringLiteral("unknown");
}

static QString sideName(pdf::PDFBleedFixupSide side)
{
    switch (side)
    {
        case pdf::PDFBleedFixupSide::Left:
            return QStringLiteral("left");
        case pdf::PDFBleedFixupSide::Right:
            return QStringLiteral("right");
        case pdf::PDFBleedFixupSide::Top:
            return QStringLiteral("top");
        case pdf::PDFBleedFixupSide::Bottom:
            return QStringLiteral("bottom");
    }
    return QStringLiteral("unknown");
}

pdf::PDFOperationResult appendAddBleedProvenance(const QString& outputPath,
                                                 const QByteArray& sourceData,
                                                 const QByteArray& outputData,
                                                 const pdf::PDFBleedFixupSettings& settings,
                                                 const pdf::PDFBleedFixupReport& report)
{
    const QString historyDirectory = QFileInfo(outputPath).absoluteFilePath() + QStringLiteral(".loop-history");
    pdf::PDFArtifactStore artifacts(historyDirectory);
    const auto input = artifacts.importBytes(sourceData,
                                             { QStringLiteral("application/pdf"), QStringLiteral("original-input.pdf") });
    const auto output = artifacts.importBytes(outputData,
                                              { QStringLiteral("application/pdf"), QStringLiteral("add-bleed-output.pdf") });
    if (!input.success || !output.success)
    {
        return pdf::PDFOperationResult(input.success ? output.errorMessage : input.errorMessage);
    }

    pdf::PDFOperationHistoryStore history(QDir(historyDirectory).filePath(QStringLiteral("history.sqlite3")));
    QString historyError;
    if (!history.open(&historyError) || !history.registerOriginalInput(input.artifact) || !history.registerArtifact(output.artifact))
    {
        return pdf::PDFOperationResult(historyError.isEmpty()
                                           ? QStringLiteral("Could not open add-bleed history.")
                                           : historyError);
    }

    const QString revisionDigest = QString::fromLatin1(QCryptographicHash::hash(sourceData, QCryptographicHash::Sha256).toHex());
    const QJsonObject parameters{
        { QStringLiteral("mode"), modeName(settings.mode) },
        { QStringLiteral("bleed_mm"), settings.bleedMM.left() },
        { QStringLiteral("force"), settings.force }
    };
    pdf::PDFOperationHistoryExecution execution;
    execution.operationId = QStringLiteral("add-bleed");
    execution.operationVersion = 1;
    execution.input = input.artifact;
    execution.parameters = parameters;
    QUuid executionId;
    if (!history.beginExecution(execution, &executionId))
    {
        return pdf::PDFOperationResult(QStringLiteral("Could not begin add-bleed history."));
    }

    pdf::PDFOperationHistoryEvent running;
    running.executionId = executionId;
    running.kind = pdf::PDFOperationHistoryEventKind::FixApplied;
    running.status = pdf::PDFOperationHistoryStatus::Running;
    running.documentRevisionDigest = revisionDigest;
    running.operatorIdentity = QStringLiteral("PdfTool");
    if (!history.appendEvent(running))
    {
        return pdf::PDFOperationResult(QStringLiteral("Could not append add-bleed history start."));
    }

    pdf::PDFOperationHistoryEvent accepted;
    accepted.executionId = executionId;
    accepted.kind = pdf::PDFOperationHistoryEventKind::FixApplied;
    accepted.status = pdf::PDFOperationHistoryStatus::Accepted;
    accepted.documentRevisionDigest = revisionDigest;
    accepted.operatorIdentity = QStringLiteral("PdfTool");
    accepted.output = output.artifact;
    accepted.resultSummary = QJsonObject{
        { QStringLiteral("command"), QStringLiteral("add-bleed") },
        { QStringLiteral("pages_changed"), report.pages.size() },
        { QStringLiteral("output_sha256"), output.artifact.sha256 }
    };
    accepted.approval.kind = pdf::PDFApprovalKind::Policy;
    accepted.approval.actorId = QStringLiteral("PdfTool");
    accepted.approval.decision = QStringLiteral("approve");
    accepted.approval.policyId = QStringLiteral("add-bleed");
    accepted.approval.rationale = QStringLiteral("Add-bleed output was written by the explicit save policy.");
    accepted.approval.decidedUtc = QDateTime::currentDateTimeUtc();
    if (!history.appendEvent(accepted))
    {
        return pdf::PDFOperationResult(QStringLiteral("Could not append add-bleed history result."));
    }

    return true;
}

static void writeReport(const PDFToolOptions& options,
                        const pdf::PDFBleedFixupSettings& settings,
                        const pdf::PDFBleedFixupReport& report)
{
    PDFOutputFormatter formatter(options.outputStyle);
    formatter.beginDocument("add-bleed", PDFToolTranslationContext::tr("Add bleed report"));
    formatter.endl();

    formatter.beginTable("settings", PDFToolTranslationContext::tr("Settings"));
    formatter.beginTableHeaderRow("header");
    formatter.writeTableHeaderColumn("key", PDFToolTranslationContext::tr("Key"), Qt::AlignLeft);
    formatter.writeTableHeaderColumn("value", PDFToolTranslationContext::tr("Value"), Qt::AlignLeft);
    formatter.endTableHeaderRow();

    auto writeSetting = [&](const QString& key, const QString& value)
    {
        formatter.beginTableRow("row");
        formatter.writeTableColumn("key", key, Qt::AlignLeft);
        formatter.writeTableColumn("value", value, Qt::AlignLeft);
        formatter.endTableRow();
    };

    writeSetting(QStringLiteral("mode"), modeName(settings.mode));
    writeSetting(QStringLiteral("dpi"), QString::number(settings.dpi));
    writeSetting(QStringLiteral("sample-pixels"), QString::number(settings.samplePixels));
    writeSetting(QStringLiteral("bleed-mm"), QStringLiteral("%1,%2,%3,%4")
                                                 .arg(settings.bleedMM.left())
                                                 .arg(settings.bleedMM.top())
                                                 .arg(settings.bleedMM.right())
                                                 .arg(settings.bleedMM.bottom()));
    writeSetting(QStringLiteral("force"), settings.force ? QStringLiteral("true") : QStringLiteral("false"));
    writeSetting(QStringLiteral("dry-run"), options.destructiveDryRun ? QStringLiteral("true") : QStringLiteral("false"));
    formatter.endTable();
    formatter.endl();

    formatter.beginTable("pages", PDFToolTranslationContext::tr("Pages"));
    formatter.beginTableHeaderRow("header");
    formatter.writeTableHeaderColumn("page", PDFToolTranslationContext::tr("Page"), Qt::AlignLeft);
    formatter.writeTableHeaderColumn("sides", PDFToolTranslationContext::tr("Sides applied"), Qt::AlignLeft);
    formatter.writeTableHeaderColumn("media", PDFToolTranslationContext::tr("MediaBox"), Qt::AlignLeft);
    formatter.writeTableHeaderColumn("bleed", PDFToolTranslationContext::tr("BleedBox"), Qt::AlignLeft);
    formatter.writeTableHeaderColumn("trim", PDFToolTranslationContext::tr("TrimBox"), Qt::AlignLeft);
    formatter.writeTableHeaderColumn("notes", PDFToolTranslationContext::tr("Notes"), Qt::AlignLeft);
    formatter.endTableHeaderRow();

    auto formatRect = [](const QRectF& rect)
    {
        return QStringLiteral("[%1 %2 %3 %4]")
            .arg(rect.left(), 0, 'f', 2)
            .arg(rect.bottom(), 0, 'f', 2)
            .arg(rect.right(), 0, 'f', 2)
            .arg(rect.top(), 0, 'f', 2);
    };

    for (const pdf::PDFBleedFixupPageReport& page : report.pages)
    {
        QStringList sides;
        for (pdf::PDFBleedFixupSide side : page.sidesApplied)
        {
            sides.append(sideName(side));
        }

        formatter.beginTableRow("page");
        formatter.writeTableColumn("page", QString::number(page.pageIndex + 1), Qt::AlignLeft);
        formatter.writeTableColumn("sides", sides.isEmpty() ? QStringLiteral("-") : sides.join(QStringLiteral(",")), Qt::AlignLeft);
        formatter.writeTableColumn("media", QStringLiteral("%1 -> %2").arg(formatRect(page.originalMediaBox), formatRect(page.newMediaBox)), Qt::AlignLeft);
        formatter.writeTableColumn("bleed", QStringLiteral("%1 -> %2").arg(formatRect(page.originalBleedBox), formatRect(page.newBleedBox)), Qt::AlignLeft);
        formatter.writeTableColumn("trim", QStringLiteral("%1 -> %2").arg(formatRect(page.originalTrimBox), formatRect(page.newTrimBox)), Qt::AlignLeft);
        formatter.writeTableColumn("notes", page.skipReasons.join(QStringLiteral("; ")), Qt::AlignLeft);
        formatter.endTableRow();
    }

    formatter.endTable();
    formatter.endDocument();
    if (options.outputStyle == PDFOutputFormatter::Style::Json)
    {
        if (options.executionContext)
        {
            options.executionContext->setData(formatter.getJsonObject());
        }
    }
    else
    {
        PDFConsole::writeText(formatter.getString(), options.outputCodec);
    }
}

PDFToolExitCode PDFToolAddBleed::execute(const PDFToolOptions& options)
{
    if (!options.destructiveDryRun && options.addBleedOutputDocument.isEmpty())
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("cli.invalid-arguments"), PDFToolTranslationContext::tr("Output document file name is not set. Use -o/--output or --dry-run."));
        return PDFToolExitCode::InvalidInvocation;
    }

    pdf::PDFDocument document;
    QByteArray sourceData;
    if (!readDocument(options, document, &sourceData, false))
    {
        return PDFToolExitCode::InputError;
    }

    pdf::PDFBleedFixupSettings settings = options.addBleedSettings;

    // A dry run reports geometry only; skip the page raster it would otherwise
    // build for edge sampling.
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

    pdf::PDFBleedFixupReport report;
    const pdf::PDFOperationResult result = pdf::PDFBleedFixup::apply(&document, settings, &report);
    if (!result)
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("operation.failed"), result.getErrorMessage());
        return PDFToolExitCode::ProcessingFailure;
    }

    if (options.outputStyle == PDFOutputFormatter::Style::Json ||
        options.destructiveReport || options.destructiveDryRun)
    {
        writeReport(options, settings, report);
    }

    if (options.destructiveDryRun)
    {
        if (options.executionContext)
        {
            options.executionContext->addOutput({ QStringLiteral("file"),
                                                  QStringLiteral("primary"),
                                                  options.addBleedOutputDocument,
                                                  QStringLiteral("planned") });
        }
        return PDFToolExitCode::Success;
    }

    if (const PDFToolExitCode blocked = validateDestructiveOutput(options, options.addBleedOutputDocument); blocked != PDFToolExitCode::Success)
    {
        return blocked;
    }

    pdf::PDFDocumentWriter writer(nullptr);
    const pdf::PDFOperationResult writeResult = writer.write(options.addBleedOutputDocument, &document, true);
    if (!writeResult)
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("output.write-failed"), PDFToolTranslationContext::tr("Failed to write output document. %1").arg(writeResult.getErrorMessage()), QJsonObject{ { QStringLiteral("path"), options.addBleedOutputDocument } });
        return PDFToolExitCode::ProcessingFailure;
    }

    QFile outputFile(options.addBleedOutputDocument);
    if (!outputFile.open(QIODevice::ReadOnly))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("history.output-unreadable"),
                         PDFToolTranslationContext::tr("The add-bleed output could not be reopened for history."));
        return PDFToolExitCode::ProcessingFailure;
    }
    const QByteArray outputData = outputFile.readAll();
    outputFile.close();
    if (const pdf::PDFOperationResult provenance = appendAddBleedProvenance(options.addBleedOutputDocument,
                                                                            sourceData,
                                                                            outputData,
                                                                            settings,
                                                                            report);
        !provenance)
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("history.write-failed"),
                         provenance.getErrorMessage());
        return PDFToolExitCode::ProcessingFailure;
    }

    if (options.executionContext)
    {
        options.executionContext->addOutput({ QStringLiteral("file"),
                                              QStringLiteral("primary"),
                                              options.addBleedOutputDocument,
                                              QStringLiteral("written") });
    }

    return PDFToolExitCode::Success;
}

PDFToolAbstractApplication::Options PDFToolAddBleed::getOptionsFlags() const
{
    return ConsoleFormat | OpenDocument | PageSelector | ColorManagementSystem | AddBleed | DestructiveWrite;
}

}   // namespace pdftool
