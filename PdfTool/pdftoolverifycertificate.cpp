#include "pdftoolverifycertificate.h"

#include "pdfpreflightcertificate.h"
#include "pdfoperationhistorystore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>

namespace pdftool
{

namespace
{

PDFToolVerifyCertificate s_verifyCertificateApplication;

bool readJson(const QString& path, QJsonObject& object, QString& error)
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

}   // namespace

QString PDFToolVerifyCertificate::getStandardString(StandardString standardString) const
{
    switch (standardString)
    {
        case Command:
            return QStringLiteral("verify-certificate");
        case Name:
            return PDFToolTranslationContext::tr("Verify certified preflight");
        case Description:
            return PDFToolTranslationContext::tr("Verify a certified-preflight JSON against a PDF and its audit history.");
    }
    return {};
}

PDFToolExitCode PDFToolVerifyCertificate::execute(const PDFToolOptions& options)
{
    if (options.preflightCertificatePath.isEmpty() || options.document.isEmpty())
    {
        reportDiagnostic(options,
                         PDFToolDiagnosticSeverity::Error,
                         QStringLiteral("cli.invalid-arguments"),
                         PDFToolTranslationContext::tr("A certificate and a document are required."));
        return PDFToolExitCode::InvalidInvocation;
    }

    QJsonObject certificateJson;
    QString error;
    if (!readJson(options.preflightCertificatePath, certificateJson, error))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("certificate.input-invalid"), error);
        return PDFToolExitCode::InputError;
    }

    pdf::PreflightCertificate certificate;
    if (!pdf::PreflightCertificate::fromJson(certificateJson, certificate, error))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("certificate.input-invalid"), error);
        return PDFToolExitCode::InputError;
    }

    QFile documentFile(options.document);
    if (!documentFile.open(QIODevice::ReadOnly))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("pdf.document-unreadable"), documentFile.errorString());
        return PDFToolExitCode::InputError;
    }
    const QByteArray documentBytes = documentFile.readAll();
    const QString historyDirectory = QFileInfo(options.document).absoluteFilePath() + QStringLiteral(".loop-history");
    const QString historyPath = QDir(historyDirectory).filePath(QStringLiteral("history.sqlite3"));
    QList<pdf::PDFOperationHistoryEvent> events;
    if (QFileInfo::exists(historyPath))
    {
        pdf::PDFOperationHistoryStore history(historyPath);
        if (!history.open(&error))
        {
            reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("certificate.audit-unavailable"), error);
            return PDFToolExitCode::InputError;
        }
        events = history.events(&error);
        if (!error.isEmpty())
        {
            reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("certificate.audit-unavailable"), error);
            return PDFToolExitCode::InputError;
        }
    }

    const pdf::PreflightCertificateVerification verification = pdf::verifyPreflightCertificate(certificate, documentBytes, events);
    if (options.executionContext)
    {
        options.executionContext->setData(QJsonObject{ { QStringLiteral("verification"), verification.toJson() } });
    }
    if (options.outputStyle != PDFOutputFormatter::Style::Json)
    {
        PDFConsole::writeData(verification.reason.toUtf8());
        PDFConsole::writeData(QByteArray("\n"));
    }
    return verification.isValid() ? PDFToolExitCode::Success : PDFToolExitCode::Findings;
}

PDFToolAbstractApplication::Options PDFToolVerifyCertificate::getOptionsFlags() const
{
    return ConsoleFormat | VerifyPreflightCertificate;
}

}   // namespace pdftool
