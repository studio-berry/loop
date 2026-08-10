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
#include "preflightprofileresolver.h"
#include "preflightengine.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace pdftool
{

namespace
{

static PDFToolPreflightApplication s_preflightApplication;

bool loadProfileJson(const QString& profilePath, QJsonObject& profile, QString& errorMessage)
{
    return pdf::PreflightEngine::loadProfile(profilePath, profile, errorMessage);
}

bool loadJobContext(const QString& contextPath, pdf::PreflightJobContext& context, QString& errorMessage)
{
    QFile contextFile(contextPath);
    if (!contextFile.open(QIODevice::ReadOnly))
    {
        errorMessage = PDFToolTranslationContext::tr("Cannot open job context '%1'.").arg(contextPath);
        return false;
    }
    if (contextFile.size() > 1024 * 1024)
    {
        errorMessage = PDFToolTranslationContext::tr("Job context '%1' exceeds the maximum supported size.").arg(contextPath);
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(contextFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        errorMessage = PDFToolTranslationContext::tr("Invalid job context JSON in '%1': %2").arg(contextPath, parseError.errorString());
        return false;
    }

    QJsonObject object = document.object();
    if (object.value(QStringLiteral("context")).isObject())
    {
        object = object.value(QStringLiteral("context")).toObject();
    }
    return pdf::PreflightJobContext::fromJson(object, context, errorMessage);
}

bool hasDirectContext(const PDFToolOptions& options)
{
    return !options.preflightClientId.isEmpty()
        || !options.preflightProductId.isEmpty()
        || !options.preflightJobType.isEmpty()
        || !options.preflightPressId.isEmpty()
        || !options.preflightStockId.isEmpty()
        || !options.preflightFinishingId.isEmpty();
}

QString defaultProfileStorePath()
{
    const QStringList candidates = {
        QDir::current().filePath(QStringLiteral("loupe-preflight/profiles")),
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../share/loupe/profiles")),
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("profiles"))
    };
    for (const QString& candidate : candidates)
    {
        if (QFileInfo(candidate).isDir())
        {
            return QDir::cleanPath(candidate);
        }
    }
    return {};
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

    const bool hasContextInput = !options.preflightJobContextPath.isEmpty()
        || hasDirectContext(options)
        || !options.preflightProfileStorePath.isEmpty();
    if (!options.preflightProfilePath.isEmpty() && hasContextInput)
    {
        reportDiagnostic(options,
                         PDFToolDiagnosticSeverity::Error,
                         QStringLiteral("cli.invalid-arguments"),
                         PDFToolTranslationContext::tr("--profile cannot be combined with contextual profile selection. Use --profile alone or provide context and a profile store."));
        return PDFToolExitCode::InvalidInvocation;
    }

    QJsonObject profileJson;
    pdf::PreflightResolvedProfile resolved;
    pdf::PreflightProfileResolver resolver;
    QString profileError;
    if (!options.preflightProfilePath.isEmpty())
    {
        if (!loadProfileJson(options.preflightProfilePath, profileJson, profileError))
        {
            reportDiagnostic(options,
                             PDFToolDiagnosticSeverity::Error,
                             QStringLiteral("cli.invalid-arguments"),
                             profileError);
            return PDFToolExitCode::InvalidInvocation;
        }
        resolved = resolver.resolveExplicitProfile(profileJson,
                                                   QFileInfo(options.preflightProfilePath).completeBaseName(),
                                                   QStringLiteral("explicit"));
    }
    else
    {
        pdf::PreflightJobContext context;
        if (!options.preflightJobContextPath.isEmpty()
            && !loadJobContext(options.preflightJobContextPath, context, profileError))
        {
            reportDiagnostic(options,
                             PDFToolDiagnosticSeverity::Error,
                             QStringLiteral("cli.invalid-arguments"),
                             profileError);
            return PDFToolExitCode::InvalidInvocation;
        }

        auto overrideContext = [](const QString& value, QString& target) {
            if (!value.isEmpty()) target = value;
        };
        overrideContext(options.preflightClientId, context.clientId);
        overrideContext(options.preflightProductId, context.productId);
        overrideContext(options.preflightJobType, context.jobType);
        overrideContext(options.preflightPressId, context.pressId);
        overrideContext(options.preflightStockId, context.stockId);
        overrideContext(options.preflightFinishingId, context.finishingId);

        const QString storePath = options.preflightProfileStorePath.isEmpty()
            ? defaultProfileStorePath()
            : options.preflightProfileStorePath;
        if (storePath.isEmpty())
        {
            profileError = PDFToolTranslationContext::tr("No profile store found. Use --profile-store <directory> or --profile <file.json>.");
            resolved.normalizedContext = context.toJson();
            resolved.errorCode = QStringLiteral("profile-store-missing");
            resolved.errorMessage = profileError;
        }
        else
        {
            pdf::PreflightProfileSnapshot snapshot;
            if (!pdf::PreflightProfileStore::loadDirectory(storePath, snapshot, profileError))
            {
                // Keep the store error as a configuration result below.
                resolved.normalizedContext = context.toJson();
                resolved.errorCode = QStringLiteral("profile-store-invalid");
                resolved.errorMessage = profileError;
            }
            else
            {
                resolved = resolver.resolve(context, snapshot);
            }
        }
    }

    if (!resolved.ok)
    {
        pdf::PreflightResult result;
        result.pass = false;
        result.inspectionComplete = false;
        result.profileName = QStringLiteral("Unresolved profile");
        pdf::PreflightFinding finding;
        finding.scope = QString::fromLatin1(pdf::PREFLIGHT_FINDING_SCOPE_DOCUMENT);
        finding.type = QStringLiteral("profile-resolution");
        finding.severity = QStringLiteral("error");
        finding.message = profileError.isEmpty() ? resolved.errorMessage : profileError;
        finding.checkId = QStringLiteral("profile-resolution");
        finding.evidence = QJsonObject{
            { QStringLiteral("code"), profileError.isEmpty() ? resolved.errorCode : QStringLiteral("profile-store") }
        };
        result.errors.append(finding);
        result.profileResolution = resolved.provenance();
        if (options.executionContext)
        {
            options.executionContext->setData(QJsonObject{
                { QStringLiteral("report"), result.toJson(options.document) }
            });
        }
        return PDFToolExitCode::Findings;
    }

    profileJson = resolved.effectiveProfile;

    pdf::PDFDocument document;
    QByteArray sourceData;
    if (!readDocument(options, document, &sourceData, false))
    {
        return PDFToolExitCode::InputError;
    }

    pdf::PDFDocumentSession session(&document);
    pdf::PreflightEngine engine(&session);

    pdf::PreflightResult result = engine.run(profileJson);
    result.profileResolution = resolved.provenance();

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
