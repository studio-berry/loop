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

#ifndef PDFAUTHORINGHISTORY_H
#define PDFAUTHORINGHISTORY_H

#include "pdfoperationhistorystore.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace pdf
{

/// Appends one accepted interactive authoring edit to the document's
/// `.loop-history` sidecar, the same chain docs/OPERATION_HISTORY.md defines and
/// issue #124's forward-rollback undo reads:
///
/// 1. the new revision's bytes are imported into the sidecar's artifact store;
/// 2. the pre-edit revision is registered as the protected original input when
///    the chain holds no rollback point for it yet, so the first edit's undo has
///    a target;
/// 3. one accepted `FixApplied` event is appended; appendEvent creates the
///    matching `PDFRollbackPoint` from the output artifact.
///
/// The sidecar lives at `<documentPath>.loop-history/history.sqlite3` and is
/// created on first use, so an unsaved interactive session still leaves a
/// verifiable chain beside the document.
///
/// Returns false with the store's message when the chain could not be extended.
/// The caller's in-memory edit is already applied and is never rolled back here:
/// a provenance failure is reported, not repaired.
LOOPLIBCORESHARED_EXPORT PDFOperationResult appendAuthoringEditEvent(const QString& documentPath,
                                                                     const QByteArray& inputBytes,
                                                                     const QByteArray& outputBytes,
                                                                     const QString& operationId,
                                                                     const QString& operatorIdentity,
                                                                     const QJsonObject& resultSummary,
                                                                     quint64* sequence = nullptr);

}   // namespace pdf

#endif   // PDFAUTHORINGHISTORY_H
