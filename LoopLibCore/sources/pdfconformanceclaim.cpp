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

#include "pdfconformanceclaim.h"

#include <QSet>

#include <algorithm>
#include <iterator>

namespace pdf
{

namespace
{

const PDFConformanceClaim kConformanceClaims[] = {
    { QLatin1String("pdfx-5n"), false, false, QLatin1String("unsupported"), QLatin1String("pdfx5-pdfa3-output"), QLatin1String("landed") },
    { QLatin1String("pdfx-5g"), false, false, QLatin1String("unsupported"), QLatin1String("pdfx5-pdfa3-output"), QLatin1String("landed") },
    { QLatin1String("pdfa-3"), false, false, QLatin1String("unsupported"), QLatin1String("pdfx5-pdfa3-output"), QLatin1String("landed") }
};

bool isIdentChar(char character)
{
    const unsigned char value = static_cast<unsigned char>(character);
    return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9');
}

bool isValueChar(char character)
{
    const unsigned char value = static_cast<unsigned char>(character);
    return isIdentChar(character) || value == '/' || value == '-' || value == ':' || value == '.';
}

bool hasBoundedMarker(const QByteArray& lower, const QByteArray& marker)
{
    int from = 0;
    while (from <= lower.size() - marker.size())
    {
        const int index = lower.indexOf(marker, from);
        if (index < 0)
        {
            return false;
        }

        const bool leftBound = index == 0 || !isIdentChar(lower.at(index - 1));
        const int after = index + marker.size();
        const bool rightBound = after >= lower.size() || !isIdentChar(lower.at(after));
        if (leftBound && rightBound)
        {
            return true;
        }
        from = index + 1;
    }
    return false;
}

QList<QByteArray> xmpValues(const QByteArray& lower, const QByteArray& key)
{
    QList<QByteArray> values;
    int from = 0;
    while (from <= lower.size() - key.size())
    {
        const int index = lower.indexOf(key, from);
        if (index < 0)
        {
            break;
        }

        int cursor = index + key.size();
        if (cursor < lower.size() && isIdentChar(lower.at(cursor)))
        {
            from = index + 1;
            continue;
        }

        while (cursor < lower.size() && (lower.at(cursor) == ' ' || lower.at(cursor) == '\t' || lower.at(cursor) == '\n' || lower.at(cursor) == '\r'))
        {
            ++cursor;
        }
        if (cursor >= lower.size())
        {
            break;
        }

        const char opener = lower.at(cursor);
        if (opener == '=')
        {
            ++cursor;
            while (cursor < lower.size() && (lower.at(cursor) == ' ' || lower.at(cursor) == '\t'))
            {
                ++cursor;
            }
            if (cursor < lower.size() && (lower.at(cursor) == '"' || lower.at(cursor) == '\''))
            {
                ++cursor;
            }
        }
        else if (opener == '>')
        {
            ++cursor;
            while (cursor < lower.size() && (lower.at(cursor) == ' ' || lower.at(cursor) == '\t' || lower.at(cursor) == '\n' || lower.at(cursor) == '\r'))
            {
                ++cursor;
            }
        }
        else
        {
            from = index + 1;
            continue;
        }

        const int valueStart = cursor;
        while (cursor < lower.size() && isValueChar(lower.at(cursor)))
        {
            ++cursor;
        }
        if (cursor > valueStart)
        {
            values.append(lower.mid(valueStart, cursor - valueStart));
        }
        from = qMax(cursor, index + 1);
    }
    return values;
}

bool declaresPdfaPart3(const QByteArray& lower)
{
    const QList<QByteArray> values = xmpValues(lower, QByteArrayLiteral("pdfaid:part"));
    return std::any_of(values.cbegin(), values.cend(), [](const QByteArray& value)
                       { return value == QByteArrayLiteral("3"); });
}

bool declaresBarePdfx5WithConformance(const QByteArray& lower, const QByteArray& conformance)
{
    bool barePdfx5 = false;
    for (const QByteArray& version : xmpValues(lower, QByteArrayLiteral("gts_pdfxversion")))
    {
        if (version == QByteArrayLiteral("pdf/x-5"))
        {
            barePdfx5 = true;
            break;
        }
    }
    if (!barePdfx5)
    {
        return false;
    }

    const QList<QByteArray> values = xmpValues(lower, QByteArrayLiteral("gts_pdfxconformance"));
    return std::any_of(values.cbegin(), values.cend(), [&conformance](const QByteArray& value)
                       { return value == conformance; });
}

}   // namespace

const PDFConformanceClaim* pdfConformanceClaimRegistry(int* count)
{
    if (count)
    {
        *count = int(std::size(kConformanceClaims));
    }
    return kConformanceClaims;
}

QString pdfConformanceClaimReportDisposition(const PDFConformanceClaim& claim)
{
    if (claim.validated)
    {
        return QStringLiteral("ok");
    }
    if (claim.reportDisposition.isEmpty() || claim.reportDisposition == QLatin1String("ok"))
    {
        return QString(PDF_CONFORMANCE_CLAIM_STATUS_UNSUPPORTED);
    }
    return QString(claim.reportDisposition);
}

const PDFConformanceClaim* pdfConformanceClaimForLevel(const QString& levelId)
{
    int count = 0;
    const PDFConformanceClaim* rows = pdfConformanceClaimRegistry(&count);
    for (int index = 0; index < count; ++index)
    {
        if (levelId == rows[index].levelId)
        {
            return &rows[index];
        }
    }
    return nullptr;
}

QStringList parseDeclaredConformanceLevels(const QByteArray& identificationText)
{
    const QByteArray lower = identificationText.toLower();
    QSet<QString> declared;
    if (hasBoundedMarker(lower, QByteArrayLiteral("pdf/x-5n")) || declaresBarePdfx5WithConformance(lower, QByteArrayLiteral("n")))
    {
        declared.insert(QStringLiteral("pdfx-5n"));
    }
    if (hasBoundedMarker(lower, QByteArrayLiteral("pdf/x-5g")) || declaresBarePdfx5WithConformance(lower, QByteArrayLiteral("g")))
    {
        declared.insert(QStringLiteral("pdfx-5g"));
    }
    if (declaresPdfaPart3(lower))
    {
        declared.insert(QStringLiteral("pdfa-3"));
    }

    QStringList ordered;
    int count = 0;
    const PDFConformanceClaim* rows = pdfConformanceClaimRegistry(&count);
    for (int index = 0; index < count; ++index)
    {
        const QString levelId = QString(rows[index].levelId);
        if (declared.contains(levelId))
        {
            ordered.append(levelId);
        }
    }
    return ordered;
}

}   // namespace pdf
