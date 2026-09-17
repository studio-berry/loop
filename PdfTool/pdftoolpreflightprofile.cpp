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

#include "pdftoolpreflightprofile.h"

#include "preflightprofileresolver.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace pdftool
{

namespace
{

static PDFToolPreflightProfileExportApplication s_preflightProfileExportApplication;
static PDFToolPreflightProfileForkApplication s_preflightProfileForkApplication;
static PDFToolPreflightProfileValidateApplication s_preflightProfileValidateApplication;

bool loadProfileObject(const QString& path, QJsonObject& profile, QString& error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        error = PDFToolTranslationContext::tr("Could not read profile '%1'.").arg(path);
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        error = PDFToolTranslationContext::tr("Profile '%1' is not valid JSON.").arg(path);
        return false;
    }
    profile = document.object();
    return true;
}

bool writeProfileBytes(const QString& path, const QByteArray& bytes, QString& error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        error = PDFToolTranslationContext::tr("Could not write profile '%1'.").arg(path);
        return false;
    }
    if (file.write(bytes) != bytes.size())
    {
        error = PDFToolTranslationContext::tr("Could not write the full profile to '%1'.").arg(path);
        return false;
    }
    return true;
}

}   // namespace

QString PDFToolPreflightProfileExportApplication::getStandardString(StandardString standardString) const
{
    switch (standardString)
    {
        case Command:
            return QStringLiteral("preflight-profile.export");
        case Name:
            return PDFToolTranslationContext::tr("Export Preflight Profile");
        case Description:
            return PDFToolTranslationContext::tr("Validate and export a Loop preflight profile with a canonical content digest.");
        default:
            Q_ASSERT(false);
            break;
    }
    return QString();
}

PDFToolExitCode PDFToolPreflightProfileExportApplication::execute(const PDFToolOptions& options)
{
    if (options.preflightProfilePath.isEmpty() || options.preflightProfileOutputPath.isEmpty())
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("preflight-profile.missing-args"),
                         PDFToolTranslationContext::tr("preflight-profile.export requires --profile and --out."));
        return PDFToolExitCode::InvalidInvocation;
    }

    QJsonObject profile;
    QString error;
    if (!loadProfileObject(options.preflightProfilePath, profile, error))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("preflight-profile.read-failed"), error);
        return PDFToolExitCode::InvalidInvocation;
    }

    const pdf::PreflightProfileImportResult imported = pdf::importPreflightProfile(profile, options.preflightProfilePath);
    if (!imported.ok)
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, imported.errorCode, imported.errorMessage);
        return PDFToolExitCode::InvalidInvocation;
    }

    const QByteArray bytes = pdf::serializePreflightProfileBytes(imported.profile);
    if (!writeProfileBytes(options.preflightProfileOutputPath, bytes, error))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("preflight-profile.write-failed"), error);
        return PDFToolExitCode::InvalidInvocation;
    }

    return PDFToolExitCode::Success;
}

PDFToolAbstractApplication::Options PDFToolPreflightProfileExportApplication::getOptionsFlags() const
{
    return Option::PreflightProfileManage;
}

QString PDFToolPreflightProfileForkApplication::getStandardString(StandardString standardString) const
{
    switch (standardString)
    {
        case Command:
            return QStringLiteral("preflight-profile.fork");
        case Name:
            return PDFToolTranslationContext::tr("Fork Preflight Profile");
        case Description:
            return PDFToolTranslationContext::tr("Fork a Loop preflight profile, recording derived_from against the parent digest.");
        default:
            Q_ASSERT(false);
            break;
    }
    return QString();
}

PDFToolExitCode PDFToolPreflightProfileForkApplication::execute(const PDFToolOptions& options)
{
    if (options.preflightProfilePath.isEmpty() || options.preflightProfileOutputPath.isEmpty() || options.preflightProfileForkId.isEmpty() ||
        options.preflightProfileForkVersion.isEmpty())
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("preflight-profile.missing-args"),
                         PDFToolTranslationContext::tr("preflight-profile.fork requires --profile, --id, --profile-version, and --out."));
        return PDFToolExitCode::InvalidInvocation;
    }

    QJsonObject profile;
    QString error;
    if (!loadProfileObject(options.preflightProfilePath, profile, error))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("preflight-profile.read-failed"), error);
        return PDFToolExitCode::InvalidInvocation;
    }

    const pdf::PreflightProfileImportResult imported = pdf::importPreflightProfile(profile, options.preflightProfilePath);
    if (!imported.ok)
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, imported.errorCode, imported.errorMessage);
        return PDFToolExitCode::InvalidInvocation;
    }

    const QJsonObject forked = pdf::forkPreflightProfile(imported.profile, options.preflightProfileForkId, options.preflightProfileForkVersion);
    const QByteArray bytes = pdf::serializePreflightProfileBytes(forked);
    if (!writeProfileBytes(options.preflightProfileOutputPath, bytes, error))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("preflight-profile.write-failed"), error);
        return PDFToolExitCode::InvalidInvocation;
    }

    return PDFToolExitCode::Success;
}

PDFToolAbstractApplication::Options PDFToolPreflightProfileForkApplication::getOptionsFlags() const
{
    return Option::PreflightProfileManage;
}

QString PDFToolPreflightProfileValidateApplication::getStandardString(StandardString standardString) const
{
    switch (standardString)
    {
        case Command:
            return QStringLiteral("preflight-profile.validate");
        case Name:
            return PDFToolTranslationContext::tr("Validate Preflight Profile");
        case Description:
            return PDFToolTranslationContext::tr("Validate a Loop preflight profile and its committed content digest.");
        default:
            Q_ASSERT(false);
            break;
    }
    return QString();
}

PDFToolExitCode PDFToolPreflightProfileValidateApplication::execute(const PDFToolOptions& options)
{
    if (options.preflightProfilePath.isEmpty())
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("preflight-profile.missing-args"),
                         PDFToolTranslationContext::tr("preflight-profile.validate requires --profile."));
        return PDFToolExitCode::InvalidInvocation;
    }

    QJsonObject profile;
    QString error;
    if (!loadProfileObject(options.preflightProfilePath, profile, error))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("preflight-profile.read-failed"), error);
        return PDFToolExitCode::InvalidInvocation;
    }

    const pdf::PreflightProfileImportResult imported = pdf::importPreflightProfile(profile, options.preflightProfilePath);
    if (!imported.ok)
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, imported.errorCode, imported.errorMessage);
        return PDFToolExitCode::InvalidInvocation;
    }

    return PDFToolExitCode::Success;
}

PDFToolAbstractApplication::Options PDFToolPreflightProfileValidateApplication::getOptionsFlags() const
{
    return Option::PreflightProfileManage;
}

}   // namespace pdftool
