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

#include "pdftoolrgbtocmyk.h"

#include "pdfdocumentwriter.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace pdftool
{

namespace
{

static PDFToolRgbToCmyk s_toolRgbToCmykApplication;

bool readProfile(const QString& path, QByteArray* data, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        if (error)
        {
            *error = PDFToolTranslationContext::tr("Unable to read ICC profile '%1'.").arg(path);
        }
        return false;
    }
    *data = file.readAll();
    if (data->isEmpty())
    {
        if (error)
        {
            *error = PDFToolTranslationContext::tr("ICC profile '%1' is empty.").arg(path);
        }
        return false;
    }
    return true;
}

QString objectKindName(pdf::PDFRgbToCmykObjectKind kind)
{
    switch (kind)
    {
        case pdf::PDFRgbToCmykObjectKind::VectorPaint: return QStringLiteral("vector-paint");
        case pdf::PDFRgbToCmykObjectKind::Image: return QStringLiteral("image");
        case pdf::PDFRgbToCmykObjectKind::InlineImage: return QStringLiteral("inline-image");
        case pdf::PDFRgbToCmykObjectKind::Form: return QStringLiteral("form");
        case pdf::PDFRgbToCmykObjectKind::AnnotationAppearance: return QStringLiteral("annotation-appearance");
        case pdf::PDFRgbToCmykObjectKind::IndexedPalette: return QStringLiteral("indexed-palette");
        case pdf::PDFRgbToCmykObjectKind::Shading: return QStringLiteral("shading");
        case pdf::PDFRgbToCmykObjectKind::Pattern: return QStringLiteral("pattern");
    }
    return QStringLiteral("unknown");
}

QJsonObject reportObject(const pdf::PDFRgbToCmykSettings& settings,
                         const pdf::PDFRgbToCmykReport& report)
{
    QJsonObject result;
    result.insert(QStringLiteral("command"), QStringLiteral("rgb-to-cmyk"));
    result.insert(QStringLiteral("target_profile"), QJsonObject{
        { QStringLiteral("name"), settings.targetProfileName },
        { QStringLiteral("bytes"), settings.targetIccData.size() }
    });
    result.insert(QStringLiteral("intent"), int(settings.intent));
    result.insert(QStringLiteral("black_point_compensation"), settings.blackPointCompensation);
    result.insert(QStringLiteral("converted"), QJsonObject{
        { QStringLiteral("vector_paints"), report.vectorPaintsConverted },
        { QStringLiteral("images"), report.imagesConverted },
        { QStringLiteral("indexed_palettes"), report.indexedPalettesConverted },
        { QStringLiteral("forms"), report.formsVisited },
        { QStringLiteral("annotation_appearances"), report.annotationAppearancesVisited }
    });
    result.insert(QStringLiteral("unsupported"), [&report]
    {
        QJsonArray values;
        for (const pdf::PDFRgbToCmykUnsupportedItem& item : report.unsupported)
        {
            values.append(QJsonObject{
                { QStringLiteral("page"), int(item.pageIndex + 1) },
                { QStringLiteral("kind"), objectKindName(item.kind) },
                { QStringLiteral("reason"), item.reason }
            });
        }
        return values;
    }());
    result.insert(QStringLiteral("output_intent_changed"), report.outputIntentChanged);
    result.insert(QStringLiteral("postflight_passed"), report.postflightPassed);
    return result;
}

} // namespace

QString PDFToolRgbToCmyk::getStandardString(StandardString standardString) const
{
    switch (standardString)
    {
        case Command: return QStringLiteral("rgb-to-cmyk");
        case Name: return PDFToolTranslationContext::tr("RGB to CMYK");
        case Description: return PDFToolTranslationContext::tr("Convert RGB PDF content through LittleCMS to a selected CMYK output condition.");
        default: break;
    }
    return QString();
}

PDFToolExitCode PDFToolRgbToCmyk::execute(const PDFToolOptions& options)
{
    if (options.rgbToCmykSettings.targetProfileName.isEmpty())
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("cli.invalid-arguments"),
                         PDFToolTranslationContext::tr("--target-profile is required."));
        return PDFToolExitCode::InvalidInvocation;
    }
    if (!options.destructiveDryRun && options.rgbToCmykOutputDocument.isEmpty())
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("cli.invalid-arguments"),
                         PDFToolTranslationContext::tr("An output document is required unless --dry-run is used."));
        return PDFToolExitCode::InvalidInvocation;
    }

    pdf::PDFRgbToCmykSettings settings = options.rgbToCmykSettings;
    QString profileError;
    if (!readProfile(settings.targetProfileName, &settings.targetIccData, &profileError))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("profile.read-failed"), profileError);
        return PDFToolExitCode::InputError;
    }
    settings.targetProfileName = QFileInfo(settings.targetProfileName).completeBaseName();

    const QString sourceProfilePath = options.rgbToCmykSettings.fallbackRgbIccId.isEmpty()
        ? QString() : QString::fromUtf8(options.rgbToCmykSettings.fallbackRgbIccId);
    if (!sourceProfilePath.isEmpty())
    {
        if (!readProfile(sourceProfilePath, &settings.fallbackRgbIccData, &profileError))
        {
            reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("profile.read-failed"), profileError);
            return PDFToolExitCode::InputError;
        }
    }

    pdf::PDFDocument document;
    if (!readDocument(options, document, nullptr, false))
    {
        return PDFToolExitCode::InputError;
    }

    settings.dryRunOnly = options.destructiveDryRun;
    if (!options.pageSelectorSelection.isEmpty())
    {
        settings.pageRange = options.pageSelectorSelection;
    }

    pdf::PDFRgbToCmykReport report;
    const pdf::PDFOperationResult result = pdf::PDFRgbToCmykFixup::writeRgbToCmyk(&document, settings, &report);
    if (!result)
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("operation.failed"), result.getErrorMessage());
        return PDFToolExitCode::ProcessingFailure;
    }

    const QJsonObject json = reportObject(settings, report);
    if (options.outputStyle == PDFOutputFormatter::Style::Json)
    {
        if (options.executionContext)
        {
            options.executionContext->setData(json);
        }
    }
    else if (options.destructiveReport || options.destructiveDryRun)
    {
        PDFConsole::writeText(QString::fromUtf8(QJsonDocument(json).toJson(QJsonDocument::Indented)), options.outputCodec);
    }

    if (options.destructiveDryRun)
    {
        if (options.executionContext)
        {
            options.executionContext->addOutput({ QStringLiteral("file"),
                                                  QStringLiteral("primary"),
                                                  options.rgbToCmykOutputDocument,
                                                  QStringLiteral("planned") });
        }
        return PDFToolExitCode::Success;
    }

    if (const PDFToolExitCode blocked = validateDestructiveOutput(options, options.rgbToCmykOutputDocument); blocked != PDFToolExitCode::Success)
    {
        return blocked;
    }

    pdf::PDFDocumentWriter writer(nullptr);
    const pdf::PDFOperationResult writeResult = writer.write(options.rgbToCmykOutputDocument, &document, true);
    if (!writeResult)
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("output.write-failed"), writeResult.getErrorMessage());
        return PDFToolExitCode::ProcessingFailure;
    }

    if (options.executionContext)
    {
        options.executionContext->addOutput({ QStringLiteral("file"),
                                              QStringLiteral("primary"),
                                              options.rgbToCmykOutputDocument,
                                              QStringLiteral("written") });
    }

    return PDFToolExitCode::Success;
}

PDFToolAbstractApplication::Options PDFToolRgbToCmyk::getOptionsFlags() const
{
    return ConsoleFormat | OpenDocument | PageSelector | ColorManagementSystem | RgbToCmyk | DestructiveWrite;
}

} // namespace pdftool
