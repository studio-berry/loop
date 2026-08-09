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

#include "pdftoolverifyredaction.h"

#include "pdfdocumentreader.h"
#include "pdfredactverifier.h"

#include <QJsonObject>

namespace pdftool
{

static PDFToolVerifyRedaction s_verifyRedactionApplication;

QString PDFToolVerifyRedaction::getStandardString(PDFToolAbstractApplication::StandardString standardString) const
{
    switch (standardString)
    {
        case Command:
            return QStringLiteral("verify-redaction");

        case Name:
            return PDFToolTranslationContext::tr("Verify Redaction");

        case Description:
            return PDFToolTranslationContext::tr("Verify that a redacted document contains no recoverable redacted content.");

        default:
            Q_ASSERT(false);
            break;
    }

    return QString();
}

PDFToolExitCode PDFToolVerifyRedaction::execute(const PDFToolOptions& options)
{
    if (options.verifyRedactionFiles.size() != 2)
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("cli.invalid-arguments"), PDFToolTranslationContext::tr("Exactly two documents must be specified: original and redacted."));
        return PDFToolExitCode::InvalidInvocation;
    }

    pdf::PDFDocumentReader reader(nullptr, [](bool* ok) -> QString { *ok = true; return QString(); }, true, false);
    const pdf::PDFDocument originalDocument = reader.readFromFile(options.verifyRedactionFiles.front());
    if (reader.getReadingResult() != pdf::PDFDocumentReader::Result::OK)
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("pdf.document-unreadable"), PDFToolTranslationContext::tr("Failed to read original document: %1").arg(reader.getErrorMessage()), QJsonObject{{QStringLiteral("path"), options.verifyRedactionFiles.front()}});
        return PDFToolExitCode::InputError;
    }

    const pdf::PDFDocument redactedDocument = reader.readFromFile(options.verifyRedactionFiles.back());
    if (reader.getReadingResult() != pdf::PDFDocumentReader::Result::OK)
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("pdf.document-unreadable"), PDFToolTranslationContext::tr("Failed to read redacted document: %1").arg(reader.getErrorMessage()), QJsonObject{{QStringLiteral("path"), options.verifyRedactionFiles.back()}});
        return PDFToolExitCode::InputError;
    }

    pdf::PDFRedactVerificationSettings settings;
    settings.redactOptions = options.verifyRedactionOptions;
    settings.checkIncrementalRewrite = options.verifyRedactionCheckIncremental;

    const pdf::PDFRedactVerification verification = pdf::PDFRedactVerifier::verify(&originalDocument,
                                                                                     &redactedDocument,
                                                                                     settings,
                                                                                     options.verifyRedactionFiles.back());

    if (verification.passed())
    {
        if (options.outputStyle == PDFOutputFormatter::Style::Json)
        {
            if (options.executionContext)
            {
                options.executionContext->setData(QJsonObject{
                    { QStringLiteral("passed"), true }
                });
            }
        }
        else
        {
            PDFConsole::writeText(QStringLiteral("pass"), options.outputCodec);
        }
        return PDFToolExitCode::Success;
    }

    for (const pdf::PDFRedactVerificationIssue& issue : verification.issues)
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("verification.failed"), QStringLiteral("%1: %2").arg(issue.checkId, issue.message), QJsonObject{{QStringLiteral("check_id"), issue.checkId}});
    }

    return PDFToolExitCode::Findings;
}

PDFToolAbstractApplication::Options PDFToolVerifyRedaction::getOptionsFlags() const
{
    return ConsoleFormat | VerifyRedaction;
}

}   // namespace pdftool
