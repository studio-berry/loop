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

#ifndef PDFOBJECTSELECTOR_H
#define PDFOBJECTSELECTOR_H

#include "pdfdocumentcontext.h"
#include "pdfglobal.h"
#include "pdfobject.h"
#include "pdfrepairoperation.h"
#include "pdfutils.h"

#include <QJsonObject>
#include <QMap>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QVector>

namespace pdf
{

class PDFDocument;

/// One resolved operation target candidate. Candidates are sorted deterministically
/// by stableId() before digesting.
struct LOOPLIBCORESHARED_EXPORT PDFObjectSelectorCandidate
{
    int pageIndex = -1;
    PDFObjectReference objectReference;
    QString objectClass;
    QString colorSpaceName;
    QString fontName;
    QString layerName;
    double effectiveDpi = 0.0;
    bool isVector = false;
    QRectF boundsPt;

    QString stableId() const;
    QJsonObject toJson() const;
    PDFRepairTarget toRepairTarget() const;
};

/// Resolved selection scope for preview and execution fencing.
struct LOOPLIBCORESHARED_EXPORT PDFObjectSelectionResult
{
    bool ok = false;
    bool empty = false;
    bool staleRevision = false;
    QString errorMessage;
    QString digest;
    PDFRevisionIdentity resolvedRevision;
    QVector<PDFObjectSelectorCandidate> candidates;

    QJsonObject previewScope() const;
    QJsonObject toJson() const;
};

/// Deterministic selector AST shared by Action Lists, Fix, and PdfTool.
class LOOPLIBCORESHARED_EXPORT PDFObjectSelector
{
public:
    static QString schemaVersion();

    PDFObjectSelector() = default;

    static PDFOperationResult fromJson(const QJsonObject& object, PDFObjectSelector* result, QStringList* errors = nullptr);
    QJsonObject toJson() const;

    static PDFOperationResult resolve(const PDFObjectSelector& selector,
                                      const PDFDocument& document,
                                      const PDFRevisionIdentity& revision,
                                      PDFObjectSelectionResult* result);

    QString schema() const { return m_schema; }
    const QJsonObject& predicate() const { return m_predicate; }

private:
    QString m_schema = schemaVersion();
    QJsonObject m_predicate;
    QMap<QString, QJsonObject> m_namedSets;
    QString m_revisionDigest;
};

LOOPLIBCORESHARED_EXPORT PDFRevisionIdentity revisionIdentityForDocument(const PDFDocument& document,
                                                                           DocumentRevision documentRevision = 0,
                                                                           quint64 cacheGeneration = 0);

LOOPLIBCORESHARED_EXPORT QString selectionDigestForCandidates(const QVector<PDFObjectSelectorCandidate>& candidates);

LOOPLIBCORESHARED_EXPORT bool filterRepairPlanTargets(PDFRepairPlan* plan, const PDFObjectSelectionResult& selection);

LOOPLIBCORESHARED_EXPORT bool repairChangesWithinSelection(const QList<PDFRepairChange>& changes,
                                                           const PDFObjectSelectionResult& selection,
                                                           QString* errorMessage = nullptr);

}   // namespace pdf

#endif   // PDFOBJECTSELECTOR_H
