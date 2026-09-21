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

#include "preflightclirun.h"

#include "pdfdocumentsession.h"
#include "pdfoperationcontrol.h"
#include "pdfpreflightverdict.h"
#include "preflightprofileresolver.h"

#include <memory>
#include <optional>

namespace pdf
{

namespace
{

bool parsePreflightCliPages(const PreflightFileInspectionRequest& request,
                            int pageCount,
                            std::optional<QSet<int>>* selectedPages,
                            QString* error)
{
    if (request.firstPage.isEmpty() && request.lastPage.isEmpty() && request.selectedPages.isEmpty())
    {
        *selectedPages = std::nullopt;
        return true;
    }
    const auto parsePage = [](const QString& text, int fallback, int* value)
    {
        if (text.isEmpty())
        {
            *value = fallback;
            return true;
        }
        bool valid = false;
        const int parsed = text.toInt(&valid);
        if (!valid || parsed < 1 || parsed > 100000)
        {
            return false;
        }
        *value = parsed;
        return true;
    };

    int first = 1;
    int last = pageCount;
    if (!parsePage(request.firstPage, 1, &first) ||
        !parsePage(request.lastPage, pageCount, &last) ||
        (!request.lastPage.isEmpty() && last < first))
    {
        *error = QStringLiteral("Invalid preflight --page-first / --page-last range.");
        return false;
    }

    QSet<int> explicitSelection;
    if (!request.selectedPages.isEmpty())
    {
        for (const QString& rawPart : request.selectedPages.split(QLatin1Char(',')))
        {
            const QString part = rawPart.trimmed();
            const int separator = part.indexOf(QLatin1Char('-'));
            if (part.isEmpty() || (separator >= 0 && part.indexOf(QLatin1Char('-'), separator + 1) >= 0))
            {
                *error = QStringLiteral("Invalid preflight --page-select range.");
                return false;
            }
            int rangeStart = 1;
            int rangeEnd = pageCount;
            const QString startText = separator >= 0 ? part.left(separator) : part;
            const QString endText = separator >= 0 ? part.mid(separator + 1) : part;
            if (!parsePage(startText, 1, &rangeStart) ||
                !parsePage(endText, pageCount, &rangeEnd) ||
                rangeEnd < rangeStart)
            {
                *error = QStringLiteral("Invalid preflight --page-select range.");
                return false;
            }
            for (int page = rangeStart; page <= rangeEnd && page <= pageCount; ++page)
            {
                explicitSelection.insert(page - 1);
            }
        }
    }

    QSet<int> effective;
    for (int page = first; page <= last && page <= pageCount; ++page)
    {
        if (request.selectedPages.isEmpty() || explicitSelection.contains(page - 1))
        {
            effective.insert(page - 1);
        }
    }
    *selectedPages = effective;
    return true;
}

}   // namespace

PreflightFileInspectionOutcome inspectPreflightFile(const PreflightFileInspectionRequest& request)
{
    PreflightFileInspectionOutcome outcome;

    bool isFirstPasswordAttempt = true;
    const auto passwordCallback = [&request, &isFirstPasswordAttempt](bool* ok) -> QString
    {
        *ok = isFirstPasswordAttempt;
        isFirstPasswordAttempt = false;
        return request.password;
    };

    PDFDocumentReader reader(nullptr, passwordCallback, request.permissiveReading, false);
    std::unique_ptr<PDFDocument, void (*)(PDFDocument*)> document(reader.readFromFileOnHeap(request.documentPath),
                                                                  &PDFDocumentReader::destroyDocument);

    outcome.readResult = reader.getReadingResult();
    outcome.readErrorMessage = reader.getErrorMessage();
    outcome.readWarnings = reader.getWarnings();
    outcome.documentReadOk = outcome.readResult == PDFDocumentReader::Result::OK;
    if (!outcome.documentReadOk)
    {
        return outcome;
    }

    outcome.sourceData = reader.getSource();

    std::unique_ptr<PDFDocumentSession, void (*)(PDFDocumentSession*)> session(
        PDFDocumentSession::createForInspection(document.get()),
        &PDFDocumentSession::destroy);

    PreflightEngine engine(session.get());
    engine.setOperationControl(request.cancellation);

    if (!PDFOperationControl::isOperationCancelled(request.cancellation))
    {
        std::optional<QSet<int>> selectedPages;
        QString selectionError;
        const int pageCount = document->getCatalog() ? int(document->getCatalog()->getPageCount()) : 0;
        if (!parsePreflightCliPages(request, pageCount, &selectedPages, &selectionError))
        {
            outcome.report.profileName = request.profile.value(QStringLiteral("name")).toString();
            outcome.report.inspectionComplete = false;
            outcome.report.errorCode = QStringLiteral("unsupported-scope");
            outcome.report.errorMessage = selectionError;
            outcome.report.pass = false;
        }
        else
        {
            outcome.report = engine.run(request.profile, request.jobSpec, request.cliBindings, request.plan, selectedPages);
        }
        outcome.inspectionRan = true;
    }

#if defined(Q_OS_WIN)
    document.release();
    session.release();
#endif

    return outcome;
}

void finalizePreflightResult(PreflightResult& result,
                             const QByteArray& documentRevisionHash,
                             const PreflightResolvedProfile& profile)
{
    result.profileResolution = profile.provenance();
    result.documentRevisionDigest = QString::fromLatin1(documentRevisionHash.toHex());
    if (result.effectiveProfileDigest.isEmpty())
    {
        result.effectiveProfileDigest = QString::fromLatin1(profile.effectiveHash);
    }
    result.pass = reducePreflightVerdict(result).isPass();
}

}   // namespace pdf
