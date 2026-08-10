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

#ifndef PDFOPERATIONHISTORYSTORE_H
#define PDFOPERATIONHISTORYSTORE_H

#include "pdfoperationhistory.h"
#include "pdfutils.h"

#include <QString>

#include <memory>

namespace pdf
{

struct PDF4QTLIBCORESHARED_EXPORT PDFOperationHistoryStoreOptions
{
    int busyTimeoutMs = 5000;
};

class PDF4QTLIBCORESHARED_EXPORT PDFOperationHistoryStore
{
public:
    explicit PDFOperationHistoryStore(QString databasePath,
                                      PDFOperationHistoryStoreOptions options = {});
    ~PDFOperationHistoryStore();

    PDFOperationHistoryStore(const PDFOperationHistoryStore&) = delete;
    PDFOperationHistoryStore& operator=(const PDFOperationHistoryStore&) = delete;

    PDFOperationResult open(QString* errorMessage = nullptr);
    void close();
    bool isOpen() const;
    const QString& databasePath() const { return m_databasePath; }

    /// Registers immutable artifact metadata before an execution can reference it.
    PDFOperationResult registerArtifact(const PDFArtifactIdentity& artifact);

    /// Begins an execution. If executionId is null, a fresh UUID is assigned.
    /// Parameters are redacted and canonically serialized before persistence.
    PDFOperationResult beginExecution(PDFOperationHistoryExecution execution,
                                       QUuid* executionId = nullptr);

    /// The only write API for history_events. There is deliberately no update or
    /// delete API; corrections and rollback are new events.
    PDFOperationResult appendEvent(PDFOperationHistoryEvent event,
                                   qint64* sequence = nullptr);

    QList<PDFOperationHistoryEvent> events(QString* errorMessage = nullptr) const;
    PDFOperationHistoryVerification verify() const;

    /// Resolves a rollback target only when it is the output of an accepted event.
    PDFOperationResult resolveRollbackTarget(const PDFRollbackRequest& request,
                                             PDFArtifactIdentity* targetArtifact) const;

private:
    class Impl;

    std::unique_ptr<Impl> m_impl;
    QString m_databasePath;
    PDFOperationHistoryStoreOptions m_options;
};

} // namespace pdf

#endif // PDFOPERATIONHISTORYSTORE_H
