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

#include "pdftoolschema.h"

#include "pdfschemaversion.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace pdftool
{

static PDFToolSchemaApplication s_schemaApplication;

QString PDFToolSchemaApplication::getStandardString(StandardString standardString) const
{
    switch (standardString)
    {
        case Command:
            return QStringLiteral("schema");

        case Name:
            return PDFToolTranslationContext::tr("Schema");

        case Description:
            return PDFToolTranslationContext::tr("Report the schema compatibility authority, or diagnose one persisted JSON artifact.");

        default:
            Q_ASSERT(false);
            break;
    }

    return QString();
}

PDFToolExitCode PDFToolSchemaApplication::execute(const PDFToolOptions& options)
{
    const QString inputPath = options.schemaInputPath.trimmed();

    if (inputPath.isEmpty())
    {
        if (options.outputStyle == PDFOutputFormatter::Style::Json && options.executionContext)
        {
            options.executionContext->setData(
                QJsonObject{ { QStringLiteral("matrix"), pdf::schemaCompatibilityMatrix() } });
        }
        else
        {
            PDFConsole::writeText(
                QString::fromUtf8(QJsonDocument(pdf::schemaCompatibilityMatrix()).toJson(QJsonDocument::Indented)),
                options.outputCodec);
        }
        return PDFToolExitCode::Success;
    }

    QFile file(inputPath);
    if (!file.open(QIODevice::ReadOnly))
    {
        reportDiagnostic(options,
                         PDFToolDiagnosticSeverity::Error,
                         QStringLiteral("schema.input-unreadable"),
                         PDFToolTranslationContext::tr("Cannot open schema artifact '%1'.").arg(inputPath),
                         QJsonObject{ { QStringLiteral("path"), inputPath } });
        return PDFToolExitCode::InputError;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        reportDiagnostic(options,
                         PDFToolDiagnosticSeverity::Error,
                         QStringLiteral("schema.invalid-json"),
                         PDFToolTranslationContext::tr("Invalid JSON in '%1': %2").arg(inputPath, parseError.errorString()),
                         QJsonObject{ { QStringLiteral("path"), inputPath } });
        return PDFToolExitCode::InputError;
    }

    const QJsonObject artifact = document.object();
    const pdf::PDFSchemaEnvelope envelope = pdf::readSchemaEnvelope(artifact);
    const pdf::PDFSchemaCompatibilityDiagnostic diagnostic =
        pdf::schemaCompatibilityDiagnostic(envelope.kind, envelope.version);
    const pdf::PDFSchemaMigrationResult prepared = pdf::prepareSchemaDocument(envelope.kind, artifact);

    bool migrationRequired = false;
    if (envelope.kind != pdf::PDFSchemaKind::Unknown)
    {
        const pdf::PDFSchemaVersion target = pdf::currentSchemaVersion(envelope.kind);
        migrationRequired = target.isValid() && envelope.version.isValid() && envelope.version.major < target.major;
    }

    const QJsonObject data{
        { QStringLiteral("input"), inputPath },
        { QStringLiteral("schema_kind"), pdf::pdfSchemaKindToString(envelope.kind) },
        { QStringLiteral("schema_version"), envelope.version.isValid() ? envelope.version.toString() : QString() },
        { QStringLiteral("compatibility"), pdf::pdfSchemaCompatibilityToString(diagnostic.compatibility) },
        { QStringLiteral("code"), diagnostic.code },
        { QStringLiteral("message"), diagnostic.message },
        { QStringLiteral("migration"),
          QJsonObject{ { QStringLiteral("required"), migrationRequired },
                       { QStringLiteral("applied"), prepared.migrated },
                       { QStringLiteral("document_ready"), !prepared.document.isEmpty() },
                       { QStringLiteral("from"), prepared.fromVersion.isValid() ? prepared.fromVersion.toString() : QString() },
                       { QStringLiteral("to"), prepared.toVersion.isValid() ? prepared.toVersion.toString() : QString() } } }
    };

    if (options.outputStyle == PDFOutputFormatter::Style::Json && options.executionContext)
    {
        options.executionContext->setData(data);
    }
    else
    {
        PDFConsole::writeText(QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Indented)), options.outputCodec);
    }

    if (!diagnostic.isCompatible())
    {
        reportDiagnostic(options,
                         PDFToolDiagnosticSeverity::Error,
                         diagnostic.code,
                         diagnostic.message,
                         QJsonObject{ { QStringLiteral("path"), inputPath } });
    }

    return diagnostic.isCompatible() ? PDFToolExitCode::Success : PDFToolExitCode::Findings;
}

PDFToolAbstractApplication::Options PDFToolSchemaApplication::getOptionsFlags() const
{
    return ConsoleFormat | SchemaDiagnostics;
}

}   // namespace pdftool
