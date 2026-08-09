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

#include "pdftoolpreflight.h"

#include "pdfdocumentsession.h"
#include "preflightengine.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace pdftool
{

namespace
{

static PDFToolPreflightApplication s_preflightApplication;

bool loadProfileJson(const QString& profilePath, QJsonObject& profile, QString& errorMessage)
{
    QFile profileFile(profilePath);
    if (!profileFile.open(QIODevice::ReadOnly))
    {
        errorMessage = PDFToolTranslationContext::tr("Cannot open profile '%1'.").arg(profilePath);
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(profileFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        errorMessage = PDFToolTranslationContext::tr("Invalid profile JSON in '%1': %2").arg(profilePath, parseError.errorString());
        return false;
    }

    profile = document.object();
    return true;
}

} // namespace

QString PDFToolPreflightApplication::getStandardString(StandardString standardString) const
{
    switch (standardString)
    {
        case Command:
            return QStringLiteral("preflight");

        case Name:
            return PDFToolTranslationContext::tr("Preflight");

        case Description:
            return PDFToolTranslationContext::tr("Run Loupe preflight checks and emit a normalized JSON report.");

        default:
            Q_ASSERT(false);
            break;
    }

    return QString();
}

PDFToolExitCode PDFToolPreflightApplication::execute(const PDFToolOptions& options)
{
    if (options.outputStyle != PDFOutputFormatter::Style::Json)
    {
        reportDiagnostic(options,
                         PDFToolDiagnosticSeverity::Error,
                         QStringLiteral("cli.invalid-arguments"),
                         PDFToolTranslationContext::tr("The preflight command only supports JSON output."));
        return PDFToolExitCode::InvalidInvocation;
    }

    if (options.document.isEmpty())
    {
        reportDiagnostic(options,
                         PDFToolDiagnosticSeverity::Error,
                         QStringLiteral("cli.invalid-arguments"),
                         PDFToolTranslationContext::tr("No document specified."));
        return PDFToolExitCode::InputError;
    }

    if (options.preflightProfilePath.isEmpty())
    {
        reportDiagnostic(options,
                         PDFToolDiagnosticSeverity::Error,
                         QStringLiteral("cli.invalid-arguments"),
                         PDFToolTranslationContext::tr("No profile specified. Use --profile <file.json>."));
        return PDFToolExitCode::InvalidInvocation;
    }

    QJsonObject profileJson;
    QString profileError;
    if (!loadProfileJson(options.preflightProfilePath, profileJson, profileError))
    {
        reportDiagnostic(options,
                         PDFToolDiagnosticSeverity::Error,
                         QStringLiteral("cli.invalid-arguments"),
                         profileError);
        return PDFToolExitCode::InvalidInvocation;
    }

    pdf::PDFDocument document;
    QByteArray sourceData;
    if (!readDocument(options, document, &sourceData, false))
    {
        return PDFToolExitCode::InputError;
    }

    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);

    pdf::PreflightResult result = engine.run(profileJson);

    if (options.executionContext)
    {
        options.executionContext->setData(QJsonObject{
            { QStringLiteral("report"), result.toJson(options.document) }
        });
    }

    return result.pass ? PDFToolExitCode::Success : PDFToolExitCode::Findings;
}

PDFToolAbstractApplication::Options PDFToolPreflightApplication::getOptionsFlags() const
{
    return ConsoleFormat | OpenDocument | PreflightProfile;
}

}   // namespace pdftool
