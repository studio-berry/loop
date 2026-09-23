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

#ifndef PDFCONFORMANCECLAIM_H
#define PDFCONFORMANCECLAIM_H

#include "pdfglobal.h"

#include <QByteArray>
#include <QLatin1String>
#include <QString>
#include <QStringList>

namespace pdf
{

/// One conformance level Loop can recognize when a document declares it.
/// Production and validation are separate facts; the report disposition is
/// what a declared claim receives, and it is never "ok" while the level is
/// not validated.
struct LOOPLIBCORESHARED_EXPORT PDFConformanceClaim
{
    QLatin1String levelId;
    bool produced = false;
    bool validated = false;
    QLatin1String reportDisposition;
    QLatin1String backlogRowId;
    QLatin1String catalogDisposition;
};

inline constexpr QLatin1String PDF_CONFORMANCE_CLAIMS_CHECK_ID("conformance-claims");
inline constexpr QLatin1String PDF_CONFORMANCE_CLAIM_UNSUPPORTED_TYPE("check-unsupported");
inline constexpr QLatin1String PDF_CONFORMANCE_CLAIM_STATUS_UNSUPPORTED("unsupported");
inline constexpr QLatin1String PDF_CONFORMANCE_CLAIM_BACKLOG_ROW("pdfx5-pdfa3-output");

/// Registry of conformance claims. \p count receives the row count when non-null.
LOOPLIBCORESHARED_EXPORT const PDFConformanceClaim* pdfConformanceClaimRegistry(int* count);

/// Maps a registry row to the check status used when a document declares it.
LOOPLIBCORESHARED_EXPORT QString pdfConformanceClaimReportDisposition(const PDFConformanceClaim& claim);

/// Returns the registry row for \p levelId, or nullptr.
LOOPLIBCORESHARED_EXPORT const PDFConformanceClaim* pdfConformanceClaimForLevel(const QString& levelId);

/// Parses identification text (catalog XMP and the document-info PDF/X keys)
/// into registry level ids, in registry order. Other standards are ignored.
LOOPLIBCORESHARED_EXPORT QStringList parseDeclaredConformanceLevels(const QByteArray& identificationText);

}   // namespace pdf

#endif
