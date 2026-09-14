// MIT License
#ifndef PDFDOCUMENTSEARCH_H
#define PDFDOCUMENTSEARCH_H

#include "pdfdocumentcontext.h"
#include "pdfglobal.h"

#include <QString>
#include <QVector>

namespace pdf
{

struct LOOPLIBCORESHARED_EXPORT PDFDocumentSearchMatch
{
    PDFInteger pageIndex = -1;
    QString matched;
    QString context;
};

struct LOOPLIBCORESHARED_EXPORT PDFDocumentSearchResult
{
    QVector<PDFDocumentSearchMatch> matches;
    PDFRevisionIdentity revision;
    bool admitted = false;
    /// True only when every page was scanned under the operation-scoped search
    /// budget. False means the matches may be partial: the budget was exhausted,
    /// or the result was revoked as stale.
    bool completed = false;
    /// True when the budget, rather than a revocation, cut the search short.
    /// Preflight reports the same condition under this name.
    bool budgetExceeded = false;
    /// Core's own description of why the search stopped short, when it did.
    QString errorMessage;
};

/// Extracts and searches the text flows for every page in the context's
/// current document. Results are admitted only while the captured revision is
/// still current, so presentation layers do not need to implement parsing or
/// revision-fencing policy themselves.
LOOPLIBCORESHARED_EXPORT PDFDocumentSearchResult searchDocumentText(PDFDocumentContext* context,
                                                                    const QString& query,
                                                                    Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive);

}   // namespace pdf

#endif   // PDFDOCUMENTSEARCH_H
