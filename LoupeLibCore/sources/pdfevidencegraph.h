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

#ifndef PDFEVIDENCEGRAPH_H
#define PDFEVIDENCEGRAPH_H

#include "pdfartifactidentity.h"
#include "pdfdocumentcontext.h"
#include "pdfglobal.h"

#include <QFlags>
#include <QJsonObject>
#include <QList>
#include <QRectF>
#include <QString>

namespace pdf
{

class PDFDocumentSession;

enum class LOUPELIBCORESHARED_EXPORT PDFEvidenceDomain
{
    Images = 1 << 0,
    Colorants = 1 << 1,
    Strokes = 1 << 2,
    OverprintTransparency = 1 << 3,
    Fonts = 1 << 4
};
Q_DECLARE_FLAGS(PDFEvidenceDomains, PDFEvidenceDomain)

struct LOUPELIBCORESHARED_EXPORT PDFEvidenceRecord
{
    QString id;
    QString producer;
    QString producerVersion;
    PDFArtifactIdentity artifact;
    PDFRevisionIdentity revision;
    PDFEvidenceDomain domain = PDFEvidenceDomain::Images;
    int page = 1;
    QString objectId;
    QString target;
    qreal observedValue = 0.0;
    QString units;
    QRectF geometry;
    QString coverageMethod;
    QString fidelity;
    qreal confidence = 1.0;
    QString incompleteReason;
    QString budgetContext;
    QJsonObject extra;

    bool isComplete() const { return incompleteReason.isEmpty(); }
    QJsonObject toJson() const;
};

struct LOUPELIBCORESHARED_EXPORT PDFEvidenceGraph
{
    QString producer = QStringLiteral("loupe-evidence-collector");
    QString producerVersion;
    PDFArtifactIdentity artifact;
    PDFRevisionIdentity revision;
    QList<PDFEvidenceRecord> records;
    bool complete = true;
    QString incompleteReason;
    QString budgetKind;
    QString budgetPool;
    qint64 budgetLimit = 0;
    qint64 budgetAttempted = 0;
    QString budgetContext;

    bool isComplete() const { return complete && incompleteReason.isEmpty(); }
    QList<PDFEvidenceRecord> recordsForDomain(PDFEvidenceDomain domain) const;
    QList<PDFEvidenceRecord> recordsForTarget(PDFEvidenceDomain domain, const QString& target) const;
    QJsonObject toJson() const;
};

struct LOUPELIBCORESHARED_EXPORT PDFEvidenceCollectSettings
{
    int colorProbeDpi = 150;
    qreal richBlackKThreshold = 0.10;
    qreal minEffectiveStrokeWidthPt = 0.0;
    qreal zeroWidthEpsilonPt = 1.0e-6;
};

class LOUPELIBCORESHARED_EXPORT PDFEvidenceCollector
{
public:
    static PDFEvidenceGraph collect(PDFDocumentSession* session,
                                    PDFEvidenceDomains domains = {},
                                    const PDFEvidenceCollectSettings& settings = {});
};

LOUPELIBCORESHARED_EXPORT QString pdfEvidenceDomainToString(PDFEvidenceDomain domain);
LOUPELIBCORESHARED_EXPORT PDFEvidenceDomains pdfEvidenceAllDomains();

}   // namespace pdf

Q_DECLARE_OPERATORS_FOR_FLAGS(pdf::PDFEvidenceDomains)

#endif   // PDFEVIDENCEGRAPH_H
