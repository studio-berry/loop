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

#ifndef PDFSCHEMAVERSION_H
#define PDFSCHEMAVERSION_H

#include "pdfglobal.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace pdf
{

enum class PDF4QTLIBCORESHARED_EXPORT PDFSchemaKind
{
    Unknown,
    PreflightReport,
    PreflightProfile,
    EvidenceGraph,
    OperationPlan,
    OperationResult,
    ProvenanceEvent,
    Certificate,
    CapabilityDiscovery,
    PackageManifest,
    ActionList,
    PdfToolEnvelope,
    OcrReport,
    HistoryDb,
    PageMasterManifest,
    PreflightDecisions
};

enum class PDF4QTLIBCORESHARED_EXPORT PDFSchemaCompatibility
{
    Compatible,
    UnsupportedMajor,
    UnknownKind
};

struct PDF4QTLIBCORESHARED_EXPORT PDFSchemaVersion
{
    quint16 major = 0;
    quint16 minor = 0;

    bool isValid() const { return major > 0; }
    bool operator==(const PDFSchemaVersion&) const = default;
    QString toString() const;
    static PDFSchemaVersion fromJsonValue(const QJsonValue& value, bool* ok = nullptr);
    QJsonValue toJsonValue() const;
};

struct PDF4QTLIBCORESHARED_EXPORT PDFSchemaEnvelope
{
    PDFSchemaKind kind = PDFSchemaKind::Unknown;
    PDFSchemaVersion version;
};

PDF4QTLIBCORESHARED_EXPORT QString pdfSchemaKindToString(PDFSchemaKind kind);
PDF4QTLIBCORESHARED_EXPORT PDFSchemaKind pdfSchemaKindFromString(const QString& value);
PDF4QTLIBCORESHARED_EXPORT PDFSchemaCompatibility checkSchemaCompatibility(PDFSchemaKind kind, PDFSchemaVersion version);
PDF4QTLIBCORESHARED_EXPORT QJsonObject migrateSchemaDocument(PDFSchemaKind kind, PDFSchemaVersion from, QJsonObject document);
PDF4QTLIBCORESHARED_EXPORT PDFSchemaEnvelope readSchemaEnvelope(const QJsonObject& document);
PDF4QTLIBCORESHARED_EXPORT void writeSchemaEnvelope(QJsonObject& document, PDFSchemaKind kind, PDFSchemaVersion version);

} // namespace pdf

#endif // PDFSCHEMAVERSION_H
