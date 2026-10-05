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

#include "pdftoolrollback.h"

#include "pdfartifactstore.h"
#include "pdfgovernedexecution.h"
#include "pdfoperationhistorystore.h"
#include "pdfsafefilewriter.h"
#include "preflightengine.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace pdftool
{

namespace
{

bool writeRollbackReport(const QString& path, const QJsonObject& report)
{
    if (path.isEmpty())
    {
        return true;
    }
    return static_cast<bool>(pdf::PDFSafeFileWriter::writeData(path,
                                                               QJsonDocument(report).toJson(QJsonDocument::Indented),
                                                               pdf::PDFSafeFileWriter::OverwritePolicy::Overwrite));
}

QString fileSha256(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return QString();
    }
    return QString::fromLatin1(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex());
}

}   // namespace

QString PDFToolRollback::getStandardString(StandardString standardString) const
{
    switch (standardString)
    {
        case Command:
            return QStringLiteral("rollback");
        case Name:
            return PDFToolTranslationContext::tr("Rollback");
        case Description:
            return PDFToolTranslationContext::tr("Restore a recorded revision as a new, revalidated governed publication.");
    }
    return QString();
}

PDFToolExitCode PDFToolRollback::execute(const PDFToolOptions& options)
{
    // The command advertises JSON-only output, so a different console format is an
    // invalid invocation rather than a silent text rendering of the payload.
    if (options.outputStyle != PDFOutputFormatter::Style::Json)
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("cli.invalid-arguments"),
                         PDFToolTranslationContext::tr("The rollback command only supports JSON output."));
        return PDFToolExitCode::InvalidInvocation;
    }

    if (options.rollbackFiles.size() != 1 || options.rollbackTargetSha256.isEmpty() ||
        options.rollbackOutputDocument.isEmpty() || options.rollbackProfilePath.isEmpty())
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("cli.invalid-arguments"),
                         PDFToolTranslationContext::tr("rollback requires one document, --to <sha256>, --output <file>, and --profile <profile>."));
        return PDFToolExitCode::InvalidInvocation;
    }

    const auto pathIdentity = [](const QString& path)
    {
        const QFileInfo info(path);
        const QString canonical = info.canonicalFilePath();
        const QString parent = info.dir().canonicalPath();
        const QString absolute = canonical.isEmpty()
                                     ? QDir::cleanPath(parent.isEmpty() ? info.absoluteFilePath() : QDir(parent).filePath(info.fileName()))
                                     : canonical;
#ifdef Q_OS_WIN
        return absolute.toCaseFolded();
#else
        return absolute;
#endif
    };
    const QString inputPath = pathIdentity(options.rollbackFiles.first());
    const QString outputPath = pathIdentity(options.rollbackOutputDocument);
    const QString profilePath = pathIdentity(options.rollbackProfilePath);
    const QString reportPath = options.rollbackReportFile.isEmpty() ? QString() : pathIdentity(options.rollbackReportFile);
    if (outputPath == inputPath || outputPath == profilePath ||
        (!reportPath.isEmpty() && (reportPath == inputPath || reportPath == outputPath || reportPath == profilePath)))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("output.path-conflict"),
                         PDFToolTranslationContext::tr("Rollback input, output, profile, and report paths must be distinct."));
        return PDFToolExitCode::InvalidInvocation;
    }

    QJsonObject profile;
    QString profileError;
    if (!pdf::PreflightEngine::loadProfile(options.rollbackProfilePath, profile, profileError))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("preflight.profile-invalid"), profileError,
                         QJsonObject{ { QStringLiteral("path"), options.rollbackProfilePath } });
        return PDFToolExitCode::InvalidInvocation;
    }

    const QString documentPath = options.rollbackFiles.first();
    const QString currentSha256 = fileSha256(documentPath);
    if (currentSha256.isEmpty())
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("pdf.document-unreadable"),
                         PDFToolTranslationContext::tr("Could not read the document to roll back."),
                         QJsonObject{ { QStringLiteral("path"), documentPath } });
        return PDFToolExitCode::InputError;
    }

    const QString historyDirectory = QFileInfo(documentPath).absoluteFilePath() + QStringLiteral(".loop-history");
    pdf::PDFOperationHistoryStore history(QDir(historyDirectory).filePath(QStringLiteral("history.sqlite3")));
    QString historyError;
    if (!history.open(&historyError))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("history.open-failed"),
                         historyError.isEmpty() ? PDFToolTranslationContext::tr("Could not open the document operation history.") : historyError,
                         QJsonObject{ { QStringLiteral("sidecar"), historyDirectory } });
        return PDFToolExitCode::ProcessingFailure;
    }

    // The target execution is derived from the recorded chain: a rollback names a
    // revision that is the accepted output of a real execution, never a caller id.
    QUuid targetExecutionId;
    for (const pdf::PDFOperationHistoryEvent& event : history.events(&historyError))
    {
        if (event.status == pdf::PDFOperationHistoryStatus::Accepted && event.output.has_value() &&
            event.output->sha256.compare(options.rollbackTargetSha256, Qt::CaseInsensitive) == 0)
        {
            targetExecutionId = event.executionId;
            break;
        }
    }
    if (!historyError.isEmpty())
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("history.read-failed"), historyError);
        return PDFToolExitCode::ProcessingFailure;
    }
    if (targetExecutionId.isNull())
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("rollback.target-not-found"),
                         PDFToolTranslationContext::tr("No accepted revision with digest '%1' is recorded for this document.").arg(options.rollbackTargetSha256),
                         QJsonObject{ { QStringLiteral("target"), options.rollbackTargetSha256 } });
        return PDFToolExitCode::InputError;
    }

    pdf::PDFRollbackRequest request;
    request.currentArtifactSha256 = currentSha256;
    request.targetArtifactSha256 = options.rollbackTargetSha256.toLower();
    request.targetExecutionId = targetExecutionId;
    request.reason = options.rollbackReason;
    request.approval.kind = pdf::PDFApprovalKind::Policy;
    request.approval.actorId = QStringLiteral("PdfTool");
    request.approval.decision = QStringLiteral("approve");
    request.approval.policyId = QStringLiteral("cli-rollback");
    request.approval.rationale = request.reason.isEmpty()
                                     ? QStringLiteral("The operator returned the document to a recorded revision.")
                                     : request.reason;
    request.approval.evidenceSha256 = request.targetArtifactSha256;
    request.approval.decisionReference = QStringLiteral("cli-rollback:%1").arg(request.targetArtifactSha256);
    request.approval.decidedUtc = QDateTime::currentDateTimeUtc();
    request.profile = profile;
    request.signOffActor = QStringLiteral("PdfTool");
    request.signOffPolicy = QStringLiteral("cli-rollback-postflight");

    pdf::PDFArtifactStore artifacts(historyDirectory);
    const pdf::PDFOperationResult rollback = history.rollbackTo(request, artifacts, options.rollbackOutputDocument);
    if (!rollback)
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("rollback.failed"), rollback.getErrorMessage(),
                         QJsonObject{ { QStringLiteral("target"), request.targetArtifactSha256 },
                                      { QStringLiteral("output"), options.rollbackOutputDocument } });
        return PDFToolExitCode::ProcessingFailure;
    }

    // The governed receipt is read back from the event the store appended; the CLI
    // never re-derives the revalidation or sign-off it is reporting.
    QJsonObject governed;
    const QList<pdf::PDFOperationHistoryEvent> appended = history.events();
    for (auto it = appended.crbegin(); it != appended.crend(); ++it)
    {
        if (it->status == pdf::PDFOperationHistoryStatus::RolledBack)
        {
            governed = it->resultSummary.value(QStringLiteral("governed")).toObject();
            break;
        }
    }

    const QJsonObject reportJson{
        { QStringLiteral("status"), QStringLiteral("rolled-back") },
        { QStringLiteral("approval"), governed.value(QStringLiteral("approval")) },
        { QStringLiteral("revalidation"), governed.value(QStringLiteral("revalidation")) },
        { QStringLiteral("sign_off"), governed.value(QStringLiteral("sign_off")) },
        { QStringLiteral("target"), QJsonObject{ { QStringLiteral("sha256"), request.targetArtifactSha256 } } },
        { QStringLiteral("output"), QJsonObject{ { QStringLiteral("path"), options.rollbackOutputDocument },
                                                 { QStringLiteral("sha256"), fileSha256(options.rollbackOutputDocument) } } },
        { QStringLiteral("history"), QJsonObject{ { QStringLiteral("sidecar"), historyDirectory },
                                                  { QStringLiteral("database"), history.databasePath() } } }
    };

    const bool reportWritten = writeRollbackReport(options.rollbackReportFile, reportJson);

    if (options.executionContext)
    {
        options.executionContext->setData(reportJson);
        options.executionContext->addOutput({ QStringLiteral("file"), QStringLiteral("primary"), options.rollbackOutputDocument, QStringLiteral("written") });
        if (reportWritten && !options.rollbackReportFile.isEmpty())
        {
            options.executionContext->addOutput({ QStringLiteral("file"), QStringLiteral("report"), options.rollbackReportFile, QStringLiteral("written") });
        }
    }

    if (!reportWritten)
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("rollback.report-write-failed"),
                         PDFToolTranslationContext::tr("The rollback was published, but its report could not be written."),
                         QJsonObject{ { QStringLiteral("path"), options.rollbackReportFile } });
        return PDFToolExitCode::ProcessingFailure;
    }

    const pdf::PDFHistoryRetentionResult retention = history.enforceRetention({}, artifacts);
    if (!retention.success)
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("history.retention-failed"),
                         PDFToolTranslationContext::tr("The rollback was recorded, but retention could not be enforced: %1").arg(retention.errorMessage));
        return PDFToolExitCode::ProcessingFailure;
    }
    return PDFToolExitCode::Success;
}

PDFToolAbstractApplication::Options PDFToolRollback::getOptionsFlags() const
{
    return ConsoleFormat | OpenDocument | Rollback;
}

static PDFToolRollback s_rollbackApplication;

}   // namespace pdftool
