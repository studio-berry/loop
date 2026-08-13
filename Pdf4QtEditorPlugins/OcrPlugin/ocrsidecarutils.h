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

#ifndef OCRSIDECARUTILS_H
#define OCRSIDECARUTILS_H

#include <QByteArray>
#include <QDir>
#include <QJsonObject>

#ifndef LOUPE_OCR_SCHEMA_VERSION
#define LOUPE_OCR_SCHEMA_VERSION 1
#endif

namespace pdfplugin::ocr
{

inline QString resolveBundlePath(const QString& applicationDirectory, const QString& relativePath)
{
    return QDir::cleanPath(QDir(applicationDirectory).filePath(relativePath));
}

inline QString getPdfToolFileName()
{
#ifdef Q_OS_WIN
    return QStringLiteral("PdfTool.exe");
#else
    return QStringLiteral("PdfTool");
#endif
}

inline bool isExpectedOcrExitCode(int exitCode)
{
    return exitCode == 0 || exitCode == 1 || exitCode == 5;
}

inline bool validateOcrReport(const QJsonObject& report, QString* errorMessage = nullptr)
{
    if (!report.contains(QStringLiteral("schema_version"))
        || !report.value(QStringLiteral("schema_version")).isDouble())
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("OCR report missing schema_version.");
        }
        return false;
    }

    if (report.value(QStringLiteral("schema_version")).toInt() != LOUPE_OCR_SCHEMA_VERSION)
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("Unsupported OCR report schema_version.");
        }
        return false;
    }

    if (!report.value(QStringLiteral("pass")).isBool()
        || !report.value(QStringLiteral("pdf")).isString()
        || !report.value(QStringLiteral("pages")).isArray()
        || !report.value(QStringLiteral("skipped_pages")).isArray()
        || !report.value(QStringLiteral("errors")).isArray())
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("OCR report is missing required fields.");
        }
        return false;
    }

    return true;
}

inline bool extractOcrReport(const QJsonObject& envelope,
                             QJsonObject* report,
                             QString* errorMessage = nullptr)
{
    const QString status = envelope.value(QStringLiteral("status")).toString();
    if (status != QStringLiteral("success") && status != QStringLiteral("partial-output"))
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("PdfTool OCR operation did not succeed.");
        }
        return false;
    }

    const QJsonValue reportValue = envelope.value(QStringLiteral("data"))
                                       .toObject()
                                       .value(QStringLiteral("report"));
    if (!reportValue.isObject())
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("PdfTool response missing OCR report.");
        }
        return false;
    }

    QJsonObject parsedReport = reportValue.toObject();
    if (!validateOcrReport(parsedReport, errorMessage))
    {
        return false;
    }

    if (report)
    {
        *report = parsedReport;
    }
    return true;
}

}   // namespace pdfplugin::ocr

#endif // OCRSIDECARUTILS_H
