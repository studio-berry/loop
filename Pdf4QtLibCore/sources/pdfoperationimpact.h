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

#ifndef PDFOPERATIONIMPACT_H
#define PDFOPERATIONIMPACT_H

#include "pdfevidencegraph.h"
#include "pdfglobal.h"

#include <QJsonObject>
#include <QSet>
#include <QStringList>

#include <optional>

namespace pdf
{

/// Declared revalidation impact of one semantic operation. Unknown or incomplete
/// impact always selects a full revalidation. Cache invalidation and check
/// selection share this model.
struct PDF4QTLIBCORESHARED_EXPORT PDFOperationImpact
{
    PDFEvidenceDomains domains;
    QSet<int> pages;
    QStringList objectIds;
    bool documentWide = false;
    bool fullRewrite = false;
    bool impactComplete = false;
    bool requiresIndependentOracle = false;

    bool isFullRevalidation() const;
    QJsonObject toJson() const;
};

/// Conservative subset of profile checks to rerun after an operation.
struct PDF4QTLIBCORESHARED_EXPORT PDFRevalidationPlan
{
    bool full = true;
    QStringList checkIds;
    QSet<int> pages;
    QString reason;

    QJsonObject toJson() const;
};

/// Maps a registered preflight check onto an Evidence Graph domain when the
/// check is graph-backed. Unmapped checks cannot be skipped by a targeted plan.
PDF4QTLIBCORESHARED_EXPORT std::optional<PDFEvidenceDomain> preflightEvidenceDomainForCheck(const QString& checkId);

/// Plans which enabled checks to rerun. Incomplete, document-wide, full-rewrite,
/// oracle-required, or unmapped-check impact falls back to a full run.
PDF4QTLIBCORESHARED_EXPORT PDFRevalidationPlan planRevalidation(const PDFOperationImpact& impact,
                                                                const QStringList& enabledCheckIds);

}   // namespace pdf

#endif   // PDFOPERATIONIMPACT_H
