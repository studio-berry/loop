#include "pdftoolevidencebundle.h"

#include "pdfoperationhistorystore.h"
#include "pdfpreflightcertificate.h"
#include "pdfpreflightevidencebundle.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QCryptographicHash>

namespace pdftool
{

namespace
{

PDFToolExportEvidenceBundle s_exportEvidenceBundleApplication;
PDFToolVerifyEvidenceBundle s_verifyEvidenceBundleApplication;

bool readJsonObject(const QString& path, QJsonObject& object, QString& error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        error = file.errorString();
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        error = parseError.errorString();
        return false;
    }
    object = document.object();
    return true;
}

bool readHistory(const QString& documentPath,
                 QList<pdf::PDFOperationHistoryEvent>& events,
                 QList<pdf::PDFRollbackPoint>& rollbackPoints,
                 QString& error)
{
    const QString historyDirectory = QFileInfo(documentPath).absoluteFilePath() + QStringLiteral(".loop-history");
    const QString historyPath = QDir(historyDirectory).filePath(QStringLiteral("history.sqlite3"));
    if (!QFileInfo::exists(historyPath))
    {
        error = QStringLiteral("The document has no operation-history sidecar, so the bundle has no provenance chain to export.");
        return false;
    }

    pdf::PDFOperationHistoryStore history(historyPath);
    if (!history.open(&error))
    {
        return false;
    }
    events = history.events(&error);
    if (!error.isEmpty())
    {
        return false;
    }
    rollbackPoints = history.rollbackPoints(&error);
    return error.isEmpty();
}

bool readDecisions(const QJsonObject& report, QList<pdf::PreflightDecision>& decisions, QString& error)
{
    for (const QJsonValue& value : report.value(QStringLiteral("decisions")).toArray())
    {
        pdf::PreflightDecision decision;
        if (!pdf::PreflightDecision::fromJson(value.toObject(), decision, error))
        {
            return false;
        }
        decisions.append(decision);
    }
    return true;
}

}   // namespace

QString PDFToolExportEvidenceBundle::getStandardString(StandardString standardString) const
{
    switch (standardString)
    {
        case Command:
            return QStringLiteral("export-evidence-bundle");
        case Name:
            return PDFToolTranslationContext::tr("Export a production handoff evidence bundle");
        case Description:
            return PDFToolTranslationContext::tr("Export the portable proof-of-preflight and sign-off bundle for a document revision.");
    }
    return {};
}

PDFToolExitCode PDFToolExportEvidenceBundle::execute(const PDFToolOptions& options)
{
    if (options.outputStyle != PDFOutputFormatter::Style::Json)
    {
        reportDiagnostic(options,
                         PDFToolDiagnosticSeverity::Error,
                         QStringLiteral("cli.invalid-arguments"),
                         PDFToolTranslationContext::tr("The export-evidence-bundle command only supports JSON output."));
        return PDFToolExitCode::InvalidInvocation;
    }

    if (options.document.isEmpty() || options.evidenceBundleOutputDirectory.isEmpty() ||
        options.evidenceBundleReportPath.isEmpty())
    {
        reportDiagnostic(options,
                         PDFToolDiagnosticSeverity::Error,
                         QStringLiteral("cli.invalid-arguments"),
                         PDFToolTranslationContext::tr("A document, a preflight report and an output directory are required."));
        return PDFToolExitCode::InvalidInvocation;
    }

    QFile documentFile(options.document);
    if (!documentFile.open(QIODevice::ReadOnly))
    {
        reportDiagnostic(options,
                         PDFToolDiagnosticSeverity::Error,
                         QStringLiteral("pdf.document-unreadable"),
                         documentFile.errorString());
        return PDFToolExitCode::InputError;
    }
    const QByteArray documentBytes = documentFile.readAll();
    documentFile.close();

    QString error;
    QJsonObject report;
    if (!readJsonObject(options.evidenceBundleReportPath, report, error))
    {
        reportDiagnostic(options,
                         PDFToolDiagnosticSeverity::Error,
                         QStringLiteral("bundle.report-invalid"),
                         error);
        return PDFToolExitCode::InputError;
    }

    pdf::PreflightEvidenceBundleRequest request;
    request.documentBytes = documentBytes;
    request.report = report;
    request.producer = QStringLiteral("PdfTool");

    if (!readDecisions(report, request.decisions, error))
    {
        reportDiagnostic(options,
                         PDFToolDiagnosticSeverity::Error,
                         QStringLiteral("bundle.report-invalid"),
                         error);
        return PDFToolExitCode::InputError;
    }

    if (!options.evidenceBundleCertificatePath.isEmpty())
    {
        QJsonObject certificateJson;
        if (!readJsonObject(options.evidenceBundleCertificatePath, certificateJson, error))
        {
            reportDiagnostic(options,
                             PDFToolDiagnosticSeverity::Error,
                             QStringLiteral("certificate.input-invalid"),
                             error);
            return PDFToolExitCode::InputError;
        }
        pdf::PreflightCertificate certificate;
        if (!pdf::PreflightCertificate::fromJson(certificateJson, certificate, error))
        {
            reportDiagnostic(options,
                             PDFToolDiagnosticSeverity::Error,
                             QStringLiteral("certificate.input-invalid"),
                             error);
            return PDFToolExitCode::InputError;
        }
        request.certificate = certificate;
    }

    if (!options.evidenceBundleSignOffPath.isEmpty() &&
        !readJsonObject(options.evidenceBundleSignOffPath, request.signOff, error))
    {
        reportDiagnostic(options,
                         PDFToolDiagnosticSeverity::Error,
                         QStringLiteral("bundle.signoff-invalid"),
                         error);
        return PDFToolExitCode::InputError;
    }

    if (!options.evidenceBundleArtifactPath.isEmpty())
    {
        QFile artifactFile(options.evidenceBundleArtifactPath);
        if (!artifactFile.open(QIODevice::ReadOnly))
        {
            reportDiagnostic(options,
                             PDFToolDiagnosticSeverity::Error,
                             QStringLiteral("bundle.artifact-unreadable"),
                             artifactFile.errorString());
            return PDFToolExitCode::InputError;
        }
        const QByteArray artifactBytes = artifactFile.readAll();
        request.output = pdf::PreflightEvidenceBundleOutput{
            QString::fromLatin1(QCryptographicHash::hash(artifactBytes, QCryptographicHash::Sha256).toHex()),
            artifactBytes.size()
        };
    }

    if (!readHistory(options.document, request.history, request.rollbackPoints, error))
    {
        reportDiagnostic(options,
                         PDFToolDiagnosticSeverity::Error,
                         QStringLiteral("bundle.history-unavailable"),
                         error);
        return PDFToolExitCode::InputError;
    }

    pdf::PreflightEvidenceBundle bundle;
    if (!pdf::buildPreflightEvidenceBundle(request, bundle, error))
    {
        reportDiagnostic(options,
                         PDFToolDiagnosticSeverity::Error,
                         QStringLiteral("bundle.refused"),
                         error);
        return PDFToolExitCode::Findings;
    }

    if (!pdf::writePreflightEvidenceBundle(bundle, options.evidenceBundleOutputDirectory, error))
    {
        reportDiagnostic(options,
                         PDFToolDiagnosticSeverity::Error,
                         QStringLiteral("bundle.output-failed"),
                         error);
        return PDFToolExitCode::ProcessingFailure;
    }

    if (options.executionContext)
    {
        options.executionContext->setData(QJsonObject{
            { QStringLiteral("manifest"), bundle.manifest },
            { QStringLiteral("bundle_directory"), QDir(options.evidenceBundleOutputDirectory).absolutePath() },
            { QStringLiteral("member_count"), bundle.members.size() + 1 },
            { QStringLiteral("authority"), QStringLiteral("internal") } });
    }
    return PDFToolExitCode::Success;
}

PDFToolAbstractApplication::Options PDFToolExportEvidenceBundle::getOptionsFlags() const
{
    return ConsoleFormat | OpenDocument | EvidenceBundleExport;
}

QString PDFToolVerifyEvidenceBundle::getStandardString(StandardString standardString) const
{
    switch (standardString)
    {
        case Command:
            return QStringLiteral("verify-evidence-bundle");
        case Name:
            return PDFToolTranslationContext::tr("Verify a production handoff evidence bundle");
        case Description:
            return PDFToolTranslationContext::tr("Verify a portable proof-of-preflight and sign-off bundle offline.");
    }
    return {};
}

PDFToolExitCode PDFToolVerifyEvidenceBundle::execute(const PDFToolOptions& options)
{
    if (options.outputStyle != PDFOutputFormatter::Style::Json)
    {
        reportDiagnostic(options,
                         PDFToolDiagnosticSeverity::Error,
                         QStringLiteral("cli.invalid-arguments"),
                         PDFToolTranslationContext::tr("The verify-evidence-bundle command only supports JSON output."));
        return PDFToolExitCode::InvalidInvocation;
    }

    if (options.evidenceBundlePath.isEmpty())
    {
        reportDiagnostic(options,
                         PDFToolDiagnosticSeverity::Error,
                         QStringLiteral("cli.invalid-arguments"),
                         PDFToolTranslationContext::tr("A bundle directory is required."));
        return PDFToolExitCode::InvalidInvocation;
    }

    if (!QFileInfo(options.evidenceBundlePath).isDir())
    {
        reportDiagnostic(options,
                         PDFToolDiagnosticSeverity::Error,
                         QStringLiteral("bundle.directory-missing"),
                         PDFToolTranslationContext::tr("The bundle directory '%1' does not exist.")
                             .arg(options.evidenceBundlePath));
        return PDFToolExitCode::InputError;
    }

    QString error;
    const pdf::PreflightEvidenceBundleVerification verification =
        pdf::verifyPreflightEvidenceBundle(options.evidenceBundlePath, &error);
    if (options.executionContext)
    {
        options.executionContext->setData(QJsonObject{ { QStringLiteral("verification"), verification.toJson() } });
    }
    return verification.valid ? PDFToolExitCode::Success : PDFToolExitCode::Findings;
}

PDFToolAbstractApplication::Options PDFToolVerifyEvidenceBundle::getOptionsFlags() const
{
    return ConsoleFormat | EvidenceBundleVerify;
}

}   // namespace pdftool
