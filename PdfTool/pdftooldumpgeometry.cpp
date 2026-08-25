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

#include "pdftooldumpgeometry.h"
#include "pdftoolstructuredoutput.h"
#include "pdfutils.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace pdftool
{

namespace
{

static PDFToolDumpGeometryApplication s_dumpGeometryApplication;

QJsonValue boxToJson(const QRectF& rect)
{
    if (!rect.isValid())
    {
        return QJsonValue::Null;
    }
    return QJsonArray{ rect.left(), rect.top(), rect.right(), rect.bottom() };
}

int rotationDegrees(pdf::PageRotation rotation)
{
    switch (rotation)
    {
        case pdf::PageRotation::None:
            return 0;
        case pdf::PageRotation::Rotate90:
            return 90;
        case pdf::PageRotation::Rotate180:
            return 180;
        case pdf::PageRotation::Rotate270:
            return 270;
    }
    return 0;
}

} // namespace

QString PDFToolDumpGeometryApplication::getStandardString(StandardString standardString) const
{
    switch (standardString)
    {
        case Command:
            return "dump-page-geometry";
        case Name:
            return PDFToolTranslationContext::tr("Dump page geometry");
        case Description:
            return PDFToolTranslationContext::tr("Emit one raw PDF-point geometry record per page.");
        default:
            Q_ASSERT(false);
            break;
    }
    return QString();
}

PDFToolExitCode PDFToolDumpGeometryApplication::execute(const PDFToolOptions& options)
{
    pdf::PDFDocument document;
    QByteArray sourceData;
    if (!readDocument(options, document, &sourceData, false))
    {
        return PDFToolExitCode::InputError;
    }

    QJsonArray pages;
    QJsonArray errors;
    const pdf::PDFInteger pageCount = document.getCatalog()->getPageCount();
    for (pdf::PDFInteger pageIndex = 0; pageIndex < pageCount; ++pageIndex)
    {
        QJsonObject pageObject;
        pageObject.insert(QStringLiteral("page_index"), pageIndex);
        try
        {
            const pdf::PDFPage* page = document.getCatalog()->getPage(pageIndex);
            pageObject.insert(QStringLiteral("status"), QStringLiteral("ok"));
            pageObject.insert(QStringLiteral("media"), boxToJson(page->getMediaBox()));
            pageObject.insert(QStringLiteral("crop"), boxToJson(page->getCropBox()));
            pageObject.insert(QStringLiteral("bleed"), boxToJson(page->getBleedBox()));
            pageObject.insert(QStringLiteral("trim"), boxToJson(page->getTrimBox()));
            pageObject.insert(QStringLiteral("rotation_deg"), rotationDegrees(page->getPageRotation()));
        }
        catch (const std::exception& exception)
        {
            const QString message = QString::fromUtf8(exception.what());
            pageObject.insert(QStringLiteral("status"), QStringLiteral("unreadable"));
            pageObject.insert(QStringLiteral("error_code"), QStringLiteral("page_unreadable"));
            pageObject.insert(QStringLiteral("error_message"), message);
            errors.append(QJsonObject{
                { QStringLiteral("page_index"), pageIndex },
                { QStringLiteral("error_code"), QStringLiteral("page_unreadable") },
                { QStringLiteral("error_message"), message }
            });
        }
        pages.append(pageObject);
    }

    QJsonObject data{
        { QStringLiteral("schema_version"), 1 },
        { QStringLiteral("command"), QStringLiteral("dump-page-geometry") },
        { QStringLiteral("document_sha256"), QString::fromLatin1(QCryptographicHash::hash(sourceData, QCryptographicHash::Sha256).toHex()) },
        { QStringLiteral("page_count"), pageCount },
        { QStringLiteral("pages"), pages },
        { QStringLiteral("errors"), errors }
    };

    if (options.outputStyle == PDFOutputFormatter::Style::Json)
    {
        if (options.executionContext)
        {
            options.executionContext->setData(data);
        }
    }
    else
    {
        PDFConsole::writeText(formatStructuredObject(data, options.outputStyle, QStringLiteral("dump-page-geometry")), options.outputCodec);
    }
    return PDFToolExitCode::Success;
}

PDFToolAbstractApplication::Options PDFToolDumpGeometryApplication::getOptionsFlags() const
{
    return ConsoleFormat | OpenDocument;
}

} // namespace pdftool
