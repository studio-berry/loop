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

#ifndef PDFTOOLENVELOPEUTILS_H
#define PDFTOOLENVELOPEUTILS_H

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

namespace pdfplugin::pdftool
{

inline bool isResultEnvelope(const QJsonObject& object, const QString& expectedCommand)
{
    return object.value(QStringLiteral("schema_version")).toInt() == 1
        && object.value(QStringLiteral("command")).toString() == expectedCommand;
}

inline QString formatDiagnosticMessages(const QJsonArray& diagnostics)
{
    QStringList messages;
    for (const QJsonValue& value : diagnostics)
    {
        const QJsonObject diagnostic = value.toObject();
        const QString message = diagnostic.value(QStringLiteral("message")).toString();
        if (!message.isEmpty())
        {
            messages.append(message);
        }
    }

    return messages.join(QStringLiteral("\n"));
}

inline QString failureDetailFromStdout(const QByteArray& stdoutData,
                                       const QString& stderrText,
                                       int exitCode,
                                       const QString& fallbackWhenNoDetail)
{
    if (!stderrText.isEmpty())
    {
        return stderrText;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(stdoutData, &parseError);
    if (parseError.error == QJsonParseError::NoError && document.isObject())
    {
        const QJsonObject envelope = document.object();
        if (envelope.value(QStringLiteral("schema_version")).toInt() == 1)
        {
            const QString diagnosticText = formatDiagnosticMessages(envelope.value(QStringLiteral("diagnostics")).toArray());
            if (!diagnosticText.isEmpty())
            {
                return diagnosticText;
            }
        }
    }

    return fallbackWhenNoDetail;
}

inline QJsonObject reportFromEnvelope(const QJsonObject& envelope)
{
    return envelope.value(QStringLiteral("data")).toObject().value(QStringLiteral("report")).toObject();
}

}   // namespace pdfplugin::pdftool

#endif // PDFTOOLENVELOPEUTILS_H
