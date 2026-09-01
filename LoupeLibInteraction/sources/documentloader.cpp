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

#include "documentloader.h"

#include "pdfthreadaffinity.h"

#include "pdfdocumentreader.h"
#include "pdfdocumentwriter.h"

#include <QFileInfo>

#include <utility>

namespace pdfinteraction
{

QString DocumentSource::displayLabel() const
{
    return QFileInfo(path).fileName();
}

PDFReaderDocumentLoader::PDFReaderDocumentLoader(std::function<QString(bool*)> queryPassword,
                                                 pdf::PDFProcessingLimits processingLimits) :
    m_queryPassword(std::move(queryPassword)),
    m_processingLimits(std::move(processingLimits))
{
}

DocumentLoadResult PDFReaderDocumentLoader::load(const DocumentSource& source,
                                                 pdf::PDFJobContext& context)
{
    // Issue #144 AC1: this is expensive and unbounded, so it must not be
    // reachable from an input handler or a frame callback. The guard does
    // not move the work -- it reports that the work is in the wrong place.
    pdf::PDFThreadAffinity::requireNotInteractive("file-io");

    DocumentLoadResult result;

    if (!source.isValid())
    {
        result.typedError = QStringLiteral("document/invalid-source");
        return result;
    }

    if (context.isCancellationRequested())
    {
        result.outcome = DocumentLoadOutcome::Cancelled;
        result.typedError = QStringLiteral("document/cancelled");
        return result;
    }

    // A host that cannot prompt must still produce a definite answer. Reporting
    // "not ok" makes the reader treat an encrypted document as cancelled rather
    // than retrying with an empty password.
    std::function<QString(bool*)> queryPassword = m_queryPassword;
    if (!queryPassword)
    {
        queryPassword = [](bool* ok)
        {
            if (ok)
            {
                *ok = false;
            }
            return QString();
        };
    }

    pdf::PDFDocumentReader reader(nullptr, queryPassword, true, false, m_processingLimits);
    reader.setOperationControl(context.operationControl());
    pdf::PDFDocument document = reader.readFromFile(source.path);

    if (context.isCancellationRequested())
    {
        result.outcome = DocumentLoadOutcome::Cancelled;
        result.typedError = QStringLiteral("document/cancelled");
        return result;
    }

    switch (reader.getReadingResult())
    {
        case pdf::PDFDocumentReader::Result::OK:
            result.outcome = DocumentLoadOutcome::Loaded;
            result.document = pdf::PDFDocumentPointer(new pdf::PDFDocument(std::move(document)));
            result.incomplete = !reader.getWarnings().isEmpty();
            break;

        case pdf::PDFDocumentReader::Result::Cancelled:
            result.outcome = DocumentLoadOutcome::Cancelled;
            result.typedError = QStringLiteral("document/cancelled");
            break;

        case pdf::PDFDocumentReader::Result::Failed:
            result.outcome = DocumentLoadOutcome::Failed;
            // The reader's message can quote document content, so it is not
            // forwarded to a presentation host.
            result.typedError = QStringLiteral("document/read-failed");
            break;
    }

    return result;
}

DocumentWriteResult PDFDocumentFileWriter::write(const DocumentSource& target,
                                                 const pdf::PDFDocument* document,
                                                 pdf::PDFJobContext& context)
{
    // Issue #144 AC1: this is expensive and unbounded, so it must not be
    // reachable from an input handler or a frame callback. The guard does
    // not move the work -- it reports that the work is in the wrong place.
    pdf::PDFThreadAffinity::requireNotInteractive("file-io");

    DocumentWriteResult result;

    if (!target.isValid() || !document)
    {
        result.typedError = QStringLiteral("document/invalid-target");
        return result;
    }

    if (context.isCancellationRequested())
    {
        result.outcome = DocumentWriteOutcome::Cancelled;
        result.typedError = QStringLiteral("document/cancelled");
        return result;
    }

    pdf::PDFDocumentWriter writer(nullptr);
    writer.setOperationControl(context.operationControl());
    const pdf::PDFOperationResult writeResult = writer.write(target.path, document, true);

    if (context.isCancellationRequested())
    {
        result.outcome = DocumentWriteOutcome::Cancelled;
        result.typedError = QStringLiteral("document/cancelled");
        return result;
    }

    if (writeResult)
    {
        result.outcome = DocumentWriteOutcome::Written;
        return result;
    }

    result.outcome = DocumentWriteOutcome::Failed;
    result.typedError = QStringLiteral("document/write-failed");
    return result;
}

}   // namespace pdfinteraction
